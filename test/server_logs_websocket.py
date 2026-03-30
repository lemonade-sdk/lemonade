"""
Log streaming websocket tests for Lemonade Server.

Usage:
    python server_logs_websocket.py
"""

import asyncio
import os
import json

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

    def _create_ticket(self, after_seq=None, headers=None):
        response = requests.post(
            f"{self.base_url}/logs/stream/ticket",
            json={"after_seq": after_seq},
            headers=headers or self._auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        return response

    async def _recv_json(self, websocket):
        raw = await asyncio.wait_for(websocket.recv(), timeout=10)
        return json.loads(raw)

    def test_000_ticket_endpoint(self):
        """Ticket creation returns a websocket URL and port."""
        response = self._create_ticket()
        self.assertEqual(response.status_code, 200)
        payload = response.json()
        self.assertIn("ticket", payload)
        self.assertIn("ws_url", payload)
        self.assertIn("websocket_port", payload)
        print(f"[OK] Ticket endpoint returned {payload['ws_url']}")

    def test_001_ticket_auth_behavior(self):
        """Ticket endpoint follows normal API auth rules."""
        api_key = os.environ.get("LEMONADE_API_KEY")
        if api_key:
            response = self._create_ticket(headers={})
            self.assertEqual(response.status_code, 401)
            response = self._create_ticket(
                headers={"Authorization": f"Bearer {api_key}"}
            )
            self.assertEqual(response.status_code, 200)
        else:
            response = self._create_ticket()
            self.assertEqual(response.status_code, 200)
        print("[OK] Ticket endpoint auth behavior is correct")

    def test_002_backlog_and_live_logs(self):
        """A new websocket gets backlog first, then live log entries."""
        asyncio.run(self._test_002_backlog_and_live_logs())

    async def _test_002_backlog_and_live_logs(self):
        trigger_response = requests.post(
            f"http://localhost:{PORT}/api/v1/test",
            json={},
            headers=self._auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(trigger_response.status_code, 200)

        ticket_response = self._create_ticket()
        self.assertEqual(ticket_response.status_code, 200)
        ticket = ticket_response.json()

        async with websockets.connect(ticket["ws_url"]) as websocket:
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

        follow_response = self._create_ticket(after_seq=last_seq)
        self.assertEqual(follow_response.status_code, 200)
        follow_ticket = follow_response.json()

        async with websockets.connect(follow_ticket["ws_url"]) as websocket:
            snapshot = await self._recv_json(websocket)
            self.assertEqual(snapshot["type"], "logs.snapshot")
            self.assertEqual(snapshot.get("entries", []), [])

            trigger_response = requests.post(
                f"http://localhost:{PORT}/api/v1/test",
                json={},
                headers=self._auth_headers(),
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(trigger_response.status_code, 200)

            for _ in range(5):
                message = await self._recv_json(websocket)
                if message["type"] == "logs.entry":
                    self.assertIn("TEST POST endpoint hit!", message["entry"]["line"])
                    break
            else:
                self.fail("Did not receive live log entry for POST /api/v1/test")


if __name__ == "__main__":
    run_server_tests(LogWebSocketTests, description="LOG WEBSOCKET TESTS")
