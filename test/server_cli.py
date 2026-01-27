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

Two test modes:
1. Persistent server mode (default): Server starts at beginning, runs all tests, stops at end
2. Ephemeral mode (--ephemeral): Each command that needs a server starts its own

Usage:
    python server_cli.py
    python server_cli.py --ephemeral
    python server_cli.py --server-binary /path/to/lemonade-server
"""

import unittest
import subprocess
import time
import socket
import sys
import os
import argparse

from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
    get_default_server_binary,
)


# Global configuration
_config = {
    "server_binary": None,
    "ephemeral": False,
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
        "--ephemeral",
        action="store_true",
        help="Run in ephemeral mode (each command starts its own server)",
    )

    args, unknown = parser.parse_known_args()

    _config["server_binary"] = args.server_binary
    _config["ephemeral"] = args.ephemeral

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
    """Stop the server using CLI."""
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


class EphemeralCLITests(CLITestBase):
    """
    CLI tests that run without a persistent server.

    Each command that needs a server starts its own ephemeral server instance.
    Tests are independent and don't share state.
    """

    @classmethod
    def setUpClass(cls):
        """Ensure no server is running before tests."""
        super().setUpClass()
        print("\n=== Ephemeral CLI tests - stopping any existing server ===")
        stop_server()

    def setUp(self):
        """Ensure server is stopped before each test."""
        stop_server()

    def tearDown(self):
        """Ensure server is stopped after each test."""
        stop_server()

    def test_001_version_no_server(self):
        """Test --version flag works without server running."""
        self.assertFalse(is_server_running(), "Server should not be running")
        result = self.assertCommandSucceeds(["--version"])
        self.assertTrue(
            len(result.stdout) > 0 or len(result.stderr) > 0,
            "Version command should produce output",
        )

    def test_002_list_ephemeral(self):
        """Test list command starts ephemeral server if needed."""
        self.assertFalse(is_server_running(), "Server should not be running initially")

        # List command should work (may start ephemeral server)
        result = run_cli_command(["list"], timeout=TIMEOUT_DEFAULT)
        # Command should complete (may or may not succeed depending on implementation)
        # We just verify it doesn't hang
        print(f"List exit code: {result.returncode}")

    def test_003_pull_ephemeral(self):
        """Test pull command works in ephemeral mode."""
        self.assertFalse(is_server_running(), "Server should not be running initially")

        # Pull should work (starts ephemeral server)
        result = run_cli_command(
            ["pull", ENDPOINT_TEST_MODEL], timeout=TIMEOUT_MODEL_OPERATION
        )
        # Should complete successfully
        self.assertEqual(result.returncode, 0, f"Pull failed: {result.stderr}")

    def test_004_serve_and_stop(self):
        """Test serve command starts server and stop command stops it."""
        self.assertFalse(is_server_running(), "Server should not be running initially")

        # Start server
        cmd = [_config["server_binary"], "serve"]
        # Add --no-tray on Windows or in CI environments (no display server in containers)
        if os.name == "nt" or os.getenv("LEMONADE_CI_MODE"):
            cmd.append("--no-tray")

        server_process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            # Wait for server to start
            self.assertTrue(
                wait_for_server_start(timeout=60),
                "Server should start within 60 seconds",
            )

            # Stop server using CLI
            stop_result = run_cli_command(["stop"], timeout=30)
            self.assertEqual(stop_result.returncode, 0, "Stop command should succeed")

            # Verify server stopped
            self.assertTrue(
                wait_for_server_stop(timeout=30),
                "Server should stop within 30 seconds",
            )

        finally:
            # Clean up
            server_process.terminate()
            try:
                server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server_process.kill()

    def test_005_status_when_stopped(self):
        """Test status command when server is not running."""
        self.assertFalse(is_server_running(), "Server should not be running")

        result = run_cli_command(["status"])
        # Status should indicate server is not running (may return non-zero)
        output = result.stdout.lower() + result.stderr.lower()
        self.assertTrue(
            "not running" in output
            or "offline" in output
            or "stopped" in output
            or result.returncode != 0,
            f"Status should indicate server is not running: {result.stdout}",
        )


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
    print(f"Mode: {'Ephemeral' if _config['ephemeral'] else 'Persistent'}")
    print(f"{'=' * 70}\n")

    # Choose test class based on mode
    if _config["ephemeral"]:
        test_class = EphemeralCLITests
    else:
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
