"""
Log streaming websocket tests for Lemonade Server.

Usage:
    python server_logs_websocket.py
"""

import asyncio
import json
import os

import requests
import websockets

from utils.server_base import ServerTestBase, run_server_tests
from utils.test_models import PORT, TIMEOUT_DEFAULT


class LogWebSocketTests(ServerTestBase):
    """Tests for websocket-based log streaming."""

    @staticmethod
    def _auth_headers():
        api_key = os.environ.get("LEMONADE_API_KEY")
        if api_key:
            return {"Authorization": f"Bearer {api_key}"}
        return {}

    def _get_ws_url(self):
        response = requests.get(
            f"{self.base_url}/health",
            headers=self._auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        ws_port = data.get("websocket_port")
        self.assertIsNotNone(ws_port, "Health endpoint must advertise websocket_port")
        return f"ws://localhost:{ws_port}/logs/stream"

    async def _subscribe(self, websocket, after_seq=None):
        await websocket.send(
            json.dumps({"type": "logs.subscribe", "after_seq": after_seq})
        )

    async def _recv_json(self, websocket):
        raw = await asyncio.wait_for(websocket.recv(), timeout=10)
        return json.loads(raw)

    def test_000_health_advertises_log_streaming(self):
        """Health endpoint advertises websocket log streaming."""
        response = requests.get(
            f"{self.base_url}/health",
            headers=self._auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertTrue(data["log_streaming"]["websocket"])
        self.assertIn("path", data["log_streaming"])
        self.assertIn("websocket_port", data)
        print(
            f"[OK] Health advertises websocket log streaming on port {data['websocket_port']}"
        )

    def test_001_backlog_and_live_logs(self):
        """A new websocket gets backlog first, then live log entries."""
        asyncio.run(self._test_001_backlog_and_live_logs())

    async def _test_001_backlog_and_live_logs(self):
        ws_url = self._get_ws_url()

        # Trigger a log line so there's something in the backlog
        requests.post(
            f"http://localhost:{PORT}/api/v1/test",
            json={},
            headers=self._auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )

        async with websockets.connect(ws_url) as websocket:
            await self._subscribe(websocket)
            snapshot = await self._recv_json(websocket)
            self.assertEqual(snapshot["type"], "logs.snapshot")

            snapshot_lines = [entry["line"] for entry in snapshot.get("entries", [])]
            self.assertTrue(
                any("TEST POST endpoint hit!" in line for line in snapshot_lines),
                "Expected backlog snapshot to include the previously emitted test log",
            )

            last_seq = None
            if snapshot.get("entries"):
                last_seq = snapshot["entries"][-1]["seq"]

        # Reconnect with after_seq to get only new entries
        async with websockets.connect(ws_url) as websocket:
            await self._subscribe(websocket, after_seq=last_seq)
            snapshot = await self._recv_json(websocket)
            self.assertEqual(snapshot["type"], "logs.snapshot")
            self.assertEqual(snapshot.get("entries", []), [])

            # Trigger another log and verify live delivery
            requests.post(
                f"http://localhost:{PORT}/api/v1/test",
                json={},
                headers=self._auth_headers(),
                timeout=TIMEOUT_DEFAULT,
            )

            for _ in range(5):
                message = await self._recv_json(websocket)
                if message["type"] == "logs.entry":
                    self.assertIn("TEST POST endpoint hit!", message["entry"]["line"])
                    break
            else:
                self.fail("Did not receive live log entry for POST /api/v1/test")

        print("[OK] Backlog snapshot and live log entries work correctly")


if __name__ == "__main__":
    run_server_tests(LogWebSocketTests, description="LOG WEBSOCKET TESTS")
