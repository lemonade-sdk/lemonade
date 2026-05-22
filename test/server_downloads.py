"""
Server-owned download registry tests for Lemonade Server.

Usage:
    python test/server_downloads.py
    python test/server_downloads.py --cli-binary /path/to/lemonade
"""

import time
import uuid

import requests

from utils.server_base import ServerTestBase, run_server_tests
from utils.test_models import PORT, TIMEOUT_DEFAULT


class ServerDownloadRegistryTests(ServerTestBase):
    """Tests the non-SSE model download registry path used by the desktop UI."""

    def _get_downloads(self):
        response = requests.get(
            f"http://localhost:{PORT}/api/v1/downloads",
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()
        data = response.json()
        self.assertIsInstance(data, list)
        return data

    def _control_download(self, download_id, action):
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/downloads/control",
            json={"id": download_id, "action": action},
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()
        return response.json()

    def _wait_for_status(self, download_id, statuses, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            downloads = self._get_downloads()
            match = next((item for item in downloads if item.get("id") == download_id), None)
            if match and match.get("status") in statuses and match.get("running") is not True:
                return match
            time.sleep(0.25)
        self.fail(f"Timed out waiting for {download_id} to reach one of {statuses}")

    def test_001_pull_subscribe_false_registers_download_and_control_can_remove(self):
        """POST /pull with subscribe=false registers a server-owned job visible via /downloads."""
        model_name = f"Definitely-Not-A-Real-Download-Test-Model-{uuid.uuid4().hex}"
        download_id = f"model:{model_name}"

        # Use an intentionally unknown model so the test exercises the registry
        # path without downloading a real model from Hugging Face. The initial
        # response should still be a JSON job snapshot, not an SSE stream.
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/pull",
            json={
                "model_name": model_name,
                "stream": True,
                "subscribe": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()

        snapshot = response.json()
        self.assertEqual(snapshot.get("id"), download_id)
        self.assertEqual(snapshot.get("type"), "model")
        self.assertEqual(snapshot.get("model_name"), model_name)
        self.assertIn(snapshot.get("status"), {"downloading", "error"})

        terminal = self._wait_for_status(download_id, {"error"})
        self.assertIn("error", terminal)

        removed = self._control_download(download_id, "remove")
        self.assertEqual(removed.get("status"), "ok")

        downloads_after_remove = self._get_downloads()
        self.assertFalse(
            any(item.get("id") == download_id for item in downloads_after_remove),
            "removed download job should not remain visible",
        )

    def test_002_download_control_validates_required_fields(self):
        """The control endpoint rejects malformed requests with a client error."""
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/downloads/control",
            json={"action": "remove"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400)
        self.assertIn("error", response.json())


if __name__ == "__main__":
    run_server_tests(ServerDownloadRegistryTests, "SERVER DOWNLOAD REGISTRY TESTS")
