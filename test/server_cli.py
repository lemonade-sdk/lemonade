"""
CLI command tests for Lemonade Server.

Tests the lemonade-server CLI commands directly (not HTTP API):
- version
- list
- pull
- status
- delete
- serve
- stop
- run
- recipes

Usage:
    python server_cli.py
    python server_cli.py --server-binary /path/to/lemonade-server
"""

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

from utils.server_base import _stop_server_via_systemd
from utils.test_models import (
    ENDPOINT_TEST_MODEL,
    PORT,
    TIMEOUT_DEFAULT,
    TIMEOUT_MODEL_OPERATION,
    USER_MODEL_MAIN_CHECKPOINT,
    USER_MODEL_NAME,
    get_default_server_binary,
)

# Global configuration
_config = {
    "server_binary": None,
    "apikey": False,
    "listen_all": False,
}


def parse_cli_args():
    """Parse command line arguments for CLI tests."""
    parser = argparse.ArgumentParser(description="Test lemonade-server CLI")
    parser.add_argument(
        "--server-binary",
        type=str,
        default=get_default_server_binary(),
        help="Path to lemonade-server binary (default: CMake build output)",
    )
    parser.add_argument(
        "--api-key",
        action="store_true",
        help="Run with API Key",
    )
    parser.add_argument(
        "--listen-all",
        action="store_true",
        help="Listens on 0.0.0.0 instead of localhost",
    )

    args, unknown = parser.parse_known_args()

    _config["server_binary"] = args.server_binary
    _config["apikey"] = args.api_key
    _config["listen_all"] = args.listen_all

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
    cmd = [_config["server_binary"]] + args
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

    # Try CLI stop command as fallback
    try:
        run_cli_command(["stop"], timeout=30)
        wait_for_server_stop()
    except Exception as e:
        print(f"Warning: Failed to stop server: {e}")


class CLITestBase(unittest.TestCase):
    """Base class for CLI tests with common utilities."""

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


class PersistentServerCLITests(CLITestBase):
    """
    CLI tests that run with a persistent server.

    The server starts once at class setup and stops at teardown.
    Tests run in order and may depend on previous test state.
    """

    @classmethod
    def setUpClass(cls):
        """Start the server for all tests."""
        super().setUpClass()
        print("\n=== Starting persistent server for CLI tests ===")

        # Stop any existing server
        stop_server()

        # Start server in background
        cmd = [_config["server_binary"], "serve"]
        # Add --no-tray on Windows or in CI environments (no display server in containers)
        if os.name == "nt" or os.getenv("LEMONADE_CI_MODE"):
            cmd.append("--no-tray")

        if _config["listen_all"]:
            cmd.append("--host")
            cmd.append("0.0.0.0")

        if _config["apikey"]:
            os.environ["LEMONADE_API_KEY"] = "api-key"

        cls._server_process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        # Wait for server to start
        if not wait_for_server_start():
            cls._server_process.terminate()
            raise RuntimeError("Failed to start server for CLI tests")

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

    def test_001_version(self):
        """Test --version flag."""
        result = self.assertCommandSucceeds(["--version"])
        # Version output should contain version number
        self.assertTrue(
            len(result.stdout) > 0 or len(result.stderr) > 0,
            "Version command should produce output",
        )

    def test_002_status_when_running(self):
        """Test status command when server is running."""
        result = self.assertCommandSucceeds(["status"])
        # Status should indicate server is running
        output = result.stdout.lower() + result.stderr.lower()
        self.assertTrue(
            "running" in output or "online" in output or "active" in output,
            f"Status should indicate server is running: {result.stdout}",
        )

    def test_003_list(self):
        """Test list command to show available models."""
        result = self.assertCommandSucceeds(["list"])
        # List should produce some output (model names or empty message)
        self.assertTrue(
            len(result.stdout) > 0,
            "List command should produce output",
        )

    def test_004_pull(self):
        """Test pull command to download a model."""
        result = self.assertCommandSucceeds(
            ["pull", ENDPOINT_TEST_MODEL], timeout=TIMEOUT_MODEL_OPERATION
        )
        # Pull should succeed
        output = result.stdout.lower() + result.stderr.lower()
        self.assertFalse(
            "error" in output and "failed" in output,
            f"Pull should not report errors: {result.stdout}",
        )

    def test_005_delete(self):
        """Test delete command to remove a model."""
        # First ensure model exists
        run_cli_command(["pull", ENDPOINT_TEST_MODEL], timeout=TIMEOUT_MODEL_OPERATION)

        # Delete the model
        result = self.assertCommandSucceeds(["delete", ENDPOINT_TEST_MODEL])
        output = result.stdout.lower() + result.stderr.lower()
        self.assertTrue(
            "success" in output or "deleted" in output or "removed" in output,
            f"Delete should indicate success: {result.stdout}",
        )

        # Re-pull for other tests
        run_cli_command(["pull", ENDPOINT_TEST_MODEL], timeout=TIMEOUT_MODEL_OPERATION)

    def test_006_recipes(self):
        """Test recipes command shows available recipes and their status."""
        result = self.assertCommandSucceeds(["recipes"])
        output = result.stdout

        # Recipes command should show a table with recipe information
        self.assertTrue(
            len(output) > 0,
            "Recipes command should produce output",
        )

        # Should contain known recipe names
        known_recipes = [
            "llamacpp",
            "whispercpp",
            "sd-cpp",
            "flm",
            "ryzenai-llm",
        ]
        for recipe in known_recipes:
            self.assertTrue(
                recipe in output.lower(),
                f"Output should contain '{recipe}' recipe: {output}",
            )

        # Should contain status indicators from backend state model
        output_lower = output.lower()
        has_status = (
            "installed" in output_lower
            or "installable" in output_lower
            or "update_required" in output_lower
            or "unsupported" in output_lower
        )
        self.assertTrue(
            has_status,
            f"Output should contain status indicators: {output}",
        )

        # Should contain backend names
        has_backend = (
            "vulkan" in output_lower
            or "cpu" in output_lower
            or "default" in output_lower
        )
        self.assertTrue(
            has_backend,
            f"Output should contain backend names: {output}",
        )

        print(f"[OK] Recipes command output shows recipe/backend status")

    def test_007_pull_json(self):
        """Test import command to download a model via JSON file"""
        json_file = os.path.join(tempfile.gettempdir(), "lemonade_pull_json.json")
        with open(json_file, "w") as f:
            f.write(
                json.dumps(
                    {
                        "id": USER_MODEL_NAME,
                        "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
                        "recipe": "llamacpp",
                    }
                )
            )

        result = self.assertCommandSucceeds(
            ["import", json_file], timeout=TIMEOUT_MODEL_OPERATION
        )
        # Pull should succeed
        output = result.stdout.lower() + result.stderr.lower()
        self.assertFalse(
            "error" in output and "failed" in output,
            f"Pull should not report errors: {result.stdout}",
        )

    def test_008_pull_malformed_json(self):
        """Test import command with malformed JSON file"""
        json_file = os.path.join(
            tempfile.gettempdir(), "lemonade_pull_malformed_json.json"
        )
        with open(json_file, "w") as f:
            f.write('{"checkpoint:')

        result = self.assertCommandFails(
            ["import", json_file], timeout=TIMEOUT_MODEL_OPERATION
        )
        # Import should fail
        output = result.stdout.lower() + result.stderr.lower()
        self.assertTrue(
            "error" in output,
            f"Import should fail: {result.stdout}",
        )

    def _get_test_backend(self):
        """Get a lightweight test backend based on platform."""
        import sys

        if sys.platform == "darwin":
            return "llamacpp", "metal"
        else:
            return "llamacpp", "cpu"

    def test_009_recipes_install(self):
        """Test recipes --install installs a backend."""
        recipe, backend = self._get_test_backend()
        target = f"{recipe}:{backend}"

        # Uninstall first (cleanup)
        run_cli_command(["recipes", "--uninstall", target], timeout=120)

        # Install
        result = self.assertCommandSucceeds(
            ["recipes", "--install", target], timeout=300
        )
        output = result.stdout.lower()
        self.assertTrue(
            "install" in output or "success" in output,
            f"Expected install confirmation in output: {result.stdout}",
        )

        # Verify via recipes list
        result = self.assertCommandSucceeds(["recipes"])
        self.assertIn(
            "installed",
            result.stdout.lower(),
            f"Expected 'installed' status after install: {result.stdout}",
        )
        print(f"[OK] recipes --install {target} succeeded")

    def test_010_recipes_uninstall(self):
        """Test recipes --uninstall removes a backend."""
        recipe, backend = self._get_test_backend()
        target = f"{recipe}:{backend}"

        # Ensure installed first
        run_cli_command(["recipes", "--install", target], timeout=300)

        # Uninstall
        result = self.assertCommandSucceeds(
            ["recipes", "--uninstall", target], timeout=120
        )
        output = result.stdout.lower()
        self.assertTrue(
            "uninstall" in output or "success" in output,
            f"Expected uninstall confirmation in output: {result.stdout}",
        )
        print(f"[OK] recipes --uninstall {target} succeeded")

    def test_011_recipes_reinstall(self):
        """Re-install after test to leave system in clean state."""
        recipe, backend = self._get_test_backend()
        target = f"{recipe}:{backend}"

        result = self.assertCommandSucceeds(
            ["recipes", "--install", target], timeout=300
        )
        print(f"[OK] Re-installed {target} for clean state")


def run_cli_tests():
    """
    Run CLI tests based on command line arguments.

    IMPORTANT: This function ensures the server is ALWAYS stopped before exiting,
    regardless of whether tests passed or failed.
    """
    args = parse_cli_args()

    print(f"\n{'=' * 70}")
    print("CLI COMMAND TESTS")
    print(f"Server binary: {_config['server_binary']}")
    print(f"{'=' * 70}\n")

    test_class = PersistentServerCLITests

    result = None
    try:
        # Create and run test suite
        loader = unittest.TestLoader()
        suite = loader.loadTestsFromTestCase(test_class)

        runner = unittest.TextTestRunner(verbosity=2, buffer=False, failfast=True)
        result = runner.run(suite)
    finally:
        # ALWAYS stop the server before exiting, regardless of test outcome
        print("\n=== Final cleanup: ensuring server is stopped ===")
        stop_server()

    sys.exit(0 if (result and result.wasSuccessful()) else 1)


if __name__ == "__main__":
    run_cli_tests()
