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

API_KEY = "secret_key"
APP_PROTOCOL = "lemonade-realtime"


def find_free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


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
        cls.port = find_free_port()

        env = os.environ.copy()
        env.pop("LEMONADE_ADMIN_API_KEY", None)
        env["LEMONADE_API_KEY"] = API_KEY

        cls.log_path = os.path.join(cls.temp_dir, "lemond.log")
        cls.log_file = open(cls.log_path, "w")
        cls.proc = subprocess.Popen(
            [cls.lemond_bin, cls.temp_dir, "--port", str(cls.port)],
            stdout=cls.log_file,
            stderr=cls.log_file,
            env=env,
        )

        cls.ws_port = None
        for _ in range(60):
            try:
                res = requests.get(f"http://localhost:{cls.port}/live", timeout=0.2)
                if res.status_code == 200:
                    health = requests.get(
                        f"http://localhost:{cls.port}/api/v1/health",
                        headers={"Authorization": f"Bearer {API_KEY}"},
                        timeout=0.5,
                    )
                    cls.ws_port = health.json().get("websocket_port")
                    break
            except Exception:
                pass
            time.sleep(0.05)

        if cls.ws_port is None:
            cls.tearDownClass()
            raise RuntimeError("Failed to start lemond server for WebSocket auth tests")

    @classmethod
    def tearDownClass(cls):
        proc = getattr(cls, "proc", None)
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
                proc.wait()
        log_file = getattr(cls, "log_file", None)
        if log_file:
            log_file.close()
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


if __name__ == "__main__":
    unittest.main()
