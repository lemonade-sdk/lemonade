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
import re
import argparse
import urllib.request
import urllib.error

from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
    get_default_server_binary,
)
from utils.server_base import _stop_server_via_systemd

# Global configuration
_config = {
    "server_binary": None,
    "ephemeral": False,
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
        "--ephemeral",
        action="store_true",
        help="Run in ephemeral mode (each command starts its own server)",
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
    _config["ephemeral"] = args.ephemeral
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

        # Should contain status indicators (installed, supported, unsupported)
        output_lower = output.lower()
        has_status = (
            "installed" in output_lower
            or "supported" in output_lower
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

    def test_006_recipes_no_server(self):
        """Test recipes command works without server running."""
        self.assertFalse(is_server_running(), "Server should not be running")

        result = self.assertCommandSucceeds(["recipes"])
        output = result.stdout

        # Recipes command should work without server and show recipe info
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

        print("[OK] Recipes command works without server running")

    def test_007_status_reports_http_port_not_websocket(self):
        """Test that status reports the HTTP port, not the WebSocket port.

        Regression test: the router listens on both an HTTP port and a
        WebSocket port (allocated from 9000+).  get_server_info() used to
        return whichever port appeared first in the OS TCP table, which is
        non-deterministic.  When the HTTP port was above the WS range
        (e.g. 11434) the status command could return 9000 instead, causing
        the Electron app to connect to the wrong port.

        This test starts the server on port 11434 (above the WS range)
        and verifies that `status` returns 11434 and that the reported
        port responds to a normal HTTP request.
        """
        TEST_PORT = 11434
        self.assertFalse(is_server_running(TEST_PORT), "Server should not be running")

        # Start server on port 11434 (above the WebSocket port range 9000+)
        cmd = [_config["server_binary"], "serve", "--port", str(TEST_PORT)]
        if os.name == "nt" or os.getenv("LEMONADE_CI_MODE"):
            cmd.append("--no-tray")

        server_process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            self.assertTrue(
                wait_for_server_start(port=TEST_PORT, timeout=60),
                f"Server should start on port {TEST_PORT}",
            )
            time.sleep(3)  # Let WebSocket server bind its port too

            # Run `lemonade-server status` and extract the reported port
            result = self.assertCommandSucceeds(["status"])
            output = result.stdout + result.stderr

            port_match = (
                re.search(r"port[:\s]+(\d+)", output, re.IGNORECASE)
                or re.search(r"localhost:(\d+)", output, re.IGNORECASE)
                or re.search(r"127\.0\.0\.1:(\d+)", output, re.IGNORECASE)
            )
            self.assertIsNotNone(
                port_match, f"Could not parse port from status output: {output}"
            )
            reported_port = int(port_match.group(1))

            # The reported port must be the HTTP port, not the WebSocket port
            self.assertEqual(
                reported_port,
                TEST_PORT,
                f"Status reported port {reported_port} but server was started on "
                f"{TEST_PORT}. get_server_info() may be returning the WebSocket port.",
            )

            # Double-check: the reported port must respond to HTTP
            try:
                url = f"http://127.0.0.1:{reported_port}/live"
                req = urllib.request.Request(url, method="GET")
                with urllib.request.urlopen(req, timeout=5) as resp:
                    status_code = resp.getcode()
            except urllib.error.URLError as e:
                self.fail(f"Port {reported_port} did not respond to HTTP /live: {e}")

            self.assertEqual(status_code, 200)
            print(f"[OK] Status correctly reports HTTP port {reported_port}")

        finally:
            server_process.terminate()
            try:
                server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server_process.kill()


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
