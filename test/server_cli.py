"""
Usage: python server_cli.py

This will launch the lemonade server and test the CLI.

If you get the `ImportError: cannot import name 'TypeIs' from 'typing_extensions'` error:
    1. pip uninstall typing_extensions
    2. pip install openai
"""

import unittest
import subprocess
import asyncio
import socket
import time
import json
from threading import Thread
import sys
import io
import httpx
from lemonade import __version__ as version_number

from utils.server_base import kill_process_on_port, PORT

try:
    from openai import OpenAI, AsyncOpenAI
except ImportError as e:
    raise ImportError("You must `pip install openai` to run this test", e)


class Testing(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        """
        Start lemonade server process
        """
        print("\n=== Starting new test ===")

        # Ensure we kill anything using the test port before and after the test
        kill_process_on_port(PORT)
        self.addCleanup(kill_process_on_port, PORT)

    def test_001_version(self):
        result = subprocess.run(
            ["lemonade-server-dev", "--version"], capture_output=True, text=True
        )

        # Check that the stdout ends with the version number (some apps rely on this)
        assert result.stdout.strip().endswith(
            version_number
        ), f"Expected stdout to end with '{version_number}', but got: '{result.stdout}'"

    def test_002_serve_status_and_stop(self):

        # First, ensure we can correctly detect that the server is not running
        result = subprocess.run(
            ["lemonade-server-dev", "status"],
            capture_output=True,
            text=True,
        )
        assert (
            result.stdout == "Server is not running\n"
        ), f"{result.stdout} {result.stderr}"

        # Now, start the server
        NON_DEFAULT_PORT = PORT + 1
        process = subprocess.Popen(
            [
                "lemonade-server-dev",
                "serve",
                "--port",
                str(NON_DEFAULT_PORT),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        # Wait a few seconds after the port is available
        time.sleep(20)

        # Now, ensure we can correctly detect that the server is running
        result = subprocess.run(
            ["lemonade-server-dev", "status"],
            capture_output=True,
            text=True,
        )
        assert (
            result.stdout == f"Server is running on port {NON_DEFAULT_PORT}\n"
        ), f"Expected stdout to end with '{NON_DEFAULT_PORT}', but got: '{result.stdout}' {result.stderr}"

        # Close the server
        result = subprocess.run(
            ["lemonade-server-dev", "stop"],
            capture_output=True,
            text=True,
        )
        assert result.stdout == "Lemonade Server stopped successfully.\n", result.stdout

        # Ensure the server is not running
        result = subprocess.run(
            ["lemonade-server-dev", "status"],
            capture_output=True,
            text=True,
        )
        assert result.stdout == "Server is not running\n", result.stdout

    def test_003_system_info_command(self):
        """
        Test the system-info CLI command with both default and verbose modes.
        """

        # Test default (non-verbose) table output
        result = subprocess.run(
            ["lemonade", "system-info"], capture_output=True, text=True, timeout=60
        )

        assert result.returncode == 0, f"system-info failed: {result.stderr}"

        # Check for expected sections in default output
        expected_sections = ["OS Version", "Processor", "Physical Memory", "Devices"]
        for section in expected_sections:
            assert (
                section in result.stdout
            ), f"Missing section '{section}' in default output"

        # Test verbose mode
        result = subprocess.run(
            ["lemonade", "system-info", "--verbose"],
            capture_output=True,
            text=True,
            timeout=60,
        )

        assert result.returncode == 0, f"verbose system-info failed: {result.stderr}"

        # Check for expected sections in verbose output
        expected_verbose_sections = [
            "OS Version",
            "Processor",
            "Physical Memory",
            "Devices",
            "Python Packages",
        ]
        for section in expected_verbose_sections:
            assert (
                section in result.stdout
            ), f"Missing section '{section}' in verbose output"

        # Test JSON output
        result = subprocess.run(
            ["lemonade", "system-info", "--format", "json"],
            capture_output=True,
            text=True,
            timeout=60,
        )

        assert result.returncode == 0, f"JSON system-info failed: {result.stderr}"

        # Should be valid JSON
        try:
            system_info = json.loads(result.stdout)
            assert isinstance(system_info, dict)
        except json.JSONDecodeError:
            assert False, f"Invalid JSON output: {result.stdout}"

        # Test JSON output (verbose mode)
        result = subprocess.run(
            ["lemonade", "system-info", "--verbose", "--format", "json"],
            capture_output=True,
            text=True,
            timeout=60,
        )

        assert (
            result.returncode == 0
        ), f"verbose JSON system-info failed: {result.stderr}"

        # Should be valid JSON with all verbose fields
        try:
            system_info = json.loads(result.stdout)
            assert isinstance(system_info, dict)
        except json.JSONDecodeError:
            assert False, f"Invalid verbose JSON output: {result.stdout}"


if __name__ == "__main__":
    unittest.main()

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
