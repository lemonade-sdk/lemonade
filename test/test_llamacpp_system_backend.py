"""
Tests for the 'system' LlamaCpp backend and llamacpp.prefer_system config.

Each test needs a fresh server with different PATH/config, so this file
manages its own server lifecycle independently of ServerTestBase.

Usage:
    python test/test_llamacpp_system_backend.py
    python test/test_llamacpp_system_backend.py --cli-binary /path/to/lemonade
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
from utils.server_base import (
    _auth_headers,
    get_cli_binary,
    parse_args,
    PORT,
    pull_model_with_retry,
    set_server_config,
)
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


# lemond detects the system llama-server version by running
# `llama-server --version` and reading one line of stdout (see
# SystemInfo::get_system_llamacpp_version in src/cpp/server/system_info.cpp).
# The real binary prints a version line and exits immediately. We must mirror
# that: otherwise the probe blocks forever on our long-lived HTTP server and
# /internal/set hangs until the client times out.
if "--version" in sys.argv or "--help" in sys.argv:
    print("version: 9999 (mock)")
    sys.exit(0)


class ReusableHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True


capture_path = os.environ.get("MOCK_LLAMA_REQUEST_PATH", "")
error_status = int(os.environ.get("MOCK_LLAMA_ERROR_STATUS", "0") or "0")
error_response = os.environ.get("MOCK_LLAMA_ERROR_RESPONSE", "")
port = int(get_arg("--port", "13305"))


class Handler(BaseHTTPRequestHandler):
    def _send_json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
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

        if error_response:
            self._send_json(json.loads(error_response), status=error_status or 400)
            return

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
    """Wait for the server's HTTP endpoint to stop accepting connections."""
    start_time = time.time()
    while time.time() - start_time < timeout:
        if not _is_server_running(port):
            return True
        time.sleep(1)
    return False


def _get_lemond_binary():
    """Find the lemond binary in the same directory as the lemonade CLI."""
    cli_binary = get_cli_binary()
    if cli_binary:
        build_dir = os.path.dirname(cli_binary)
        name = "lemond.exe" if os.name == "nt" else "lemond"
        candidate = os.path.join(build_dir, name)
        if os.path.exists(candidate):
            return candidate
    return shutil.which("lemond") or "lemond"


def _pick_free_port():
    """Return an unused TCP port assigned by the OS on the IPv4 loopback.

    Each lemond instance this test starts uses a fresh OS-assigned port. That
    sidesteps a TCP rebind hazard: lemond (the server) initiates the close on
    shutdown, so its accepted socket on the listen port lingers in FIN_WAIT2 /
    TIME_WAIT. Once the process is gone that socket is orphaned and, on Linux,
    can keep the port un-bindable for up to ~60s even with SO_REUSEADDR. Because
    this test restarts lemond many times in quick succession, reusing one fixed
    port makes startup flaky in CI; a fresh port is always immediately bindable.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


def _stop_server():
    """Stop the currently-running server via /internal/shutdown."""
    try:
        requests.post(
            f"http://localhost:{PORT}/internal/shutdown",
            headers=_auth_headers(),
            timeout=5,
        )
        _wait_for_server_stop(PORT)
    except Exception as e:
        print(f"Warning: Failed to stop server: {e}")


def _server_healthy(port=PORT):
    """True if lemond answers a health check on the port."""
    try:
        response = requests.get(
            f"http://localhost:{port}/api/v1/health",
            headers=_auth_headers(),
            timeout=2,
        )
        return response.status_code == 200
    except Exception:
        return False


def _start_server(wrapped_server=None, backend=None, config_updates=None):
    """Start a fresh lemond on a new OS-assigned port and wait until ready.

    Each call binds a brand-new port (see _pick_free_port) and republishes it as
    the module-level ``PORT`` so the rest of the test talks to the current
    instance. Binding a fresh port every time means the rapid restarts this test
    performs never contend for a port still held in a lingering TCP teardown
    state by a previously stopped instance.
    """
    global PORT

    lemond_binary = _get_lemond_binary()
    cache_dir = LlamaCppSystemBackendTests.cache_dir

    # Stop the instance we previously started, if any, so it releases its
    # resources before we launch the next one.
    if _is_server_running(PORT):
        _stop_server()

    # Redirect output to a log file rather than a PIPE: lemond runs with
    # debug logging here, and an undrained PIPE can fill its buffer and block
    # the process before it opens the port. The log is printed on failure.
    log_path = os.path.join(tempfile.gettempdir(), "lemond_test.log")
    last_log = ""

    proc = None
    started = False
    max_attempts = 5
    for _attempt in range(1, max_attempts + 1):
        port = _pick_free_port()
        PORT = port
        cmd = [lemond_binary, cache_dir, "--port", str(port)]

        with open(log_path, "w", encoding="utf-8") as log_file:
            proc = subprocess.Popen(
                cmd,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                env=os.environ.copy(),
            )

        # Wait for this instance to become healthy, bailing early if it exits
        # so we can retry on a fresh port.
        deadline = time.time() + 30
        while time.time() < deadline:
            if proc.poll() is not None:
                break  # lemond exited early -> retry on a new port
            if _server_healthy(port):
                started = True
                break
            time.sleep(1)

        if started:
            break

        # This attempt failed: clean up before retrying on a new port.
        try:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=10)
        except Exception:
            pass
        try:
            with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                last_log = f.read()
        except OSError:
            last_log = ""

    if not started:
        print("=== lemond startup log (last attempt) ===")
        print(last_log)
        raise RuntimeError(f"lemond failed to start after {max_attempts} attempts")

    runtime_config = {}
    if wrapped_server == "llamacpp" and backend:
        runtime_config["llamacpp"] = {"backend": backend}
    if config_updates:
        runtime_config.update(config_updates)
    if runtime_config:
        set_server_config(runtime_config, port=PORT)

    print("Server started successfully")


class LlamaCppSystemBackendTests(unittest.TestCase):
    """
    Tests for the 'system' LlamaCpp backend and the llamacpp.prefer_system config option.

    Each test needs a fresh server with different PATH/config,
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

        # Dedicated cache_dir so the test doesn't disturb the user's real cache.
        # log_level=debug surfaces backend behavior in the logs.
        cls.cache_dir = tempfile.mkdtemp(prefix="lemonade_llamacpp_test_")
        with open(os.path.join(cls.cache_dir, "config.json"), "w") as cf:
            json.dump({"log_level": "debug"}, cf)

        # Store original PATH to restore later
        cls.original_path = os.environ.get("PATH", "")

    @classmethod
    def tearDownClass(cls):
        # Stop any server we started
        _stop_server()
        # Clean up temporary directories and restore PATH
        shutil.rmtree(cls.temp_bin_dir)
        shutil.rmtree(cls.cache_dir, ignore_errors=True)
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
        os.environ.pop("MOCK_LLAMA_REQUEST_PATH", None)
        os.environ.pop("MOCK_LLAMA_ERROR_STATUS", None)
        os.environ.pop("MOCK_LLAMA_ERROR_RESPONSE", None)
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

        pull_model_with_retry(ENDPOINT_TEST_MODEL, port=PORT)
        cls._model_pulled = True

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_001_system_llamacpp_not_in_path(self):
        """
        Verify that is_llamacpp_installed('system') is False when llama-server is not in PATH.
        """
        self._remove_dummy_llama_server_from_path()  # Ensure it's not in PATH
        _start_server(config_updates={"llamacpp": {"prefer_system": False}})

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
        _start_server(config_updates={"llamacpp": {"prefer_system": False}})

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertEqual(backends["system"]["state"], "installed")

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_003_prefer_system_llamacpp_enabled_and_available(self):
        """
        Verify 'system' backend is preferred when llamacpp.prefer_system=true in config
        and llama-server is in PATH.
        """
        self._add_dummy_llama_server_to_path()
        _start_server(config_updates={"llamacpp": {"prefer_system": True}})

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
        Verify fallback to another backend when llamacpp.prefer_system=true in config
        but llama-server is NOT in PATH.
        """
        self._remove_dummy_llama_server_from_path()  # Ensure it's not in PATH
        _start_server(config_updates={"llamacpp": {"prefer_system": True}})

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
        Verify behavior of llamacpp.prefer_system config when llama-server is in PATH.
        - When unset or false: system should NOT be default (explicitly disabled by default)
        - When set to true: system backend is preferred if available
        """
        self._add_dummy_llama_server_to_path()
        # Test with unset (default behavior) - system should NOT be default (it's disabled by default)
        _start_server(config_updates={"llamacpp": {"prefer_system": False}})

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
        _start_server(config_updates={"llamacpp": {"prefer_system": False}})

        response = requests.get(f"http://localhost:{PORT}/api/v1/system-info")
        data = response.json()
        self.assertIn("recipes", data)
        self.assertIn("llamacpp", data["recipes"])
        # When explicitly set to false, system should not be default (same as unset)
        self.assertNotEqual(data["recipes"]["llamacpp"]["default_backend"], "system")

        backends = self._get_llamacpp_backends()
        self.assertIn("system", backends)
        self.assertEqual(backends["system"]["state"], "installed")

    def _require_system_backend_blocked_by_hip_plugin(self):
        """Skip unless the running server reports the 'system' llamacpp backend
        as blocked by a missing HIP plugin.

        LEMONADE_GGML_HIP_PATH only affects is_ggml_hip_plugin_available(),
        which system_info.cpp consults solely for the 'system' backend, only
        when an AMD GPU is present (/sys/class/kfd) and llama-server is in PATH.
        Its only observable effect is the system backend's "HIP plugin
        libggml-hip.so not installed" message. Callers must add llama-server to
        PATH and start the server before calling this.
        """
        system = self._get_llamacpp_backends().get("system", {})
        if "HIP plugin" not in system.get("message", ""):
            self.skipTest(
                "Requires an AMD GPU without an FHS-installed HIP plugin; "
                f"system backend reported: {system}"
            )

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_005a_hip_path_override_satisfies_hip_plugin_check(self):
        """A valid LEMONADE_GGML_HIP_PATH satisfies the HIP-plugin check that
        otherwise blocks the 'system' backend on AMD GPU hosts."""
        if not os.path.exists("/sys/class/kfd"):
            self.skipTest("Requires an AMD GPU (/sys/class/kfd present)")
        self._add_dummy_llama_server_to_path()

        # Baseline: without the override the system backend is blocked by a
        # missing HIP plugin, otherwise the override has nothing to satisfy.
        _start_server()
        self._require_system_backend_blocked_by_hip_plugin()
        _stop_server()

        valid_so = os.path.join(self.temp_bin_dir, "libggml-hip.so")
        with open(valid_so, "w", encoding="utf-8") as handle:
            handle.write("stub")

        # _start_server() launches lemond with os.environ.copy(), so set the
        # override in this process's environment and clean it up afterwards.
        self.addCleanup(os.environ.pop, "LEMONADE_GGML_HIP_PATH", None)
        os.environ["LEMONADE_GGML_HIP_PATH"] = valid_so
        _start_server()
        system = self._get_llamacpp_backends().get("system", {})
        self.assertNotIn("HIP plugin", system.get("message", ""), system)

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_005b_hip_path_override_invalid_is_ignored(self):
        """An invalid LEMONADE_GGML_HIP_PATH (nonexistent file or directory) is
        ignored: the 'system' backend stays blocked by the missing HIP plugin."""
        if not os.path.exists("/sys/class/kfd"):
            self.skipTest("Requires an AMD GPU (/sys/class/kfd present)")
        self._add_dummy_llama_server_to_path()

        _start_server()
        self._require_system_backend_blocked_by_hip_plugin()
        _stop_server()

        self.addCleanup(os.environ.pop, "LEMONADE_GGML_HIP_PATH", None)

        # Nonexistent file: the override is ignored.
        os.environ["LEMONADE_GGML_HIP_PATH"] = os.path.join(
            self.temp_bin_dir, "missing", "libggml-hip.so"
        )
        _start_server()
        system = self._get_llamacpp_backends().get("system", {})
        self.assertIn("HIP plugin", system.get("message", ""), system)
        _stop_server()

        # A directory is not a regular file and must also be ignored.
        os.environ["LEMONADE_GGML_HIP_PATH"] = self.temp_bin_dir
        _start_server()
        system = self._get_llamacpp_backends().get("system", {})
        self.assertIn("HIP plugin", system.get("message", ""), system)

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

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_008_backend_context_error_preserves_http_status(self):
        """Verify backend context-window errors stay HTTP 400 and OpenAI-shaped."""
        self._write_llama_server(MOCK_LLAMA_SERVER_PYTHON)
        self._add_dummy_llama_server_to_path()

        error_message = (
            "request (67311 tokens) exceeds the available context size "
            "(65536 tokens), try increasing it"
        )
        os.environ["MOCK_LLAMA_ERROR_STATUS"] = "400"
        os.environ["MOCK_LLAMA_ERROR_RESPONSE"] = json.dumps(
            {"error": {"message": error_message, "type": "invalid_request_error"}}
        )
        self.addCleanup(os.environ.pop, "MOCK_LLAMA_ERROR_STATUS", None)
        self.addCleanup(os.environ.pop, "MOCK_LLAMA_ERROR_RESPONSE", None)

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
                "max_tokens": 8,
            },
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(response.status_code, 400)
        error = response.json()["error"]
        self.assertEqual(error["type"], "invalid_request_error")
        self.assertEqual(error["code"], "context_length_exceeded")
        self.assertEqual(error["status_code"], 400)
        self.assertIn("exceeds the available context size", error["message"])


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
