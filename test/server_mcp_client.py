#!/usr/bin/env python3
"""End-to-end tests for Lemonade's external MCP client administration API.

The suite starts its own isolated lemond instance with an admin key and an
in-process loopback MCP Streamable HTTP server. No external network access,
model download, or local MCP process launch is required.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import requests

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from utils.test_models import get_default_lemond_binary

ADMIN_KEY = "mcp-admin-test-key"
BEARER_ENV = "LEMONADE_MCP_TEST_BEARER"
BEARER_SECRET = "mcp-bearer-secret-value"
REQUEST_TIMEOUT = 8


def find_free_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])
    finally:
        sock.close()


class MockMcpState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.session_counter = 0
        self.tools_list_count: dict[str, int] = {}
        self.authorizations: list[str] = []
        self.ping_replies: list[dict[str, Any]] = []
        self.deleted_sessions: list[str] = []

    def new_session(self) -> str:
        with self.lock:
            self.session_counter += 1
            return f"mock-session-{self.session_counter}"

    def record_authorization(self, value: str) -> None:
        with self.lock:
            self.authorizations.append(value)

    def increment_tools_list(self, session_id: str) -> int:
        with self.lock:
            count = self.tools_list_count.get(session_id, 0) + 1
            self.tools_list_count[session_id] = count
            return count

    def record_ping_reply(self, message: dict[str, Any]) -> None:
        with self.lock:
            self.ping_replies.append(message)

    def record_delete(self, session_id: str) -> None:
        with self.lock:
            self.deleted_sessions.append(session_id)

    def delete_count(self) -> int:
        with self.lock:
            return len(self.deleted_sessions)

    def saw_bearer_secret(self) -> bool:
        expected = f"Bearer {BEARER_SECRET}"
        with self.lock:
            return expected in self.authorizations

    def saw_successful_ping_reply(self) -> bool:
        with self.lock:
            return any(
                item.get("id") == "server-ping-1"
                and isinstance(item.get("result"), dict)
                and not item["result"]
                for item in self.ping_replies
            )


class MockMcpHandler(BaseHTTPRequestHandler):
    server_version = "LemonadeMockMCP/1.0"

    @property
    def state(self) -> MockMcpState:
        return self.server.state  # type: ignore[attr-defined]

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def _read_json(self) -> dict[str, Any]:
        raw_length = self.headers.get("Content-Length", "0")
        length = int(raw_length) if raw_length.isdigit() else 0
        raw = self.rfile.read(length) if length else b"{}"
        value = json.loads(raw.decode("utf-8"))
        if not isinstance(value, dict):
            raise ValueError("MCP request body must be a JSON object")
        return value

    def _write_empty(self, status: int) -> None:
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _write_json(
        self,
        status: int,
        payload: dict[str, Any],
        *,
        session_id: str | None = None,
    ) -> None:
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        if session_id:
            self.send_header("Mcp-Session-Id", session_id)
        self.end_headers()
        self.wfile.write(data)

    def _write_sse(
        self,
        messages: list[dict[str, Any]],
        *,
        session_id: str | None = None,
    ) -> None:
        body = "".join(
            f"data: {json.dumps(message, separators=(',', ':'))}\n\n"
            for message in messages
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(body)))
        if session_id:
            self.send_header("Mcp-Session-Id", session_id)
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802 - stdlib handler API
        path = self.path.split("?", 1)[0]
        if path not in {"/mcp-json", "/mcp-sse", "/mcp-ping", "/mcp-expire"}:
            self._write_empty(404)
            return

        try:
            message = self._read_json()
        except Exception as exc:
            self._write_json(
                400,
                {
                    "jsonrpc": "2.0",
                    "id": None,
                    "error": {"code": -32700, "message": str(exc)},
                },
            )
            return

        self.state.record_authorization(self.headers.get("Authorization", ""))

        # Client replies to server-initiated requests are ordinary JSON-RPC
        # responses sent as a second POST on the same MCP endpoint.
        if (
            "method" not in message
            and "id" in message
            and ("result" in message or "error" in message)
        ):
            if str(message.get("id", "")).startswith("server-ping"):
                self.state.record_ping_reply(message)
            self._write_empty(202)
            return

        method = message.get("method")
        request_id = message.get("id")

        if method == "initialize":
            session_id = self.state.new_session()
            response = {
                "jsonrpc": "2.0",
                "id": request_id,
                "result": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {"tools": {"listChanged": True}},
                    "serverInfo": {"name": "lemonade-test-mcp", "version": "1.0"},
                },
            }
            if path == "/mcp-sse":
                self._write_sse([response], session_id=session_id)
            else:
                self._write_json(200, response, session_id=session_id)
            return

        if method == "notifications/initialized":
            self._write_empty(202)
            return

        session_id = self.headers.get("Mcp-Session-Id", "")
        if not session_id:
            self._write_empty(400)
            return

        if method == "tools/list":
            list_count = self.state.increment_tools_list(session_id)
            if path == "/mcp-expire" and list_count == 2:
                # The first tools/list is part of connect. The second is a
                # refresh request and simulates an expired server session.
                self._write_empty(404)
                return

            response = {
                "jsonrpc": "2.0",
                "id": request_id,
                "result": {
                    "tools": [
                        {
                            "name": "echo",
                            "description": "Echo a message for integration tests",
                            "inputSchema": {
                                "type": "object",
                                "properties": {"message": {"type": "string"}},
                            },
                        }
                    ]
                },
            }
            if path == "/mcp-ping":
                self._write_sse(
                    [
                        {
                            "jsonrpc": "2.0",
                            "id": "server-ping-1",
                            "method": "ping",
                        },
                        response,
                    ]
                )
            elif path == "/mcp-sse":
                self._write_sse([response])
            else:
                self._write_json(200, response)
            return

        if method == "tools/call":
            params = message.get("params")
            if not isinstance(params, dict):
                params = {}
            arguments = params.get("arguments")
            if not isinstance(arguments, dict):
                arguments = {}
            text = str(arguments.get("message", ""))
            self._write_json(
                200,
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "result": {
                        "content": [{"type": "text", "text": text}],
                        "structuredContent": {"echo": text},
                    },
                },
            )
            return

        self._write_json(
            200,
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "error": {"code": -32601, "message": "Method not found"},
            },
        )

    def do_DELETE(self) -> None:  # noqa: N802 - stdlib handler API
        path = self.path.split("?", 1)[0]
        if path not in {"/mcp-json", "/mcp-sse", "/mcp-ping", "/mcp-expire"}:
            self._write_empty(404)
            return
        self.state.record_delete(self.headers.get("Mcp-Session-Id", ""))
        self._write_empty(204)


class MockMcpServer:
    def __init__(self) -> None:
        self.state = MockMcpState()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), MockMcpHandler)
        self.server.daemon_threads = True
        self.server.state = self.state  # type: ignore[attr-defined]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self) -> int:
        return int(self.server.server_address[1])

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)


class McpClientEndpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls.lemond_bin = get_default_lemond_binary()
        if not cls.lemond_bin or not os.path.exists(cls.lemond_bin):
            raise unittest.SkipTest(
                f"lemond binary not found at {cls.lemond_bin!r}; "
                "source/install build required"
            )

        cls.mock = MockMcpServer()
        cls.mock.start()
        cls.temp_dir = tempfile.mkdtemp(prefix="lemond_mcp_http_")
        cls.port = find_free_port()
        cls.log_path = os.path.join(cls.temp_dir, "lemond.log")
        cls.log_file = open(cls.log_path, "w", encoding="utf-8")

        env = os.environ.copy()
        env.pop("LEMONADE_API_KEY", None)
        env["LEMONADE_ADMIN_API_KEY"] = ADMIN_KEY
        env[BEARER_ENV] = BEARER_SECRET
        env["LEMONADE_DISABLE_SYSTEMD_JOURNAL"] = "1"

        cls.proc = subprocess.Popen(
            [cls.lemond_bin, cls.temp_dir, "--port", str(cls.port)],
            stdout=cls.log_file,
            stderr=cls.log_file,
            env=env,
        )
        cls.root_url = f"http://127.0.0.1:{cls.port}"
        cls.mcp_url = f"{cls.root_url}/internal/mcp"
        cls.admin_headers = {"Authorization": f"Bearer {ADMIN_KEY}"}

        try:
            cls._wait_for_server()
        except Exception:
            cls._stop_lemond()
            cls.mock.stop()
            cls._close_log()
            shutil.rmtree(cls.temp_dir, ignore_errors=True)
            raise

    @classmethod
    def tearDownClass(cls) -> None:
        cls._stop_lemond()
        mock = getattr(cls, "mock", None)
        if mock:
            mock.stop()
        cls._close_log()
        temp_dir = getattr(cls, "temp_dir", None)
        if temp_dir:
            shutil.rmtree(temp_dir, ignore_errors=True)
        super().tearDownClass()

    @classmethod
    def _close_log(cls) -> None:
        log_file = getattr(cls, "log_file", None)
        if log_file and not log_file.closed:
            log_file.close()

    @classmethod
    def _stop_lemond(cls) -> None:
        proc = getattr(cls, "proc", None)
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

    @classmethod
    def _log_tail(cls) -> str:
        log_file = getattr(cls, "log_file", None)
        if log_file and not log_file.closed:
            log_file.flush()
        path = Path(getattr(cls, "log_path", ""))
        if not path.exists():
            return "<no lemond log>"
        text = path.read_text(encoding="utf-8", errors="replace")
        return text[-5000:]

    @classmethod
    def _wait_for_server(cls) -> None:
        deadline = time.monotonic() + 15
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if cls.proc.poll() is not None:
                raise RuntimeError(
                    f"lemond exited with code {cls.proc.returncode}\n{cls._log_tail()}"
                )
            try:
                response = requests.get(f"{cls.root_url}/live", timeout=0.5)
                if response.status_code == 200:
                    return
            except requests.RequestException as exc:
                last_error = exc
            time.sleep(0.05)
        raise RuntimeError(
            f"lemond did not become ready: {last_error}\n{cls._log_tail()}"
        )

    def setUp(self) -> None:
        self._delete_all_servers_best_effort()

    def tearDown(self) -> None:
        self._delete_all_servers_best_effort()

    def _request(
        self,
        method: str,
        path: str,
        *,
        payload: dict[str, Any] | None = None,
        auth: bool = True,
        expected: int | tuple[int, ...] = 200,
    ) -> requests.Response:
        headers = self.admin_headers if auth else {}
        response = requests.request(
            method,
            f"{self.mcp_url}{path}",
            headers=headers,
            json=payload,
            timeout=REQUEST_TIMEOUT,
        )
        expected_codes = (expected,) if isinstance(expected, int) else expected
        self.assertIn(
            response.status_code,
            expected_codes,
            f"{method} {path}: expected {expected_codes}, got {response.status_code}; "
            f"body={response.text[:2000]!r}\nlemond log:\n{self._log_tail()}",
        )
        return response

    def _list_servers(self) -> list[dict[str, Any]]:
        payload = self._request("GET", "/servers").json()
        servers = payload.get("servers", [])
        self.assertIsInstance(servers, list)
        return servers

    def _delete_all_servers_best_effort(self) -> None:
        try:
            response = requests.get(
                f"{self.mcp_url}/servers",
                headers=self.admin_headers,
                timeout=2,
            )
            if response.status_code != 200:
                return
            servers = response.json().get("servers", [])
            for item in servers:
                server_id = item.get("id") if isinstance(item, dict) else None
                if server_id:
                    requests.delete(
                        f"{self.mcp_url}/servers/{server_id}",
                        headers=self.admin_headers,
                        timeout=2,
                    )
        except Exception:
            pass

    def _config(
        self,
        server_id: str,
        path: str = "/mcp-json",
        *,
        bearer: bool = False,
    ) -> dict[str, Any]:
        config: dict[str, Any] = {
            "id": server_id,
            "name": f"Test {server_id}",
            "transport": "streamable-http",
            "url": f"http://127.0.0.1:{self.mock.port}{path}",
            "timeout_ms": 5000,
            "enabled": True,
        }
        if bearer:
            config["bearer_token"] = f"${{{BEARER_ENV}}}"
        return config

    def test_001_admin_gate_is_fail_closed(self) -> None:
        public_health = requests.get(
            f"{self.root_url}/api/v1/health", timeout=REQUEST_TIMEOUT
        )
        self.assertEqual(public_health.status_code, 200)

        unauthorized = self._request("GET", "/servers", auth=False, expected=(401, 403))
        self.assertNotEqual(unauthorized.status_code, 200)

        authorized = self._request("GET", "/servers")
        self.assertEqual(authorized.status_code, 200)

        keyless_dir = tempfile.mkdtemp(prefix="lemond_mcp_keyless_")
        keyless_port = find_free_port()
        keyless_env = os.environ.copy()
        keyless_env.pop("LEMONADE_API_KEY", None)
        keyless_env.pop("LEMONADE_ADMIN_API_KEY", None)
        keyless_proc = subprocess.Popen(
            [self.lemond_bin, keyless_dir, "--port", str(keyless_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=keyless_env,
        )
        try:
            keyless_root = f"http://127.0.0.1:{keyless_port}"
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                try:
                    if (
                        requests.get(f"{keyless_root}/live", timeout=0.5).status_code
                        == 200
                    ):
                        break
                except requests.RequestException:
                    pass
                time.sleep(0.05)
            else:
                self.fail("keyless lemond did not become ready")

            health = requests.get(
                f"{keyless_root}/api/v1/health", timeout=REQUEST_TIMEOUT
            )
            self.assertEqual(health.status_code, 200)
            for method in ("GET", "OPTIONS"):
                response = requests.request(
                    method,
                    f"{keyless_root}/internal/mcp/servers",
                    timeout=REQUEST_TIMEOUT,
                )
                self.assertEqual(response.status_code, 403)
        finally:
            if keyless_proc.poll() is None:
                keyless_proc.terminate()
                try:
                    keyless_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    keyless_proc.kill()
                    keyless_proc.wait(timeout=5)
            shutil.rmtree(keyless_dir, ignore_errors=True)

    def test_010_http_config_validation_and_secret_persistence(self) -> None:
        created = self._request(
            "POST",
            "/servers",
            payload={"server": self._config("valid-http", bearer=True)},
        ).json()
        self.assertIn("server", created)

        persisted_path = Path(self.temp_dir) / "mcp_servers.json"
        persisted = persisted_path.read_text(encoding="utf-8")
        self.assertNotIn(BEARER_SECRET, persisted)
        self.assertIn(f"${{{BEARER_ENV}}}", persisted)

        bad_configs = [
            {
                "id": "bad-port",
                "transport": "streamable-http",
                "url": "http://127.0.0.1:70000/mcp",
            },
            {
                "id": "plain-remote",
                "transport": "streamable-http",
                "url": "http://example.invalid/mcp",
            },
            {
                "id": "raw-bearer",
                "transport": "streamable-http",
                "url": f"http://127.0.0.1:{self.mock.port}/mcp-json",
                "bearer_token": "plaintext-secret",
            },
            {
                "id": "http-with-command",
                "transport": "streamable-http",
                "url": f"http://127.0.0.1:{self.mock.port}/mcp-json",
                "command": "python",
            },
            {
                "id": "stdio-with-url",
                "transport": "stdio",
                "command": "python",
                "url": f"http://127.0.0.1:{self.mock.port}/mcp-json",
            },
        ]
        for config in bad_configs:
            with self.subTest(server_id=config["id"]):
                self._request(
                    "POST", "/servers", payload={"server": config}, expected=400
                )

        insecure_opt_in = {
            "id": "plain-remote-opt-in",
            "transport": "streamable-http",
            "url": "http://example.invalid/mcp",
            "allow_insecure_http": True,
        }
        response = self._request(
            "POST", "/servers", payload={"server": insecure_opt_in}
        )
        self.assertEqual(
            response.json().get("server", {}).get("allow_insecure_http"), True
        )

    def test_020_test_endpoint_is_non_persistent_and_supports_json_and_sse(
        self,
    ) -> None:
        before = {item.get("id") for item in self._list_servers()}

        for suffix, path in (("json", "/mcp-json"), ("sse", "/mcp-sse")):
            with self.subTest(transport_response=suffix):
                response = self._request(
                    "POST",
                    "/servers/test",
                    payload={
                        "server": self._config(
                            f"probe-{suffix}", path, bearer=(suffix == "json")
                        )
                    },
                )
                self.assertIsInstance(response.json(), dict)

        after = {item.get("id") for item in self._list_servers()}
        self.assertEqual(before, after)
        self.assertTrue(
            self.mock.state.saw_bearer_secret(),
            "mock MCP server never received the resolved bearer credential",
        )

    def test_030_all_lifecycle_endpoints_and_tool_call(self) -> None:
        server_id = "lifecycle"
        self._request(
            "POST",
            "/servers",
            payload={"server": self._config(server_id, bearer=True)},
        )
        self._request("POST", f"/servers/{server_id}/connect")

        listed = self._list_servers()
        lifecycle = next(item for item in listed if item.get("id") == server_id)
        self.assertTrue(lifecycle.get("connected"))

        tools_payload = self._request("GET", "/tools").json()
        tools = tools_payload.get("tools", [])
        self.assertTrue(
            any(
                isinstance(tool, dict)
                and tool.get("server_id") == server_id
                and tool.get("name") == "echo"
                for tool in tools
            )
        )

        call = self._request(
            "POST",
            f"/servers/{server_id}/tools/call",
            payload={"name": "echo", "arguments": {"message": "hello-mcp"}},
        ).json()
        self.assertEqual(
            call.get("result", {}).get("content", [{}])[0].get("text"),
            "hello-mcp",
        )

        refreshed = self._request("POST", f"/servers/{server_id}/refresh-tools").json()
        self.assertIn("server", refreshed)

        deletes_before = self.mock.state.delete_count()
        self._request("POST", f"/servers/{server_id}/disconnect")
        self.assertGreater(
            self.mock.state.delete_count(),
            deletes_before,
            "disconnect did not issue best-effort MCP session DELETE",
        )

        self._request("DELETE", f"/servers/{server_id}")
        self.assertNotIn(server_id, {item.get("id") for item in self._list_servers()})

    def test_040_server_initiated_ping_is_answered(self) -> None:
        self._request(
            "POST",
            "/servers/test",
            payload={"server": self._config("ping-probe", "/mcp-ping")},
        )
        self.assertTrue(
            self.mock.state.saw_successful_ping_reply(),
            "client host did not answer the server-initiated MCP ping request",
        )

    def test_050_expired_http_session_is_invalidated_and_can_reconnect(self) -> None:
        server_id = "session-expiry"
        self._request(
            "POST",
            "/servers",
            payload={"server": self._config(server_id, "/mcp-expire")},
        )
        self._request("POST", f"/servers/{server_id}/connect")

        # The mock expires the session on the second tools/list. The HTTP route
        # reports the upstream failure, and the client should clear its local
        # session so a later connect can initialize a new one.
        self._request("POST", f"/servers/{server_id}/refresh-tools", expected=502)
        self._request("POST", f"/servers/{server_id}/connect")

        listed = self._list_servers()
        reconnected = next(item for item in listed if item.get("id") == server_id)
        self.assertTrue(reconnected.get("connected"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
