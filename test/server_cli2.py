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

This test file focuses on the CLI client functionality with a persistent server.
The server starts automatically at the beginning of tests and stops at the end.

Usage:
    python server_cli2.py
    python server_cli2.py --server-binary /path/to/lemonade-server
"""

import argparse
import json
import os
import platform
import socket
import subprocess
import sys
import tempfile
import time
import unittest

from utils.server_base import _stop_server_via_systemd
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


def is_server_running(port=PORT):
    """Check if the server is running on the given port."""
    try:
        conn = socket.create_connection(("localhost", port), timeout=2)
        conn.close()
        return True
    except (socket.error, socket.timeout):
        return False


def wait_for_server_start(port=PORT, timeout=60):
    """Wait for server to start."""
    start_time = time.time()
    while time.time() - start_time < timeout:
        if is_server_running(port):
            return True
        time.sleep(1)
    return False


def wait_for_server_stop(port=PORT, timeout=30):
    """Wait for server to stop."""
    start_time = time.time()
    while time.time() - start_time < timeout:
        if not is_server_running(port):
            return True
        time.sleep(1)
    return False


def stop_server():
    """Stop the server using systemctl on Linux, or CLI as fallback."""
    # Try systemd first on Linux
    if _stop_server_via_systemd():
        wait_for_server_stop()
        return

    # Try server stop command as fallback (lemonade-server has stop command)
    try:
        result = subprocess.run(
            [_config["server_binary"], "stop"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        if result.stdout:
            print(f"stdout: {result.stdout}")
        if result.stderr:
            print(f"stderr: {result.stderr}")
        wait_for_server_stop()
    except Exception as e:
        print(f"Warning: Failed to stop server: {e}")


def get_cli_binary():
    """Get the CLI binary path (same as server binary but called 'lemonade')."""
    server_binary = _config["server_binary"]
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


def run_cli_command(args, timeout=60, check=False):
    """
    Run a CLI command and return the result.

    Args:
        args: List of command arguments (without the binary)
        timeout: Command timeout in seconds
        check: If True, raise CalledProcessError on non-zero exit

    Returns:
        subprocess.CompletedProcess result
    """
    cmd = [get_cli_binary()] + args
    print(f"Running: {' '.join(cmd)}")

    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        encoding="utf-8",
        errors="replace",
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

    The server starts once at class setup and stops at teardown.
    Tests run in order and may depend on previous test state.
    """

    @classmethod
    def setUpClass(cls):
        """Start the server for all tests."""
        super().setUpClass()
        print("\n=== Starting persistent server for CLI client tests ===")

        # Stop any existing server
        stop_server()

        # Start server in background
        cmd = [_config["server_binary"], "serve"]
        # Add --no-tray on Windows or in CI environments (no display server in containers)
        if os.name == "nt" or os.getenv("LEMONADE_CI_MODE"):
            cmd.append("--no-tray")

        cls._server_process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        # Wait for server to start
        if not wait_for_server_start():
            cls._server_process.terminate()
            raise RuntimeError("Failed to start server for CLI client tests")

        print("Server started successfully")
        time.sleep(3)  # Additional wait for full initialization

    @classmethod
    def tearDownClass(cls):
        """Stop the server after all tests."""
        print("\n=== Stopping persistent server ===")
        stop_server()
        if hasattr(cls, "_server_process") and cls._server_process:
            cls._server_process.terminate()
            cls._server_process.wait(timeout=10)
        super().tearDownClass()

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

    # =============================================================================
    # Load Tests
    # =============================================================================

    def test_060_load_with_ctx_size(self):
        """Test load command with --ctx-size option."""
        result = run_cli_command(
            ["load", ENDPOINT_TEST_MODEL, "--ctx-size", "8192"],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Load with ctx-size exit code: {result.returncode}")

    def test_061_load_with_save_options(self):
        """Test load command with --save-options flag."""
        result = run_cli_command(
            ["load", ENDPOINT_TEST_MODEL, "--save-options"],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Load with save-options exit code: {result.returncode}")

    # =============================================================================
    # Unload Tests
    # =============================================================================

    def test_070_unload_with_model(self):
        """Test unload command with model name."""
        result = run_cli_command(
            ["unload", ENDPOINT_TEST_MODEL],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Unload with model exit code: {result.returncode}")

    def test_071_unload_without_model(self):
        """Test unload command without model name (unloads all)."""
        result = run_cli_command(
            ["unload"],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Unload without model exit code: {result.returncode}")

    # =============================================================================
    # Delete Tests
    # =============================================================================

    def test_080_delete_model(self):
        """Test delete command with model name."""
        result = run_cli_command(
            ["delete", ENDPOINT_TEST_MODEL],
            timeout=TIMEOUT_DEFAULT,
        )
        print(f"Delete model exit code: {result.returncode}")


def run_cli_client_tests():
    """
    Run CLI client tests based on command line arguments.

    IMPORTANT: This function ensures the server is ALWAYS stopped before exiting,
    regardless of whether tests passed or failed.
    """
    args = parse_cli_args()

    print(f"\n{'=' * 70}")
    print("CLI CLIENT TESTS")
    print(f"Server binary: {_config['server_binary']}")
    print(f"CLI binary: {get_cli_binary()}")
    print(f"{'=' * 70}\n")

    result = None
    try:
        # Create and run test suite
        loader = unittest.TestLoader()
        suite = loader.loadTestsFromTestCase(PersistentServerCLIClientTests)

        runner = unittest.TextTestRunner(verbosity=2, buffer=False, failfast=True)
        result = runner.run(suite)
    finally:
        # ALWAYS stop the server before exiting, regardless of test outcome
        print("\n=== Final cleanup: ensuring server is stopped ===")
        stop_server()

    sys.exit(0 if (result and result.wasSuccessful()) else 1)


if __name__ == "__main__":
    run_cli_client_tests()
