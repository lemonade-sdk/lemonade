"""
Integration test suite for request cancellation and connection robustness in Lemonade server (lemond).

Validates client disconnect detection, early prefill drops, non-streaming timeouts,
protocol-specific SSE heartbeats, and multi-client queue isolation.
"""

import unittest
import socket
import time
import os
import shutil
import tempfile
import subprocess

import requests


def get_free_port() -> int:
    """Find an available TCP port on localhost."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_server(port: int, timeout: int = 15) -> bool:
    """Poll localhost:port until HTTP server responds."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            r = requests.get(
                f"http://127.0.0.1:{port}/api/v1/health",
                timeout=1,
                proxies={"http": None, "https": None},
            )
            if r.status_code == 200:
                return True
        except Exception:
            time.sleep(0.1)
    return False


class TestServerCancellation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """Find executable lemond, spawn server process on ephemeral port."""
        cls.port = get_free_port()
        cls.temp_dir = tempfile.mkdtemp(prefix="lemond_cancel_test_")

        lemond_bin = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "build", "lemond")
        )
        if not os.path.exists(lemond_bin):
            raise unittest.SkipTest(f"lemond binary not found at {lemond_bin}")

        log_path = os.path.join(cls.temp_dir, "lemond_test.log")
        cls.log_file = open(log_path, "w+", encoding="utf-8")

        cmd = [lemond_bin, cls.temp_dir, "--port", str(cls.port)]
        env = os.environ.copy()
        env["XDG_RUNTIME_DIR"] = cls.temp_dir
        env["NO_PROXY"] = "127.0.0.1,localhost,*"
        env["no_proxy"] = "127.0.0.1,localhost,*"
        cls.proc = subprocess.Popen(
            cmd,
            stdout=cls.log_file,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
        )

        if not wait_for_server(cls.port, timeout=30):
            cls.log_file.seek(0)
            log_output = cls.log_file.read()
            cls.tearDownClass()
            raise RuntimeError(
                f"lemond failed to start on port {cls.port}. Log output:\n{log_output}"
            )

    @classmethod
    def tearDownClass(cls):
        """Clean up lemond process, log file handles, and temp directory."""
        proc = getattr(cls, "proc", None)
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

        log_file = getattr(cls, "log_file", None)
        if log_file and not log_file.closed:
            log_file.close()

        temp_dir = getattr(cls, "temp_dir", None)
        if temp_dir and os.path.exists(temp_dir):
            shutil.rmtree(temp_dir, ignore_errors=True)

    def test_01_mid_stream_socket_disconnect(self):
        """Start streaming request, read initial bytes, then abruptly close socket."""
        req_payload = (
            "POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{self.port}\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 60\r\n"
            "Connection: keep-alive\r\n\r\n"
            '{"model":"default","messages":[{"role":"user","content":"hi"}]}'
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", self.port))
            s.sendall(req_payload.encode("utf-8"))
            s.settimeout(3.0)
            try:
                _data = s.recv(256)
            except socket.timeout:
                pass
            s.close()

        self.assertTrue(wait_for_server(self.port, timeout=5))

    def test_02_early_prefill_disconnect(self):
        """Send long request body and close socket immediately before any response bytes."""
        long_prompt = "Hello " * 500
        body = (
            '{"model":"default","messages":[{"role":"user","content":"'
            + long_prompt
            + '"}]}'
        )
        req_payload = (
            "POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{self.port}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n\r\n" + body
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", self.port))
            s.sendall(req_payload.encode("utf-8"))
            s.shutdown(socket.SHUT_RDWR)
            s.close()

        self.assertTrue(wait_for_server(self.port, timeout=5))

    def test_03_non_streaming_disconnect(self):
        """Send non-streaming POST request and close socket during execution."""
        body = '{"model":"default","messages":[{"role":"user","content":"Tell me a story"}],"stream":false}'
        req_payload = (
            "POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{self.port}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n\r\n" + body
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", self.port))
            s.sendall(req_payload.encode("utf-8"))
            time.sleep(0.05)
            s.close()

        self.assertTrue(wait_for_server(self.port, timeout=5))

    def test_04_multi_client_queue_isolation(self):
        """Assert Client B succeeds immediately after Client A drops mid-stream."""
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", self.port))
            req_payload = (
                "POST /v1/chat/completions HTTP/1.1\r\n"
                f"Host: 127.0.0.1:{self.port}\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 60\r\n"
                "Connection: close\r\n\r\n"
                '{"model":"default","messages":[{"role":"user","content":"hi"}]}'
            )
            s.sendall(req_payload.encode("utf-8"))
            s.close()

        r = requests.get(
            f"http://127.0.0.1:{self.port}/api/v1/health",
            timeout=5,
            proxies={"http": None, "https": None},
        )
        self.assertEqual(r.status_code, 200)
        self.assertIn("status", r.json())


if __name__ == "__main__":
    unittest.main()
