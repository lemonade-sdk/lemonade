"""
Request log endpoint tests.

Integration tests that require PostgreSQL are skipped unless
LEMONADE_REQUEST_LOG_DATABASE_URL is set in the environment of the running server.
"""

import os
import time
import unittest

import requests

from utils.server_base import ServerTestBase, _auth_headers
from utils.test_models import PORT, TIMEOUT_DEFAULT


class RequestLogTests(ServerTestBase):
    """Tests for /api/v1/request-log/* review endpoints."""

    def test_000_request_log_endpoints_registered(self):
        """Verify request-log endpoints are registered on v0 and v1."""
        for endpoint in ("request-log/recent", "request-log/search", "request-log/stats"):
            for version in ("v0", "v1"):
                url = f"http://localhost:{PORT}/api/{version}/{endpoint}"
                response = requests.head(url, timeout=TIMEOUT_DEFAULT)
                self.assertNotEqual(
                    response.status_code,
                    404,
                    f"Endpoint {endpoint} is not registered on {version}",
                )

    def test_001_request_log_auth_when_api_key_configured(self):
        """When LEMONADE_API_KEY is set, review endpoints require auth."""
        api_key = os.environ.get("LEMONADE_API_KEY")
        if not api_key:
            self.skipTest("LEMONADE_API_KEY is not set on the test runner")

        url = f"{self.base_url}/request-log/recent"
        unauth = requests.get(url, timeout=TIMEOUT_DEFAULT)
        if unauth.status_code == 503:
            self.skipTest("Request logging is not enabled on the running server")

        self.assertEqual(unauth.status_code, 401)

        authed = requests.get(url, headers=_auth_headers(), timeout=TIMEOUT_DEFAULT)
        self.assertIn(authed.status_code, (200, 503))
        if authed.status_code == 200:
            payload = authed.json()
            self.assertIn("entries", payload)
            self.assertIsInstance(payload["entries"], list)

    def test_002_request_log_search_keep_alive_integration(self):
        """When DB logging is active, keep_alive requests appear in search results."""
        if not os.environ.get("LEMONADE_REQUEST_LOG_DATABASE_URL"):
            self.skipTest("LEMONADE_REQUEST_LOG_DATABASE_URL is not configured")

        marker_model = "request-log-test-model"
        chat_url = f"http://localhost:{PORT}/api/chat"
        payload = {
            "model": marker_model,
            "messages": [],
            "keep_alive": 0,
        }
        response = requests.post(
            chat_url,
            json=payload,
            headers=_auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertIn(response.status_code, (200, 400, 404, 422, 500))

        search_url = f"{self.base_url}/request-log/search"
        params = {"keep_alive": "0", "limit": 20}
        deadline = time.time() + 15
        found = False
        while time.time() < deadline:
            search = requests.get(
                search_url,
                params=params,
                headers=_auth_headers(),
                timeout=TIMEOUT_DEFAULT,
            )
            if search.status_code == 503:
                self.skipTest("Request logging is not enabled on the running server")
            self.assertEqual(search.status_code, 200)
            entries = search.json().get("entries", [])
            for entry in entries:
                if entry.get("path") == "/api/chat" and entry.get("keep_alive") == "0":
                    found = True
                    redacted = entry.get("redacted_body")
                    if redacted is not None:
                        dumped = str(redacted)
                        self.assertNotIn("Bearer", dumped)
                        self.assertNotIn("secret-value", dumped)
                    break
            if found:
                break
            time.sleep(1)

        self.assertTrue(found, "Expected /api/chat keep_alive=0 request in search results")

    def test_003_request_log_stats(self):
        """Stats endpoint returns aggregate JSON when logging is enabled."""
        url = f"{self.base_url}/request-log/stats"
        response = requests.get(
            url,
            params={"since": "1h"},
            headers=_auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        if response.status_code == 503:
            self.skipTest("Request logging is not enabled on the running server")

        self.assertEqual(response.status_code, 200)
        payload = response.json()
        self.assertIn("total_requests", payload)
        self.assertIn("by_endpoint_type", payload)
        self.assertIn("by_model", payload)


if __name__ == "__main__":
    from utils.server_base import run_server_tests

    run_server_tests(RequestLogTests, "REQUEST LOG TESTS")
