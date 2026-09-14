#!/usr/bin/env python3
"""Unit tests for the large-request memory benchmark client."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

MODULE_PATH = (
    Path(__file__).resolve().parents[1] / "tools" / "benchmark_large_request_memory.py"
)
MODULE_SPEC = importlib.util.spec_from_file_location(
    "benchmark_large_request_memory", MODULE_PATH
)
if MODULE_SPEC is None or MODULE_SPEC.loader is None:
    raise RuntimeError(f"could not load benchmark module from {MODULE_PATH}")
benchmark = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = benchmark
MODULE_SPEC.loader.exec_module(benchmark)


class FakeResponse:
    def read(self) -> bytes:
        return b"{}"


class RecordingConnection:
    requests: list[tuple[str, str, bytes | None, dict[str, str]]] = []

    def __init__(self, host: str, port: int, timeout: float):
        self.host = host
        self.port = port
        self.timeout = timeout

    def request(
        self,
        method: str,
        path: str,
        body: bytes | None,
        headers: dict[str, str],
    ) -> None:
        self.requests.append((method, path, body, headers.copy()))

    def getresponse(self) -> FakeResponse:
        response = FakeResponse()
        response.status = 200
        return response

    def close(self) -> None:
        pass


class BenchmarkAuthenticationTests(unittest.TestCase):
    def setUp(self) -> None:
        RecordingConnection.requests.clear()

    def request_headers(self, body: bytes | None = None) -> dict[str, str]:
        with mock.patch.object(
            benchmark.http.client, "HTTPConnection", RecordingConnection
        ):
            benchmark.request_json(
                "http://127.0.0.1:8000", "POST", "/api/v1/test", body=body
            )
        return RecordingConnection.requests[-1][3]

    def test_request_without_credentials_has_no_authorization_header(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=True):
            headers = self.request_headers(body=b"{}")

        self.assertEqual(headers, {"Content-Type": "application/json"})

    def test_regular_api_key_is_forwarded(self) -> None:
        with mock.patch.dict(
            os.environ, {"LEMONADE_API_KEY": "regular-secret"}, clear=True
        ):
            headers = self.request_headers()

        self.assertEqual(headers["Authorization"], "Bearer regular-secret")

    def test_admin_api_key_takes_precedence(self) -> None:
        with mock.patch.dict(
            os.environ,
            {
                "LEMONADE_API_KEY": "regular-secret",
                "LEMONADE_ADMIN_API_KEY": "admin-secret",
            },
            clear=True,
        ):
            headers = self.request_headers(body=b"{}")

        self.assertEqual(headers["Authorization"], "Bearer admin-secret")
        self.assertEqual(headers["Content-Type"], "application/json")


if __name__ == "__main__":
    unittest.main()
