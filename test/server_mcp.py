"""
Integration tests for the MCP gateway endpoint (POST /mcp).

Requires a Lemonade server to already be running on port 13305.

Covers the JSON-RPC 2.0 envelope plus the four tools exposed by the gateway:
- lemonade_list_models
- lemonade_chat
- lemonade_transcribe_audio   (smoke-tested via schema only; needs Whisper)
- lemonade_generate_image     (smoke-tested via schema only; needs SD)

The "live" chat tool uses a small model so the suite stays fast.

Usage:
    python test/server_mcp.py
"""

import json
import os
import sys

# Make the `utils` package importable when this file is executed directly.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import requests  # noqa: E402

from utils.server_base import ServerTestBase, run_server_tests  # noqa: E402
from utils.test_models import (  # noqa: E402
    ENDPOINT_TEST_MODEL,
    PORT,
    TIMEOUT_DEFAULT,
    TIMEOUT_MODEL_OPERATION,
)

MCP_URL = f"http://localhost:{PORT}/mcp"


def _auth_headers():
    """Bearer-token header when LEMONADE_API_KEY is set."""
    api_key = os.environ.get("LEMONADE_API_KEY")
    if api_key:
        return {"Authorization": f"Bearer {api_key}"}
    return {}


def _post(payload, timeout=TIMEOUT_DEFAULT):
    """POST a JSON-RPC body to /mcp and return the requests.Response."""
    return requests.post(
        MCP_URL,
        json=payload,
        headers=_auth_headers(),
        timeout=timeout,
    )


class McpGatewayTests(ServerTestBase):
    """Tests for POST /mcp."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        # Ensure the small chat model is available for live tool calls.
        print(f"\n[SETUP] Ensuring {ENDPOINT_TEST_MODEL} is pulled...")
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/pull",
            json={"model_name": ENDPOINT_TEST_MODEL},
            headers=_auth_headers(),
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        if response.status_code != 200:
            print(f"[SETUP] Warning: pull returned {response.status_code}")

    # ---------------------------------------------------------------------
    # JSON-RPC envelope
    # ---------------------------------------------------------------------

    def test_001_get_returns_405(self):
        """GET /mcp must return 405 with Allow: POST."""
        response = requests.get(
            MCP_URL, headers=_auth_headers(), timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(response.status_code, 405)
        self.assertEqual(response.headers.get("Allow"), "POST")

    def test_002_parse_error_returns_minus_32700(self):
        """Malformed JSON body must produce JSON-RPC -32700."""
        response = requests.post(
            MCP_URL,
            data="this is not json",
            headers={**_auth_headers(), "Content-Type": "application/json"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertEqual(body["jsonrpc"], "2.0")
        self.assertEqual(body["error"]["code"], -32700)

    def test_003_method_not_found_returns_minus_32601(self):
        """Unknown JSON-RPC method must produce -32601."""
        response = _post({"jsonrpc": "2.0", "id": 1, "method": "nonsense"})
        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertEqual(body["id"], 1)
        self.assertEqual(body["error"]["code"], -32601)

    def test_004_missing_method_returns_minus_32600(self):
        """A request object with no `method` is an invalid request."""
        response = _post({"jsonrpc": "2.0", "id": 7})
        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertEqual(body["error"]["code"], -32600)

    def test_005_notification_returns_202(self):
        """JSON-RPC notification (no `id`) must produce 202 with no body."""
        response = _post({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.assertEqual(response.status_code, 202)
        self.assertEqual(response.text, "")

    def test_006_batch_filters_notifications(self):
        """Batched request: notifications drop out of the response array."""
        response = _post(
            [
                {"jsonrpc": "2.0", "id": "a", "method": "ping"},
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                {"jsonrpc": "2.0", "id": "b", "method": "ping"},
            ]
        )
        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertIsInstance(body, list)
        self.assertEqual(len(body), 2)
        ids = sorted(entry["id"] for entry in body)
        self.assertEqual(ids, ["a", "b"])

    # ---------------------------------------------------------------------
    # initialize / ping / tools/list
    # ---------------------------------------------------------------------

    def test_010_initialize(self):
        """initialize must advertise tools capability and protocol version."""
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"protocolVersion": "2025-06-18", "capabilities": {}},
            }
        )
        self.assertEqual(response.status_code, 200)
        body = response.json()
        result = body["result"]
        self.assertIn("protocolVersion", result)
        self.assertIn("tools", result["capabilities"])
        self.assertEqual(result["serverInfo"]["name"], "lemonade-mcp")
        self.assertIn("version", result["serverInfo"])

    def test_011_ping(self):
        """ping returns an empty result object."""
        response = _post({"jsonrpc": "2.0", "id": 2, "method": "ping"})
        body = response.json()
        self.assertEqual(body["result"], {})

    def test_012_tools_list(self):
        """tools/list must include the five gateway tools, each with a schema."""
        response = _post({"jsonrpc": "2.0", "id": 3, "method": "tools/list"})
        body = response.json()
        tools = body["result"]["tools"]
        names = {tool["name"] for tool in tools}
        expected = {
            "lemonade_list_models",
            "lemonade_chat",
            "lemonade_transcribe_audio",
            "lemonade_generate_image",
            "lemonade_omni",
        }
        self.assertTrue(expected.issubset(names), f"missing tools: {expected - names}")
        for tool in tools:
            self.assertIn("description", tool)
            self.assertIn("inputSchema", tool)
            self.assertEqual(tool["inputSchema"]["type"], "object")

        # lemonade_omni's only required arg is `messages`; `model` is optional
        # so callers can fall back to the default (LMX-Omni-5.5B-Lite).
        omni = next(t for t in tools if t["name"] == "lemonade_omni")
        required = set(omni["inputSchema"].get("required", []))
        self.assertEqual(required, {"messages"})
        self.assertIn("model", omni["inputSchema"]["properties"])
        self.assertIn("output_dir", omni["inputSchema"]["properties"])

    # ---------------------------------------------------------------------
    # tools/call error paths
    # ---------------------------------------------------------------------

    def test_020_tools_call_unknown_tool_returns_is_error(self):
        """An unknown tool name must return isError=true (not a JSON-RPC error)."""
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "tools/call",
                "params": {"name": "does_not_exist", "arguments": {}},
            }
        )
        body = response.json()
        self.assertTrue(body["result"]["isError"])
        self.assertIn("content", body["result"])

    def test_021_tools_call_missing_required_arg(self):
        """Missing required tool args bubble up as isError=true."""
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "tools/call",
                "params": {"name": "lemonade_chat", "arguments": {"model": "x"}},
            }
        )
        body = response.json()
        self.assertTrue(body["result"]["isError"])

    def test_022_omni_rejects_non_collection_model(self):
        """
        lemonade_omni must reject a plain LLM (recipe != collection.omni) with
        isError=true, pointing the caller at lemonade_chat. Uses the tiny test
        model that's already pulled in setUpClass — no extra download.
        """
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "tools/call",
                "params": {
                    "name": "lemonade_omni",
                    "arguments": {
                        "model": ENDPOINT_TEST_MODEL,
                        "messages": [{"role": "user", "content": "hi"}],
                    },
                },
            }
        )
        body = response.json()
        self.assertTrue(body["result"]["isError"], msg=str(body))
        text = body["result"]["content"][0]["text"]
        self.assertIn("Omni collection", text)
        self.assertIn("lemonade_chat", text)

    def test_023_omni_rejects_unknown_model(self):
        """Unknown model name must surface as isError=true, not a 404."""
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "tools/call",
                "params": {
                    "name": "lemonade_omni",
                    "arguments": {
                        "model": "definitely-not-a-real-model",
                        "messages": [{"role": "user", "content": "hi"}],
                    },
                },
            }
        )
        body = response.json()
        self.assertTrue(body["result"]["isError"], msg=str(body))
        self.assertIn("Unknown model", body["result"]["content"][0]["text"])

    # ---------------------------------------------------------------------
    # Live tool invocations
    # ---------------------------------------------------------------------

    def test_030_chat_tool_against_tiny_model(self):
        """lemonade_chat must round-trip a small completion."""
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 10,
                "method": "tools/call",
                "params": {
                    "name": "lemonade_chat",
                    "arguments": {
                        "model": ENDPOINT_TEST_MODEL,
                        "messages": [
                            {"role": "user", "content": "Say hello in 3 words."},
                        ],
                        "max_tokens": 16,
                    },
                },
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        body = response.json()
        self.assertNotIn("error", body, msg=str(body))
        self.assertFalse(body["result"]["isError"], msg=str(body["result"]))
        content = body["result"]["content"]
        self.assertGreaterEqual(len(content), 1)
        self.assertEqual(content[0]["type"], "text")
        # Some tiny test models emit empty strings — just assert the shape.
        self.assertIsInstance(content[0]["text"], str)


if __name__ == "__main__":
    run_server_tests(McpGatewayTests, description="MCP GATEWAY TESTS")
