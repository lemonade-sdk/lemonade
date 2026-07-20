"""
CLI client tests for Lemonade CLI (lemonade command).

Tests the lemonade CLI client commands (HTTP client for Lemonade Server):
- status
- list
- export
- backends
- import (from JSON file)
- pull with labels and checkpoints
- load
- unload
- delete

Expects a running server (started by the installer or manually).

Usage:
    python server_cli2.py
    python server_cli2.py --cli-binary /path/to/lemonade
"""

import argparse
import glob
import json
import os
import platform
import re
import requests
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

from utils.server_base import _auth_headers, set_server_config, wait_for_server
from utils.test_models import (
    ENDPOINT_TEST_MODEL,
    MULTI_REPO_MODEL_A_CACHE_DIR,
    MULTI_REPO_MODEL_A_MAIN,
    MULTI_REPO_MODEL_A_NAME,
    MULTI_REPO_MODEL_B_CACHE_DIR,
    MULTI_REPO_MODEL_B_MAIN,
    MULTI_REPO_MODEL_B_NAME,
    MULTI_REPO_SHARED_CACHE_DIR,
    MULTI_REPO_SHARED_CHECKPOINT,
    PORT,
    SHARED_REPO_MODEL_A_CHECKPOINT,
    SHARED_REPO_MODEL_A_NAME,
    SHARED_REPO_MODEL_B_CHECKPOINT,
    SHARED_REPO_MODEL_B_NAME,
    TIMEOUT_DEFAULT,
    TIMEOUT_MODEL_OPERATION,
    USER_MODEL_MAIN_CHECKPOINT,
    USER_MODEL_TE_CHECKPOINT,
    USER_MODEL_NAME,
    get_default_cli_binary,
    get_hf_cache_dir_candidates,
)


def _checkpoint_variant_path(checkpoint):
    """Return the repo-relative variant path for a HF checkpoint string."""
    parts = checkpoint.split(":", 1)
    if len(parts) != 2:
        return ""
    return os.path.join(*parts[1].split("/"))


def _find_cached_checkpoint(cache_root, repo_cache_dir, checkpoint):
    """Return the on-disk snapshot path for a checkpoint, if present."""
    variant_path = _checkpoint_variant_path(checkpoint)
    if not variant_path:
        return None

    pattern = os.path.join(cache_root, repo_cache_dir, "snapshots", "*", variant_path)
    matches = glob.glob(pattern)
    if matches:
        return matches[0]
    return None


def _resolve_hf_cache_root(repo_cache_dirs, checkpoint_specs=None):
    """Pick the HF cache root that actually contains the downloaded repo artifacts."""
    diagnostics = []
    matches = []

    for hf_cache in get_hf_cache_dir_candidates():
        missing = [
            repo_dir
            for repo_dir in repo_cache_dirs
            if not os.path.isdir(os.path.join(hf_cache, repo_dir))
        ]
        checkpoint_paths = []
        if not missing and checkpoint_specs:
            for repo_cache_dir, checkpoint in checkpoint_specs:
                checkpoint_path = _find_cached_checkpoint(
                    hf_cache, repo_cache_dir, checkpoint
                )
                if checkpoint_path is None:
                    missing.append(f"{repo_cache_dir}:{checkpoint}")
                else:
                    checkpoint_paths.append(checkpoint_path)

        if not missing:
            probe_paths = [
                os.path.join(hf_cache, repo_cache_dir)
                for repo_cache_dir in repo_cache_dirs
            ] + checkpoint_paths
            newest_mtime = max(os.path.getmtime(path) for path in probe_paths)
            matches.append((newest_mtime, hf_cache))
            continue
        diagnostics.append(f"{hf_cache} (missing: {', '.join(missing)})")

    if matches:
        matches.sort(reverse=True)
        return matches[0][1]

    raise AssertionError(
        "Could not resolve HF cache root after pull. Checked: " + "; ".join(diagnostics)
    )


# Global configuration
_config = {
    "cli_binary": None,
    "cli_api_key": None,
}

IS_WINDOWS = platform.system() == "Windows"
WINDOWS_LAUNCH_STUB_SKIP_REASON = "Windows launch-stub execution uses non-native script binaries; covered on Unix runners"


def get_cli_binary():
    """Get the lemonade CLI binary path."""
    return _config["cli_binary"] or get_default_cli_binary()


def parse_cli_args():
    """Parse command line arguments for CLI client tests."""
    parser = argparse.ArgumentParser(description="Test lemonade CLI client")
    parser.add_argument(
        "--cli-binary",
        type=str,
        default=get_default_cli_binary(),
        help="Path to lemonade CLI binary (default: CMake build output)",
    )
    parser.add_argument(
        "--api-key",
        type=str,
        default=None,
        help="API key to pass to all CLI invocations (for servers requiring auth)",
    )

    args = parser.parse_args()

    _config["cli_binary"] = args.cli_binary
    _config["cli_api_key"] = args.api_key

    return args


def run_cli_command(args, timeout=60, check=False, env=None, input_text=None):
    """
    Run a CLI command and return the result.

    If --api-key was provided to the test runner, it is automatically prepended
    to every CLI invocation so commands work against a server requiring auth.

    Args:
        args: List of command arguments (without the binary)
        timeout: Command timeout in seconds
        check: If True, raise CalledProcessError on non-zero exit
        env: Optional environment override for subprocess
        input_text: Optional stdin text for interactive prompts

    Returns:
        subprocess.CompletedProcess result
    """
    cli_args = list(args)
    if _config["cli_api_key"]:
        cli_args = ["--api-key", _config["cli_api_key"]] + cli_args
    cli_binary = get_cli_binary()
    if os.path.isabs(cli_binary):
        resolved_cli_binary = cli_binary
    elif os.path.sep in cli_binary or (os.path.altsep and os.path.altsep in cli_binary):
        resolved_cli_binary = os.path.abspath(cli_binary)
    else:
        resolved_cli_binary = shutil.which(cli_binary) or cli_binary

    cmd = [resolved_cli_binary] + args
    print(f"Running: {' '.join(cmd)}")

    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        input=input_text,
        timeout=timeout,
        encoding="utf-8",
        errors="replace",
        env=env,
    )

    if result.stdout:
        print(f"stdout: {result.stdout}")
    if result.stderr:
        print(f"stderr: {result.stderr}")

    if check and result.returncode != 0:
        raise subprocess.CalledProcessError(
            result.returncode, cmd, result.stdout, result.stderr
        )

    return result


def _is_transient_cli_pull_failure(result):
    """Return True when a failed CLI pull looks like a transient server/HF issue."""
    if result.returncode == 0:
        return False

    output = f"{result.stdout or ''}\n{result.stderr or ''}".lower()
    transient_statuses = {408, 409, 429, 500, 502, 503, 504}
    observed_statuses = {
        int(match) for match in re.findall(r"(?:status\s*[:=]|http\s*)(\d{3})", output)
    }

    if observed_statuses.intersection(transient_statuses):
        return True

    return (
        "too many requests" in output
        or "rate limit" in output
        or "temporarily unavailable" in output
        or "connection reset" in output
        or "connection aborted" in output
        or "connection refused" in output
        or "timed out" in output
        or "timeout" in output
    )


def run_cli_pull_command_with_retry(args, timeout=TIMEOUT_MODEL_OPERATION, attempts=3):
    """Run a CLI pull command with bounded retry for transient setup failures.

    The command still has to succeed with exit code 0. This only retries failures
    that look transient, such as HF rate limits or temporary server/network errors.
    """
    last_result = None

    for attempt in range(1, attempts + 1):
        if attempt > 1:
            time.sleep(min(30, 2 ** (attempt - 1)))

        result = run_cli_command(args, timeout=timeout)
        if result.returncode == 0:
            return result

        last_result = result
        if _is_transient_cli_pull_failure(result) and attempt < attempts:
            print(
                f"Transient CLI pull failure, attempt {attempt}/{attempts}. "
                "Retrying..."
            )
            continue

        break

    return last_result


class PersistentServerCLIClientTests(unittest.TestCase):
    """
    CLI client tests that run with a persistent server.

    Expects a running server (started by the installer or manually).
    Tests run in order and may depend on previous test state.
    """

    @classmethod
    def setUpClass(cls):
        """Verify server is running."""
        super().setUpClass()
        print("\n=== Verifying server is reachable for CLI client tests ===")
        try:
            wait_for_server(timeout=30)
        except TimeoutError:
            raise RuntimeError(
                "Server is not running on port %d. "
                "Start the server before running tests." % PORT
            )
        print("Server is reachable")

    def assertCommandSucceeds(self, args, timeout=60):
        """Assert that a CLI command succeeds (exit code 0)."""
        result = run_cli_command(args, timeout=timeout)
        self.assertEqual(
            result.returncode,
            0,
            f"Command failed with exit code {result.returncode}: {result.stderr}",
        )
        return result

    def assertCommandFails(self, args, timeout=60):
        """Assert that a CLI command fails (non-zero exit code)."""
        result = run_cli_command(args, timeout=timeout)
        self.assertNotEqual(
            result.returncode,
            0,
            f"Command unexpectedly succeeded: {result.stdout}",
        )
        return result

    def _write_fake_agent(self, directory, agent_name, capture_file):
        """Create a tiny fake agent binary that captures argv and selected env vars."""
        exe_path = os.path.join(directory, agent_name)
        script = f"""#!/usr/bin/env python3
import json
import os
import sys

payload = {{
    "argv": sys.argv[1:],
    "env": {{
        "ANTHROPIC_BASE_URL": os.environ.get("ANTHROPIC_BASE_URL", ""),
        "ANTHROPIC_AUTH_TOKEN": os.environ.get("ANTHROPIC_AUTH_TOKEN", ""),
        "LEMONADE_API_KEY": os.environ.get("LEMONADE_API_KEY", ""),
        "OPENAI_BASE_URL": os.environ.get("OPENAI_BASE_URL", ""),
        "OPENAI_API_KEY": os.environ.get("OPENAI_API_KEY", ""),
        "ANTHROPIC_DEFAULT_SONNET_MODEL": os.environ.get("ANTHROPIC_DEFAULT_SONNET_MODEL", ""),
        "CLAUDE_CODE_SUBAGENT_MODEL": os.environ.get("CLAUDE_CODE_SUBAGENT_MODEL", ""),
    }},
}}

with open({capture_file!r}, "w", encoding="utf-8") as f:
    json.dump(payload, f)

print("fake-agent-ok")
sys.exit(0)
"""
        with open(exe_path, "w", encoding="utf-8") as f:
            f.write(script)
        os.chmod(exe_path, 0o755)
        return exe_path

    def _build_stubbed_agent_env(self, stub_dir):
        """Build isolated env so PATH resolves fake agents and avoids first-run side effects."""
        env = os.environ.copy()
        env.pop("OPENAI_BASE_URL", None)
        env["PATH"] = stub_dir + os.pathsep + env.get("PATH", "")
        env["HOME"] = stub_dir
        env["XDG_CONFIG_HOME"] = os.path.join(stub_dir, ".config")
        env["XDG_CACHE_HOME"] = os.path.join(stub_dir, ".cache")
        env["XDG_DATA_HOME"] = os.path.join(stub_dir, ".local", "share")
        return env

    def _build_noop_opener_env(self, stub_dir):
        """Build env with no-op URL opener binaries to avoid GUI popups in run tests on Unix."""
        opener_script = "#!/usr/bin/env sh\nexit 0\n"
        for name in ["xdg-open", "open"]:
            opener_path = os.path.join(stub_dir, name)
            with open(opener_path, "w", encoding="utf-8") as f:
                f.write(opener_script)
            os.chmod(opener_path, 0o755)

        return self._build_stubbed_agent_env(stub_dir)

    def _build_missing_agent_env(self, stub_dir):
        """Build env that guarantees agent binary lookup fails deterministically on all OSes."""
        env = self._build_stubbed_agent_env(stub_dir)
        env["PATH"] = stub_dir
        return env

    # =============================================================================
    # Version Tests
    # =============================================================================

    def test_005_version(self):
        """Test --version flag."""
        result = self.assertCommandSucceeds(["--version"])
        self.assertTrue(
            len(result.stdout) > 0 or len(result.stderr) > 0,
            "Version command should produce output",
        )

    # =============================================================================
    # Status Tests
    # =============================================================================

    def test_010_status(self):
        """Test status command."""
        result = self.assertCommandSucceeds(["status"])
        output = result.stdout + result.stderr
        print(f"Status output: {output}")

    def test_011_status_with_global_options(self):
        """Test status command with global options."""
        result = run_cli_command(
            ["--host", "127.0.0.1", "--port", str(PORT), "status"],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Status with options exit code: {result.returncode}")

    # =============================================================================
    # List Tests
    # =============================================================================

    def _section_between(self, output, start_header, end_header=None):
        lines = output.splitlines()
        self.assertIn(start_header, lines)

        start = lines.index(start_header) + 1
        end = (
            lines.index(end_header)
            if end_header and end_header in lines
            else len(lines)
        )

        return "\n".join(lines[start:end])

    def _parse_model_table(self, section_text):
        parsed_models = []
        for line in section_text.splitlines():
            line = line.strip()
            if (
                not line
                or line.startswith("Model Name")
                or line.startswith("---")
                or "No local models" in line
                or "No models available" in line
            ):
                continue
            parts = line.split()
            if len(parts) >= 2:
                name = parts[0]
                downloaded = parts[1] == "Yes"
                details = parts[2] if len(parts) > 2 else ""
                parsed_models.append(
                    {"name": name, "downloaded": downloaded, "details": details}
                )
        return parsed_models

    def test_020_list(self):
        """Test list command."""
        result = self.assertCommandSucceeds(["list"])
        output = result.stdout + result.stderr
        print(f"List output: {output}")
        output_lines = output.splitlines()
        self.assertIn("Local", output_lines)
        self.assertIn("Available for Download", output_lines)
        self.assertLess(
            output_lines.index("Local"),
            output_lines.index("Available for Download"),
        )

        local_section = self._section_between(output, "Local", "Available for Download")
        available_section = self._section_between(output, "Available for Download")

        local_models = self._parse_model_table(local_section)
        available_models = self._parse_model_table(available_section)

        if not local_models:
            self.assertIn("No local models downloaded.", local_section)
        else:
            for m in local_models:
                self.assertTrue(
                    m["downloaded"],
                    f"Model {m['name']} in Local section should be downloaded (Yes)",
                )

        for m in available_models:
            self.assertFalse(
                m["downloaded"],
                f"Model {m['name']} in Available for Download section should not be downloaded (No)",
            )

    def test_021_list_downloaded_flag(self):
        """Test list --downloaded flag."""
        # Get all models from the full list to know what's available vs downloaded
        all_result = self.assertCommandSucceeds(["list"])
        all_output = all_result.stdout + all_result.stderr
        available_section = self._section_between(all_output, "Available for Download")
        available_models = self._parse_model_table(available_section)

        result = self.assertCommandSucceeds(["list", "--downloaded"])
        output = result.stdout + result.stderr
        print(f"List --downloaded output: {output}")
        output_lines = output.splitlines()
        self.assertNotIn("Local", output_lines)
        self.assertNotIn("Available for Download", output_lines)

        # A fresh cache has no local models yet, so --downloaded may return
        # the empty local-state message instead of a table.
        if "No local models downloaded." in output:
            downloaded_models = []
        else:
            self.assertIn("Model Name", output)
            self.assertIn("Downloaded", output)
            self.assertIn("Details", output)
            downloaded_models = self._parse_model_table(output)

        for m in downloaded_models:
            self.assertTrue(
                m["downloaded"],
                f"Model {m['name']} in --downloaded list should be downloaded (Yes)",
            )

        non_downloaded_names = [
            m["name"] for m in available_models if not m["downloaded"]
        ]
        if non_downloaded_names:
            downloaded_names = [m["name"] for m in downloaded_models]
            for name in non_downloaded_names:
                self.assertNotIn(
                    name,
                    downloaded_names,
                    f"Non-downloaded model {name} should not be in --downloaded list",
                )

    # =============================================================================
    # Export Tests
    # =============================================================================

    def test_030_export_with_output_file(self):
        """Test export command with --output flag."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            temp_file = f.name

        try:
            result = run_cli_command(
                ["export", ENDPOINT_TEST_MODEL, "--output", temp_file],
                timeout=TIMEOUT_DEFAULT,
            )
            print(f"Export to file exit code: {result.returncode}")
        finally:
            if os.path.exists(temp_file):
                os.unlink(temp_file)

    def test_031_export_to_stdout(self):
        """Test export command without --output (prints to stdout)."""
        result = run_cli_command(
            ["export", ENDPOINT_TEST_MODEL],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Export to stdout exit code: {result.returncode}")

    # =============================================================================
    # Recipes Tests
    # =============================================================================

    def test_040_backends(self):
        """Test backends command."""
        result = self.assertCommandSucceeds(["backends"])
        output = result.stdout + result.stderr
        self.assertTrue(
            len(output) > 0,
            f"Backends command should produce output: {output}",
        )
        print(f"Backends output: {output}")

    @unittest.skipIf(
        platform.system() == "Darwin", "llamacpp:cpu not supported on macOS"
    )
    def test_041_backends_install(self):
        """Test backends install."""
        result = self.assertCommandSucceeds(["backends", "install", "llamacpp:cpu"])
        print(f"Backends install exit code: {result.returncode}")

    @unittest.skipIf(
        platform.system() == "Darwin", "llamacpp:cpu not supported on macOS"
    )
    def test_042_backends_uninstall(self):
        """Test backends uninstall."""
        result = self.assertCommandSucceeds(["backends", "uninstall", "llamacpp:cpu"])
        print(f"Backends uninstall exit code: {result.returncode}")

    # =============================================================================
    # Runtime Config Tests
    # =============================================================================

    def test_043_listen_all_via_runtime_config(self):
        """Test that setting host to 0.0.0.0 via /internal/set works."""
        try:
            # Set host to 0.0.0.0 (listen on all interfaces).
            try:
                set_server_config({"host": "0.0.0.0"})
                print("[OK] Set host to 0.0.0.0 via /internal/set")
            except Exception as e:
                self.fail(f"Failed to set host to 0.0.0.0: {e}")

            # Wait for server to finish rebinding. Use 127.0.0.1 explicitly
            # because 0.0.0.0 only binds IPv4, and "localhost" may resolve to
            # ::1 (IPv6) in some environments (e.g. Fedora containers).
            for i in range(30):
                try:
                    response = requests.get(
                        f"http://127.0.0.1:{PORT}/api/v1/health",
                        headers=_auth_headers(),
                        timeout=2,
                    )
                    if response.status_code == 200:
                        break
                except requests.ConnectionError:
                    pass
                time.sleep(1)
            else:
                self.fail(
                    "Server did not become reachable on 127.0.0.1 after rebind to 0.0.0.0"
                )

            # Verify the server still responds (status command should work).
            result = self.assertCommandSucceeds(["status"])
            output = result.stdout.lower() + result.stderr.lower()
            self.assertTrue(
                "running" in output or "online" in output or "active" in output,
                f"Status should indicate server is running on 0.0.0.0: {result.stdout}",
            )

            # Verify via health endpoint too (use 127.0.0.1 for same IPv4 reason).
            response = requests.get(
                f"http://127.0.0.1:{PORT}/api/v1/health",
                headers=_auth_headers(),
                timeout=10,
            )
            self.assertEqual(response.status_code, 200)
        finally:
            # Best-effort restore: this test runs inside CI jobs that execute
            # multiple modules against one long-lived server, so cleanup must not
            # mask the primary assertion failure. reset_server_state also restores
            # the host before subsequent CI suites.
            try:
                response = requests.post(
                    f"http://127.0.0.1:{PORT}/internal/set",
                    json={"host": "localhost"},
                    headers=_auth_headers(),
                    timeout=10,
                )
                if response.status_code < 400:
                    print("[OK] Restored host to localhost")
                else:
                    print(
                        "Warning: Failed to restore host to localhost: "
                        f"{response.status_code}: {response.text}"
                    )
            except Exception as e:  # noqa: BLE001 - best-effort cleanup
                print(f"Warning: Failed to restore host to localhost: {e}")

    def test_044_config_set_flat_backend_key(self):
        """Flat backend keys (e.g. vllm_args) map to their nested form (issue #1824)."""
        try:
            # The flat underscore form is what the CLI flags and docs use; it
            # must be accepted and stored under the nested "vllm.args" key.
            self.assertCommandSucceeds(
                ["config", "set", "vllm_args=--max-num-seqs 128"]
            )

            result = self.assertCommandSucceeds(["config"])
            output = result.stdout
            self.assertIn(
                "vllm.args",
                output,
                f"vllm.args should appear in config output: {output}",
            )
            self.assertIn(
                "--max-num-seqs 128",
                output,
                f"flat vllm_args value should be stored under vllm.args: {output}",
            )
        finally:
            # Restore the default (empty) value so later tests are unaffected.
            run_cli_command(["config", "set", "vllm.args="])

    def test_044_config_set_cli(self):
        """Verify that CLI config set parses nested dotted keys and modifies the server config."""
        # 1. Set some values using the CLI config set
        result = self.assertCommandSucceeds(
            [
                "config",
                "set",
                "telemetry.otlp.endpoint=http://127.0.0.1:4444/v1/traces",
                "telemetry.otlp.protocol=http/json",
            ]
        )
        print(f"Config set output: {result.stdout}")

        # 2. Query the params to verify it parsed correctly and merged
        response = requests.get(
            f"http://localhost:{PORT}/api/v1/params",
            headers=_auth_headers(),
            timeout=10,
        )
        self.assertEqual(response.status_code, 200)
        telemetry = response.json().get("telemetry", {})
        self.assertEqual(
            telemetry.get("otlp", {}).get("endpoint"), "http://127.0.0.1:4444/v1/traces"
        )
        self.assertEqual(telemetry.get("otlp", {}).get("protocol"), "http/json")

    # =============================================================================
    # Pull Tests
    # =============================================================================

    def test_050_pull_with_checkpoint(self):
        """Test pull command with --checkpoint option."""
        result = run_cli_pull_command_with_retry(
            [
                "pull",
                USER_MODEL_NAME,
                "--checkpoint",
                "main",
                USER_MODEL_MAIN_CHECKPOINT,
                "--recipe",
                "llamacpp",
            ],
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        print(f"Pull with checkpoint exit code: {result.returncode}")

    def test_051_pull_with_labels(self):
        """Test pull command with --label option."""
        result = run_cli_pull_command_with_retry(
            [
                "pull",
                USER_MODEL_NAME,
                "--checkpoint",
                "main",
                USER_MODEL_MAIN_CHECKPOINT,
                "--recipe",
                "llamacpp",
                "--label",
                "reasoning",
                "--label",
                "coding",
            ],
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        print(f"Pull with labels exit code: {result.returncode}")

    def test_052_pull_invalid_label(self):
        """Test pull command with invalid label should fail validation."""
        result = self.assertCommandFails(
            [
                "pull",
                USER_MODEL_NAME,
                "--checkpoint",
                "main",
                USER_MODEL_MAIN_CHECKPOINT,
                "--recipe",
                "llamacpp",
                "--label",
                "invalid-label",
            ],
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        output = result.stdout + result.stderr
        self.assertIn(
            "run with --help",
            output.lower(),
            f"Should show validation error for invalid label: {output}",
        )

    def test_053_pull_with_multiple_checkpoints(self):
        """Test pull command with multiple checkpoints (e.g., main + mmproj)."""
        result = run_cli_pull_command_with_retry(
            [
                "pull",
                USER_MODEL_NAME,
                "--checkpoint",
                "main",
                USER_MODEL_MAIN_CHECKPOINT,
                "--checkpoint",
                "text_encoder",
                USER_MODEL_TE_CHECKPOINT,
                "--recipe",
                "sd-cpp",
            ],
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        print(f"Pull with multiple checkpoints exit code: {result.returncode}")

    def test_054_pull_registered_name(self):
        """Test pull command with a registered model name (no flags)."""
        result = run_cli_pull_command_with_retry(
            ["pull", ENDPOINT_TEST_MODEL],
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"Command failed with exit code {result.returncode}: {result.stderr}",
        )
        output = result.stdout.lower() + result.stderr.lower()
        self.assertFalse(
            "error" in output and "failed" in output,
            f"Pull should not report errors: {result.stdout}",
        )

    def test_055_pull_components_omni_collection(self):
        """Test pull command with --components flag registers an omni collection."""
        collection_name = f"user.CliColl-{uuid.uuid4().hex[:8]}"
        # Unique user.<name> entries surface under the bare public name.
        public_name = collection_name[5:]
        try:
            result = run_cli_pull_command_with_retry(
                [
                    "pull",
                    collection_name,
                    "--recipe",
                    "collection.omni",
                    "--components",
                    ENDPOINT_TEST_MODEL,
                ],
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(
                result.returncode,
                0,
                f"Command failed with exit code {result.returncode}: {result.stderr}",
            )
            output = result.stdout.lower() + result.stderr.lower()
            self.assertFalse(
                "error" in output and "failed" in output,
                f"Pull should not report errors: {result.stdout}",
            )

            # Verify the collection landed in /models with components populated.
            response = requests.get(
                f"http://localhost:{PORT}/api/v1/models?show_all=true",
                headers=_auth_headers(),
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(response.status_code, 200)
            entry = next(
                (m for m in response.json()["data"] if m["id"] == public_name),
                None,
            )
            self.assertIsNotNone(entry, f"{public_name} should appear in /models")
            self.assertEqual(entry.get("recipe"), "collection.omni")
            self.assertEqual(entry.get("components"), [ENDPOINT_TEST_MODEL])
        finally:
            try:
                requests.post(
                    f"http://localhost:{PORT}/api/v1/delete",
                    json={"model_name": collection_name},
                    headers=_auth_headers(),
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_056_pull_components_missing_for_omni_recipe(self):
        """`--recipe collection.omni` without --components is rejected by the CLI."""
        result = self.assertCommandFails(
            [
                "pull",
                f"user.MissingComps-{uuid.uuid4().hex[:8]}",
                "--recipe",
                "collection.omni",
            ],
            timeout=TIMEOUT_DEFAULT,
        )
        output = (result.stdout + result.stderr).lower()
        self.assertIn(
            "--components",
            output,
            f"Error should point at the missing --components flag: {output}",
        )

    # =============================================================================
    # Import Tests
    # =============================================================================

    def test_060_import_json_file(self):
        """Test import command with JSON configuration file."""
        json_data = {
            "model_name": USER_MODEL_NAME,
            "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
            "recipe": "llamacpp",
            "labels": ["reasoning"],
        }

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json_file = f.name
            json.dump(json_data, f)

        try:
            result = run_cli_command(
                ["import", json_file],
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            print(f"Import from JSON exit code: {result.returncode}")
        finally:
            if os.path.exists(json_file):
                os.unlink(json_file)

    def test_061_import_malformed_json(self):
        """Test import command with malformed JSON file should fail."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json_file = f.name
            f.write('{"model_name": "test", "recipe": "llamacpp"')

        try:
            result = self.assertCommandFails(
                ["import", json_file],
                timeout=TIMEOUT_DEFAULT,
            )
            output = result.stdout + result.stderr
            self.assertIn(
                "error",
                output.lower(),
                f"Should show JSON parse error: {output}",
            )
        finally:
            if os.path.exists(json_file):
                os.unlink(json_file)

    def test_062_import_missing_model_name(self):
        """Test import command with JSON missing model_name should fail."""
        json_data = {
            "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
            "recipe": "llamacpp",
        }

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json_file = f.name
            json.dump(json_data, f)

        try:
            result = self.assertCommandFails(
                ["import", json_file],
                timeout=TIMEOUT_DEFAULT,
            )
            output = result.stdout + result.stderr
            self.assertIn(
                "error",
                output.lower(),
                f"Should show error about missing model_name: {output}",
            )
        finally:
            if os.path.exists(json_file):
                os.unlink(json_file)

    def test_063_import_missing_recipe(self):
        """Test import command with JSON missing recipe should fail."""
        json_data = {
            "model_name": USER_MODEL_NAME,
            "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
        }

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json_file = f.name
            json.dump(json_data, f)

        try:
            result = self.assertCommandFails(
                ["import", json_file],
                timeout=TIMEOUT_DEFAULT,
            )
            output = result.stdout + result.stderr
            self.assertIn(
                "error",
                output.lower(),
                f"Should show error about missing recipe: {output}",
            )
        finally:
            if os.path.exists(json_file):
                os.unlink(json_file)

    def test_064_import_missing_checkpoint(self):
        """Test import command with JSON missing checkpoint should fail."""
        json_data = {
            "model_name": USER_MODEL_NAME,
            "recipe": "llamacpp",
        }

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json_file = f.name
            json.dump(json_data, f)

        try:
            result = self.assertCommandFails(
                ["import", json_file],
                timeout=TIMEOUT_DEFAULT,
            )
            output = result.stdout + result.stderr
            self.assertIn(
                "error",
                output.lower(),
                f"Should show error about missing checkpoint: {output}",
            )
        finally:
            if os.path.exists(json_file):
                os.unlink(json_file)

    def test_065_import_with_id_alias(self):
        """Test import command with JSON using 'id' as alias for model_name."""
        json_data = {
            "id": USER_MODEL_NAME,
            "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
            "recipe": "llamacpp",
        }

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json_file = f.name
            json.dump(json_data, f)

        try:
            result = run_cli_command(
                ["import", json_file],
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            print(f"Import from JSON with id alias exit code: {result.returncode}")
        finally:
            if os.path.exists(json_file):
                os.unlink(json_file)

    def test_066_import_nonexistent_file(self):
        """Test import command with nonexistent file should fail."""
        result = self.assertCommandFails(
            ["import", "nonexistent/path/to/file.json"],
            timeout=TIMEOUT_DEFAULT,
        )
        output = result.stdout + result.stderr
        self.assertIn(
            "error",
            output.lower(),
            f"Should show error about nonexistent file: {output}",
        )

    def test_067_import_remote_noninteractive_requires_directory(self):
        """Remote import should fail in non-interactive mode without --directory."""
        result = self.assertCommandFails(
            ["import", "--skip-prompt"],
            timeout=TIMEOUT_DEFAULT,
        )
        output = result.stdout + result.stderr
        self.assertIn(
            "--directory",
            output.lower(),
            f"Should require --directory in non-interactive mode: {output}",
        )

    def test_068_import_remote_noninteractive_requires_recipe_file(self):
        """Remote import should fail in non-interactive mode without --recipe-file."""
        result = self.assertCommandFails(
            ["import", "--skip-prompt", "--directory", "coding-agents"],
            timeout=TIMEOUT_DEFAULT,
        )
        output = result.stdout + result.stderr
        self.assertIn(
            "--recipe-file",
            output.lower(),
            f"Should require --recipe-file in non-interactive mode: {output}",
        )

    # =============================================================================
    # Load Tests
    # =============================================================================

    def test_070_load_with_ctx_size(self):
        """Test load command with --ctx-size option."""
        result = run_cli_command(
            ["load", ENDPOINT_TEST_MODEL, "--ctx-size", "8192"],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Load with ctx-size exit code: {result.returncode}")

    def test_071_load_with_save_options(self):
        """Test load command with --save-options flag."""
        result = run_cli_command(
            ["load", ENDPOINT_TEST_MODEL, "--save-options"],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Load with save-options exit code: {result.returncode}")

    # =============================================================================
    # Run Tests
    # =============================================================================

    def test_100_run_with_model(self):
        """Test run command with explicit model."""
        with tempfile.TemporaryDirectory(prefix="lemonade-open-stub-") as temp_dir:
            env = self._build_noop_opener_env(temp_dir)
            result = run_cli_command(
                ["run", ENDPOINT_TEST_MODEL],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )
            self.assertEqual(result.returncode, 0)

    def test_101_run_with_combined_options(self):
        """Test run command with --ctx-size and --save-options together."""
        with tempfile.TemporaryDirectory(prefix="lemonade-open-stub-") as temp_dir:
            env = self._build_noop_opener_env(temp_dir)
            result = run_cli_command(
                ["run", ENDPOINT_TEST_MODEL, "--ctx-size", "2048", "--save-options"],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )
            self.assertEqual(result.returncode, 0)

    def test_102_run_with_host_port(self):
        """Test run command using global --host/--port options."""
        with tempfile.TemporaryDirectory(prefix="lemonade-open-stub-") as temp_dir:
            env = self._build_noop_opener_env(temp_dir)
            result = run_cli_command(
                [
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(PORT),
                    "run",
                    ENDPOINT_TEST_MODEL,
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )
            self.assertEqual(result.returncode, 0)

    # =============================================================================
    # Launch Tests
    # =============================================================================
    # parser-only tests: run on all OSes
    # process-execution tests: run on non-Windows only with shebang-based fake binaries

    def test_110_launch_without_agent_prompts_selection(self):
        """Launch without an agent argument should present agent choices and continue."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "claude_capture_agent_prompt.json")
            self._write_fake_agent(temp_dir, "claude", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                [
                    "launch",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
                input_text="1\n",
            )

            self.assertEqual(result.returncode, 0)
            self.assertTrue(
                os.path.exists(capture_path),
                "Fake claude binary was not executed",
            )

            output = result.stdout + result.stderr
            self.assertIn("Select an agent to launch", output)
            self.assertIn("Selected agent: claude", output)

    def test_111_launch_invalid_agent_rejected(self):
        """Launch should reject unsupported agent names."""
        result = self.assertCommandFails(
            ["launch", "invalid-agent", "--model", ENDPOINT_TEST_MODEL],
            timeout=TIMEOUT_DEFAULT,
        )
        output = result.stdout + result.stderr
        self.assertIn("run with --help", output.lower())

    def test_112_launch_claude_with_fake_binary_and_api_key(self):
        """Launch should execute fake claude binary and wire expected auth/model env vars."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "claude_capture.json")
            self._write_fake_agent(temp_dir, "claude", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                [
                    "launch",
                    "claude",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                    "--api-key",
                    "test-api-key",
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)
            self.assertTrue(
                os.path.exists(capture_path),
                "Fake claude binary was not executed",
            )

            with open(capture_path, "r", encoding="utf-8") as f:
                payload = json.load(f)

            self.assertEqual(payload["env"]["ANTHROPIC_AUTH_TOKEN"], "test-api-key")
            self.assertEqual(payload["env"]["LEMONADE_API_KEY"], "test-api-key")
            self.assertEqual(
                payload["env"]["ANTHROPIC_DEFAULT_SONNET_MODEL"],
                ENDPOINT_TEST_MODEL,
            )
            self.assertEqual(
                payload["env"]["CLAUDE_CODE_SUBAGENT_MODEL"],
                ENDPOINT_TEST_MODEL,
            )

            output = result.stdout + result.stderr
            self.assertIn(
                "Model was provided explicitly; skipping recipe import prompts.",
                output,
            )
            self.assertIn("Launching claude", output)

    def test_113_launch_codex_with_fake_binary(self):
        """Launch should execute fake codex binary and pass expected argv/env."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "codex_capture.json")
            self._write_fake_agent(temp_dir, "codex", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                [
                    "launch",
                    "codex",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)
            self.assertTrue(
                os.path.exists(capture_path),
                "Fake codex binary was not executed",
            )

            with open(capture_path, "r", encoding="utf-8") as f:
                payload = json.load(f)

            argv = payload["argv"]
            self.assertIn("-c", argv)
            self.assertIn("-m", argv)
            self.assertIn(ENDPOINT_TEST_MODEL, argv)
            self.assertTrue(
                any(arg.startswith("model_providers.lemonade=") for arg in argv),
                "Expected injected Lemonade model provider config in codex args",
            )
            self.assertIn('model_provider="lemonade"', argv)
            self.assertEqual(payload["env"]["OPENAI_BASE_URL"], "")
            # OPENAI_API_KEY mirrors whatever lemonade was running with — the
            # default "lemonade" when no key is set, or the LEMONADE_API_KEY
            # env value when the test job sets one (e.g. Test API Key CI job).
            self.assertEqual(
                payload["env"]["OPENAI_API_KEY"],
                os.environ.get("LEMONADE_API_KEY", "lemonade"),
            )

    def test_114_launch_claude_defaults_and_host_normalization(self):
        """Claude launch should default auth token and normalize wildcard host to localhost."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "claude_capture_defaults.json")
            self._write_fake_agent(temp_dir, "claude", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                [
                    "--host",
                    "0.0.0.0",
                    "launch",
                    "claude",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)
            self.assertTrue(
                os.path.exists(capture_path),
                "Fake claude binary was not executed",
            )

            with open(capture_path, "r", encoding="utf-8") as f:
                payload = json.load(f)

            # Both env vars mirror the lemonade api key in use — default
            # "lemonade" when no key is set, or LEMONADE_API_KEY when the
            # test job sets one (e.g. Test API Key CI job).
            expected_key = os.environ.get("LEMONADE_API_KEY", "lemonade")
            self.assertEqual(payload["env"]["ANTHROPIC_AUTH_TOKEN"], expected_key)
            self.assertEqual(payload["env"]["LEMONADE_API_KEY"], expected_key)
            self.assertEqual(
                payload["env"]["ANTHROPIC_BASE_URL"], f"http://localhost:{PORT}"
            )

    def test_115_launch_claude_windows_prefers_cmd_over_npm_shim(self):
        """On Windows, launch must run claude.cmd, not npm's extensionless shell script."""
        if not IS_WINDOWS:
            self.skipTest(
                "Windows-only: npm shim vs .cmd resolution does not apply on Unix"
            )

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "claude_cmd_capture.txt")

            # Recreate what 'npm install -g' leaves on PATH: a Unix shell
            # script named just "claude" next to claude.cmd. Windows cannot
            # run the shell script, so the launcher must pick the .cmd.
            with open(os.path.join(temp_dir, "claude"), "w", encoding="utf-8") as f:
                f.write('#!/bin/sh\nexec node "$(dirname "$0")/claude.js" "$@"\n')
            with open(os.path.join(temp_dir, "claude.cmd"), "w", encoding="utf-8") as f:
                f.write(f'@echo off\necho fake-agent-ok> "{capture_path}"\nexit /b 0\n')

            env = self._build_stubbed_agent_env(temp_dir)
            result = run_cli_command(
                ["launch", "claude", "--model", ENDPOINT_TEST_MODEL],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(
                result.returncode,
                0,
                "launch failed; the extensionless npm shim was likely picked "
                "instead of claude.cmd (Error 193)",
            )
            self.assertTrue(
                os.path.exists(capture_path),
                "claude.cmd was not executed",
            )

    def test_102c_launch_codex_provider_default(self):
        """Codex launch -p should select default provider without injecting provider config."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(
                temp_dir, "codex_capture_user_config_default.json"
            )
            self._write_fake_agent(temp_dir, "codex", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)
            result = run_cli_command(
                [
                    "launch",
                    "codex",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                    "-p",
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)
            with open(capture_path, "r", encoding="utf-8") as f:
                payload = json.load(f)

            argv = payload["argv"]
            self.assertIn('model_provider="lemonade"', argv)
            self.assertFalse(
                any(arg.startswith("model_providers.lemonade=") for arg in argv)
            )

    def test_102d_launch_codex_provider_custom(self):
        """Codex launch --provider PROVIDER should target custom provider name."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(
                temp_dir, "codex_capture_user_config_custom.json"
            )
            self._write_fake_agent(temp_dir, "codex", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)
            result = run_cli_command(
                [
                    "launch",
                    "codex",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                    "--provider",
                    "custom-provider",
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)
            with open(capture_path, "r", encoding="utf-8") as f:
                payload = json.load(f)

            argv = payload["argv"]
            self.assertIn('model_provider="custom-provider"', argv)
            self.assertFalse(
                any(arg.startswith("model_providers.custom-provider=") for arg in argv)
            )

    def test_102e_launch_codex_provider_without_config_check(self):
        """Codex --provider should not read/validate config.toml in launcher."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(
                temp_dir, "codex_capture_provider_no_config_check.json"
            )
            self._write_fake_agent(temp_dir, "codex", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                [
                    "launch",
                    "codex",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                    "--provider",
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)
            with open(capture_path, "r", encoding="utf-8") as f:
                payload = json.load(f)

            argv = payload["argv"]
            self.assertIn('model_provider="lemonade"', argv)
            self.assertFalse(
                any(arg.startswith("model_providers.lemonade=") for arg in argv)
            )

    def test_102f_launch_codex_provider_custom_without_config_check(self):
        """Codex --provider custom name should not be launcher-validated against config.toml."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(
                temp_dir, "codex_capture_provider_custom_no_config_check.json"
            )
            self._write_fake_agent(temp_dir, "codex", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                [
                    "launch",
                    "codex",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                    "--provider",
                    "missing-in-config",
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)
            with open(capture_path, "r", encoding="utf-8") as f:
                payload = json.load(f)

            argv = payload["argv"]
            self.assertIn('model_provider="missing-in-config"', argv)
            self.assertFalse(
                any(
                    arg.startswith("model_providers.missing-in-config=") for arg in argv
                )
            )

    def test_102g_launch_claude_provider_rejected(self):
        """--provider should be rejected for non-codex agents."""
        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            env = self._build_missing_agent_env(temp_dir)
            result = run_cli_command(
                [
                    "launch",
                    "claude",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                    "--provider",
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertNotEqual(result.returncode, 0)
            output = result.stdout + result.stderr
            self.assertIn(
                "Error: --provider is only supported for 'lemonade launch codex'.",
                output,
            )

    def test_102h_launch_agent_args_passthrough(self):
        """--agent-args should be tokenized and appended to agent argv."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "claude_capture_agent_args.json")
            self._write_fake_agent(temp_dir, "claude", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                [
                    "launch",
                    "claude",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                    "--agent-args",
                    "--approval-mode never --custom 'a b'",
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)
            with open(capture_path, "r", encoding="utf-8") as f:
                payload = json.load(f)

            argv = payload["argv"]
            self.assertIn("--approval-mode", argv)
            self.assertIn("never", argv)
            self.assertIn("--custom", argv)
            self.assertIn("a b", argv)

    def test_103_launch_explicit_model_with_repo_flags_is_deterministic(self):
        """Explicit model should skip import flow even when repo flags are present."""
        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            env = self._build_missing_agent_env(temp_dir)

            result = run_cli_command(
                [
                    "launch",
                    "claude",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                    "--directory",
                    "coding-agents",
                    "--recipe-file",
                    "dummy.json",
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertNotEqual(result.returncode, 0)
            output = result.stdout + result.stderr
            self.assertIn(
                "Model was provided explicitly; skipping recipe import prompts.",
                output,
            )
            self.assertIn("Agent binary not found", output)

    def test_116_launch_missing_binary_fails_fast(self):
        """Without a stub and no real claude installed, launch should fail at binary lookup."""
        if shutil.which("claude") is not None:
            self.skipTest(
                "Real claude binary installed; missing-binary behavior not deterministic"
            )

        result = run_cli_command(
            ["launch", "claude", "--model", ENDPOINT_TEST_MODEL],
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertNotEqual(result.returncode, 0)
        output = result.stdout + result.stderr
        self.assertIn("Agent binary not found", output)

    def test_117_launch_opencode_with_fake_binary(self):
        """Launch should execute fake opencode binary with -m Lemonade/MODEL."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "opencode_capture.json")
            self._write_fake_agent(temp_dir, "opencode", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                ["launch", "opencode", "--model", ENDPOINT_TEST_MODEL],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)
            self.assertTrue(
                os.path.exists(capture_path),
                "Fake opencode binary was not executed",
            )

            with open(capture_path, "r", encoding="utf-8") as f:
                payload = json.load(f)

            argv = payload["argv"]
            self.assertIn("-m", argv)
            model_idx = argv.index("-m") + 1
            self.assertEqual(argv[model_idx], f"Lemonade/{ENDPOINT_TEST_MODEL}")

    def test_118_launch_opencode_creates_config(self):
        """Launch opencode should create opencode.json with Lemonade provider."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "opencode_capture_cfg.json")
            self._write_fake_agent(temp_dir, "opencode", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                ["launch", "opencode", "--model", ENDPOINT_TEST_MODEL],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)

            config_path = os.path.join(temp_dir, ".config", "opencode", "opencode.json")
            self.assertTrue(
                os.path.exists(config_path),
                f"opencode.json not created at {config_path}",
            )

            with open(config_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)

            self.assertIn("provider", cfg)
            self.assertIn("Lemonade", cfg["provider"])
            lemonade = cfg["provider"]["Lemonade"]
            self.assertEqual(lemonade["npm"], "@ai-sdk/openai-compatible")
            self.assertIn("baseURL", lemonade["options"])
            # Mirrors lemonade api key in use; LEMONADE_API_KEY env wins.
            self.assertEqual(
                lemonade["options"]["apiKey"],
                os.environ.get("LEMONADE_API_KEY", "lemonade"),
            )
            self.assertIn(ENDPOINT_TEST_MODEL, lemonade["models"])
            self.assertEqual(
                lemonade["models"][ENDPOINT_TEST_MODEL]["contextWindow"],
                40960,
            )

    def test_119_launch_opencode_refreshes_model_entries(self):
        """Launch opencode should refresh Lemonade models and remove stale entries."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "opencode_capture_merge.json")
            self._write_fake_agent(temp_dir, "opencode", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            config_dir = os.path.join(temp_dir, ".config", "opencode")
            os.makedirs(config_dir)
            config_path = os.path.join(config_dir, "opencode.json")
            existing = {
                "$schema": "https://opencode.ai/config.json",
                "provider": {
                    "anthropic": {
                        "models": {"claude-3.5-sonnet": {}},
                    },
                    "Lemonade": {
                        "npm": "@ai-sdk/openai-compatible",
                        "name": "Lemonade Server (local)",
                        "options": {"baseURL": "http://old:9999/v1"},
                        "models": {
                            "User-Custom-Model": {
                                "name": "My Model",
                                "contextWindow": 8192,
                            }
                        },
                    },
                },
            }
            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(existing, f)

            result = run_cli_command(
                ["launch", "opencode", "--model", ENDPOINT_TEST_MODEL],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)

            with open(config_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)

            self.assertIn("anthropic", cfg["provider"])
            self.assertIn("claude-3.5-sonnet", cfg["provider"]["anthropic"]["models"])
            self.assertNotIn("User-Custom-Model", cfg["provider"]["Lemonade"]["models"])
            self.assertIn(ENDPOINT_TEST_MODEL, cfg["provider"]["Lemonade"]["models"])
            self.assertNotEqual(
                cfg["provider"]["Lemonade"]["options"]["baseURL"],
                "http://old:9999/v1",
            )

    def test_120_launch_opencode_with_api_key_sets_config(self):
        """When --api-key is provided, opencode.json should contain the same apiKey."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "opencode_capture_key.json")
            self._write_fake_agent(temp_dir, "opencode", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            result = run_cli_command(
                [
                    "launch",
                    "opencode",
                    "--model",
                    ENDPOINT_TEST_MODEL,
                    "--api-key",
                    "real-secret-key",
                ],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)

            config_path = os.path.join(temp_dir, ".config", "opencode", "opencode.json")
            with open(config_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)

            self.assertEqual(
                cfg["provider"]["Lemonade"]["options"].get("apiKey"),
                "real-secret-key",
            )

    def test_121_launch_opencode_backfills_schema_on_existing_config(self):
        """Launch opencode should add $schema when syncing an existing config missing it."""
        if IS_WINDOWS:
            self.skipTest(WINDOWS_LAUNCH_STUB_SKIP_REASON)

        with tempfile.TemporaryDirectory(prefix="lemonade-launch-stub-") as temp_dir:
            capture_path = os.path.join(temp_dir, "opencode_capture_schema.json")
            self._write_fake_agent(temp_dir, "opencode", capture_path)
            env = self._build_stubbed_agent_env(temp_dir)

            config_dir = os.path.join(temp_dir, ".config", "opencode")
            os.makedirs(config_dir)
            config_path = os.path.join(config_dir, "opencode.json")
            existing = {
                "provider": {
                    "Lemonade": {
                        "npm": "@ai-sdk/openai-compatible",
                        "name": "Lemonade Server (local)",
                        "options": {"baseURL": "http://old:9999/v1"},
                        "models": {},
                    }
                }
            }
            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(existing, f)

            result = run_cli_command(
                ["launch", "opencode", "--model", ENDPOINT_TEST_MODEL],
                timeout=TIMEOUT_DEFAULT,
                env=env,
            )

            self.assertEqual(result.returncode, 0)

            with open(config_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)

            self.assertEqual(cfg.get("$schema"), "https://opencode.ai/config.json")

    # =============================================================================
    # Unload Tests
    # =============================================================================

    def test_080_unload_with_model(self):
        """Test unload command with model name."""
        result = run_cli_command(
            ["unload", ENDPOINT_TEST_MODEL],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Unload with model exit code: {result.returncode}")

    def test_081_unload_without_model(self):
        """Test unload command without model name (unloads all)."""
        result = run_cli_command(
            ["unload"],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Unload without model exit code: {result.returncode}")

    # =============================================================================
    # Delete Tests
    # =============================================================================

    def test_090_delete_model(self):
        """Test delete command with model name."""
        result = run_cli_command(
            ["delete", ENDPOINT_TEST_MODEL],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Delete model exit code: {result.returncode}")

    def test_090a_naming_spec_list_shows_canonical_shadowed_rows(self):
        """`list` prints API ids verbatim, so shadowed sources appear under their canonical IDs.

        Verifies the copy-paste-safe contract: when a user.* shadows a built-in,
        the list shows both `<bare>` (winner) and `builtin.<bare>` (shadowed),
        and either string is directly usable in `lemonade load`/`delete`.
        """
        bare = ENDPOINT_TEST_MODEL  # known built-in
        user_canonical = f"user.{bare}"

        try:
            pull_response = requests.post(
                f"http://localhost:{PORT}/api/v1/pull",
                json={
                    "model_name": user_canonical,
                    "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
                    "recipe": "llamacpp",
                    "stream": False,
                },
                headers=_auth_headers(),
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(pull_response.status_code, 200)

            result = self.assertCommandSucceeds(["list"], timeout=TIMEOUT_DEFAULT)
            output = result.stdout + result.stderr

            # Winner appears as bare id; shadowed built-in appears under its canonical id.
            self.assertIn(bare, output)
            self.assertIn(f"builtin.{bare}", output)
            # The user.* canonical form must NOT appear in output — winners emit bare.
            self.assertNotIn(user_canonical, output)

            print(
                f"[OK] list shows both {bare} (user winner) and builtin.{bare} (shadowed)"
            )
        finally:
            run_cli_command(["delete", user_canonical], timeout=TIMEOUT_DEFAULT)

    def test_091_delete_preserves_shared_repo(self):
        """Test that deleting one model preserves files used by another model sharing the same repo."""
        # Import two user models that share the same HF repo (different GGUF quants)
        for name, checkpoint in [
            (SHARED_REPO_MODEL_A_NAME, SHARED_REPO_MODEL_A_CHECKPOINT),
            (SHARED_REPO_MODEL_B_NAME, SHARED_REPO_MODEL_B_CHECKPOINT),
        ]:
            json_file = os.path.join(tempfile.gettempdir(), f"lemonade_{name}.json")
            with open(json_file, "w") as f:
                f.write(
                    json.dumps(
                        {
                            "id": name,
                            "checkpoint": checkpoint,
                            "recipe": "llamacpp",
                        }
                    )
                )
            self.assertCommandSucceeds(
                ["import", json_file], timeout=TIMEOUT_MODEL_OPERATION
            )

        # Pull both models (downloads both quants into the same models-- directory)
        for name in [SHARED_REPO_MODEL_A_NAME, SHARED_REPO_MODEL_B_NAME]:
            self.assertCommandSucceeds(["pull", name], timeout=TIMEOUT_MODEL_OPERATION)

        # Verify both show as downloaded
        result = self.assertCommandSucceeds(["list", "--downloaded"])
        output = result.stdout + result.stderr
        self.assertIn(
            "SharedRepo-TestA",
            output,
            "Model A should be listed as downloaded before delete",
        )
        self.assertIn(
            "SharedRepo-TestB",
            output,
            "Model B should be listed as downloaded before delete",
        )

        # Delete model A — model B's files should be preserved
        self.assertCommandSucceeds(
            ["delete", SHARED_REPO_MODEL_A_NAME], timeout=TIMEOUT_MODEL_OPERATION
        )

        # Verify model B is still listed as downloaded
        result = self.assertCommandSucceeds(["list", "--downloaded"])
        output = result.stdout + result.stderr
        self.assertIn(
            "SharedRepo-TestB",
            output,
            "Model B should still be downloaded after deleting model A",
        )
        self.assertNotIn(
            "SharedRepo-TestA",
            output,
            "Model A should no longer be listed after delete",
        )

        # Clean up: delete model B
        self.assertCommandSucceeds(
            ["delete", SHARED_REPO_MODEL_B_NAME], timeout=TIMEOUT_MODEL_OPERATION
        )

    @unittest.skipIf(
        sys.platform == "darwin",
        "macOS .pkg installs to /Library/Application Support/lemonade/hub, "
        "which the HF cache resolver does not check",
    )
    def test_092_delete_preserves_cross_repo_dependency(self):
        """Test multi-repo dependency cleanup in the persistent CLI suite.

        Scenario:
          Model A: main from repo1, text_encoder from repo2 (shared)
          Model B: main from repo3, text_encoder from repo2 (shared)

          - Download A -> downloads repo1 + repo2
          - Download B -> downloads repo3, repo2 already present
          - Delete A -> removes A's main checkpoint file only; repo dirs may remain
            if earlier persistent tests imported another model from the same repo
          - Delete B -> deletes repo3 + repo2

        Verifies both CLI output and on-disk HF cache state at each step.
        """
        # Import both models with multi-checkpoint configs
        for name, main_cp in [
            (MULTI_REPO_MODEL_A_NAME, MULTI_REPO_MODEL_A_MAIN),
            (MULTI_REPO_MODEL_B_NAME, MULTI_REPO_MODEL_B_MAIN),
        ]:
            json_file = os.path.join(tempfile.gettempdir(), f"lemonade_{name}.json")
            with open(json_file, "w") as f:
                f.write(
                    json.dumps(
                        {
                            "id": name,
                            "checkpoints": {
                                "main": main_cp,
                                "text_encoder": MULTI_REPO_SHARED_CHECKPOINT,
                            },
                            "recipe": "llamacpp",
                        }
                    )
                )
            self.assertCommandSucceeds(
                ["import", json_file], timeout=TIMEOUT_MODEL_OPERATION
            )

        # Pull both models
        for name in [MULTI_REPO_MODEL_A_NAME, MULTI_REPO_MODEL_B_NAME]:
            self.assertCommandSucceeds(["pull", name], timeout=TIMEOUT_MODEL_OPERATION)

        # Verify both show as downloaded
        result = self.assertCommandSucceeds(["list", "--downloaded"])
        output = result.stdout + result.stderr
        self.assertIn(
            "MultiRepo-TestA", output, "Model A should be listed as downloaded"
        )
        self.assertIn(
            "MultiRepo-TestB", output, "Model B should be listed as downloaded"
        )

        hf_cache = _resolve_hf_cache_root(
            [
                MULTI_REPO_MODEL_A_CACHE_DIR,
                MULTI_REPO_SHARED_CACHE_DIR,
                MULTI_REPO_MODEL_B_CACHE_DIR,
            ],
            [
                (MULTI_REPO_MODEL_A_CACHE_DIR, MULTI_REPO_MODEL_A_MAIN),
                (MULTI_REPO_SHARED_CACHE_DIR, MULTI_REPO_SHARED_CHECKPOINT),
                (MULTI_REPO_MODEL_B_CACHE_DIR, MULTI_REPO_MODEL_B_MAIN),
            ],
        )
        repo1_path = os.path.join(hf_cache, MULTI_REPO_MODEL_A_CACHE_DIR)
        repo2_path = os.path.join(hf_cache, MULTI_REPO_SHARED_CACHE_DIR)
        repo3_path = os.path.join(hf_cache, MULTI_REPO_MODEL_B_CACHE_DIR)
        model_a_main_path = _find_cached_checkpoint(
            hf_cache, MULTI_REPO_MODEL_A_CACHE_DIR, MULTI_REPO_MODEL_A_MAIN
        )
        shared_checkpoint_path = _find_cached_checkpoint(
            hf_cache, MULTI_REPO_SHARED_CACHE_DIR, MULTI_REPO_SHARED_CHECKPOINT
        )
        model_b_main_path = _find_cached_checkpoint(
            hf_cache, MULTI_REPO_MODEL_B_CACHE_DIR, MULTI_REPO_MODEL_B_MAIN
        )

        # Verify all three repo dirs exist on disk after download
        self.assertTrue(
            os.path.isdir(repo1_path), f"repo1 dir should exist: {repo1_path}"
        )
        self.assertTrue(
            os.path.isdir(repo2_path), f"shared repo dir should exist: {repo2_path}"
        )
        self.assertTrue(
            os.path.isdir(repo3_path), f"repo3 dir should exist: {repo3_path}"
        )
        self.assertIsNotNone(
            model_a_main_path,
            f"Model A main checkpoint should exist in snapshots under {repo1_path}",
        )
        self.assertIsNotNone(
            shared_checkpoint_path,
            f"Shared checkpoint should exist in snapshots under {repo2_path}",
        )
        self.assertIsNotNone(
            model_b_main_path,
            f"Model B main checkpoint should exist in snapshots under {repo3_path}",
        )
        print("[OK] All three HF cache repo directories present after pull")

        # Delete Model A -- Model B (and shared text_encoder repo2) should be preserved
        self.assertCommandSucceeds(
            ["delete", MULTI_REPO_MODEL_A_NAME], timeout=TIMEOUT_MODEL_OPERATION
        )

        # Verify Model B is still downloaded via CLI
        result = self.assertCommandSucceeds(["list", "--downloaded"])
        output = result.stdout + result.stderr
        self.assertIn("MultiRepo-TestB", output, "Model B should still be downloaded")
        self.assertNotIn("MultiRepo-TestA", output, "Model A should be gone")

        # Verify on-disk: Model A file deleted, repo2 (shared) preserved, repo3 preserved.
        # repo1 directory may remain because this suite is persistent and other imported
        # models can reference the same repo.
        self.assertFalse(
            os.path.exists(model_a_main_path),
            f"Model A main checkpoint should be deleted after removing Model A: {model_a_main_path}",
        )
        self.assertTrue(
            os.path.isdir(repo2_path),
            f"shared repo should be preserved (still needed by Model B): {repo2_path}",
        )
        self.assertTrue(
            os.path.exists(shared_checkpoint_path),
            "Shared checkpoint should still exist after removing Model A",
        )
        self.assertTrue(
            os.path.isdir(repo3_path),
            f"repo3 should still exist (Model B main): {repo3_path}",
        )
        self.assertTrue(
            os.path.exists(model_b_main_path),
            "Model B main checkpoint should still exist after removing Model A",
        )
        print("[OK] After deleting A: A main file gone, shared repo2 + repo3 preserved")

        # Delete Model B -- should clean up repo3 and shared repo2
        self.assertCommandSucceeds(
            ["delete", MULTI_REPO_MODEL_B_NAME], timeout=TIMEOUT_MODEL_OPERATION
        )

        # Verify both gone from CLI
        result = self.assertCommandSucceeds(["list", "--downloaded"])
        output = result.stdout + result.stderr
        self.assertNotIn("MultiRepo-TestA", output, "Model A should not be listed")
        self.assertNotIn("MultiRepo-TestB", output, "Model B should not be listed")

        # Verify on-disk: repo2 shared file and repo3 main file deleted, and both
        # unique repo directories removed once the last dependent model is gone.
        self.assertFalse(
            os.path.exists(shared_checkpoint_path),
            "Shared checkpoint should be deleted after removing the last dependent model",
        )
        self.assertFalse(
            os.path.exists(model_b_main_path),
            f"Model B main checkpoint should be deleted after removing Model B: {model_b_main_path}",
        )
        self.assertFalse(
            os.path.isdir(repo2_path),
            f"shared repo should be deleted after removing last dependent: {repo2_path}",
        )
        self.assertFalse(
            os.path.isdir(repo3_path),
            f"repo3 should be deleted after removing Model B: {repo3_path}",
        )
        print("[OK] After deleting B: all repo directories cleaned up")


class CLIHelpDocsConsistencyTests(unittest.TestCase):
    """Lightweight checks that compare CLI help semantics with docs text."""

    def test_899_run_load_recipe_options_are_grouped(self):
        """Run/load help should keep every recipe flag under the intended group."""
        expected_groups = {
            "General Options:": ["--ctx-size", "--merge-args"],
            "Llama.cpp Backend Options:": [
                "--llamacpp",
                "--llamacpp-device",
                "--llamacpp-args",
            ],
            "Stable Diffusion Options:": ["--sdcpp", "--sdcpp-args"],
            "vLLM Options:": ["--vllm", "--vllm-args"],
            "Whisper.cpp Options:": ["--whispercpp", "--whispercpp-args"],
        }

        for command in ("run", "load"):
            with self.subTest(command=command):
                result = run_cli_command([command, "--help"], timeout=TIMEOUT_DEFAULT)
                self.assertEqual(result.returncode, 0)

                help_output = result.stdout + result.stderr
                group_positions = {
                    group: help_output.find(group) for group in expected_groups
                }
                for group, position in group_positions.items():
                    self.assertNotEqual(position, -1, f"Missing help group: {group}")

                ordered_groups = sorted(
                    group_positions.items(), key=lambda item: item[1]
                )
                for index, (group, start) in enumerate(ordered_groups):
                    end = (
                        ordered_groups[index + 1][1]
                        if index + 1 < len(ordered_groups)
                        else len(help_output)
                    )
                    section = help_output[start:end]

                    for flag in expected_groups[group]:
                        self.assertTrue(
                            any(
                                line.strip().startswith(flag)
                                and (
                                    len(line.strip()) == len(flag)
                                    or line.strip()[len(flag)] in " ,"
                                )
                                for line in section.splitlines()
                            ),
                            f"{flag} missing from {group} in `{command} --help`",
                        )

    def test_900_launch_docs_match_help_text(self):
        """The launch model-selection wording in docs should match actual CLI behavior/help."""
        result = run_cli_command(["launch", "--help"], timeout=TIMEOUT_DEFAULT)
        self.assertEqual(result.returncode, 0)

        help_output = result.stdout + result.stderr
        self.assertIn("Agents:", help_output)
        self.assertIn("claude", help_output)
        self.assertIn("codex", help_output)
        self.assertIn("opencode", help_output)
        self.assertNotIn("--model", help_output)
        self.assertNotIn("--directory", help_output)
        self.assertNotIn("--recipe-file", help_output)
        self.assertNotIn("--agent-args", help_output)
        self.assertNotIn("--provider", help_output)
        self.assertNotIn("--ctx-size", help_output)
        self.assertNotIn("LEMONADE_CTX_SIZE", help_output)

        claude_result = run_cli_command(
            ["launch", "claude", "--help"], timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(claude_result.returncode, 0)
        claude_help = claude_result.stdout + claude_result.stderr
        self.assertNotIn("--provider", claude_help)
        self.assertNotIn("--ctx-size", claude_help)
        self.assertNotIn("LEMONADE_CTX_SIZE", claude_help)
        self.assertNotIn("--llamacpp", claude_help)

        opencode_result = run_cli_command(
            ["launch", "opencode", "--help"], timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(opencode_result.returncode, 0)
        opencode_help = opencode_result.stdout + opencode_result.stderr
        self.assertNotIn("--provider", opencode_help)
        self.assertNotIn("--ctx-size", opencode_help)
        self.assertNotIn("LEMONADE_CTX_SIZE", opencode_help)

        codex_result = run_cli_command(
            ["launch", "codex", "--help"], timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(codex_result.returncode, 0)
        codex_help = codex_result.stdout + codex_result.stderr
        self.assertIn("Use model provider name for Codex", codex_help)
        self.assertIn("--provider", codex_help)
        self.assertNotIn("--ctx-size", codex_help)
        self.assertNotIn("LEMONADE_CTX_SIZE", codex_help)

        docs_path = os.path.join(
            os.path.dirname(__file__), "..", "docs", "guide", "cli.md"
        )
        with open(docs_path, "r", encoding="utf-8") as f:
            docs_text = f.read()

        self.assertNotIn("`--use-recipe`", docs_text)
        self.assertNotIn(
            "Import a recipe from `lemonade-sdk/recipes` before launch",
            docs_text,
        )
        self.assertIn(
            "`--recipe-file` is only used for remote recipe import",
            docs_text,
        )
        self.assertIn(
            "For local recipe files, run `lemonade import <LOCAL_RECIPE_JSON>` first",
            docs_text,
        )
        self.assertIn("--provider,-p [PROVIDER]", docs_text)
        self.assertIn("--agent-args ARGS", docs_text)
        launch_section = docs_text.split("## Options for launch", 1)[1].split(
            "## Options for scan", 1
        )[0]
        self.assertNotIn("--ctx-size", launch_section)
        self.assertNotIn("--llamacpp", launch_section)


class CLIUrlSchemeTests(unittest.TestCase):
    """Tests URL scheme parsing, HTTPS/TLS connections, and port overrides in the C++ CLI client."""

    @classmethod
    def setUpClass(cls):
        import http.server
        import threading
        import socket

        class MockHTTPHandler(http.server.BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def do_GET(self):
                if self.path.startswith("/api/v1/models") or self.path.startswith(
                    "/api/v0/models"
                ):
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    self.wfile.write(b'{"data":[]}')
                else:
                    self.send_response(404)
                    self.end_headers()

        # Find a free port
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.bind(("127.0.0.1", 0))
        cls.mock_port = s.getsockname()[1]
        s.close()

        cls.server = http.server.HTTPServer(
            ("127.0.0.1", cls.mock_port), MockHTTPHandler
        )
        cls.thread = threading.Thread(target=cls.server.serve_forever)
        cls.thread.daemon = True
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def test_http_scheme_port_default(self):
        """Should connect successfully and default port when http:// scheme is used."""
        env = os.environ.copy()
        env["LEMONADE_HOST"] = f"http://127.0.0.1:{self.mock_port}"
        result = run_cli_command(["list"], env=env, timeout=10)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0)
        self.assertIn("No models available", output)

    def test_https_scheme_connection_attempt(self):
        """Should attempt a secure TLS connection when https:// scheme is used."""
        # Connecting https://127.0.0.1:mock_port will attempt a TLS handshake on our HTTP server.
        # This will fail (since the server is HTTP), but it verifies that:
        # 1. The hostname/port were parsed correctly to 127.0.0.1:mock_port.
        # 2. It attempted a TLS handshake.
        env = os.environ.copy()
        env["LEMONADE_HOST"] = f"https://127.0.0.1:{self.mock_port}"
        result = run_cli_command(["list"], env=env, timeout=10)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0)
        # Verify it either tried to establish connection/handshake or failed because HTTPS is compiled out
        self.assertTrue(
            "Could not connect to Lemonade server" in output
            or "HTTPS support is not compiled" in output
            or "'https' scheme is not supported" in output,
            f"Expected connection error or HTTPS unsupported error, got: {output}",
        )


def run_cli_client_tests():
    """Run CLI client tests based on command line arguments."""
    args = parse_cli_args()

    print(f"\n{'=' * 70}")
    print("CLI CLIENT TESTS")
    print(f"CLI binary: {get_cli_binary()}")
    print(f"{'=' * 70}\n")

    # Create and run test suite
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    suite.addTests(loader.loadTestsFromTestCase(PersistentServerCLIClientTests))
    suite.addTests(loader.loadTestsFromTestCase(CLIHelpDocsConsistencyTests))
    suite.addTests(loader.loadTestsFromTestCase(CLIUrlSchemeTests))

    runner = unittest.TextTestRunner(verbosity=2, buffer=False, failfast=True)
    result = runner.run(suite)

    sys.exit(0 if (result and result.wasSuccessful()) else 1)


if __name__ == "__main__":
    run_cli_client_tests()
