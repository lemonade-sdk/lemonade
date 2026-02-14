import os
import sys
import shutil
import tempfile
import stat
import unittest
from unittest.mock import patch, MagicMock

import requests
from utils.server_base import ServerTestBase, run_server_tests, parse_args, PORT

args = parse_args()  # Initialize global _config

# Define a dummy executable content (e.g., a simple shell script)
# On Linux/macOS, a shell script that exits 0
# On Windows, a batch file that exits 0
DUMMY_LLAMA_SERVER_LINUX_MAC = """#!/bin/bash
exit 0
"""

DUMMY_LLAMA_SERVER_WINDOWS = """@echo off
exit 0
"""


class LlamaCppSystemBackendTests(ServerTestBase):
    """
    Tests for the 'system' LlamaCpp backend and the LEMONADE_LLAMACPP_PREFER_SYSTEM option.
    """

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        # Create a temporary directory for our dummy llama-server executable
        cls.temp_bin_dir = tempfile.mkdtemp()
        cls.dummy_llama_server_path = os.path.join(cls.temp_bin_dir, "llama-server")
        if os.name == "nt":  # Windows
            cls.dummy_llama_server_path += ".exe"
            with open(cls.dummy_llama_server_path, "w") as f:
                f.write(DUMMY_LLAMA_SERVER_WINDOWS)
        else:  # Linux/macOS
            with open(cls.dummy_llama_server_path, "w") as f:
                f.write(DUMMY_LLAMA_SERVER_LINUX_MAC)
            # Make it executable
            os.chmod(
                cls.dummy_llama_server_path,
                os.stat(cls.dummy_llama_server_path).st_mode | stat.S_IEXEC,
            )

        # Store original PATH to restore later
        cls.original_path = os.environ.get("PATH", "")

    @classmethod
    def tearDownClass(cls):
        super().tearDownClass()
        # Clean up temporary directory and restore PATH
        shutil.rmtree(cls.temp_bin_dir)
        os.environ["PATH"] = cls.original_path

    def setUp(self):
        super().setUp()
        self.stop_server()
        # Reset environment variables for each test
        os.environ.pop("LEMONADE_LLAMACPP_PREFER_SYSTEM", None)
        os.environ["PATH"] = self.original_path  # Ensure PATH is clean before each test

    def _add_dummy_llama_server_to_path(self):
        """Adds the directory containing the dummy llama-server to PATH."""
        os.environ["PATH"] = self.temp_bin_dir + os.pathsep + self.original_path

    def _remove_dummy_llama_server_from_path(self):
        """Removes the dummy llama-server directory from PATH."""
        os.environ["PATH"] = self.original_path

    def _get_llamacpp_backends(self):
        """Fetches the list of supported llamacpp backends from the server."""
        response = requests.get(f"http://localhost:{PORT}/api/v1/system-info")
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertIn("recipes", data)
        self.assertIn("llamacpp", data["recipes"])
        self.assertIn("backends", data["recipes"]["llamacpp"])
        return data["recipes"]["llamacpp"]["backends"]

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_001_system_llamacpp_not_in_path(self):
        """
        Verify that is_llamacpp_installed('system') is False when llama-server is not in PATH.
        """
        self._remove_dummy_llama_server_from_path()  # Ensure it's not in PATH
        self.stop_server()
        self.start_server()

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        # In the new backend manager, state is 'unsupported' if not in PATH
        self.assertEqual(backends["system"]["state"], "unsupported")
        self.assertIn("message", backends["system"])
        self.assertIn("llama-server not found in PATH", backends["system"]["message"])

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_002_system_llamacpp_in_path(self):
        """
        Verify that is_llamacpp_installed('system') is True when llama-server is in PATH.
        """
        self._add_dummy_llama_server_to_path()  # Add dummy to PATH
        self.stop_server()
        self.start_server()

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertEqual(backends["system"]["state"], "installed")

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_003_prefer_system_llamacpp_enabled_and_available(self):
        """
        Verify 'system' backend is preferred when LEMONADE_LLAMACPP_PREFER_SYSTEM=true
        and llama-server is in PATH.
        """
        self._add_dummy_llama_server_to_path()
        os.environ["LEMONADE_LLAMACPP_PREFER_SYSTEM"] = "true"
        self.stop_server()
        self.start_server()

        response = requests.get(f"http://localhost:{PORT}/api/v1/system-info")
        data = response.json()

        self.assertIn("recipes", data)
        self.assertIn("llamacpp", data["recipes"])
        self.assertEqual(data["recipes"]["llamacpp"]["default_backend"], "system")

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertEqual(backends["system"]["state"], "installed")

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_004_prefer_system_llamacpp_enabled_but_not_available(self):
        """
        Verify fallback to another backend when LEMONADE_LLAMACPP_PREFER_SYSTEM=true
        but llama-server is NOT in PATH.
        """
        self._remove_dummy_llama_server_from_path()  # Ensure it's not in PATH
        os.environ["LEMONADE_LLAMACPP_PREFER_SYSTEM"] = "true"
        self.stop_server()
        self.start_server()

        response = requests.get(f"http://localhost:{PORT}/api/v1/system-info")
        data = response.json()

        self.assertIn("recipes", data)
        self.assertIn("llamacpp", data["recipes"])
        # Should not be system
        self.assertNotEqual(data["recipes"]["llamacpp"]["default_backend"], "system")

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertEqual(backends["system"]["state"], "unsupported")
        self.assertIn("llama-server not found in PATH", backends["system"]["message"])

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_005_prefer_system_llamacpp_disabled_or_unset(self):
        """
        Verify behavior of LEMONADE_LLAMACPP_PREFER_SYSTEM when llama-server is in PATH.
        - When unset: system should be default (first in preference order)
        - When set to 'false': system should be skipped, fallback to next backend
        """
        self._add_dummy_llama_server_to_path()
        # Test with unset (default behavior) - system should be default since it's first in preference order
        os.environ.pop("LEMONADE_LLAMACPP_PREFER_SYSTEM", None)
        self.stop_server()
        self.start_server()

        response = requests.get(f"http://localhost:{PORT}/api/v1/system-info")
        data = response.json()
        self.assertIn("recipes", data)
        self.assertIn("llamacpp", data["recipes"])
        # System backend is first in preference order, so when available and unset, it should be default
        self.assertEqual(data["recipes"]["llamacpp"]["default_backend"], "system")

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertEqual(backends["system"]["state"], "installed")

        self.stop_server()

        # Test with false - system backend should be explicitly skipped
        os.environ["LEMONADE_LLAMACPP_PREFER_SYSTEM"] = "false"
        self.start_server()

        response = requests.get(f"http://localhost:{PORT}/api/v1/system-info")
        data = response.json()
        self.assertIn("recipes", data)
        self.assertIn("llamacpp", data["recipes"])
        # When explicitly set to false, system should not be default
        self.assertNotEqual(data["recipes"]["llamacpp"]["default_backend"], "system")

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertEqual(backends["system"]["state"], "installed")


if __name__ == "__main__":
    run_server_tests(LlamaCppSystemBackendTests, "LLAMACPP SYSTEM BACKEND TESTS")
