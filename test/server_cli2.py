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
    python server_cli2.py --server-binary /path/to/lemonade-server
"""

import argparse
import glob
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

from utils.server_base import wait_for_server
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
    get_default_server_binary,
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
    "server_binary": None,
}

IS_WINDOWS = platform.system() == "Windows"
WINDOWS_LAUNCH_STUB_SKIP_REASON = "Windows launch-stub execution uses non-native script binaries; covered on Unix runners"


def get_cli_binary():
    """Get the CLI binary path (same as server binary but called 'lemonade')."""
    server_binary = _config["server_binary"] or get_default_server_binary()
    # Replace 'lemonade-server' with 'lemonade' in the path
    return server_binary.replace("lemonade-server", "lemonade")


def get_legacy_cli_binary():
    """Get the deprecated lemonade-server shim binary path."""
    return _config["server_binary"] or get_default_server_binary()


def parse_cli_args():
    """Parse command line arguments for CLI client tests."""
    parser = argparse.ArgumentParser(description="Test lemonade CLI client")
    parser.add_argument(
        "--server-binary",
        type=str,
        default=get_default_server_binary(),
        help="Path to lemonade-server binary (default: CMake build output)",
    )

    args = parser.parse_args()

    _config["server_binary"] = args.server_binary

    return args


def run_cli_command(args, timeout=60, check=False, env=None, input_text=None):
    """
    Run a CLI command and return the result.

    Args:
        args: List of command arguments (without the binary)
        timeout: Command timeout in seconds
        check: If True, raise CalledProcessError on non-zero exit
        env: Optional environment override for subprocess
        input_text: Optional stdin text for interactive prompts

    Returns:
        subprocess.CompletedProcess result
    """
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


def run_legacy_cli_command(args, timeout=60, check=False):
    """Run the deprecated lemonade-server shim and return the result."""
    legacy_binary = get_legacy_cli_binary()
    cmd = [legacy_binary] + args
    print(f"Running legacy shim: {' '.join(cmd)}")

    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        encoding="utf-8",
        errors="replace",
    )

    if result.stdout:
        print(f"legacy stdout: {result.stdout}")
    if result.stderr:
        print(f"legacy stderr: {result.stderr}")

    if check and result.returncode != 0:
        raise subprocess.CalledProcessError(
            result.returncode, cmd, result.stdout, result.stderr
        )

    return result


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

    def test_020_list(self):
        """Test list command."""
        result = self.assertCommandSucceeds(["list"])
        output = result.stdout + result.stderr
        print(f"List output: {output}")

    def test_021_list_downloaded_flag(self):
        """Test list --downloaded flag."""
        result = self.assertCommandSucceeds(["list", "--downloaded"])
        output = result.stdout + result.stderr
        print(f"List --downloaded output: {output}")

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
    # Pull Tests
    # =============================================================================

    def test_050_pull_with_checkpoint(self):
        """Test pull command with --checkpoint option."""
        result = run_cli_command(
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
        result = run_cli_command(
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
        result = run_cli_command(
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
        result = self.assertCommandSucceeds(
            ["pull", ENDPOINT_TEST_MODEL],
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        output = result.stdout.lower() + result.stderr.lower()
        self.assertFalse(
            "error" in output and "failed" in output,
            f"Pull should not report errors: {result.stdout}",
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
            self.assertEqual(payload["env"]["OPENAI_API_KEY"], "lemonade")

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

            self.assertEqual(payload["env"]["ANTHROPIC_AUTH_TOKEN"], "lemonade")
            self.assertEqual(payload["env"]["LEMONADE_API_KEY"], "lemonade")
            self.assertEqual(
                payload["env"]["ANTHROPIC_BASE_URL"], f"http://localhost:{PORT}"
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
            self.assertIn("only supported for the codex agent", output)

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
            self.assertEqual(lemonade["options"]["apiKey"], "lemonade")
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

    def test_900_launch_docs_match_help_text(self):
        """The launch model-selection wording in docs should match actual CLI behavior/help."""
        result = run_cli_command(["launch", "--help"], timeout=TIMEOUT_DEFAULT)
        self.assertEqual(result.returncode, 0)

        help_output = result.stdout + result.stderr
        self.assertIn(
            "Remote recipe directory used only if you choose recipe import at prompt",
            help_output,
        )
        self.assertIn(
            "Remote recipe JSON filename used only if you choose recipe import at prompt",
            help_output,
        )
        self.assertIn(
            "Use model provider name for Codex",
            help_output,
        )
        self.assertIn(
            "Custom arguments to pass directly to the launched agent process",
            help_output,
        )

        docs_path = os.path.join(
            os.path.dirname(__file__), "..", "docs", "lemonade-cli.md"
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

    def test_901_legacy_pull_deprecation_message(self):
        """The legacy shim should not forward pull args and should print migration guidance."""
        result = run_legacy_cli_command(
            ["pull", "Qwen3-0.6B-GGUF"], timeout=TIMEOUT_DEFAULT
        )
        self.assertNotEqual(result.returncode, 0)

        output = result.stdout + result.stderr
        self.assertIn("This command is deprecated.", output)
        self.assertIn("use 'lemonade pull --help' instead", output.lower())
        self.assertIn("Built-in model: lemonade pull Qwen3-0.6B-GGUF", output)
        self.assertIn(
            "Checkpoint:     lemonade pull unsloth/Qwen3-8B-GGUF:Q4_K_M", output
        )
        self.assertIn(
            "Manual pull:    lemonade pull user.MyModel --checkpoint main org/repo:Q4_K_M --recipe llamacpp",
            output,
        )


def run_cli_client_tests():
    """Run CLI client tests based on command line arguments."""
    args = parse_cli_args()

    print(f"\n{'=' * 70}")
    print("CLI CLIENT TESTS")
    print(f"Server binary: {_config['server_binary']}")
    print(f"CLI binary: {get_cli_binary()}")
    print(f"{'=' * 70}\n")

    # Create and run test suite
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    suite.addTests(loader.loadTestsFromTestCase(PersistentServerCLIClientTests))
    suite.addTests(loader.loadTestsFromTestCase(CLIHelpDocsConsistencyTests))

    runner = unittest.TextTestRunner(verbosity=2, buffer=False, failfast=True)
    result = runner.run(suite)

    sys.exit(0 if (result and result.wasSuccessful()) else 1)


if __name__ == "__main__":
    run_cli_client_tests()
