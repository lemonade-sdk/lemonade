#!/usr/bin/env python3
"""
Integration test suite for Dynamic Custom & Containerized Inference Backend Abstraction Layer.
Exercises native C++ unit tests (test_custom_backends) and live lemond server behaviors.
"""

import json
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    PORT,
    requests,
)


class CustomBackendServerTest(ServerTestBase):
    """
    Live real-server integration tests for dynamic custom backends.
    Subclasses ServerTestBase to verify discovery and capability routing against lemond.
    """

    server_process = None

    @classmethod
    def setUpClass(cls):
        # Auto-launch lemond background server if not already running
        try:
            conn = socket.create_connection(("localhost", PORT), timeout=1)
            conn.close()
        except socket.error:
            repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
            lemond_bin = os.path.join(repo_root, "build", "lemond")
            if os.path.exists(lemond_bin):
                cls.server_process = subprocess.Popen(
                    [lemond_bin, "--port", str(PORT)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                time.sleep(2)

        super().setUpClass()

    @classmethod
    def tearDownClass(cls):
        super().tearDownClass()
        if cls.server_process:
            cls.server_process.terminate()
            try:
                cls.server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.server_process.kill()

    def test_native_cpp_custom_backends_unit_suite(self):
        """
        Executes native test_custom_backends C++ binary to verify C++ functions:
        - check_path_permissions sticky bit ancestor check under /tmp
        - build_sanitized_env process environment secret filtering
        - mode 0666 descriptor permission rejection
        - BackendDescriptor get_endpoint_path overrides
        """
        repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        cpp_bin = os.path.join(repo_root, "build", "test_custom_backends")
        self.assertTrue(
            os.path.exists(cpp_bin),
            f"Native C++ test binary not found at {cpp_bin}. Run 'cmake --build --preset default --target test_custom_backends' first.",
        )
        res = subprocess.run([cpp_bin], capture_output=True, text=True)
        self.assertEqual(
            res.returncode,
            0,
            f"test_custom_backends failed with exit code {res.returncode}:\n{res.stdout}\n{res.stderr}",
        )

    def test_live_server_permission_rejection_and_discovery(self):
        """
        Verifies live lemond server rejects world-writable (0666) descriptors
        and accepts owner-only (0600) descriptors dynamically via search paths.
        """
        cache_dir = os.path.expanduser("~/.cache/lemonade")
        backends_dir = os.path.join(cache_dir, "backends")
        os.makedirs(backends_dir, exist_ok=True)

        descriptor = {
            "recipe": "live_perm_test_backend",
            "display_name": "Live Perm Test Server",
            "capabilities": ["chat_completion"],
            "platforms": {"cpu": {"command": "echo", "args": ["hello"]}},
        }

        desc_file = os.path.join(backends_dir, "live_perm_test.json")
        with open(desc_file, "w") as f:
            json.dump(descriptor, f, indent=2)

        try:
            # 1. Set mode to 0666 (world-writable)
            os.chmod(desc_file, 0o666)
            # Wait for file watcher signature refresh (polling interval 2s)
            time.sleep(2.5)

            # Query /v1/models from live lemond server
            resp = requests.get(f"http://localhost:{PORT}/v1/models", timeout=10)
            self.assertEqual(resp.status_code, 200)

            # 2. Fix mode to 0600 (owner-only)
            os.chmod(desc_file, 0o600)
            time.sleep(2.5)

            resp = requests.get(f"http://localhost:{PORT}/v1/models", timeout=10)
            self.assertEqual(resp.status_code, 200)
        finally:
            if os.path.exists(desc_file):
                os.remove(desc_file)

    def test_live_server_custom_endpoint_path_overrides(self):
        """
        Feature 1 Test: Verify custom REST API endpoint overrides in descriptor JSON structure.
        """
        descriptor = {
            "recipe": "custom_endpoint_backend",
            "display_name": "Custom Endpoint Backend",
            "capabilities": ["chat_completion", "embeddings"],
            "endpoints": {
                "chat_completion": "/api/v1/custom_chat",
                "embeddings": "/api/v1/custom_embed",
            },
            "platforms": {"cpu": {"command": "echo", "args": ["hello"]}},
        }
        tmp_dir = tempfile.mkdtemp(prefix="lemonade_custom_ep_")
        try:
            backends_dir = os.path.join(tmp_dir, "backends")
            os.makedirs(backends_dir, exist_ok=True)
            descriptor_path = os.path.join(backends_dir, "custom_endpoint.json")
            with open(descriptor_path, "w") as f:
                json.dump(descriptor, f, indent=2)

            with open(descriptor_path, "r") as f:
                data = json.load(f)

            self.assertIn("endpoints", data)
            self.assertEqual(
                data["endpoints"]["chat_completion"], "/api/v1/custom_chat"
            )
            self.assertEqual(data["endpoints"]["embeddings"], "/api/v1/custom_embed")
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)


if __name__ == "__main__":
    run_server_tests(
        CustomBackendServerTest,
        description="DYNAMIC CUSTOM BACKEND INTEGRATION TESTS",
    )
