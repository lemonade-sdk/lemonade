"""
CLI command tests for Lemonade Server.

Tests the lemonade CLI commands directly (not HTTP API):
- version
- list
- pull
- status
- delete
- serve
- stop
- run
- recipes

Expects a running server (started by the installer or manually).

Usage:
    python server_cli.py
    python server_cli.py --server-binary /path/to/lemonade
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest

import requests
from utils.server_base import wait_for_server, set_server_config, _auth_headers
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
}


def parse_cli_args():
    """Parse command line arguments for CLI tests."""
    parser = argparse.ArgumentParser(description="Test lemonade CLI")
    parser.add_argument(
        "--server-binary",
        type=str,
        default=get_default_server_binary(),
        help="Path to lemonade CLI binary (default: CMake build output)",
    )

    args, unknown = parser.parse_known_args()

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

    Expects a running server (started by the installer or manually).
    Tests run in order and may depend on previous test state.
    """

    @classmethod
    def setUpClass(cls):
        """Verify server is running."""
        super().setUpClass()
        print("\n=== Verifying server is reachable for CLI tests ===")
        try:
            wait_for_server(timeout=30)
        except TimeoutError:
            raise RuntimeError(
                "Server is not running on port %d. "
                "Start the server before running tests." % PORT
            )
        print("Server is reachable")

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
        """Test import command with malformed JSON file reports an error."""
        json_file = os.path.join(
            tempfile.gettempdir(), "lemonade_pull_malformed_json.json"
        )
        with open(json_file, "w") as f:
            f.write('{"checkpoint:')

        result = run_cli_command(["import", json_file], timeout=TIMEOUT_MODEL_OPERATION)
        # CLI may exit 0 or non-zero, but must report an error in output
        output = result.stdout.lower() + result.stderr.lower()
        self.assertIn(
            "error",
            output,
            f"Import of malformed JSON should report an error: {output}",
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

    def test_012_listen_all_via_runtime_config(self):
        """Test that setting host to 0.0.0.0 via /internal/set works."""
        # Set host to 0.0.0.0 (listen on all interfaces)
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

        # Verify the server still responds (status command should work)
        result = self.assertCommandSucceeds(["status"])
        output = result.stdout.lower() + result.stderr.lower()
        self.assertTrue(
            "running" in output or "online" in output or "active" in output,
            f"Status should indicate server is running on 0.0.0.0: {result.stdout}",
        )

        # Verify via health endpoint too (use 127.0.0.1 for same IPv4 reason)
        response = requests.get(
            f"http://127.0.0.1:{PORT}/api/v1/health",
            headers=_auth_headers(),
            timeout=10,
        )
        self.assertEqual(response.status_code, 200)

        # Restore host back to localhost. Use 127.0.0.1 directly since
        # the server is currently bound to 0.0.0.0 (IPv4 only).
        try:
            requests.post(
                f"http://127.0.0.1:{PORT}/internal/set",
                json={"host": "localhost"},
                headers=_auth_headers(),
                timeout=10,
            )
            print("[OK] Restored host to localhost")
        except Exception as e:
            # Best-effort restore — don't fail the test
            print(f"Warning: Failed to restore host to localhost: {e}")


def run_cli_tests():
    """Run CLI tests based on command line arguments."""
    args = parse_cli_args()

    print(f"\n{'=' * 70}")
    print("CLI COMMAND TESTS")
    print(f"Server binary: {_config['server_binary']}")
    print(f"{'=' * 70}\n")

    test_class = PersistentServerCLITests

    # Create and run test suite
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(test_class)

    runner = unittest.TextTestRunner(verbosity=2, buffer=False, failfast=True)
    result = runner.run(suite)

    sys.exit(0 if (result and result.wasSuccessful()) else 1)


if __name__ == "__main__":
    run_cli_tests()
