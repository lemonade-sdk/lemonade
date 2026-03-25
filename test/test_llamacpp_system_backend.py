"""
Tests for the 'system' LlamaCpp backend and LEMONADE_LLAMACPP_PREFER_SYSTEM.

Each test needs a fresh server with different PATH/env vars, so this file
manages its own server lifecycle independently of ServerTestBase.

Usage:
    python test/test_llamacpp_system_backend.py
    python test/test_llamacpp_system_backend.py --server-binary /path/to/lemonade-server
"""

import json
import os
import sys
import shutil
import socket
import subprocess
import tempfile
import time
import stat
import unittest

import requests
from utils.server_base import wait_for_server, parse_args, get_server_binary, PORT
from utils.test_models import (
    ENDPOINT_TEST_MODEL,
    TIMEOUT_DEFAULT,
    TIMEOUT_MODEL_OPERATION,
)

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

MOCK_LLAMA_SERVER_PYTHON = """#!/usr/bin/env python3
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def get_arg(flag, default):
    if flag in sys.argv:
        index = sys.argv.index(flag)
        if index + 1 < len(sys.argv):
            return sys.argv[index + 1]
    return default


class ReusableHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True


capture_path = os.environ.get("MOCK_LLAMA_REQUEST_PATH", "")
port = int(get_arg("--port", "8000"))


class Handler(BaseHTTPRequestHandler):
    def _send_json(self, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._send_json({"status": "ok"})
            return
        self.send_error(404)

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8")
        if capture_path:
            with open(capture_path, "w", encoding="utf-8") as handle:
                handle.write(body)

        request_json = json.loads(body)
        if request_json.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()

            chunks = [
                {
                    "id": "chatcmpl-mock",
                    "object": "chat.completion.chunk",
                    "choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}],
                },
                {
                    "id": "chatcmpl-mock",
                    "object": "chat.completion.chunk",
                    "choices": [{"index": 0, "delta": {"content": "hello"}, "finish_reason": None}],
                },
                {
                    "id": "chatcmpl-mock",
                    "object": "chat.completion.chunk",
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                },
            ]

            for chunk in chunks:
                self.wfile.write(f"data: {json.dumps(chunk)}\\n\\n".encode("utf-8"))
            self.wfile.write(b"data: [DONE]\\n\\n")
            self.wfile.flush()
            return

        self._send_json(
            {
                "id": "chatcmpl-mock",
                "object": "chat.completion",
                "choices": [
                    {
                        "index": 0,
                        "message": {"role": "assistant", "content": "hello"},
                        "finish_reason": "stop",
                    }
                ],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
            }
        )

    def log_message(self, format, *args):
        return


ReusableHTTPServer(("127.0.0.1", port), Handler).serve_forever()
"""


def _is_server_running(port=PORT):
    """Check if the server is running on the given port."""
    try:
        conn = socket.create_connection(("localhost", port), timeout=2)
        conn.close()
        return True
    except (socket.error, socket.timeout):
        return False


def _wait_for_server_stop(port=PORT, timeout=30):
    """Wait for server to stop."""
    start_time = time.time()
    while time.time() - start_time < timeout:
        if not _is_server_running(port):
            return True
        time.sleep(1)
    return False


def _stop_server():
    """Stop the server via CLI stop command."""
    server_binary = get_server_binary()
    try:
        subprocess.run(
            [server_binary, "stop"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        _wait_for_server_stop()
    except Exception as e:
        print(f"Warning: Failed to stop server: {e}")


def _start_server(wrapped_server=None, backend=None):
    """Start the server and wait for it to be ready."""
    server_binary = get_server_binary()
    cmd = [server_binary, "serve", "--log-level", "debug"]

    if os.name == "nt" or os.getenv("LEMONADE_CI_MODE"):
        cmd.append("--no-tray")

    # Add wrapped server / backend args if specified
    if wrapped_server == "llamacpp" and backend:
        cmd.extend(["--llamacpp", backend])

    if sys.platform == "win32":
        subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=os.environ.copy(),
        )
    else:
        subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=os.environ.copy(),
        )

    wait_for_server(timeout=60)
    print("Server started successfully")


class LlamaCppSystemBackendTests(unittest.TestCase):
    """
    Tests for the 'system' LlamaCpp backend and the LEMONADE_LLAMACPP_PREFER_SYSTEM option.

    Each test needs a fresh server with different PATH/env vars,
    so this class manages its own server lifecycle.
    """

    _model_pulled = False

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        # Create a temporary directory for our dummy llama-server executable
        cls.temp_bin_dir = tempfile.mkdtemp()
        cls.dummy_llama_server_path = os.path.join(cls.temp_bin_dir, "llama-server")
        if os.name == "nt":
            cls.dummy_llama_server_path += ".exe"
        cls._write_llama_server(
            DUMMY_LLAMA_SERVER_WINDOWS
            if os.name == "nt"
            else DUMMY_LLAMA_SERVER_LINUX_MAC
        )

        # Store original PATH to restore later
        cls.original_path = os.environ.get("PATH", "")

    @classmethod
    def tearDownClass(cls):
        # Stop any server we started
        _stop_server()
        # Clean up temporary directory and restore PATH
        shutil.rmtree(cls.temp_bin_dir)
        os.environ["PATH"] = cls.original_path
        super().tearDownClass()

    @classmethod
    def _write_llama_server(cls, script_contents):
        with open(cls.dummy_llama_server_path, "w", encoding="utf-8") as handle:
            handle.write(script_contents)
        if os.name != "nt":
            os.chmod(
                cls.dummy_llama_server_path,
                os.stat(cls.dummy_llama_server_path).st_mode | stat.S_IEXEC,
            )

    def setUp(self):
        print(f"\n=== Starting test: {self._testMethodName} ===")
        _stop_server()
        # Reset environment variables for each test
        os.environ.pop("LEMONADE_LLAMACPP_PREFER_SYSTEM", None)
        os.environ.pop("MOCK_LLAMA_REQUEST_PATH", None)
        os.environ["PATH"] = self.original_path  # Ensure PATH is clean before each test
        self._write_llama_server(
            DUMMY_LLAMA_SERVER_WINDOWS
            if os.name == "nt"
            else DUMMY_LLAMA_SERVER_LINUX_MAC
        )

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

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return

        response = requests.post(
            f"http://localhost:{PORT}/api/v1/pull",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        if response.status_code != 200:
            raise AssertionError(
                f"Expected 200 from /api/v1/pull, got {response.status_code}"
            )
        cls._model_pulled = True

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_001_system_llamacpp_not_in_path(self):
        """
        Verify that is_llamacpp_installed('system') is False when llama-server is not in PATH.
        """
        self._remove_dummy_llama_server_from_path()  # Ensure it's not in PATH
        _start_server()

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
        _start_server()

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
        _start_server()

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
        _start_server()

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
        - When unset: system should NOT be default (explicitly disabled by default)
        - When set to 'false': system should be skipped, fallback to next backend
        """
        self._add_dummy_llama_server_to_path()
        # Test with unset (default behavior) - system should NOT be default (it's disabled by default)
        os.environ.pop("LEMONADE_LLAMACPP_PREFER_SYSTEM", None)
        _start_server()

        response = requests.get(f"http://localhost:{PORT}/api/v1/system-info")
        data = response.json()
        self.assertIn("recipes", data)
        self.assertIn("llamacpp", data["recipes"])
        # By default, system backend is not preferred, should fall back to next backend
        self.assertNotEqual(data["recipes"]["llamacpp"]["default_backend"], "system")

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertEqual(backends["system"]["state"], "installed")

        _stop_server()

        # Test with false - system backend should be explicitly skipped (same as default)
        os.environ["LEMONADE_LLAMACPP_PREFER_SYSTEM"] = "false"
        _start_server()

        response = requests.get(f"http://localhost:{PORT}/api/v1/system-info")
        data = response.json()
        self.assertIn("recipes", data)
        self.assertIn("llamacpp", data["recipes"])
        # When explicitly set to false, system should not be default (same as unset)
        self.assertNotEqual(data["recipes"]["llamacpp"]["default_backend"], "system")

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertEqual(backends["system"]["state"], "installed")

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_006_thinking_false_maps_to_no_think_for_chat_streams(self):
        """Verify thinking:false is mapped to /no_think prefix on the last user message."""
        self._write_llama_server(MOCK_LLAMA_SERVER_PYTHON)
        self._add_dummy_llama_server_to_path()

        capture_path = os.path.join(self.temp_bin_dir, "captured_chat_request.json")
        os.environ["MOCK_LLAMA_REQUEST_PATH"] = capture_path
        self.addCleanup(os.environ.pop, "MOCK_LLAMA_REQUEST_PATH", None)

        _stop_server()
        _start_server(wrapped_server="llamacpp", backend="system")
        self._ensure_model_pulled()

        load_response = requests.post(
            f"http://localhost:{PORT}/api/v1/load",
            json={"model_name": ENDPOINT_TEST_MODEL, "llamacpp_backend": "system"},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(load_response.status_code, 200)

        response = requests.post(
            f"http://localhost:{PORT}/api/v1/chat/completions",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "messages": [{"role": "user", "content": "Say hello."}],
                "stream": True,
                "thinking": False,
                "max_tokens": 8,
            },
            stream=True,
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

        lines = [
            raw.decode("utf-8") if isinstance(raw, bytes) else raw
            for raw in response.iter_lines()
            if raw
        ]
        self.assertTrue(any("[DONE]" in line for line in lines))

        with open(capture_path, "r", encoding="utf-8") as handle:
            forwarded_request = json.load(handle)

        self.assertEqual(
            forwarded_request["messages"][-1]["content"],
            "/no_think\nSay hello.",
        )
        self.assertNotIn("thinking", forwarded_request)
        self.assertNotIn("enable_thinking", forwarded_request)

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_007_enable_thinking_takes_precedence_over_thinking_false(self):
        """Verify enable_thinking:true takes precedence over thinking:false."""
        self._write_llama_server(MOCK_LLAMA_SERVER_PYTHON)
        self._add_dummy_llama_server_to_path()

        capture_path = os.path.join(
            self.temp_bin_dir, "captured_chat_request_precedence.json"
        )
        os.environ["MOCK_LLAMA_REQUEST_PATH"] = capture_path
        self.addCleanup(os.environ.pop, "MOCK_LLAMA_REQUEST_PATH", None)

        _stop_server()
        _start_server(wrapped_server="llamacpp", backend="system")
        self._ensure_model_pulled()

        load_response = requests.post(
            f"http://localhost:{PORT}/api/v1/load",
            json={"model_name": ENDPOINT_TEST_MODEL, "llamacpp_backend": "system"},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(load_response.status_code, 200)

        response = requests.post(
            f"http://localhost:{PORT}/api/v1/chat/completions",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "messages": [{"role": "user", "content": "Say hello."}],
                "stream": False,
                "enable_thinking": True,
                "thinking": False,
                "max_tokens": 8,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

        with open(capture_path, "r", encoding="utf-8") as handle:
            forwarded_request = json.load(handle)

        self.assertEqual(forwarded_request["messages"][-1]["content"], "Say hello.")
        self.assertNotIn("thinking", forwarded_request)
        self.assertNotIn("enable_thinking", forwarded_request)


def _run_tests():
    """Run llamacpp system backend tests."""
    print(f"\n{'=' * 70}")
    print("LLAMACPP SYSTEM BACKEND TESTS")
    print(f"{'=' * 70}\n")

    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(LlamaCppSystemBackendTests)
    runner = unittest.TextTestRunner(verbosity=2, buffer=False, failfast=True)
    result = runner.run(suite)
    sys.exit(0 if (result and result.wasSuccessful()) else 1)


if __name__ == "__main__":
    _run_tests()
