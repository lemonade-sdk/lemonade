"""
Process/server integration tests for the external 'system' LlamaCpp backend.

Pure PATH discovery, prefer_system selection/fallback, and HIP plugin resolution
are covered directly in C++ by test/cpp/test_llamacpp_system_backend.cpp. This
suite keeps the behavior that genuinely crosses the lemond/llama-server process
boundary and shares one server lifecycle across those assertions.

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
# `llama-server --version` and reading one line of stdout from the llamacpp
# backend resolver in src/cpp/server/backends/llamacpp/llamacpp_server.cpp.
# The real binary prints a version line and exits immediately. We must mirror
# that: otherwise the probe blocks forever on our long-lived HTTP server and
# /internal/set hangs until the client times out.
if "--version" in sys.argv or "--help" in sys.argv:
    print("version: 9999 (mock)")
    sys.exit(0)


class ReusableHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True


capture_path = os.environ.get("MOCK_LLAMA_REQUEST_PATH", "")
control_path = os.environ.get("MOCK_LLAMA_CONTROL_PATH", "")
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

        control = {}
        if control_path:
            try:
                with open(control_path, "r", encoding="utf-8") as handle:
                    control = json.load(handle)
            except (OSError, json.JSONDecodeError):
                control = {}

        error_response = control.get("error_response")
        if error_response:
            self._send_json(
                error_response,
                status=int(control.get("error_status", 400) or 400),
            )
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

    This suite owns one lemond instance. An OS-assigned port avoids colliding
    with another test/server while retaining retry-on-startup-failure behavior.
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
    """Start the suite's lemond on a fresh OS-assigned port and wait until ready."""
    global PORT

    lemond_binary = _get_lemond_binary()
    cache_dir = LlamaCppSystemBackendTests.cache_dir

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

    try:
        runtime_config = {}
        if wrapped_server == "llamacpp" and backend:
            runtime_config["llamacpp"] = {"backend": backend}
        if config_updates:
            runtime_config.update(config_updates)
        if runtime_config:
            set_server_config(runtime_config, port=PORT)
    except Exception:
        # lemond may already be healthy here; never leak it if post-start
        # configuration fails during suite setup.
        _stop_server()
        raise

    print("Server started successfully")


class LlamaCppSystemBackendTests(unittest.TestCase):
    """Process/HTTP integration coverage for the external system llama-server."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls._active = False
        cls._linux_setup = sys.platform.startswith("linux")
        if not cls._linux_setup:
            return

        cls.temp_bin_dir = tempfile.mkdtemp(prefix="lemonade_llamacpp_mock_bin_")
        cls.cache_dir = tempfile.mkdtemp(prefix="lemonade_llamacpp_test_")
        cls.dummy_llama_server_path = os.path.join(cls.temp_bin_dir, "llama-server")
        cls.capture_path = os.path.join(cls.temp_bin_dir, "captured_chat_request.json")
        cls.control_path = os.path.join(cls.temp_bin_dir, "mock_control.json")
        cls.original_env = {
            name: os.environ.get(name)
            for name in ("PATH", "MOCK_LLAMA_REQUEST_PATH", "MOCK_LLAMA_CONTROL_PATH")
        }

        try:
            cls._write_llama_server(MOCK_LLAMA_SERVER_PYTHON)

            # Keep the external-system integration deterministic on AMD hosts as
            # well: production accepts the plugin beside a PATH llama-server.
            with open(
                os.path.join(cls.temp_bin_dir, "libggml-hip.so"),
                "w",
                encoding="utf-8",
            ) as handle:
                handle.write("stub")

            with open(os.path.join(cls.cache_dir, "config.json"), "w") as cf:
                json.dump({"log_level": "debug"}, cf)

            original_path = cls.original_env["PATH"] or ""
            os.environ["PATH"] = cls.temp_bin_dir + os.pathsep + original_path
            os.environ["MOCK_LLAMA_REQUEST_PATH"] = cls.capture_path
            os.environ["MOCK_LLAMA_CONTROL_PATH"] = cls.control_path
            cls._write_mock_control({})

            # One lemond and one spawned mock llama-server are enough for all
            # process-boundary assertions in this file.
            _start_server(wrapped_server="llamacpp", backend="system")
            cls._active = True
            pull_model_with_retry(ENDPOINT_TEST_MODEL, port=PORT)

            load_response = requests.post(
                f"http://localhost:{PORT}/api/v1/load",
                json={"model_name": ENDPOINT_TEST_MODEL, "llamacpp_backend": "system"},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            if load_response.status_code != 200:
                raise RuntimeError(
                    "Failed to load system llamacpp integration model: "
                    f"{load_response.status_code} {load_response.text}"
                )
        except Exception:
            if cls._active:
                _stop_server()
            cls._restore_environment()
            shutil.rmtree(cls.temp_bin_dir, ignore_errors=True)
            shutil.rmtree(cls.cache_dir, ignore_errors=True)
            raise

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "_linux_setup", False):
            if cls._active:
                _stop_server()
            shutil.rmtree(cls.temp_bin_dir, ignore_errors=True)
            shutil.rmtree(cls.cache_dir, ignore_errors=True)
            cls._restore_environment()
        super().tearDownClass()

    @classmethod
    def _restore_environment(cls):
        for name, value in cls.original_env.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value

    @classmethod
    def _write_llama_server(cls, script_contents):
        with open(cls.dummy_llama_server_path, "w", encoding="utf-8") as handle:
            handle.write(script_contents)
        os.chmod(
            cls.dummy_llama_server_path,
            os.stat(cls.dummy_llama_server_path).st_mode | stat.S_IEXEC,
        )

    @classmethod
    def _write_mock_control(cls, control):
        with open(cls.control_path, "w", encoding="utf-8") as handle:
            json.dump(control, handle)

    def setUp(self):
        print(f"\n=== Starting test: {self._testMethodName} ===")
        if os.path.exists(self.capture_path):
            os.remove(self.capture_path)
        self._write_mock_control({})

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_006_thinking_false_maps_to_no_think_for_chat_streams(self):
        """Verify thinking:false is mapped to /no_think prefix on the last user message."""
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

        with open(self.capture_path, "r", encoding="utf-8") as handle:
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
    def test_006a_thinking_false_maps_to_no_think_for_non_streaming_chat(self):
        """Verify non-streaming chat preserves thinking-control normalization."""
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/chat/completions",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "messages": [{"role": "user", "content": "Say hello."}],
                "stream": False,
                "thinking": False,
                "max_tokens": 8,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

        with open(self.capture_path, "r", encoding="utf-8") as handle:
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

        with open(self.capture_path, "r", encoding="utf-8") as handle:
            forwarded_request = json.load(handle)

        self.assertEqual(forwarded_request["messages"][-1]["content"], "Say hello.")
        self.assertNotIn("thinking", forwarded_request)
        self.assertNotIn("enable_thinking", forwarded_request)

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "System backend only supported on Linux"
    )
    def test_008_backend_context_error_preserves_http_status(self):
        """Verify backend context-window errors stay HTTP 400 and OpenAI-shaped."""
        error_message = (
            "request (67311 tokens) exceeds the available context size "
            "(65536 tokens), try increasing it"
        )
        self._write_mock_control(
            {
                "error_status": 400,
                "error_response": {
                    "error": {
                        "message": error_message,
                        "type": "invalid_request_error",
                    }
                },
            }
        )

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
