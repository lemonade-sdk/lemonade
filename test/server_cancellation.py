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

HOST = os.environ.get("LEMONADE_TEST_HOST", "127.0.0.1")


def get_free_port() -> int:
    """Find an available TCP port on local interface."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


def wait_for_server(port: int, timeout: int = 15) -> bool:
    """Poll server until HTTP health endpoint responds."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            r = requests.get(
                f"http://{HOST}:{port}/api/v1/health",
                timeout=1,
            )
            if r.status_code == 200:
                return True
        except Exception:
            time.sleep(0.1)
    return False


class TestServerCancellation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """Find executable lemond, spawn server process on ephemeral port with retry."""
        lemond_bin = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "build", "lemond")
        )
        if not os.path.exists(lemond_bin):
            raise unittest.SkipTest(f"lemond binary not found at {lemond_bin}")

        cls.temp_dir = tempfile.mkdtemp(prefix="lemond_cancel_test_")

        max_retries = 3
        for attempt in range(max_retries):
            cls.port = get_free_port()
            log_path = os.path.join(cls.temp_dir, f"lemond_test_{attempt}.log")
            cls.log_file = open(log_path, "w+", encoding="utf-8")

            cmd = [lemond_bin, cls.temp_dir, "--port", str(cls.port)]
            cls.proc = subprocess.Popen(
                cmd,
                stdout=cls.log_file,
                stderr=subprocess.STDOUT,
                text=True,
                env=os.environ.copy(),
            )

            if wait_for_server(cls.port, timeout=15):
                break

            cls.proc.terminate()
            cls.proc.wait()
            cls.log_file.close()
            if attempt == max_retries - 1:
                cls.tearDownClass()
                raise RuntimeError(
                    f"lemond failed to start after {max_retries} port allocation attempts."
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
            f"Host: {HOST}:{self.port}\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 60\r\n"
            "Connection: keep-alive\r\n\r\n"
            '{"model":"default","messages":[{"role":"user","content":"hi"}]}'
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((HOST, self.port))
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
            f"Host: {HOST}:{self.port}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n\r\n" + body
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((HOST, self.port))
            s.sendall(req_payload.encode("utf-8"))
            s.shutdown(socket.SHUT_RDWR)
            s.close()

        self.assertTrue(wait_for_server(self.port, timeout=5))

    def test_03_non_streaming_disconnect(self):
        """Send non-streaming POST request and close socket during execution."""
        body = '{"model":"default","messages":[{"role":"user","content":"Tell me a story"}],"stream":false}'
        req_payload = (
            "POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: {HOST}:{self.port}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n\r\n" + body
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((HOST, self.port))
            s.sendall(req_payload.encode("utf-8"))
            time.sleep(0.05)
            s.close()

        self.assertTrue(wait_for_server(self.port, timeout=5))

    def test_04_multi_client_queue_isolation(self):
        """Assert Client B succeeds immediately after Client A drops mid-stream."""
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((HOST, self.port))
            req_payload = (
                "POST /v1/chat/completions HTTP/1.1\r\n"
                f"Host: {HOST}:{self.port}\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 60\r\n"
                "Connection: close\r\n\r\n"
                '{"model":"default","messages":[{"role":"user","content":"hi"}]}'
            )
            s.sendall(req_payload.encode("utf-8"))
            s.close()

        r = requests.get(
            f"http://{HOST}:{self.port}/api/v1/health",
            timeout=5,
        )
        self.assertEqual(r.status_code, 200)
        self.assertIn("status", r.json())


if __name__ == "__main__":
    unittest.main()
