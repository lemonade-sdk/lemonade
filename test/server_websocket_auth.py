#!/usr/bin/env python3
"""
Integration tests for WebSocket API-key authentication via the
Sec-WebSocket-Protocol header.

Clients that cannot set request headers (browsers) authenticate by offering the
registered application subprotocol ("lemonade-realtime") alongside a base64url
credential subprotocol ("bearer.<base64url(api_key)>"). These tests exercise
that path for both /realtime and /logs/stream, on the dedicated WebSocket port
and the main HTTP port, plus the backward-compatible api_key query parameter.
"""

import asyncio
import base64
import contextlib
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from utils.test_models import get_default_lemond_binary
from utils.server_fixture import (
    allocate_free_port,
    lemond_server,
    make_clean_env,
    wait_for_http_health,
)

API_KEY = "secret_key"
APP_PROTOCOL = "lemonade-realtime"


def credential_protocol(api_key):
    encoded = base64.urlsafe_b64encode(api_key.encode()).decode().rstrip("=")
    return f"bearer.{encoded}"


def auth_subprotocols(api_key):
    return [APP_PROTOCOL, credential_protocol(api_key)]


class WebSocketAuthTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lemond_bin = get_default_lemond_binary()
        if not os.path.exists(cls.lemond_bin):
            raise unittest.SkipTest(
                f"lemond binary not found at {cls.lemond_bin}; "
                "skipping (source-build only)"
            )
        cls.temp_dir = tempfile.mkdtemp(prefix="lemond_ws_auth_")
        cls.port = allocate_free_port()

        env = make_clean_env(cls.temp_dir)
        env["LEMONADE_API_KEY"] = API_KEY

        cls._exit_stack = contextlib.ExitStack()
        try:
            cls.log_path = os.path.join(cls.temp_dir, "lemond.log")
            cls.log_file = cls._exit_stack.enter_context(
                open(cls.log_path, "w", encoding="utf-8")
            )
            cls.proc = cls._exit_stack.enter_context(
                lemond_server(
                    port=cls.port,
                    cache_dir=cls.temp_dir,
                    binary_path=cls.lemond_bin,
                    env=env,
                    wait_health=True,
                    health_headers={"Authorization": f"Bearer {API_KEY}"},
                    stdout=cls.log_file,
                    stderr=subprocess.STDOUT,
                )
            )

            health = requests.get(
                f"http://localhost:{cls.port}/api/v1/health",
                headers={"Authorization": f"Bearer {API_KEY}"},
                timeout=2,
            )
            cls.ws_port = health.json().get("websocket_port")
            if cls.ws_port is None:
                raise RuntimeError("Failed to get websocket_port from health response")
        except Exception:
            cls._exit_stack.close()
            import shutil

            shutil.rmtree(cls.temp_dir, ignore_errors=True)
            raise

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "_exit_stack"):
            cls._exit_stack.close()
        temp_dir = getattr(cls, "temp_dir", None)
        if temp_dir:
            import shutil

            shutil.rmtree(temp_dir, ignore_errors=True)

    # --- realtime -----------------------------------------------------------

    def test_001_realtime_subprotocol_auth_ws_port(self):
        """Valid subprotocol credential upgrades /realtime on the WS port."""

        async def run():
            uri = f"ws://localhost:{self.ws_port}/realtime"
            async with websockets.connect(
                uri, subprotocols=auth_subprotocols(API_KEY)
            ) as ws:
                self.assertEqual(ws.subprotocol, APP_PROTOCOL)
                msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                self.assertEqual(msg.get("type"), "session.created")

        asyncio.run(run())

    def test_002_realtime_subprotocol_auth_main_port(self):
        """Valid subprotocol credential upgrades /v1/realtime on the main port."""

        async def run():
            uri = f"ws://localhost:{self.port}/v1/realtime"
            async with websockets.connect(
                uri, subprotocols=auth_subprotocols(API_KEY)
            ) as ws:
                self.assertEqual(ws.subprotocol, APP_PROTOCOL)
                msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                self.assertEqual(msg.get("type"), "session.created")

        asyncio.run(run())

    def test_003_realtime_wrong_key_rejected(self):
        """A subprotocol credential with the wrong key is rejected."""

        async def run():
            uri = f"ws://localhost:{self.ws_port}/realtime"
            with self.assertRaises(
                (
                    websockets.exceptions.InvalidStatus,
                    websockets.exceptions.InvalidHandshake,
                )
            ):
                async with websockets.connect(
                    uri, subprotocols=auth_subprotocols("wrong_key")
                ):
                    pass

        asyncio.run(run())

    def test_004_realtime_query_param_backward_compat(self):
        """The legacy api_key query parameter still authenticates /realtime."""

        async def run():
            uri = f"ws://localhost:{self.ws_port}/realtime?api_key={API_KEY}"
            async with websockets.connect(uri) as ws:
                msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                self.assertEqual(msg.get("type"), "session.created")

        asyncio.run(run())

    # --- logs ---------------------------------------------------------------

    def test_005_logs_subprotocol_auth_ws_port(self):
        """Valid subprotocol credential upgrades /logs/stream on the WS port."""

        async def run():
            uri = f"ws://localhost:{self.ws_port}/logs/stream"
            async with websockets.connect(
                uri, subprotocols=auth_subprotocols(API_KEY)
            ) as ws:
                self.assertEqual(ws.subprotocol, APP_PROTOCOL)
                await ws.send(json.dumps({"type": "logs.subscribe", "after_seq": None}))
                msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                self.assertEqual(msg.get("type"), "logs.snapshot")

        asyncio.run(run())

    def test_006_logs_subprotocol_auth_main_port(self):
        """Valid subprotocol credential upgrades /logs/stream on the main port."""

        async def run():
            uri = f"ws://localhost:{self.port}/logs/stream"
            async with websockets.connect(
                uri, subprotocols=auth_subprotocols(API_KEY)
            ) as ws:
                self.assertEqual(ws.subprotocol, APP_PROTOCOL)
                await ws.send(json.dumps({"type": "logs.subscribe", "after_seq": None}))
                msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                self.assertEqual(msg.get("type"), "logs.snapshot")

        asyncio.run(run())

    def test_007_logs_wrong_key_rejected(self):
        """A subprotocol credential with the wrong key is rejected for logs."""

        async def run():
            uri = f"ws://localhost:{self.ws_port}/logs/stream"
            with self.assertRaises(
                (
                    websockets.exceptions.InvalidStatus,
                    websockets.exceptions.InvalidHandshake,
                )
            ):
                async with websockets.connect(
                    uri, subprotocols=auth_subprotocols("wrong_key")
                ):
                    pass

        asyncio.run(run())

    def test_008_logs_missing_credentials_rejected(self):
        """No credentials at all is rejected for /logs/stream."""

        async def run():
            uri = f"ws://localhost:{self.ws_port}/logs/stream"
            with self.assertRaises(
                (
                    websockets.exceptions.InvalidStatus,
                    websockets.exceptions.InvalidHandshake,
                )
            ):
                async with websockets.connect(uri):
                    pass

        asyncio.run(run())

    # --- origin -------------------------------------------------------------

    def test_009_tauri_windows_origin_accepted(self):
        """The Windows Tauri WebView2 origin (http://tauri.localhost) upgrades."""

        async def run():
            uri = f"ws://localhost:{self.port}/logs/stream"
            async with websockets.connect(
                uri,
                subprotocols=auth_subprotocols(API_KEY),
                origin="http://tauri.localhost",
            ) as ws:
                await ws.send(json.dumps({"type": "logs.subscribe", "after_seq": None}))
                msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                self.assertEqual(msg.get("type"), "logs.snapshot")

        asyncio.run(run())

    def test_010_foreign_origin_rejected(self):
        """A cross-site browser origin is rejected even with valid credentials."""

        async def run():
            uri = f"ws://localhost:{self.port}/logs/stream"
            with self.assertRaises(
                (
                    websockets.exceptions.InvalidStatus,
                    websockets.exceptions.InvalidHandshake,
                )
            ):
                async with websockets.connect(
                    uri,
                    subprotocols=auth_subprotocols(API_KEY),
                    origin="http://evil.example.com",
                ):
                    pass

        asyncio.run(run())

    def test_011_desktop_app_origins_accepted(self):
        """Desktop app webview origins (file://, app://., jan://) upgrade and subscribe."""

        async def run():
            uri = f"ws://localhost:{self.port}/logs/stream"
            for test_origin in ["file://", "app://.", "jan://app"]:
                async with websockets.connect(
                    uri,
                    subprotocols=auth_subprotocols(API_KEY),
                    origin=test_origin,
                ) as ws:
                    await ws.send(
                        json.dumps({"type": "logs.subscribe", "after_seq": None})
                    )
                    msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                    self.assertEqual(msg.get("type"), "logs.snapshot")

            # Verify sandboxed iframe 'null' origin is rejected for security (CSWSH prevention)
            with self.assertRaises(
                (
                    websockets.exceptions.InvalidStatus,
                    websockets.exceptions.InvalidHandshake,
                )
            ):
                async with websockets.connect(
                    uri,
                    subprotocols=auth_subprotocols(API_KEY),
                    origin="null",
                ):
                    pass

        asyncio.run(run())

    def test_012_spans_anonymous_connection_gate(self):
        """Spans stream upgrades anonymously but rejects messages before authentication."""

        async def run():
            uri = f"ws://localhost:{self.port}/spans/stream"
            # Note: Do not pass any subprotocols or credentials
            async with websockets.connect(uri) as ws:
                # 1. Verify it upgraded successfully (anonymous is accepted)
                # Let's send a non-auth message
                await ws.send(json.dumps({"type": "logs.subscribe"}))

                # 2. Verify it is rejected
                msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                self.assertEqual(msg.get("type"), "error")
                self.assertIn(
                    "Unauthorized. Please authenticate first",
                    msg.get("error", {}).get("message", ""),
                )

                # 3. Now authenticate
                await ws.send(json.dumps({"type": "auth", "token": API_KEY}))
                auth_resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                self.assertEqual(auth_resp.get("type"), "auth.ok")

        asyncio.run(run())

    def test_013_admin_only_auth(self):
        """Under admin-only auth configuration (LEMONADE_API_KEY empty, LEMONADE_ADMIN_API_KEY set),
        verify that WebSocket connection retains admin token and can authenticate."""
        temp_dir = tempfile.mkdtemp(prefix="lemond_admin_only_")
        port = allocate_free_port()
        env = make_clean_env(temp_dir)
        env["LEMONADE_ADMIN_API_KEY"] = "admin_secret"

        try:
            with lemond_server(
                port=port,
                cache_dir=temp_dir,
                env=env,
                wait_health=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            ):
                health = requests.get(
                    f"http://localhost:{port}/api/v1/health", timeout=0.5
                )
                ws_port = health.json().get("websocket_port")
                self.assertIsNotNone(
                    ws_port, "Failed to get websocket_port for admin-only test"
                )

                # Now let's connect to /spans/stream with the admin subprotocol
                async def run():
                    uri = f"ws://localhost:{port}/v1/spans/stream"
                    async with websockets.connect(
                        uri,
                        subprotocols=auth_subprotocols("admin_secret"),
                    ) as ws:
                        # Connection should upgrade successfully
                        self.assertEqual(ws.subprotocol, APP_PROTOCOL)

                        # Now trigger a span from a public request without an Authorization token
                        def trigger():
                            payload = {
                                "model": "nonexistent-model",
                                "messages": [{"role": "user", "content": "hello"}],
                            }
                            requests.post(
                                f"http://localhost:{port}/api/v1/chat/completions",
                                json=payload,
                                timeout=5,
                            )

                        loop = asyncio.get_running_loop()
                        await loop.run_in_executor(None, trigger)

                        # Assert that the admin WebSocket receives the span
                        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                        self.assertIn("name", msg)
                        self.assertEqual(msg.get("name"), "chat.completions")

                asyncio.run(run())
        finally:
            import shutil

            shutil.rmtree(temp_dir, ignore_errors=True)

    def test_014_null_origin_explicit_allowlist(self):
        """Verify that Origin: null is accepted when allowed origins config contains 'null'."""
        temp_dir = tempfile.mkdtemp(prefix="lemond_null_origin_")
        port = allocate_free_port()
        env = make_clean_env(temp_dir)
        env["LEMONADE_API_KEY"] = API_KEY
        env["LEMONADE_ALLOWED_ORIGINS"] = "null"

        try:
            with lemond_server(
                port=port,
                cache_dir=temp_dir,
                env=env,
                wait_health=True,
                health_headers={"Authorization": f"Bearer {API_KEY}"},
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            ):
                health = requests.get(
                    f"http://localhost:{port}/api/v1/health",
                    headers={"Authorization": f"Bearer {API_KEY}"},
                    timeout=0.5,
                )
                ws_port = health.json().get("websocket_port")
                self.assertIsNotNone(
                    ws_port, "Failed to get websocket_port for null origin test"
                )

                # Connect with Origin: null
                async def run():
                    uri = f"ws://localhost:{ws_port}/logs/stream"
                    async with websockets.connect(
                        uri,
                        subprotocols=auth_subprotocols(API_KEY),
                        origin="null",
                    ) as ws:
                        await ws.send(json.dumps({"type": "logs.subscribe"}))
                        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3.0))
                        self.assertEqual(msg.get("type"), "logs.snapshot")

                asyncio.run(run())
        finally:
            import shutil

            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
