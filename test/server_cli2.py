"""
CLI client tests for Lemonade CLI (lemonade command).

Tests the lemonade CLI client commands (HTTP client for Lemonade Server):
- status
- list
- export
- recipes
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
    PORT,
    TIMEOUT_DEFAULT,
    TIMEOUT_MODEL_OPERATION,
    USER_MODEL_MAIN_CHECKPOINT,
    USER_MODEL_TE_CHECKPOINT,
    USER_MODEL_NAME,
    get_default_server_binary,
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

    def test_040_recipes(self):
        """Test recipes command."""
        result = self.assertCommandSucceeds(["recipes"])
        output = result.stdout + result.stderr
        self.assertTrue(
            len(output) > 0,
            f"Recipes command should produce output: {output}",
        )
        print(f"Recipes output: {output}")

    def test_041_recipes_install(self):
        """Test recipes --install."""
        result = self.assertCommandSucceeds(["recipes", "--install", "llamacpp:cpu"])
        print(f"Recipes --install exit code: {result.returncode}")

    def test_042_recipes_uninstall(self):
        """Test recipes --uninstall."""
        result = self.assertCommandSucceeds(["recipes", "--uninstall", "llamacpp:cpu"])
        print(f"Recipes --uninstall exit code: {result.returncode}")

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
            self.assertIn("--oss", argv)
            self.assertIn("-m", argv)
            self.assertIn(ENDPOINT_TEST_MODEL, argv)
            self.assertTrue(payload["env"]["OPENAI_BASE_URL"].endswith("/v1/"))
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

    def test_115_launch_with_model_and_directory_flags_is_deterministic(self):
        """A provided model should skip import flow even when directory flags are present."""
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


class CLIHelpDocsConsistencyTests(unittest.TestCase):
    """Lightweight checks that compare launch help semantics with CLI docs text."""

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
