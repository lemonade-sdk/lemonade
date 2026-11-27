"""
Integration tests for the MCP gateway endpoint (POST /mcp).

Requires a Lemonade server to already be running on port 13305.

Covers the JSON-RPC 2.0 envelope plus the pre-existing MCP tool catalog:
- lemonade_list_models
- lemonade_chat
- lemonade_transcribe_audio   (smoke-tested via schema only; needs Whisper)
- lemonade_generate_image     (smoke-tested via schema only; needs SD)
- lemonade_omni
- lemonade_docs

The "live" chat tool uses a small model so the suite stays fast.

Usage:
    python test/server_mcp.py
"""

import json
import os
import sys
import uuid

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


def _admin_headers():
    """Bearer-token header for /internal routes, which only the admin key opens."""
    admin_key = os.environ.get("LEMONADE_ADMIN_API_KEY")
    if admin_key:
        return {"Authorization": f"Bearer {admin_key}"}
    return _auth_headers()


def _post(payload, timeout=TIMEOUT_DEFAULT):
    """POST a JSON-RPC body to /mcp and return the requests.Response."""
    return requests.post(
        MCP_URL,
        json=payload,
        headers=_auth_headers(),
        timeout=timeout,
    )


def _call_tool(name, arguments, request_id=100, timeout=TIMEOUT_DEFAULT):
    """Call one MCP tool and return the decoded tool result."""
    response = _post(
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        },
        timeout=timeout,
    )
    response.raise_for_status()
    return response.json()["result"]


MCP_HUB_EXPECTED_TOOLS = {
    "lemonade_list_models",
    "lemonade_get_model_info",
    "lemonade_load_model",
    "lemonade_unload_model",
    "lemonade_get_loaded_models",
    "lemonade_get_server_health",
    "lemonade_pull_model",
    "lemonade_delete_model",
    "lemonade_get_system_info",
    "lemonade_list_backends",
    "lemonade_install_backend",
    "lemonade_generate_image",
    "lemonade_generate_audio",
    "lemonade_text_to_speech",
    "lemonade_transcribe_audio",
    "lemonade_chat",
    "lemonade_omni",
}


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

    def test_000_cors_preflight(self):
        """OPTIONS /mcp preflight must return 204, reflect concrete allowed origin, and allow required headers."""
        headers = {
            "Origin": "http://localhost:3000",
            "Access-Control-Request-Method": "POST",
            "Access-Control-Request-Headers": "MCP-Protocol-Version, Mcp-Session-Id",
        }
        response = requests.options(
            MCP_URL,
            headers={**_auth_headers(), **headers},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 204)
        allow_origin = response.headers.get("Access-Control-Allow-Origin")
        self.assertEqual(allow_origin, "http://localhost:3000")
        allow_headers = response.headers.get("Access-Control-Allow-Headers", "").lower()
        self.assertIn("mcp-protocol-version", allow_headers)
        self.assertIn("mcp-session-id", allow_headers)

    def test_000_cors_headers_on_post(self):
        """POST /mcp must reflect the allowed origin in CORS headers."""
        response = requests.post(
            MCP_URL,
            json={"jsonrpc": "2.0", "id": 1, "method": "ping"},
            headers={
                **_auth_headers(),
                "Origin": "http://localhost:3000",
                "MCP-Protocol-Version": "2025-06-18",
                "Mcp-Session-Id": "test-session-123",
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(
            response.headers.get("Access-Control-Allow-Origin"), "http://localhost:3000"
        )

    def test_000_cors_disallowed_origin(self):
        """OPTIONS and POST /mcp from disallowed origin must return 403."""
        headers = {
            "Origin": "http://evil.com",
            "Access-Control-Request-Method": "POST",
        }
        response = requests.options(
            MCP_URL,
            headers={**_auth_headers(), **headers},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 403)

        response = requests.post(
            MCP_URL,
            json={"jsonrpc": "2.0", "id": 1, "method": "ping"},
            headers={**_auth_headers(), "Origin": "http://evil.com"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 403)

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
        """tools/list must expose the pre-existing registry tools with valid schemas."""
        response = _post({"jsonrpc": "2.0", "id": 3, "method": "tools/list"})
        body = response.json()
        tools = body["result"]["tools"]
        names = {tool["name"] for tool in tools}
        self.assertEqual(
            len(names), len(tools), "tools/list contains duplicate tool names"
        )
        expected = {
            "lemonade_list_models",
            "lemonade_chat",
            "lemonade_transcribe_audio",
            "lemonade_generate_image",
            "lemonade_edit_image",
            "lemonade_omni",
            "lemonade_docs",
            "lemonade_web_search",
        }
        self.assertTrue(expected.issubset(names), f"missing tools: {expected - names}")
        self.assertTrue(
            MCP_HUB_EXPECTED_TOOLS.issubset(names),
            f"missing MCP hub tools: {MCP_HUB_EXPECTED_TOOLS - names}",
        )
        edit = next(tool for tool in tools if tool["name"] == "lemonade_edit_image")
        self.assertEqual(
            set(edit["inputSchema"].get("required", [])), {"prompt", "image"}
        )
        edit_props = edit["inputSchema"]["properties"]
        self.assertIn("model", edit_props)
        self.assertNotIn("image_base64", edit_props)
        self.assertNotIn("allow_download", edit_props)

        for tool in tools:
            self.assertIn("description", tool)
            self.assertIsInstance(tool["description"], str)
            self.assertTrue(tool["description"].strip())
            self.assertIn("inputSchema", tool)
            self.assertEqual(tool["inputSchema"]["type"], "object")

            meta = tool.get("_meta", {})
            result_contract = meta.get("lemonade/result", {})
            result_description = result_contract.get("description", "")
            self.assertIsInstance(
                result_description,
                str,
                f"{tool['name']}: missing result description",
            )
            self.assertTrue(
                result_description.strip(),
                f"{tool['name']}: missing result description",
            )

            for arg, arg_schema in tool["inputSchema"].get("properties", {}).items():
                arg_description = arg_schema.get("description", "")
                self.assertIsInstance(
                    arg_description,
                    str,
                    f"{tool['name']}.{arg}: missing argument description",
                )
                self.assertTrue(
                    arg_description.strip(),
                    f"{tool['name']}.{arg}: missing argument description",
                )

        # lemonade_omni's only required arg is `messages`; `model` is optional
        # so callers can fall back to the default (LMX-Omni-5.5B-Lite).
        omni = next(t for t in tools if t["name"] == "lemonade_omni")
        required = set(omni["inputSchema"].get("required", []))
        self.assertEqual(required, {"messages"})
        self.assertIn("model", omni["inputSchema"]["properties"])
        self.assertIn("output_dir", omni["inputSchema"]["properties"])
        web = next(t for t in tools if t["name"] == "lemonade_web_search")
        self.assertEqual(web["inputSchema"]["required"], ["query"])
        self.assertEqual(
            web["annotations"],
            {
                "readOnlyHint": True,
                "destructiveHint": False,
                "openWorldHint": True,
            },
        )
        self.assertEqual(web["inputSchema"]["properties"]["max_results"]["minimum"], 1)
        self.assertEqual(
            web["inputSchema"]["properties"]["search_type"]["enum"], ["web", "news"]
        )

    # ---------------------------------------------------------------------
    # tools/call error paths
    # ---------------------------------------------------------------------

    def test_013_list_models_emits_prefixed_collection_id(self):
        """lemonade_list_models emits the canonical `user.` collection id (issue #2788).

        The MCP listing tool serializes the same get_downloaded_models() mapping
        as /v1/models and Ollama /api/tags, so a registered `user.*` collection
        must appear under its prefixed id, not its bare name.
        """
        canonical_name = f"user.McpColl-{uuid.uuid4().hex[:8]}"
        bare_name = canonical_name[5:]
        base = f"http://localhost:{PORT}/api/v1"
        try:
            pull = requests.post(
                f"{base}/pull",
                json={
                    "model_name": canonical_name,
                    "recipe": "collection.omni",
                    "components": [ENDPOINT_TEST_MODEL],
                },
                headers=_auth_headers(),
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(pull.status_code, 200, pull.text)

            response = _post(
                {
                    "jsonrpc": "2.0",
                    "id": 20,
                    "method": "tools/call",
                    "params": {
                        "name": "lemonade_list_models",
                        "arguments": {"include_suggested": False},
                    },
                }
            )
            self.assertEqual(response.status_code, 200)
            body = response.json()
            self.assertFalse(body["result"]["isError"], msg=str(body))
            # content[1] carries the JSON payload as a text block.
            payload = json.loads(body["result"]["content"][1]["text"])
            available = {m["model_name"] for m in payload["available"]}
            self.assertIn(
                canonical_name,
                available,
                f"lemonade_list_models should list {canonical_name} under its user. id",
            )
            self.assertNotIn(
                bare_name,
                available,
                "Collection must not also be listed under its bare name",
            )
            entry = next(
                m for m in payload["available"] if m["model_name"] == canonical_name
            )
            self.assertEqual(entry.get("recipe"), "collection.omni")
        finally:
            try:
                requests.post(
                    f"{base}/delete",
                    json={"model_name": canonical_name},
                    headers=_auth_headers(),
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

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

    def test_021a_tools_call_rejects_wrong_argument_type(self):
        """Schema validation must reject wrong types before the tool handler runs."""
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 51,
                "method": "tools/call",
                "params": {
                    "name": "lemonade_generate_image",
                    "arguments": {
                        "model": "definitely-not-a-real-model",
                        "prompt": "validation test",
                        "n": "many",
                    },
                },
            }
        )
        body = response.json()
        self.assertTrue(body["result"]["isError"], msg=str(body))
        message = body["result"]["content"][0]["text"]
        self.assertIn("$.n", message)
        self.assertIn("expected integer", message)

    def test_021b_tools_call_rejects_value_below_minimum(self):
        """Schema validation must enforce numeric minimum constraints."""
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 52,
                "method": "tools/call",
                "params": {
                    "name": "lemonade_generate_image",
                    "arguments": {
                        "model": "definitely-not-a-real-model",
                        "prompt": "validation test",
                        "n": 0,
                    },
                },
            }
        )
        body = response.json()
        self.assertTrue(body["result"]["isError"], msg=str(body))
        message = body["result"]["content"][0]["text"]
        self.assertIn("$.n", message)
        self.assertIn("value must be >= 1", message)

    def test_021c_tools_call_optional_null_does_not_preempt_validation(self):
        """Optional null args must remain handler-compatible, as before the registry."""
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 53,
                "method": "tools/call",
                "params": {
                    "name": "lemonade_generate_image",
                    "arguments": {
                        "model": None,
                        "prompt": "validation test",
                        "n": 0,
                    },
                },
            }
        )
        body = response.json()
        self.assertTrue(body["result"]["isError"], msg=str(body))
        message = body["result"]["content"][0]["text"]
        self.assertNotIn("$.model", message)
        self.assertIn("$.n", message)
        self.assertIn("value must be >= 1", message)

    def test_021d_web_search_rejects_invalid_args_without_network(self):
        """Invalid calls must fail before any external request is attempted."""
        for arguments in (
            {},
            {"query": ""},
            {"query": "  "},
            {"query": "x" * 257},
            {"query": 123},
            {"query": "test", "max_results": 6},
            {"query": "test", "max_results": "five"},
            {"query": "test", "search_type": "images"},
            {"query": "test", "search_type": 42},
        ):
            response = _post(
                {
                    "jsonrpc": "2.0",
                    "id": 54,
                    "method": "tools/call",
                    "params": {"name": "lemonade_web_search", "arguments": arguments},
                }
            )
            self.assertEqual(response.status_code, 200)
            body = response.json()
            self.assertTrue(body["result"]["isError"], msg=str(arguments))

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
    def test_024_edit_image_rejects_generation_only_model_before_decoding(self):
        """A generation-only model must fail capability selection before decode/load."""
        models_response = requests.get(
            f"http://localhost:{PORT}/v1/models",
            params={"show_all": "true", "capability": "image"},
            headers=_auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(models_response.status_code, 200, models_response.text)
        generation_models = [
            item["id"]
            for item in models_response.json()["data"]
            if item.get("capability") == "image"
            and "image-edit" not in item.get("capabilities", [])
        ]
        if not generation_models:
            self.skipTest("No generation-only image model supported on this host")

        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 8,
                "method": "tools/call",
                "params": {
                    "name": "lemonade_edit_image",
                    "arguments": {
                        "model": generation_models[0],
                        "prompt": "make it blue",
                        "image": "",
                    },
                },
            }
        )
        body = response.json()
        self.assertTrue(body["result"]["isError"], msg=str(body))
        self.assertIn(
            "does not advertise image-edit support",
            body["result"]["content"][0]["text"],
        )

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

    def test_031_chat_tool_resolves_a_model_alias(self):
        """An alias must work as the `model` argument, as it does on the REST endpoints."""
        alias = f"mcp-alias-{uuid.uuid4().hex[:8]}"
        base = f"http://localhost:{PORT}"

        registered = requests.post(
            f"{base}/internal/aliases",
            json={"alias": alias, "target": ENDPOINT_TEST_MODEL},
            headers=_admin_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            registered.status_code,
            200,
            msg=f"could not register alias: {registered.text[:200]}",
        )
        self.addCleanup(
            requests.delete,
            f"{base}/internal/aliases/{alias}",
            headers=_admin_headers(),
            timeout=TIMEOUT_DEFAULT,
        )

        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 11,
                "method": "tools/call",
                "params": {
                    "name": "lemonade_chat",
                    "arguments": {
                        "model": alias,
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
        self.assertFalse(
            body["result"]["isError"],
            msg=f"alias was not resolved: {body['result']}",
        )

    def test_032_omni_tool_resolves_a_model_alias(self):
        """Omni resolves its explicit `model` on its own path, so it needs its own case."""
        alias = f"mcp-alias-{uuid.uuid4().hex[:8]}"
        base = f"http://localhost:{PORT}"

        registered = requests.post(
            f"{base}/internal/aliases",
            json={"alias": alias, "target": ENDPOINT_TEST_MODEL},
            headers=_admin_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            registered.status_code,
            200,
            msg=f"could not register alias: {registered.text[:200]}",
        )
        self.addCleanup(
            requests.delete,
            f"{base}/internal/aliases/{alias}",
            headers=_admin_headers(),
            timeout=TIMEOUT_DEFAULT,
        )

        # The target is a plain LLM, so Omni rejects it. Which rejection comes
        # back is the point: naming the target proves the alias was resolved,
        # while "Unknown model '<alias>'" would mean it was passed through.
        response = _post(
            {
                "jsonrpc": "2.0",
                "id": 12,
                "method": "tools/call",
                "params": {
                    "name": "lemonade_omni",
                    "arguments": {
                        "model": alias,
                        "messages": [
                            {"role": "user", "content": "Say hello in 3 words."},
                        ],
                    },
                },
            },
            timeout=TIMEOUT_DEFAULT,
        )
        body = response.json()
        self.assertNotIn("error", body, msg=str(body))
        self.assertTrue(
            body["result"]["isError"],
            msg=f"a plain LLM should not be accepted as an Omni collection: {body['result']}",
        )
        text = body["result"]["content"][0]["text"]
        self.assertIn(
            "is not an Omni collection",
            text,
            msg=f"alias was not resolved before the recipe check: {text}",
        )
        self.assertIn(
            ENDPOINT_TEST_MODEL,
            text,
            msg=f"error names the alias instead of its target: {text}",
        )
        self.assertNotIn(
            alias,
            text,
            msg=f"error still refers to the unresolved alias: {text}",
        )

    def test_024_get_model_info_tool(self):
        """Named model detail is available through MCP without a duplicate listing tool."""
        result = _call_tool(
            "lemonade_get_model_info",
            {"model_name": ENDPOINT_TEST_MODEL},
            request_id=24,
        )
        self.assertFalse(result["isError"], msg=str(result))
        self.assertEqual(result["structuredContent"]["model_name"], ENDPOINT_TEST_MODEL)
        self.assertIn("recipe", result["structuredContent"])

    def test_025_unload_requires_explicit_model_name(self):
        """Missing model_name must never fall through to REST's unload-all behavior."""
        base = f"http://localhost:{PORT}/api/v1"
        load = requests.post(
            f"{base}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            headers=_auth_headers(),
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(load.status_code, 200, load.text)

        result = _call_tool("lemonade_unload_model", {}, request_id=25)
        self.assertTrue(result["isError"], msg=str(result))

        health = requests.get(
            f"{base}/health", headers=_auth_headers(), timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(health.status_code, 200, health.text)
        loaded = {item["model_name"] for item in health.json()["all_models_loaded"]}
        self.assertIn(ENDPOINT_TEST_MODEL, loaded)

    def test_026_load_and_unload_tools(self):
        """Explicit MCP load/unload round-trips through the existing server handlers."""
        loaded = _call_tool(
            "lemonade_load_model",
            {"model_name": ENDPOINT_TEST_MODEL, "ctx_size": 4096.0},
            request_id=26,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertFalse(loaded["isError"], msg=str(loaded))
        self.assertEqual(loaded["structuredContent"]["status"], "success")

        unloaded = _call_tool(
            "lemonade_unload_model", {"model_name": ENDPOINT_TEST_MODEL}, request_id=27
        )
        self.assertFalse(unloaded["isError"], msg=str(unloaded))
        self.assertEqual(unloaded["structuredContent"]["status"], "success")

    def test_026a_load_ctx_size_rejects_invalid_numbers(self):
        """Fractional and overflowing context sizes are rejected without a load."""
        for size in (4096.5, float(2**63)):
            with self.subTest(size=size):
                result = _call_tool(
                    "lemonade_load_model",
                    {"model_name": ENDPOINT_TEST_MODEL, "ctx_size": size},
                    request_id=261,
                )
                self.assertTrue(result["isError"], msg=str(result))

    def test_026b_options_ctx_size_rejects_invalid_numbers(self):
        """Nested ctx_size must reject fractional and overflowing values too."""
        for size in (4096.5, float(2**63)):
            with self.subTest(size=size):
                result = _call_tool(
                    "lemonade_load_model",
                    {"model_name": ENDPOINT_TEST_MODEL, "options": {"ctx_size": size}},
                    request_id=262,
                )
                self.assertTrue(result["isError"], msg=str(result))
                self.assertIn("ctx_size", result["content"][0]["text"])

    def test_027_pull_existing_model_tool(self):
        """Pull reuses /pull; the already-cached endpoint model makes this deterministic."""
        result = _call_tool(
            "lemonade_pull_model",
            {"model_name": ENDPOINT_TEST_MODEL, "do_not_upgrade": True},
            request_id=28,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertFalse(result["isError"], msg=str(result))

    def test_028_delete_requires_confirmation(self):
        """Delete is exact and requires confirm=true before calling the REST handler."""
        canonical_name = f"user.McpDelete-{uuid.uuid4().hex[:8]}"
        base = f"http://localhost:{PORT}/api/v1"
        try:
            register = requests.post(
                f"{base}/pull",
                json={
                    "model_name": canonical_name,
                    "recipe": "collection.omni",
                    "components": [ENDPOINT_TEST_MODEL],
                },
                headers=_auth_headers(),
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(register.status_code, 200, register.text)

            refused = _call_tool(
                "lemonade_delete_model",
                {"model_name": canonical_name, "confirm": False},
                request_id=29,
            )
            self.assertTrue(refused["isError"], msg=str(refused))

            still_there = _call_tool(
                "lemonade_get_model_info", {"model_name": canonical_name}, request_id=30
            )
            self.assertFalse(still_there["isError"], msg=str(still_there))

            deleted = _call_tool(
                "lemonade_delete_model",
                {"model_name": canonical_name, "confirm": True},
                request_id=31,
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertFalse(deleted["isError"], msg=str(deleted))
        finally:
            try:
                requests.post(
                    f"{base}/delete",
                    json={"model_name": canonical_name},
                    headers=_auth_headers(),
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_012_all_advertised_tools_have_dispatch(self):
        """Every tools/list entry must resolve to a real tools/call dispatch."""
        listed = _post({"jsonrpc": "2.0", "id": 120, "method": "tools/list"}).json()[
            "result"
        ]["tools"]
        listed_names = {tool["name"] for tool in listed}
        self.assertTrue(
            MCP_HUB_EXPECTED_TOOLS.issubset(listed_names),
            msg=(
                "Missing expected MCP tools: "
                f"{sorted(MCP_HUB_EXPECTED_TOOLS - listed_names)}"
            ),
        )
        by_name = {tool["name"]: tool for tool in listed}
        install_props = by_name["lemonade_install_backend"]["inputSchema"]["properties"]
        for name in ("recipe", "backend"):
            self.assertEqual(install_props[name]["pattern"], r"\S")
        expected_annotations = {
            "lemonade_list_models": {
                "readOnlyHint": True,
                "destructiveHint": False,
                "openWorldHint": False,
            },
            "lemonade_get_model_info": {
                "readOnlyHint": True,
                "destructiveHint": False,
                "openWorldHint": False,
            },
            "lemonade_load_model": {
                "readOnlyHint": False,
                "destructiveHint": False,
                "openWorldHint": True,
            },
            "lemonade_unload_model": {
                "readOnlyHint": False,
                "destructiveHint": False,
                "openWorldHint": False,
            },
            "lemonade_get_loaded_models": {
                "readOnlyHint": True,
                "destructiveHint": False,
                "openWorldHint": False,
            },
            "lemonade_get_server_health": {
                "readOnlyHint": True,
                "destructiveHint": False,
                "openWorldHint": False,
            },
            "lemonade_pull_model": {
                "readOnlyHint": False,
                "destructiveHint": False,
                "openWorldHint": True,
            },
            "lemonade_delete_model": {
                "readOnlyHint": False,
                "destructiveHint": True,
                "openWorldHint": False,
            },
            "lemonade_get_system_info": {
                "readOnlyHint": True,
                "destructiveHint": False,
                "openWorldHint": False,
            },
            "lemonade_list_backends": {
                "readOnlyHint": True,
                "destructiveHint": False,
                "openWorldHint": False,
            },
            "lemonade_install_backend": {
                "readOnlyHint": False,
                "destructiveHint": True,
                "openWorldHint": True,
            },
        }
        for name, expected in expected_annotations.items():
            annotations = by_name[name].get("annotations", {})
            for key, value in expected.items():
                self.assertEqual(
                    annotations.get(key),
                    value,
                    msg=f"{name} annotation {key}: {annotations}",
                )

        # McpTool pairs every advertised descriptor with its handler.
        # Probe server-owned adapters with schema-valid arguments which fail
        # before any load, pull, install, delete or media generation.
        safe_probes = {
            "lemonade_get_model_info": {"model_name": ""},
            "lemonade_load_model": {"model_name": ""},
            "lemonade_unload_model": {"model_name": ""},
            "lemonade_pull_model": {"model_name": ""},
            "lemonade_delete_model": {"model_name": "", "confirm": True},
            "lemonade_install_backend": {"recipe": "", "backend": ""},
            "lemonade_generate_audio": {"prompt": "", "model": "nonexistent"},
            "lemonade_text_to_speech": {"input": "", "model": "nonexistent"},
        }
        for index, (name, arguments) in enumerate(safe_probes.items(), start=121):
            self.assertIn(name, listed_names)
            result = _call_tool(name, arguments, request_id=index)
            text = json.dumps(result)
            self.assertTrue(result["isError"], msg=name)
            self.assertNotIn("Unknown tool:", text, msg=name)
            self.assertNotIn("Unknown Lemonade server tool:", text, msg=name)
            self.assertNotIn("Invalid arguments for MCP tool", text, msg=name)


if __name__ == "__main__":
    run_server_tests(McpGatewayTests, description="MCP GATEWAY TESTS")
