import os
import shutil
import tempfile
import stat
import unittest
from unittest.mock import patch, MagicMock

from utils.server_base import ServerTestBase, run_server_tests
from utils.capabilities import get_capabilities

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
        capabilities = get_capabilities(self.base_url)
        self.assertIn("recipes", capabilities)
        self.assertIn("llamacpp", capabilities["recipes"])
        self.assertIn("backends", capabilities["recipes"]["llamacpp"])
        return capabilities["recipes"]["llamacpp"]["backends"]

    def test_001_system_llamacpp_not_in_path(self):
        """
        Verify that is_llamacpp_installed('system') is False when llama-server is not in PATH.
        """
        self._remove_dummy_llama_server_from_path()  # Ensure it's not in PATH
        self.start_server()

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertFalse(backends["system"]["available"])
        self.assertIn("error", backends["system"])
        self.assertIn("llama-server not found in PATH", backends["system"]["error"])

    def test_002_system_llamacpp_in_path(self):
        """
        Verify that is_llamacpp_installed('system') is True when llama-server is in PATH.
        """
        self._add_dummy_llama_server_to_path()  # Add dummy to PATH
        self.start_server()

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertTrue(backends["system"]["available"])
        self.assertNotIn("error", backends["system"])  # Should not have an error

    def test_003_prefer_system_llamacpp_enabled_and_available(self):
        """
        Verify 'system' backend is preferred when LEMONADE_LLAMACPP_PREFER_SYSTEM=true
        and llama-server is in PATH.
        """
        self._add_dummy_llama_server_to_path()
        os.environ["LEMONADE_LLAMACPP_PREFER_SYSTEM"] = "true"
        self.start_server()

        capabilities = get_capabilities(self.base_url)
        # Check that the overall supported backend for llamacpp is 'system'
        self.assertIn("llamacpp", capabilities["supported_recipes"])
        self.assertEqual(
            capabilities["supported_recipes"]["llamacpp"]["preferred_backend"], "system"
        )

        # Also check in the detailed backend list
        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertTrue(backends["system"]["available"])

        # Verify that if an explicit backend is NOT chosen, 'system' is the default
        # This is implicitly tested by preferred_backend.
        # To make it more explicit, one would typically load a model without specifying backend
        # and see which one gets used, but for capability testing, preferred_backend is sufficient.

    def test_004_prefer_system_llamacpp_enabled_but_not_available(self):
        """
        Verify fallback to another backend when LEMONADE_LLAMACPP_PREFER_SYSTEM=true
        but llama-server is NOT in PATH.
        """
        self._remove_dummy_llama_server_from_path()  # Ensure it's not in PATH
        os.environ["LEMONADE_LLAMACPP_PREFER_SYSTEM"] = "true"
        self.start_server()

        capabilities = get_capabilities(self.base_url)
        self.assertIn("llamacpp", capabilities["supported_recipes"])

        # 'system' should not be preferred, and it should fallback to another available backend
        preferred = capabilities["supported_recipes"]["llamacpp"]["preferred_backend"]
        self.assertNotEqual(preferred, "system")

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertFalse(backends["system"]["available"])
        self.assertIn("error", backends["system"])
        self.assertIn("llama-server not found in PATH", backends["system"]["error"])

        # Assert that some other backend is preferred and available
        self.assertTrue(backends[preferred]["available"])
        self.assertNotEqual(preferred, "")

    def test_005_prefer_system_llamacpp_disabled_or_unset(self):
        """
        Verify original preference order is maintained when LEMONADE_LLAMACPP_PREFER_SYSTEM
        is false or unset, even if llama-server is in PATH.
        """
        self._add_dummy_llama_server_to_path()
        # Test with unset (default behavior)
        os.environ.pop("LEMONADE_LLAMACPP_PREFER_SYSTEM", None)
        self.start_server()

        capabilities = get_capabilities(self.base_url)
        self.assertIn("llamacpp", capabilities["supported_recipes"])
        preferred_unset = capabilities["supported_recipes"]["llamacpp"][
            "preferred_backend"
        ]

        # 'system' should not be preferred, and it should fallback to original preference order
        # which is usually vulkan or metal before system.
        self.assertNotEqual(preferred_unset, "system")

        self.stop_server()  # Stop to restart with different env var

        # Test with false
        os.environ["LEMONADE_LLAMACPP_PREFER_SYSTEM"] = "false"
        self.start_server()

        capabilities = get_capabilities(self.base_url)
        self.assertIn("llamacpp", capabilities["supported_recipes"])
        preferred_false = capabilities["supported_recipes"]["llamacpp"][
            "preferred_backend"
        ]
        self.assertNotEqual(preferred_false, "system")
        self.assertEqual(
            preferred_unset, preferred_false
        )  # Should be same as unset behavior


if __name__ == "__main__":
    run_server_tests(LlamaCppSystemBackendTests, "LLAMACPP SYSTEM BACKEND TESTS")
