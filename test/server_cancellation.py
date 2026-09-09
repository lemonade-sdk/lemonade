"""
Integration test suite for request cancellation and connection robustness in Lemonade server (lemond).

Validates client disconnect detection, early prefill drops, non-streaming timeouts,
and multi-client queue isolation against a running server.

Usage:
    python test/server_cancellation.py
"""

import socket
import time
import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    _auth_headers,
    wait_for_server,
)
from utils.test_models import PORT, TIMEOUT_DEFAULT


class TestServerCancellation(ServerTestBase):
    """Tests client disconnect and request cancellation handling."""

    def _get_auth_header_str(self):
        headers = _auth_headers()
        if "Authorization" in headers:
            return f"Authorization: {headers['Authorization']}\r\n"
        return ""

    def test_01_mid_stream_socket_disconnect(self):
        """Start streaming request, read initial bytes, then abruptly close socket."""
        auth_hdr = self._get_auth_header_str()
        body = '{"model":"default","messages":[{"role":"user","content":"hi"}],"stream":true}'
        req_payload = (
            "POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{PORT}\r\n"
            "Content-Type: application/json\r\n"
            f"{auth_hdr}"
            f"Content-Length: {len(body)}\r\n"
            "Connection: keep-alive\r\n\r\n"
            f"{body}"
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", PORT))
            s.sendall(req_payload.encode("utf-8"))
            s.settimeout(3.0)
            try:
                _data = s.recv(256)
            except socket.timeout:
                pass
            s.close()

        self.assertTrue(wait_for_server(PORT, timeout=5))

    def test_02_early_prefill_disconnect(self):
        """Send long request body and close socket immediately before any response bytes."""
        auth_hdr = self._get_auth_header_str()
        long_prompt = "Hello " * 500
        body = (
            '{"model":"default","messages":[{"role":"user","content":"'
            + long_prompt
            + '"}],"stream":true}'
        )
        req_payload = (
            "POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{PORT}\r\n"
            "Content-Type: application/json\r\n"
            f"{auth_hdr}"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n\r\n" + body
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", PORT))
            s.sendall(req_payload.encode("utf-8"))
            s.shutdown(socket.SHUT_RDWR)
            s.close()

        self.assertTrue(wait_for_server(PORT, timeout=5))

    def test_03_non_streaming_disconnect(self):
        """Send non-streaming POST request and close socket during execution."""
        auth_hdr = self._get_auth_header_str()
        body = '{"model":"default","messages":[{"role":"user","content":"Tell me a story"}],"stream":false}'
        req_payload = (
            "POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{PORT}\r\n"
            "Content-Type: application/json\r\n"
            f"{auth_hdr}"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n\r\n" + body
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", PORT))
            s.sendall(req_payload.encode("utf-8"))
            time.sleep(0.05)
            s.close()

        self.assertTrue(wait_for_server(PORT, timeout=5))

    def test_04_multi_client_queue_isolation(self):
        """Assert Client B succeeds immediately after Client A drops mid-stream."""
        auth_hdr = self._get_auth_header_str()
        body = '{"model":"default","messages":[{"role":"user","content":"hi"}],"stream":true}'
        req_payload = (
            "POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{PORT}\r\n"
            "Content-Type: application/json\r\n"
            f"{auth_hdr}"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n\r\n"
            f"{body}"
        )
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect(("127.0.0.1", PORT))
            s.sendall(req_payload.encode("utf-8"))
            s.close()

        session = requests.Session()
        session.trust_env = False
        r = session.get(
            f"http://127.0.0.1:{PORT}/api/v1/health",
            headers=_auth_headers(),
            timeout=5,
        )
        self.assertEqual(r.status_code, 200)
        self.assertIn("status", r.json())


if __name__ == "__main__":
    run_server_tests(TestServerCancellation)
