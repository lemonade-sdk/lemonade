#!/usr/bin/env python3
"""
Integration tests for the WebSocket telemetry security policy.
Covers Admin Privilege Rule, User Token Match Rule, Guest Isolation Rule,
WebSocket Origin Verification, Ephemeral Session Tokens, and Admin Fallback.
"""

import asyncio
import json
import os
import sys
import socket
import time
import tempfile
import subprocess
import unittest
import requests
import websockets

# Make the `utils` package importable when this file is executed directly.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from utils.test_models import get_default_lemond_binary


def find_free_port():
    s = socket.socket()
    s.bind(("", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def get_span_attributes(span_data):
    attrs_raw = span_data.get("attributes", [])
    attrs = {}
    for item in attrs_raw:
        key = item.get("key")
        val_obj = item.get("value", {})
        val = (
            val_obj.get("stringValue")
            or val_obj.get("intValue")
            or val_obj.get("doubleValue")
        )
        attrs[key] = val
    return attrs


def trigger_span(
    port, auth_token=None, client_session_id=None, model="nonexistent-model"
):
    headers = {}
    if auth_token:
        headers["Authorization"] = f"Bearer {auth_token}"
    if client_session_id:
        headers["X-Client-Session-Id"] = client_session_id

    payload = {
        "model": model,
        "messages": [{"role": "user", "content": "hello"}],
    }
    requests.post(
        f"http://localhost:{port}/api/v1/chat/completions",
        json=payload,
        headers=headers,
        timeout=5,
    )


class TelemetrySecurityTestBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lemond_bin = get_default_lemond_binary()
        if not os.path.exists(cls.lemond_bin):
            raise RuntimeError(
                f"lemond binary not found at {cls.lemond_bin}. Build it first."
            )
        cls.procs = []
        cls.temp_dirs = []

    @classmethod
    def tearDownClass(cls):
        for proc, log_file, log_file_path in cls.procs:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except Exception:
                    try:
                        proc.kill()
                        proc.wait()
                    except Exception:
                        pass
            try:
                log_file.close()
            except Exception:
                pass
            pass
        for temp_dir in cls.temp_dirs:
            try:
                import shutil

                shutil.rmtree(temp_dir)
            except Exception:
                pass

    @classmethod
    def start_server(cls, env_overrides=None):
        temp_dir = tempfile.mkdtemp(prefix="lemond_ws_sec_")
        cls.temp_dirs.append(temp_dir)

        port = find_free_port()
        env = os.environ.copy()

        # Clean all relevant env vars by default so tests are isolated
        for k in [
            "LEMONADE_API_KEY",
            "LEMONADE_ADMIN_API_KEY",
            "LEMONADE_ALLOWED_ORIGINS",
        ]:
            env.pop(k, None)

        if env_overrides:
            for k, v in env_overrides.items():
                if v is None:
                    env.pop(k, None)
                else:
                    env[k] = v

        log_file_path = os.path.join(temp_dir, "lemond.log")
        log_file = open(log_file_path, "w")
        proc = subprocess.Popen(
            [cls.lemond_bin, temp_dir, "--port", str(port)],
            stdout=log_file,
            stderr=log_file,
            env=env,
        )
        cls.procs.append((proc, log_file, log_file_path))

        # Wait for server to start
        for _ in range(60):
            try:
                res = requests.get(f"http://localhost:{port}/live", timeout=0.2)
                if res.status_code == 200:
                    # Get ws port from /health
                    headers = {}
                    api_key = (
                        env_overrides.get("LEMONADE_API_KEY") if env_overrides else None
                    )
                    if api_key:
                        headers["Authorization"] = f"Bearer {api_key}"

                    health_res = requests.get(
                        f"http://localhost:{port}/api/v1/health",
                        headers=headers,
                        timeout=0.5,
                    )
                    ws_port = health_res.json().get("websocket_port")
                    return port, ws_port
            except Exception:
                pass
            time.sleep(0.05)

        proc.terminate()
        try:
            with open(log_file_path, "r") as f:
                log_content = f.read()
        except Exception:
            log_content = "Could not read log file"
        raise RuntimeError(
            f"Failed to start temporary lemond server on port {port}. Log:\n{log_content}"
        )


class TelemetryAuthSecurityTests(TelemetrySecurityTestBase):
    """Telemetry Security tests in Authenticated Mode (Config A)."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.port, cls.ws_port = cls.start_server(
            {"LEMONADE_API_KEY": "user_key", "LEMONADE_ADMIN_API_KEY": "admin_key"}
        )

    def test_001_admin_privilege_rule(self):
        """Verify Admin Privilege Rule: Admin client receives all spans globally."""

        async def run_test():
            uri = f"ws://localhost:{self.ws_port}/spans/stream?api_key=admin_key"
            async with websockets.connect(uri) as websocket:
                await asyncio.sleep(0.2)
                trigger_span(self.port, auth_token="user_key", model="model-user")
                trigger_span(self.port, auth_token="admin_key", model="model-admin")

                spans = []
                for _ in range(2):
                    msg = await asyncio.wait_for(websocket.recv(), timeout=3.0)
                    spans.append(json.loads(msg))

                models = {get_span_attributes(s).get("llm.model_name") for s in spans}
                self.assertIn("model-user", models)
                self.assertIn("model-admin", models)

        asyncio.run(run_test())

    def test_002_user_token_match_rule(self):
        """Verify User Token Match Rule: Regular users only see their own spans (via message handshake)."""

        async def run_test():
            uri_user = f"ws://localhost:{self.ws_port}/spans/stream"
            async with websockets.connect(uri_user) as ws_user:
                # Send auth handshake message
                await ws_user.send(json.dumps({"type": "auth", "token": "user_key"}))

                # Expect auth.ok acknowledgement
                ack_msg = await asyncio.wait_for(ws_user.recv(), timeout=3.0)
                ack = json.loads(ack_msg)
                self.assertEqual(ack.get("type"), "auth.ok")

                trigger_span(self.port, auth_token="user_key", model="model-user")
                msg = await asyncio.wait_for(ws_user.recv(), timeout=3.0)
                span_data = json.loads(msg)
                attrs = get_span_attributes(span_data)
                self.assertNotIn("lemon.auth_token", attrs)
                self.assertNotIn("lemon.auth_token_hash", attrs)

                trigger_span(self.port, auth_token="admin_key", model="model-admin")

                with self.assertRaises(asyncio.TimeoutError):
                    await asyncio.wait_for(ws_user.recv(), timeout=2.0)

        asyncio.run(run_test())


class TelemetryGuestSecurityTests(TelemetrySecurityTestBase):
    """Telemetry Security tests in Guest Mode (Config B)."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.port, cls.ws_port = cls.start_server()

    def test_003_guest_isolation_rule(self):
        """Verify Guest Isolation Rule: Guests cannot capture user private traces."""

        async def run_test():
            uri_guest = f"ws://localhost:{self.ws_port}/spans/stream?client_session_id=guest_session"
            async with websockets.connect(uri_guest) as ws_guest:
                await asyncio.sleep(0.2)
                trigger_span(
                    self.port,
                    auth_token=None,
                    client_session_id="guest_session",
                    model="model-anonymous",
                )
                msg = await asyncio.wait_for(ws_guest.recv(), timeout=3.0)
                span_data = json.loads(msg)
                attrs = get_span_attributes(span_data)
                self.assertNotIn("lemon.auth_token", attrs)
                self.assertNotIn("lemon.auth_token_hash", attrs)

                trigger_span(
                    self.port,
                    auth_token="private_token",
                    client_session_id="guest_session",
                    model="model-private",
                )

                with self.assertRaises(asyncio.TimeoutError):
                    await asyncio.wait_for(ws_guest.recv(), timeout=2.0)

        asyncio.run(run_test())

    def test_004_websocket_origin_verification(self):
        """Verify WebSocket Origin Verification: Reject non-local Origins, accept missing/local."""

        async def run_test():
            try:
                async with websockets.connect(
                    f"ws://localhost:{self.ws_port}/spans/stream",
                    additional_headers={"Origin": "http://malicious.com"},
                ):
                    self.fail("Expected connection to be rejected for malicious origin")
            except (
                websockets.exceptions.InvalidStatus,
                websockets.exceptions.InvalidHandshake,
                TypeError,
            ):
                pass

            async with websockets.connect(
                f"ws://localhost:{self.ws_port}/spans/stream",
                additional_headers={"Origin": "http://localhost:3000"},
            ) as ws:
                pass

            async with websockets.connect(
                f"ws://localhost:{self.ws_port}/spans/stream",
                additional_headers={"Origin": "http://127.0.0.1:8080"},
            ) as ws:
                pass

            async with websockets.connect(
                f"ws://localhost:{self.ws_port}/spans/stream"
            ) as ws:
                pass

        asyncio.run(run_test())

    def test_005_ephemeral_session_tokens(self):
        """Verify Ephemeral Session Tokens: Isolate connections using client_session_id."""

        async def run_test():
            uri_session = f"ws://localhost:{self.ws_port}/spans/stream?client_session_id=session_123"
            async with websockets.connect(uri_session) as ws:
                await asyncio.sleep(0.2)
                trigger_span(
                    self.port, client_session_id="session_123", model="model-matched"
                )
                msg = await asyncio.wait_for(ws.recv(), timeout=3.0)
                span_data = json.loads(msg)
                attrs = get_span_attributes(span_data)
                self.assertEqual(attrs.get("lemon.client_session_id"), "session_123")

                trigger_span(
                    self.port, client_session_id="session_456", model="model-mismatched"
                )

                with self.assertRaises(asyncio.TimeoutError):
                    await asyncio.wait_for(ws.recv(), timeout=2.0)

        asyncio.run(run_test())

    def test_007_upgrade_on_main_http_port(self):
        """Verify that clients can upgrade to WebSocket spans stream directly on the main HTTP port."""

        async def run_test():
            uri = f"ws://localhost:{self.port}/spans/stream?client_session_id=main_port_session"
            async with websockets.connect(uri) as ws:
                await asyncio.sleep(0.2)
                trigger_span(
                    self.port,
                    client_session_id="main_port_session",
                    model="model-main-port",
                )
                msg = await asyncio.wait_for(ws.recv(), timeout=3.0)
                span_data = json.loads(msg)
                self.assertEqual(span_data.get("name"), "chat.completions")

        asyncio.run(run_test())

    def test_011_http_preflight_cors_and_pna(self):
        """Verify HTTP CORS and PNA preflight responses: allow loopback, reject malicious, Vary: Origin."""
        # 1. Allowed loopback origin (OPTIONS)
        headers = {
            "Origin": "http://localhost:3000",
            "Access-Control-Request-Private-Network": "true",
            "Access-Control-Request-Method": "POST",
        }
        res = requests.options(
            f"http://localhost:{self.port}/api/v1/chat/completions",
            headers=headers,
            timeout=5,
        )
        self.assertEqual(res.status_code, 204)
        self.assertEqual(
            res.headers.get("Access-Control-Allow-Origin"), "http://localhost:3000"
        )
        self.assertEqual(
            res.headers.get("Access-Control-Allow-Private-Network"), "true"
        )
        self.assertEqual(res.headers.get("Vary"), "Origin")

        # 2. Rejected malicious origin (OPTIONS returns 403)
        headers = {
            "Origin": "http://malicious.com",
            "Access-Control-Request-Private-Network": "true",
            "Access-Control-Request-Method": "POST",
        }
        res = requests.options(
            f"http://localhost:{self.port}/api/v1/chat/completions",
            headers=headers,
            timeout=5,
        )
        self.assertEqual(res.status_code, 403)
        self.assertNotIn("Access-Control-Allow-Origin", res.headers)
        self.assertNotIn("Access-Control-Allow-Private-Network", res.headers)
        self.assertEqual(res.headers.get("Vary"), "Origin")

        # 3. Rejected malicious origin (POST returns 403, not dispatched)
        headers = {
            "Origin": "http://malicious.com",
        }
        res = requests.post(
            f"http://localhost:{self.port}/api/v1/chat/completions",
            json={
                "model": "test-model",
                "messages": [{"role": "user", "content": "hi"}],
            },
            headers=headers,
            timeout=5,
        )
        self.assertEqual(res.status_code, 403)

        # 3b. Rejected malicious origin (text/plain POST to /mcp returns 403, not dispatched)
        headers = {
            "Origin": "http://malicious.com",
            "Content-Type": "text/plain",
        }
        res = requests.post(
            f"http://localhost:{self.port}/mcp",
            data='{"jsonrpc": "2.0", "method": "tools/list", "params": {}, "id": 1}',
            headers=headers,
            timeout=5,
        )
        self.assertEqual(res.status_code, 403)

        # 4. Clients without Origin header continue to work (e.g. CLI/API scripts)
        res = requests.post(
            f"http://localhost:{self.port}/api/v1/chat/completions",
            json={
                "model": "test-model",
                "messages": [{"role": "user", "content": "hi"}],
            },
            headers={"Authorization": "Bearer admin_key"},
            timeout=5,
        )
        # Should not be 403 (it should proceed to route lookup/auth and return 400 or 404 since model is not loaded)
        self.assertNotEqual(res.status_code, 403)

        # 5. Explicitly configured remote origin succeeds, loopback still succeeds, other fails
        remote_port, _ = self.start_server(
            {
                "LEMONADE_ALLOWED_ORIGINS": "https://app.example.com",
                "LEMONADE_ADMIN_API_KEY": "admin_key",
            }
        )

        # Configured remote origin OPTIONS preflight
        res = requests.options(
            f"http://localhost:{remote_port}/api/v1/chat/completions",
            headers={
                "Origin": "https://app.example.com",
                "Access-Control-Request-Method": "POST",
            },
            timeout=5,
        )
        self.assertEqual(res.status_code, 204)
        self.assertEqual(
            res.headers.get("Access-Control-Allow-Origin"), "https://app.example.com"
        )

        # Configured remote origin POST actual request
        res = requests.post(
            f"http://localhost:{remote_port}/api/v1/chat/completions",
            json={
                "model": "test-model",
                "messages": [{"role": "user", "content": "hi"}],
            },
            headers={
                "Origin": "https://app.example.com",
                "Authorization": "Bearer admin_key",
            },
            timeout=5,
        )
        self.assertNotEqual(res.status_code, 403)

        # Loopback origin OPTIONS preflight still succeeds
        res = requests.options(
            f"http://localhost:{remote_port}/api/v1/chat/completions",
            headers={
                "Origin": "http://localhost:3000",
                "Access-Control-Request-Method": "POST",
            },
            timeout=5,
        )
        self.assertEqual(res.status_code, 204)

        # Unconfigured remote origin OPTIONS preflight returns 403
        res = requests.options(
            f"http://localhost:{remote_port}/api/v1/chat/completions",
            headers={
                "Origin": "https://unconfigured.com",
                "Access-Control-Request-Method": "POST",
            },
            timeout=5,
        )
        self.assertEqual(res.status_code, 403)

        # 6. Regression test: Request without Origin header still returns Vary: Origin
        res = requests.post(
            f"http://localhost:{self.port}/api/v1/chat/completions",
            json={
                "model": "test-model",
                "messages": [{"role": "user", "content": "hi"}],
            },
            headers={"Authorization": "Bearer admin_key"},
            timeout=5,
        )
        self.assertEqual(res.headers.get("Vary"), "Origin")


class TelemetryFallbackSecurityTests(TelemetrySecurityTestBase):
    """Telemetry Security tests in Fallback Mode (Config C)."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.port, cls.ws_port = cls.start_server(
            {
                "LEMONADE_API_KEY": "user_key",
            }
        )

    def test_006_admin_fallback_rule(self):
        """Verify Admin Fallback: User key inherits admin clearance if admin key is unset."""

        async def run_test():
            uri = f"ws://localhost:{self.ws_port}/spans/stream?api_key=user_key"
            async with websockets.connect(uri) as websocket:
                pass

        asyncio.run(run_test())

        payload = {"telemetry": {"enabled": False}}
        res = requests.post(
            f"http://localhost:{self.port}/internal/set",
            json=payload,
            headers={"Authorization": "Bearer user_key"},
            timeout=2,
        )
        self.assertEqual(res.status_code, 200)

        res = requests.post(
            f"http://localhost:{self.port}/internal/set",
            json=payload,
            headers={"Authorization": "Bearer other_key"},
            timeout=2,
        )
        self.assertEqual(res.status_code, 401)


class TelemetryOTLPSecurityTests(TelemetrySecurityTestBase):
    def test_008_otlp_api_key_redaction(self):
        """Verify that OTLP telemetry does not export lemon.auth_token."""
        import threading
        from http.server import HTTPServer, BaseHTTPRequestHandler

        class MockOTLPHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                content_length = int(self.headers["Content-Length"])
                post_data = self.rfile.read(content_length)
                try:
                    data = json.loads(post_data.decode("utf-8"))
                    self.server.received_data.append(data)
                except Exception as e:
                    print("Error parsing mock OTLP data:", e)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"status": "ok"}')

            def log_message(self, format, *args):
                pass

        class MockOTLPServer(HTTPServer):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, **kwargs)
                self.received_data = []

        mock_port = find_free_port()
        mock_server = MockOTLPServer(("127.0.0.1", mock_port), MockOTLPHandler)
        server_thread = threading.Thread(target=mock_server.serve_forever)
        server_thread.daemon = True
        server_thread.start()

        try:
            port, ws_port = self.start_server(
                {"LEMONADE_API_KEY": "user_key", "LEMONADE_ADMIN_API_KEY": "admin_key"}
            )

            payload = {
                "telemetry": {
                    "enabled": True,
                    "otlp": {
                        "endpoint": f"http://127.0.0.1:{mock_port}/v1/traces",
                        "protocol": "http/json",
                        "send_batch_size": 1,
                        "batch_timeout_s": 0.01,
                    },
                }
            }
            res = requests.post(
                f"http://127.0.0.1:{port}/internal/set",
                json=payload,
                headers={"Authorization": "Bearer admin_key"},
                timeout=2,
            )
            self.assertEqual(res.status_code, 200)

            trigger_span(port, auth_token="user_key", model="model-otlp-test")

            found_span = False
            for _ in range(50):
                if mock_server.received_data:
                    for req in mock_server.received_data:
                        if "resourceSpans" in req:
                            for rs in req["resourceSpans"]:
                                for ss in rs.get("scopeSpans", []):
                                    for span in ss.get("spans", []):
                                        found_span = True
                                        attrs = get_span_attributes(span)
                                        self.assertNotIn("lemon.auth_token", attrs)
                                        self.assertNotIn("lemon.auth_token_hash", attrs)
                if found_span:
                    break
                time.sleep(0.1)

            self.assertTrue(found_span, "Should have received span in mock OTLP server")
        finally:
            mock_server.shutdown()
            mock_server.server_close()
            server_thread.join()

    def test_009_guest_session_isolation_negative(self):
        """Verify guest session isolation: guest without matching session ID receives no data."""
        port, ws_port = self.start_server()

        async def run_test():
            uri_session = (
                f"ws://localhost:{ws_port}/spans/stream?client_session_id=session_123"
            )
            uri_no_session = f"ws://localhost:{ws_port}/spans/stream"

            async with websockets.connect(
                uri_session
            ) as ws_guest_session, websockets.connect(
                uri_no_session
            ) as ws_guest_no_session:

                await ws_guest_session.send(
                    json.dumps({"type": "auth", "client_session_id": "session_123"})
                )
                ack1 = json.loads(
                    await asyncio.wait_for(ws_guest_session.recv(), timeout=3.0)
                )
                self.assertEqual(ack1.get("type"), "auth.ok")

                await ws_guest_no_session.send(json.dumps({"type": "auth"}))
                ack2 = json.loads(
                    await asyncio.wait_for(ws_guest_no_session.recv(), timeout=3.0)
                )
                self.assertEqual(ack2.get("type"), "auth.ok")

                trigger_span(
                    port,
                    auth_token=None,
                    client_session_id="session_123",
                    model="guest-session-model",
                )

                msg1 = await asyncio.wait_for(ws_guest_session.recv(), timeout=3.0)
                span1 = json.loads(msg1)
                self.assertEqual(span1.get("name"), "chat.completions")

                with self.assertRaises(asyncio.TimeoutError):
                    await asyncio.wait_for(ws_guest_no_session.recv(), timeout=2.0)

        asyncio.run(run_test())

    def test_010_realtime_upgrade_requires_auth(self):
        """Verify connection upgrades fail immediately for /realtime and /logs/stream without auth."""
        port, ws_port = self.start_server({"LEMONADE_API_KEY": "user_key"})

        async def run_test():
            try:
                async with websockets.connect(f"ws://localhost:{ws_port}/realtime"):
                    self.fail("Expected realtime upgrade to fail without credentials")
            except (
                websockets.exceptions.InvalidStatus,
                websockets.exceptions.InvalidHandshake,
            ):
                pass

            try:
                async with websockets.connect(f"ws://localhost:{ws_port}/logs/stream"):
                    self.fail("Expected logs upgrade to fail without credentials")
            except (
                websockets.exceptions.InvalidStatus,
                websockets.exceptions.InvalidHandshake,
            ):
                pass

            async with websockets.connect(
                f"ws://localhost:{ws_port}/spans/stream"
            ) as ws:
                pass

        asyncio.run(run_test())

    def test_012_websocket_origin_security(self):
        """Verify WebSocket origin validation: configured non-local origin succeeds, mismatch fails."""
        port, ws_port = self.start_server(
            {
                "LEMONADE_ALLOWED_ORIGINS": "https://app.example.com",
                "LEMONADE_ADMIN_API_KEY": "admin_key",
            }
        )

        async def run_test():
            # 1. Configured non-local origin succeeds
            async with websockets.connect(
                f"ws://localhost:{ws_port}/spans/stream?api_key=admin_key",
                additional_headers={"Origin": "https://app.example.com"},
            ) as ws:
                pass

            # 2. Scheme mismatch fails
            try:
                async with websockets.connect(
                    f"ws://localhost:{ws_port}/spans/stream?api_key=admin_key",
                    additional_headers={"Origin": "http://app.example.com"},
                ):
                    self.fail("Expected WebSocket to fail due to scheme mismatch")
            except (
                websockets.exceptions.InvalidStatus,
                websockets.exceptions.InvalidHandshake,
            ):
                pass

            # 3. Port mismatch fails
            try:
                async with websockets.connect(
                    f"ws://localhost:{ws_port}/spans/stream?api_key=admin_key",
                    additional_headers={"Origin": "https://app.example.com:8443"},
                ):
                    self.fail("Expected WebSocket to fail due to port mismatch")
            except (
                websockets.exceptions.InvalidStatus,
                websockets.exceptions.InvalidHandshake,
            ):
                pass

            # 4. Unconfigured origin fails
            try:
                async with websockets.connect(
                    f"ws://localhost:{ws_port}/spans/stream?api_key=admin_key",
                    additional_headers={"Origin": "https://unconfigured.com"},
                ):
                    self.fail("Expected WebSocket to fail due to unconfigured origin")
            except (
                websockets.exceptions.InvalidStatus,
                websockets.exceptions.InvalidHandshake,
            ):
                pass

        asyncio.run(run_test())


if __name__ == "__main__":
    unittest.main()
