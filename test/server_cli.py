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
- backends

Expects a running server (started by the installer or manually).

Usage:
    python server_cli.py
    python server_cli.py --server-binary /path/to/lemonade
"""

import argparse
import glob
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
    TIMEOUT_MODEL_OPERATION,
    USER_MODEL_MAIN_CHECKPOINT,
    USER_MODEL_NAME,
    get_default_server_binary,
    get_hf_cache_dir_candidates,
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

    def test_006_backends(self):
        """Test backends command shows available recipes and their status."""
        result = self.assertCommandSucceeds(["backends"])
        output = result.stdout

        # Backends command should show a table with recipe information
        self.assertTrue(
            len(output) > 0,
            "Backends command should produce output",
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

        print("[OK] backends command output shows recipe/backend status")

    def _get_test_backend(self):
        """Get a lightweight test backend based on platform."""
        import sys

        if sys.platform == "darwin":
            return "llamacpp", "metal"
        else:
            return "llamacpp", "cpu"

    def test_009_backends_install(self):
        """Test backends install installs a backend."""
        recipe, backend = self._get_test_backend()
        target = f"{recipe}:{backend}"

        # Uninstall first (cleanup)
        run_cli_command(["backends", "uninstall", target], timeout=120)

        # Install
        result = self.assertCommandSucceeds(
            ["backends", "install", target], timeout=300
        )
        output = result.stdout.lower()
        self.assertTrue(
            "install" in output or "success" in output,
            f"Expected install confirmation in output: {result.stdout}",
        )

        # Verify via backends list
        result = self.assertCommandSucceeds(["backends"])
        self.assertIn(
            "installed",
            result.stdout.lower(),
            f"Expected 'installed' status after install: {result.stdout}",
        )
        print(f"[OK] backends install {target} succeeded")

    def test_010_backends_uninstall(self):
        """Test backends uninstall removes a backend."""
        recipe, backend = self._get_test_backend()
        target = f"{recipe}:{backend}"

        # Ensure installed first
        run_cli_command(["backends", "install", target], timeout=300)

        # Uninstall
        result = self.assertCommandSucceeds(
            ["backends", "uninstall", target], timeout=120
        )
        output = result.stdout.lower()
        self.assertTrue(
            "uninstall" in output or "success" in output,
            f"Expected uninstall confirmation in output: {result.stdout}",
        )
        print(f"[OK] backends uninstall {target} succeeded")

    def test_011_backends_reinstall(self):
        """Re-install after test to leave system in clean state."""
        recipe, backend = self._get_test_backend()
        target = f"{recipe}:{backend}"

        result = self.assertCommandSucceeds(
            ["backends", "install", target], timeout=300
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

    def test_013_delete_preserves_shared_repo(self):
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
    def test_014_delete_preserves_cross_repo_dependency(self):
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
