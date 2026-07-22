"""
Integration tests for the OpenInference telemetry feature (Option B).

Covers:
- Configuration validation (POST /v1/params).
- Out-of-band tracing for chat completions (non-streaming).
- Out-of-band tracing for chat completions (streaming).
- Trace context validation (span name, kind, attributes, traceId, spanId).
- User and Session ID tracking.
- Error path tracking.

Usage:
    python test/server_telemetry.py
"""

import json
import os
import sys
import time
import http.server
import threading
import socket
import struct
from queue import Queue

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


# Protobuf decoder helper functions for OTLP
def decode_varint(data, pos):
    val = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            break
        shift += 7
    return val, pos


def parse_protobuf(data, pos=0, end=None):
    if end is None:
        end = len(data)
    fields = []
    while pos < end:
        key, pos = decode_varint(data, pos)
        field_num = key >> 3
        wire_type = key & 0x7
        if wire_type == 0:
            val, pos = decode_varint(data, pos)
            fields.append((field_num, "varint", val))
        elif wire_type == 1:
            val = int.from_bytes(data[pos : pos + 8], "little")
            pos += 8
            fields.append((field_num, "fixed64", val))
        elif wire_type == 2:
            length, pos = decode_varint(data, pos)
            val = data[pos : pos + length]
            pos += length
            fields.append((field_num, "length_delimited", val))
        elif wire_type == 5:
            val = int.from_bytes(data[pos : pos + 4], "little")
            pos += 4
            fields.append((field_num, "fixed32", val))
        else:
            raise ValueError(f"Unknown wire type {wire_type}")
    return fields


def parse_any_value(data):
    fields = parse_protobuf(data)
    for field_num, wire_type, val in fields:
        if field_num == 1:
            return {"stringValue": val.decode("utf-8", errors="replace")}
        elif field_num == 2:
            return {"boolValue": bool(val)}
        elif field_num == 3:
            return {"intValue": val}
        elif field_num == 4:
            double_bytes = val.to_bytes(8, "little")
            d_val = struct.unpack("<d", double_bytes)[0]
            return {"doubleValue": d_val}
    return {}


def parse_key_value(data):
    fields = parse_protobuf(data)
    key = ""
    value = {}
    for field_num, wire_type, val in fields:
        if field_num == 1:
            key = val.decode("utf-8", errors="replace")
        elif field_num == 2:
            value = parse_any_value(val)
    return {"key": key, "value": value}


def parse_status(data):
    fields = parse_protobuf(data)
    status = {}
    for field_num, wire_type, val in fields:
        if field_num == 2:
            status["message"] = val.decode("utf-8", errors="replace")
        elif field_num == 3:
            status["code"] = val
    return status


def parse_span(data):
    fields = parse_protobuf(data)
    span = {}
    span["attributes"] = []
    for field_num, wire_type, val in fields:
        if field_num == 1:
            span["traceId"] = val.hex()
        elif field_num == 2:
            span["spanId"] = val.hex()
        elif field_num == 4:
            span["parentSpanId"] = val.hex()
        elif field_num == 5:
            span["name"] = val.decode("utf-8", errors="replace")
        elif field_num == 6:
            span["kind"] = val
        elif field_num == 7:
            span["startTimeUnixNano"] = str(val)
        elif field_num == 8:
            span["endTimeUnixNano"] = str(val)
        elif field_num == 9:
            span["attributes"].append(parse_key_value(val))
        elif field_num == 15:
            span["status"] = parse_status(val)
    return span


def parse_instrumentation_scope(data):
    fields = parse_protobuf(data)
    scope = {}
    for field_num, wire_type, val in fields:
        if field_num == 1:
            scope["name"] = val.decode("utf-8", errors="replace")
        elif field_num == 2:
            scope["version"] = val.decode("utf-8", errors="replace")
    return scope


def parse_scope_spans(data):
    fields = parse_protobuf(data)
    scope_span = {}
    scope_span["spans"] = []
    for field_num, wire_type, val in fields:
        if field_num == 1:
            scope_span["scope"] = parse_instrumentation_scope(val)
        elif field_num == 2:
            scope_span["spans"].append(parse_span(val))
    return scope_span


def parse_resource(data):
    fields = parse_protobuf(data)
    resource = {"attributes": []}
    for field_num, wire_type, val in fields:
        if field_num == 1:
            resource["attributes"].append(parse_key_value(val))
    return resource


def parse_resource_spans(data):
    fields = parse_protobuf(data)
    res_span = {}
    res_span["scopeSpans"] = []
    for field_num, wire_type, val in fields:
        if field_num == 1:
            res_span["resource"] = parse_resource(val)
        elif field_num == 2:
            res_span["scopeSpans"].append(parse_scope_spans(val))
    return res_span


def parse_export_trace_request(data):
    fields = parse_protobuf(data)
    resource_spans = []
    for field_num, wire_type, val in fields:
        if field_num == 1:
            resource_spans.append(parse_resource_spans(val))
    return {"resourceSpans": resource_spans}


# Mock OTLP Collector HTTP Server
class MockOTLPHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        post_data = self.rfile.read(content_length)

        content_type = self.headers.get("Content-Type", "")

        body = None
        if "application/x-protobuf" in content_type:
            try:
                body = parse_export_trace_request(post_data)
            except Exception as e:
                self.send_response(400)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(f"Failed to parse protobuf: {e}".encode())
                return
        else:
            try:
                body = json.loads(post_data.decode("utf-8"))
            except Exception as e:
                self.send_response(400)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(f"Failed to parse JSON: {e}".encode())
                return

        # Enqueue the received request data for the test main thread to assert
        self.server.request_queue.put(
            {"path": self.path, "headers": dict(self.headers), "body": body}
        )

        if "X-Test-Error" in self.headers:
            status_code = int(self.headers["X-Test-Error"])
            self.send_response(status_code)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"Mocked error response")
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"status": "success"}')

    def log_message(self, format, *args):
        # Suppress logging request details to stdout to keep test runs clean
        pass


class MockOTLPServer(http.server.HTTPServer):
    def __init__(self, server_address, RequestHandlerClass):
        super().__init__(server_address, RequestHandlerClass)
        self.request_queue = Queue()


def find_free_port():
    s = socket.socket()
    s.bind(("", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _get_header_value(headers, target_key):
    for k, v in headers.items():
        if k.lower() == target_key.lower():
            return v
    return None


class TelemetryTestBase(ServerTestBase):
    """Shared OTLP mock + helpers for all telemetry test classes."""

    _pull_base_model = False

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.mock_port = find_free_port()
        cls.mock_server = MockOTLPServer(("127.0.0.1", cls.mock_port), MockOTLPHandler)
        cls.mock_thread = threading.Thread(target=cls.mock_server.serve_forever)
        cls.mock_thread.daemon = True
        cls.mock_thread.start()

        if cls._pull_base_model:
            print(
                f"\n[SETUP] Ensuring {ENDPOINT_TEST_MODEL} is pulled for {cls.__name__}..."
            )
            cls._auth_post(
                f"http://localhost:{PORT}/api/v1/pull",
                {"model_name": ENDPOINT_TEST_MODEL},
                timeout=TIMEOUT_MODEL_OPERATION,
            )

    @classmethod
    def tearDownClass(cls):
        try:
            cls._auth_post(
                f"http://localhost:{PORT}/api/v1/params",
                {"telemetry": {"enabled": False}},
            )
        except Exception:
            pass
        cls.mock_server.shutdown()
        cls.mock_server.server_close()
        cls.mock_thread.join()
        super().tearDownClass()

    @classmethod
    def _auth_post(cls, url, json_body, timeout=TIMEOUT_DEFAULT, headers=None):
        req_headers = dict(headers or {})
        api_key = os.environ.get("LEMONADE_API_KEY")
        if api_key:
            req_headers["Authorization"] = f"Bearer {api_key}"
        return requests.post(url, json=json_body, headers=req_headers, timeout=timeout)

    @classmethod
    def _auth_get(cls, url, timeout=TIMEOUT_DEFAULT):
        headers = {}
        api_key = os.environ.get("LEMONADE_API_KEY")
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        return requests.get(url, headers=headers, timeout=timeout)

    def setUp(self):
        super().setUp()
        while not self.mock_server.request_queue.empty():
            self.mock_server.request_queue.get()

    def _enable_telemetry(self, headers=None, endpoint=None, semantics=None, **kwargs):
        telemetry_params = {
            "enabled": True,
            "hide_inputs": kwargs.pop("hide_inputs", False),
            "hide_outputs": kwargs.pop("hide_outputs", False),
            "hide_thinking": kwargs.pop("hide_thinking", False),
            "trust_incoming_trace_context": kwargs.pop(
                "trust_incoming_trace_context", False
            ),
            "max_queue_capacity": kwargs.pop("max_queue_capacity", 1000),
        }
        otlp_params = {
            "endpoint": endpoint or f"http://127.0.0.1:{self.mock_port}/v1/traces",
            "protocol": kwargs.pop("protocol", "http/protobuf"),
            "semantics": semantics or ["openinference"],
            "headers": headers or {},
            "max_retries": kwargs.pop("max_retries", 0),
            "retry_backoff_base_s": kwargs.pop("retry_backoff_base_s", 5.0),
            "send_batch_size": kwargs.pop("send_batch_size", 1),
            "batch_timeout_s": kwargs.pop("batch_timeout_s", 1.0),
        }

        for k, v in kwargs.items():
            otlp_params[k] = v

        payload = {"telemetry": {**telemetry_params, "otlp": otlp_params}}
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 200, res.text)

    def _wait_for_span(self, span_name=None, timeout=5.0, queue=None):
        q = queue or self.mock_server.request_queue
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not q.empty():
                received = q.get()
                print(f"[DEBUG _wait_for_span] Received: {received}")
                if span_name is None:
                    return received
                body = received.get("body", {})
                rs = body.get("resourceSpans", [])
                if rs:
                    spans = rs[0].get("scopeSpans", [{}])[0].get("spans", [])
                    if spans:
                        print(
                            f"[DEBUG _wait_for_span] Span name: {spans[0].get('name')}"
                        )
                        if spans[0]["name"] == span_name:
                            return received
                    else:
                        print(
                            "[DEBUG _wait_for_span] scopeSpans is empty or has no spans"
                        )
                else:
                    print("[DEBUG _wait_for_span] resourceSpans is empty")
            time.sleep(0.1)
        return None

    def _extract_span(self, span_received):
        span = span_received["body"]["resourceSpans"][0]["scopeSpans"][0]["spans"][0]
        attrs = {a["key"]: a["value"] for a in span["attributes"]}
        return span, attrs

    def _chat_completion(self, content, port=PORT, request_headers=None, **extra):
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "user", "content": content}],
            "temperature": 0.0,
            **extra,
        }
        res = self._auth_post(
            f"http://localhost:{port}/api/v1/chat/completions",
            payload,
            headers=request_headers,
        )
        self.assertEqual(res.status_code, 200, res.text)
        return res

    def _assert_standard_token_counts(
        self, attrs, prompt_key, completion_key=None, total_key=None
    ):
        if prompt_key:
            self.assertIn("llm.token_count.prompt", attrs)
            self.assertEqual(
                attrs["llm.token_count.prompt"]["intValue"],
                attrs[prompt_key]["intValue"],
            )
        if completion_key:
            self.assertIn("llm.token_count.completion", attrs)
            self.assertEqual(
                attrs["llm.token_count.completion"]["intValue"],
                attrs[completion_key]["intValue"],
            )
        if total_key:
            self.assertIn("llm.token_count.total", attrs)
            self.assertEqual(
                attrs["llm.token_count.total"]["intValue"],
                attrs[total_key]["intValue"],
            )

    def _assert_performance_metrics(self, attrs):
        self.assertIn("llm.performance.time_to_first_token", attrs)
        self.assertIn("llm.performance.tokens_per_second", attrs)
        self.assertGreater(
            attrs["llm.performance.time_to_first_token"]["doubleValue"], 0.0
        )
        self.assertGreater(
            attrs["llm.performance.tokens_per_second"]["doubleValue"], 0.0
        )


class ConfigTests(TelemetryTestBase):
    """Parameter validation and toggle endpoint."""

    def test_001_config_validation(self):
        """Verify that telemetry block fields are validated correctly."""
        # Valid update
        payload = {
            "telemetry": {
                "enabled": False,
                "otlp": {
                    "endpoint": "http://127.0.0.1:4317/v1/traces",
                    "headers": {"Authorization": "test-key"},
                },
            }
        }
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 200, res.text)

        # Invalid: enabled is not a boolean
        payload = {"telemetry": {"enabled": "yes"}}
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 400)
        self.assertIn("enabled", res.json()["error"])

        # Invalid: endpoint is not a string
        payload = {"telemetry": {"otlp": {"endpoint": 123}}}
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 400)
        self.assertIn("endpoint", res.json()["error"])

        # Invalid: headers is not an object
        payload = {"telemetry": {"otlp": {"headers": "Bearer token"}}}
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 400)
        self.assertIn("headers", res.json()["error"])

        # Invalid: protocol is not a string
        payload = {"telemetry": {"otlp": {"protocol": 123}}}
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 400)
        self.assertIn("protocol", res.json()["error"])

        # Invalid: protocol value is not supported
        payload = {"telemetry": {"otlp": {"protocol": "http/xml"}}}
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 400)
        self.assertIn("protocol", res.json()["error"])

        # Invalid: max_retries is not an integer
        payload = {"telemetry": {"otlp": {"max_retries": "3"}}}
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 400)
        self.assertIn("max_retries", res.json()["error"])

        # Invalid: max_retries is negative
        payload = {"telemetry": {"otlp": {"max_retries": -1}}}
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 400)
        self.assertIn("max_retries", res.json()["error"])

        # Invalid: semantics is not an array
        payload = {"telemetry": {"otlp": {"semantics": "openinference"}}}
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 400)
        self.assertIn("semantics", res.json()["error"])

        # Invalid: semantics contains unsupported semantic
        payload = {
            "telemetry": {"otlp": {"semantics": ["openinference", "custom_fmt"]}}
        }
        res = self._auth_post(f"http://localhost:{PORT}/api/v1/params", payload)
        self.assertEqual(res.status_code, 400)
        self.assertIn("semantics", res.json()["error"])

    def test_002_telemetry_toggle(self):
        """Verify internal endpoint to toggle telemetry on/off."""
        # 1. Turn telemetry off via /internal/set
        res = self._auth_post(
            f"http://localhost:{PORT}/internal/set", {"telemetry": {"enabled": False}}
        )
        self.assertEqual(res.status_code, 200, res.text)
        self.assertEqual(res.json().get("status"), "success")
        self.assertFalse(
            res.json().get("updated", {}).get("telemetry", {}).get("enabled")
        )

        # 2. Get params to verify it is disabled
        res = self._auth_get(f"http://localhost:{PORT}/api/v1/params")
        self.assertEqual(res.status_code, 200, res.text)
        self.assertFalse(res.json().get("telemetry", {}).get("enabled"))

        # 3. Turn telemetry back on via /internal/set
        res = self._auth_post(
            f"http://localhost:{PORT}/internal/set", {"telemetry": {"enabled": True}}
        )
        self.assertEqual(res.status_code, 200, res.text)
        self.assertEqual(res.json().get("status"), "success")
        self.assertTrue(
            res.json().get("updated", {}).get("telemetry", {}).get("enabled")
        )

        # 4. Get params to verify it is enabled again
        res = self._auth_get(f"http://localhost:{PORT}/api/v1/params")
        self.assertEqual(res.status_code, 200, res.text)
        self.assertTrue(res.json().get("telemetry", {}).get("enabled"))

        # 5. Check authorization: call without admin key (when configured)
        admin_key = os.environ.get("LEMONADE_ADMIN_API_KEY")
        if admin_key:
            # Send requests directly without headers
            res = requests.post(
                f"http://localhost:{PORT}/internal/set",
                json={"telemetry": {"enabled": True}},
            )
            self.assertEqual(res.status_code, 401)
        else:
            api_key = os.environ.get("LEMONADE_API_KEY")
            if api_key:
                res = requests.post(
                    f"http://localhost:{PORT}/internal/set",
                    json={"telemetry": {"enabled": True}},
                )
                self.assertEqual(res.status_code, 401)

    def test_013_telemetry_toggle_preserves_settings(self):
        """Verify that toggling telemetry preserves otlp endpoint, semantics, etc. and rejects unknown keys."""
        # 1. Enable telemetry and set specific OTLP settings
        custom_endpoint = f"http://127.0.0.1:{self.mock_port}/v1/custom-traces"
        custom_headers = {"X-Custom": "Val"}
        self._enable_telemetry(
            endpoint=custom_endpoint,
            headers=custom_headers,
            semantics=["otel_genai"],
            protocol="http/json",
        )

        # 2. Toggle telemetry off via internal endpoint
        res = self._auth_post(
            f"http://localhost:{PORT}/internal/set", {"telemetry": {"enabled": False}}
        )
        self.assertEqual(res.status_code, 200, res.text)

        # 3. Verify settings are preserved
        res = self._auth_get(f"http://localhost:{PORT}/api/v1/params")
        self.assertEqual(res.status_code, 200)
        telemetry = res.json().get("telemetry", {})
        self.assertFalse(telemetry.get("enabled"))
        self.assertEqual(telemetry.get("otlp", {}).get("endpoint"), custom_endpoint)
        self.assertEqual(telemetry.get("otlp", {}).get("headers"), custom_headers)
        self.assertEqual(telemetry.get("otlp", {}).get("semantics"), ["otel_genai"])
        self.assertEqual(telemetry.get("otlp", {}).get("protocol"), "http/json")

        # 4. Toggle telemetry back on
        res = self._auth_post(
            f"http://localhost:{PORT}/internal/set", {"telemetry": {"enabled": True}}
        )
        self.assertEqual(res.status_code, 200)

        # 5. Verify settings are still preserved and semantics is still otel_genai
        res = self._auth_get(f"http://localhost:{PORT}/api/v1/params")
        self.assertEqual(res.status_code, 200)
        telemetry = res.json().get("telemetry", {})
        self.assertTrue(telemetry.get("enabled"))
        self.assertEqual(telemetry.get("otlp", {}).get("endpoint"), custom_endpoint)
        self.assertEqual(telemetry.get("otlp", {}).get("semantics"), ["otel_genai"])

        # 6. Verify unknown telemetry keys are rejected
        res = self._auth_post(
            f"http://localhost:{PORT}/api/v1/params",
            {"telemetry": {"unknown_field": True}},
        )
        self.assertEqual(res.status_code, 400)
        self.assertIn("unknown_field", res.json().get("error", ""))

        # 7. Verify unknown OTLP keys are rejected
        res = self._auth_post(
            f"http://localhost:{PORT}/api/v1/params",
            {"telemetry": {"otlp": {"unknown_otlp_field": "val"}}},
        )
        self.assertEqual(res.status_code, 400)
        self.assertIn("unknown_otlp_field", res.json().get("error", ""))


class CoreTracingTests(TelemetryTestBase):
    """Non-streaming, streaming, user/session metadata, JSON protocol."""

    _pull_base_model = True

    def test_003_non_streaming_telemetry_span(self):
        """Verify standard non-streaming completions submit valid OpenInference traces."""
        self._enable_telemetry(headers={"X-Test-Header": "Passed"})
        self._chat_completion("Say hello exactly.")

        span_received = self._wait_for_span()
        self.assertIsNotNone(
            span_received, "Telemetry span was not received by the OTLP mock receiver."
        )
        self.assertEqual(span_received["path"], "/v1/traces")
        self.assertEqual(
            _get_header_value(span_received["headers"], "x-test-header"), "Passed"
        )

        span, attrs = self._extract_span(span_received)
        self.assertEqual(span["name"], "chat.completions")
        self.assertEqual(span["kind"], 2)  # Server span
        self.assertIn("traceId", span)
        self.assertIn("spanId", span)

        # Verify OpenInference attributes
        self.assertEqual(attrs["openinference.span.kind"]["stringValue"], "LLM")
        self.assertEqual(attrs["llm.model_name"]["stringValue"], ENDPOINT_TEST_MODEL)
        self.assertIn("input.value", attrs)
        self.assertIn("output.value", attrs)

        # Non-streaming should have valid usage token counts
        self.assertIn("llm.usage.prompt_tokens", attrs)
        self.assertIn("llm.usage.completion_tokens", attrs)
        self.assertGreater(attrs["llm.usage.prompt_tokens"]["intValue"], 0)
        self.assertGreater(attrs["llm.usage.completion_tokens"]["intValue"], 0)

        # Standard OpenInference token count attributes
        self._assert_standard_token_counts(
            attrs,
            prompt_key="llm.usage.prompt_tokens",
            completion_key="llm.usage.completion_tokens",
        )
        self.assertIn("llm.token_count.total", attrs)

        # Verify performance metrics are present and valid
        self._assert_performance_metrics(attrs)

    def test_004_streaming_telemetry_span(self):
        """Verify streaming completions accumulate outputs and transmit tracing correctly."""
        self._enable_telemetry()

        # Run streaming chat completion
        res = self._chat_completion("Write a single word.", stream=True)

        # Read the chunks completely to trigger sink.done() and State destruction
        chunks = []
        for chunk in res.iter_lines():
            if chunk:
                chunks.append(chunk.decode("utf-8"))

        span_received = self._wait_for_span()
        self.assertIsNotNone(
            span_received,
            "Telemetry span was not received for streaming chat completion.",
        )

        span, attrs = self._extract_span(span_received)
        self.assertEqual(attrs["openinference.span.kind"]["stringValue"], "LLM")
        self.assertEqual(attrs["llm.model_name"]["stringValue"], ENDPOINT_TEST_MODEL)
        self.assertIn("input.value", attrs)
        self.assertIn("output.value", attrs)
        self.assertGreater(len(attrs["output.value"]["stringValue"]), 0)

        # Best-effort token counts should be populated
        self.assertIn("llm.usage.completion_tokens", attrs)
        self.assertGreater(attrs["llm.usage.completion_tokens"]["intValue"], 0)

        # Standard OpenInference token count attributes
        self.assertIn("llm.token_count.completion", attrs)
        self.assertGreater(attrs["llm.token_count.completion"]["intValue"], 0)

        # Verify performance metrics are present and valid in streaming
        self._assert_performance_metrics(attrs)

    def test_005_session_and_user_tracking(self):
        """Verify that user and session IDs are parsed and mapped to the span attributes."""
        self._enable_telemetry()

        self._chat_completion("Hi.", user="user-abc-123", session_id="session-xyz-999")

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received)
        span, attrs = self._extract_span(span_received)
        self.assertEqual(attrs["openinference.user.id"]["stringValue"], "user-abc-123")
        self.assertEqual(
            attrs["openinference.session.id"]["stringValue"], "session-xyz-999"
        )

    def test_006_json_telemetry_span(self):
        """Verify that when protocol is http/json, the payload is transmitted as standard JSON."""
        self._enable_telemetry(headers={"X-JSON-Header": "OK"}, protocol="http/json")
        self._chat_completion("Hello json.")

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received, "JSON Telemetry span was not received.")
        self.assertEqual(span_received["path"], "/v1/traces")
        self.assertEqual(
            _get_header_value(span_received["headers"], "content-type"),
            "application/json",
        )
        self.assertEqual(
            _get_header_value(span_received["headers"], "x-json-header"), "OK"
        )


# A fixed, valid W3C traceparent (version 00): non-zero 32-hex trace id and
# 16-hex parent id with the sampled flag set.
_TRACE_ID = "4bf92f3577b34da6a3ce929d0e0e4736"
_PARENT_ID = "00f067aa0ba902b7"
_TRACEPARENT = f"00-{_TRACE_ID}-{_PARENT_ID}-01"


class TraceContextTests(TelemetryTestBase):
    """W3C traceparent ingestion gated by telemetry.trust_incoming_trace_context."""

    _pull_base_model = True

    def test_017_adopts_incoming_trace_context(self):
        """With the flag enabled, a valid traceparent sets the span's trace id and parent."""
        self._enable_telemetry(trust_incoming_trace_context=True)
        self._chat_completion(
            "Say hello.", request_headers={"traceparent": _TRACEPARENT}
        )

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received, "Telemetry span was not received.")
        span, _ = self._extract_span(span_received)
        self.assertEqual(span["traceId"], _TRACE_ID)
        self.assertEqual(span.get("parentSpanId"), _PARENT_ID)

    def test_018_adopts_incoming_trace_context_json(self):
        """The http/json OTLP path also adopts the incoming trace context."""
        self._enable_telemetry(trust_incoming_trace_context=True, protocol="http/json")
        self._chat_completion(
            "Say hello.", request_headers={"traceparent": _TRACEPARENT}
        )

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received, "Telemetry span was not received.")
        span, _ = self._extract_span(span_received)
        self.assertEqual(span["traceId"], _TRACE_ID)
        self.assertEqual(span.get("parentSpanId"), _PARENT_ID)

    def test_019_ignores_traceparent_when_disabled(self):
        """With the flag disabled (default), the traceparent is ignored: fresh root span."""
        self._enable_telemetry(trust_incoming_trace_context=False)
        self._chat_completion(
            "Say hello.", request_headers={"traceparent": _TRACEPARENT}
        )

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received, "Telemetry span was not received.")
        span, _ = self._extract_span(span_received)
        self.assertNotEqual(span["traceId"], _TRACE_ID)
        self.assertNotIn("parentSpanId", span)

    def test_020_malformed_traceparent_falls_back_to_root(self):
        """Malformed headers are ignored safely; the span starts a fresh root trace."""
        self._enable_telemetry(trust_incoming_trace_context=True)
        malformed = {
            "too_short": "not-a-valid-header",
            "zero_trace_id": f"00-{'0' * 32}-{_PARENT_ID}-01",
            "zero_parent_id": f"00-{_TRACE_ID}-{'0' * 16}-01",
            "forbidden_version": f"ff-{_TRACE_ID}-{_PARENT_ID}-01",
            "v00_extra_field": f"00-{_TRACE_ID}-{_PARENT_ID}-01-extra",
        }
        for label, header in malformed.items():
            with self.subTest(case=label):
                self._chat_completion("Hi.", request_headers={"traceparent": header})
                span_received = self._wait_for_span()
                self.assertIsNotNone(span_received, "Telemetry span was not received.")
                span, _ = self._extract_span(span_received)
                self.assertNotEqual(span["traceId"], _TRACE_ID)
                self.assertNotIn("parentSpanId", span)

    def test_021_future_version_traceparent_supported(self):
        """A higher version with a trailing field is parsed using the first four fields."""
        self._enable_telemetry(trust_incoming_trace_context=True)
        future = f"01-{_TRACE_ID}-{_PARENT_ID}-01-extra"
        self._chat_completion("Hi.", request_headers={"traceparent": future})

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received, "Telemetry span was not received.")
        span, _ = self._extract_span(span_received)
        self.assertEqual(span["traceId"], _TRACE_ID)
        self.assertEqual(span.get("parentSpanId"), _PARENT_ID)


class PrivacyTests(TelemetryTestBase):
    """hide_inputs, hide_outputs, hide_thinking controls."""

    _pull_base_model = True

    def test_007_telemetry_hide_inputs(self):
        """Verify that hide_inputs config hides input message content in spans."""
        self._enable_telemetry(hide_inputs=True)
        self._chat_completion("Keep this prompt secret.")

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received, "Telemetry span was not received.")
        span, attrs = self._extract_span(span_received)

        self.assertEqual(attrs["input.value"]["stringValue"], "[REDACTED]")
        self.assertEqual(
            attrs["llm.input_messages.0.message.content"]["stringValue"], "[REDACTED]"
        )
        self.assertNotEqual(attrs["output.value"]["stringValue"], "[REDACTED]")

    def test_008_telemetry_hide_outputs(self):
        """Verify that hide_outputs config hides output assistant content in spans."""
        self._enable_telemetry(hide_outputs=True)
        self._chat_completion("Reveal the magic word.")

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received, "Telemetry span was not received.")
        span, attrs = self._extract_span(span_received)

        self.assertEqual(attrs["output.value"]["stringValue"], "[REDACTED]")
        self.assertEqual(
            attrs["llm.output_messages.0.message.content"]["stringValue"], "[REDACTED]"
        )
        self.assertEqual(
            attrs["input.value"]["stringValue"],
            '[{"content":"Reveal the magic word.","role":"user"}]',
        )

    def test_009_telemetry_hide_thinking(self):
        """Verify that hide_thinking config strips reasoning tags in spans."""
        self._enable_telemetry(hide_thinking=True)
        self._chat_completion("Think and say hello.")

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received, "Telemetry span was not received.")
        span, attrs = self._extract_span(span_received)

        output_val = attrs["output.value"]["stringValue"]
        self.assertNotIn("<think>", output_val)
        self.assertNotIn("</think>", output_val)
        self.assertNotIn("<|think|>", output_val)
        self.assertNotIn("</|think|>", output_val)


class ErrorTests(TelemetryTestBase):
    """Error span recording across all request types."""

    _pull_base_model = True

    def test_010_error_tracking(self):
        """Verify that failed chat completions record error attributes on the span."""
        self._enable_telemetry()

        # Request a nonexistent model to trigger a load failure
        completion_payload = {
            "model": "nonexistent-model-name-error",
            "messages": [{"role": "user", "content": "Hi."}],
        }
        res = self._auth_post(
            f"http://localhost:{PORT}/api/v1/chat/completions", completion_payload
        )
        self.assertNotEqual(res.status_code, 200)

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received)
        span, attrs = self._extract_span(span_received)
        self.assertEqual(span["status"]["code"], 2)  # StatusCode::kError = 2
        self.assertIn("message", span["status"])
        self.assertIn("nonexistent-model-name-error", span["status"]["message"])

    def test_011_load_failure_telemetry_span(self):
        """Verify that a model loading failure on embeddings creates an error span."""
        self._enable_telemetry(headers={"X-Test-Header": "LoadFailure"})

        # Call embeddings with non-existent model
        embeddings_payload = {
            "model": "non-existent-embedding-model",
            "input": "test",
        }
        res = self._auth_post(
            f"http://localhost:{PORT}/api/v1/embeddings", embeddings_payload
        )
        self.assertNotEqual(res.status_code, 200)

        # Wait for span
        span_received = self._wait_for_span(span_name="embeddings")

        self.assertIsNotNone(
            span_received, "Embedding load failure telemetry span was not received."
        )
        span, _ = self._extract_span(span_received)
        self.assertEqual(span["name"], "embeddings")
        self.assertEqual(span["status"]["code"], 2)  # ERROR code

    def test_012_streaming_error_telemetry_span(self):
        """Verify that a streaming error (e.g. invalid parameter) logs error details on the span."""
        self._enable_telemetry(headers={"X-Test-Header": "StreamError"})

        # Call streaming completions with invalid messages schema (string instead of array)
        completion_payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": "Hi.",
            "stream": True,
        }
        res = self._auth_post(
            f"http://localhost:{PORT}/api/v1/chat/completions", completion_payload
        )
        self.assertEqual(res.status_code, 200)
        self.assertIn("error", res.text)

        # Wait for span
        span_received = self._wait_for_span(span_name="chat.completions")

        self.assertIsNotNone(
            span_received, "Streaming error telemetry span was not received."
        )
        span, _ = self._extract_span(span_received)
        self.assertEqual(span["name"], "chat.completions")
        self.assertEqual(span["status"]["code"], 2)  # ERROR code
        # Ensure status message is captured
        self.assertIn("message", span["status"])
        self.assertTrue(len(span["status"]["message"]) > 0)


class ModelTypeTests(TelemetryTestBase):
    """Embeddings and reranker span coverage."""

    def test_013_embeddings_telemetry_span(self):
        """Verify embeddings request submits valid OpenInference embedding traces."""
        model_name = "nomic-embed-text-v1-GGUF"
        # Pull model first
        print(f"\n[TEST] Pulling embedding model {model_name}...")
        res = self._auth_post(
            f"http://localhost:{PORT}/api/v1/pull",
            {"model_name": model_name},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(res.status_code, 200, res.text)

        self._enable_telemetry(headers={"X-Test-Header": "Embeddings"})

        # Call embeddings
        embeddings_payload = {
            "model": model_name,
            "input": "This is a test of the embedding telemetry output.",
        }
        res = self._auth_post(
            f"http://localhost:{PORT}/api/v1/embeddings", embeddings_payload
        )
        self.assertEqual(res.status_code, 200, res.text)

        # Wait for span
        span_received = self._wait_for_span(span_name="embeddings")

        self.assertIsNotNone(
            span_received, "Embedding telemetry span was not received."
        )
        span, attrs = self._extract_span(span_received)
        self.assertEqual(span["name"], "embeddings")
        self.assertEqual(span["kind"], 2)

        self.assertEqual(attrs["openinference.span.kind"]["stringValue"], "EMBEDDING")
        self.assertEqual(attrs["embedding.model_name"]["stringValue"], model_name)
        self.assertIn("input.value", attrs)
        self.assertIn("output.value", attrs)
        self.assertIn("embedding.usage.prompt_tokens", attrs)
        self.assertIn("embedding.usage.total_tokens", attrs)
        self.assertGreaterEqual(attrs["embedding.usage.prompt_tokens"]["intValue"], 0)
        self.assertGreaterEqual(attrs["embedding.usage.total_tokens"]["intValue"], 0)

        # Standard OpenInference token count attributes
        self._assert_standard_token_counts(
            attrs,
            prompt_key="embedding.usage.prompt_tokens",
            total_key="embedding.usage.total_tokens",
        )

    def test_014_reranker_telemetry_span(self):
        """Verify reranker request submits valid OpenInference reranker traces."""
        model_name = "jina-reranker-v1-tiny-en-GGUF"
        # Pull model first
        print(f"\n[TEST] Pulling reranker model {model_name}...")
        res = self._auth_post(
            f"http://localhost:{PORT}/api/v1/pull",
            {"model_name": model_name},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(res.status_code, 200, res.text)

        self._enable_telemetry(headers={"X-Test-Header": "Reranker"})

        # Call reranking
        reranking_payload = {
            "model": model_name,
            "query": "What is lemonade?",
            "documents": [
                "Lemonade is a drink made of water, sugar, and lemon juice.",
                "Lemonade is a local LLM server.",
                "The quick brown fox jumps over the lazy dog.",
            ],
        }
        res = self._auth_post(
            f"http://localhost:{PORT}/api/v1/reranking", reranking_payload
        )
        self.assertEqual(res.status_code, 200, res.text)

        # Wait for span
        span_received = self._wait_for_span(span_name="reranking")

        self.assertIsNotNone(span_received, "Reranker telemetry span was not received.")
        span, attrs = self._extract_span(span_received)
        self.assertEqual(span["name"], "reranking")
        self.assertEqual(span["kind"], 2)

        self.assertEqual(attrs["openinference.span.kind"]["stringValue"], "RERANKER")
        self.assertEqual(attrs["reranker.model_name"]["stringValue"], model_name)
        self.assertIn("input.value", attrs)
        self.assertIn("output.value", attrs)
        self.assertIn("reranker.usage.prompt_tokens", attrs)
        self.assertIn("reranker.usage.total_tokens", attrs)
        self.assertGreaterEqual(attrs["reranker.usage.prompt_tokens"]["intValue"], 0)
        self.assertGreaterEqual(attrs["reranker.usage.total_tokens"]["intValue"], 0)

        # Standard OpenInference token count attributes
        self._assert_standard_token_counts(
            attrs,
            prompt_key="reranker.usage.prompt_tokens",
            total_key="reranker.usage.total_tokens",
        )


class ReliabilityTests(TelemetryTestBase):
    """Retry backoff and OTEL env-var header injection."""

    _pull_base_model = True

    def test_015_telemetry_retries(self):
        """Verify that telemetry spans are retried if the endpoint is temporarily unreachable."""
        # Find a free port but do NOT start a server on it yet
        inactive_port = find_free_port()

        # Configure telemetry pointing to this inactive port with 2 retries and 1.0s backoff base
        self._enable_telemetry(
            endpoint=f"http://127.0.0.1:{inactive_port}/v1/traces",
            max_retries=2,
            retry_backoff_base_s=1.0,
        )

        # Trigger completion - this will queue a telemetry task which will fail and begin backoff retry (1s delay)
        self._chat_completion("Trigger trace for retry.")

        # Wait 0.5s to ensure the first attempt fails and backoff starts
        time.sleep(0.5)

        # Start a temporary Mock OTLP server on that previously inactive port
        temp_collector = MockOTLPServer(("127.0.0.1", inactive_port), MockOTLPHandler)
        temp_thread = threading.Thread(target=temp_collector.serve_forever)
        temp_thread.daemon = True
        temp_thread.start()

        # The telemetry worker should retry (after 5s sleep) and succeed
        span_received = None
        try:
            span_received = self._wait_for_span(
                timeout=10.0, queue=temp_collector.request_queue
            )
        finally:
            temp_collector.shutdown()
            temp_collector.server_close()
            temp_thread.join()

        self.assertIsNotNone(
            span_received,
            "Span was not retried and delivered successfully to the newly active port.",
        )
        span, attrs = self._extract_span(span_received)
        self.assertEqual(span["name"], "chat.completions")

    def test_016_otel_exporter_headers(self):
        """Verify that OTEL_EXPORTER_OTLP_HEADERS environment variable is parsed and included in telemetry requests."""
        import subprocess
        from utils.test_models import get_default_lemond_binary

        # Set environment variable in python process
        env_key = "OTEL_EXPORTER_OTLP_HEADERS"
        original_env_val = os.environ.get(env_key)
        os.environ[env_key] = "X-Env-Header=EnvValue, Authorization=Bearer env_token"

        lemond_bin = get_default_lemond_binary()
        temp_port = find_free_port()

        # Start a temporary server process with the updated env
        env = os.environ.copy()
        proc = subprocess.Popen(
            [lemond_bin, "./build/temp_cache", "--port", str(temp_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=env,
        )

        try:
            # Wait for temporary server to start
            for i in range(30):
                try:
                    requests.get(
                        f"http://localhost:{temp_port}/api/v1/models", timeout=1
                    )
                    break
                except:
                    time.sleep(1)
            else:
                self.fail("Temporary lemond server failed to start")

            # Enable telemetry pointing to mock collector on the temporary server
            config_payload = {
                "telemetry": {
                    "enabled": True,
                    "otlp": {
                        "endpoint": f"http://127.0.0.1:{self.mock_port}/v1/traces",
                        "headers": {},
                    },
                }
            }
            res = self._auth_post(
                f"http://localhost:{temp_port}/api/v1/params", config_payload
            )
            self.assertEqual(res.status_code, 200, res.text)

            # Send completions request to the temporary server
            self._chat_completion("Hi.", port=temp_port)

            # Wait for mock collector to receive telemetry span
            span_received = self._wait_for_span()

            self.assertIsNotNone(
                span_received,
                "Telemetry span was not received by the OTLP mock receiver.",
            )

            # Assert headers case-insensitively
            headers = span_received["headers"]
            val_env = _get_header_value(headers, "x-env-header")
            val_auth = _get_header_value(headers, "authorization")

            self.assertIsNotNone(
                val_env, "x-env-header was not found in received headers"
            )
            self.assertIsNotNone(
                val_auth, "authorization header was not found in received headers"
            )
            self.assertEqual(val_env.lower(), "EnvValue".lower())
            self.assertEqual(val_auth.lower(), "Bearer env_token".lower())

        finally:
            # Terminate temporary server
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

            # Clean up os.environ
            if original_env_val is not None:
                os.environ[env_key] = original_env_val
            else:
                os.environ.pop(env_key, None)

    def test_016_config_telemetry_headers(self):
        """Verify that configuration-based telemetry headers are validated, sanitized, and disallowed keys are rejected."""
        # Enable telemetry with a mix of valid, invalid, and disallowed headers
        headers_payload = {
            "X-Valid-Header": "ValidValue",
            "Content-Type": "application/json",  # disallowed
            "Content-Length": "123",  # disallowed
            "X-Invalid\nHeader": "SomeValue",  # invalid key (LF in middle)
            "X-Invalid-Value": "SomeValue\0",  # invalid value (NUL)
        }
        self._enable_telemetry(headers=headers_payload)

        # Trigger completion to send a span
        payload = {
            "model": "builtin.nonexistent-model-name-error",
            "messages": [
                {"role": "user", "content": "Trigger trace for config headers test."}
            ],
        }
        self._auth_post(f"http://127.0.0.1:{PORT}/v1/chat/completions", payload)

        # Wait for mock collector to receive telemetry span
        span_received = self._wait_for_span()
        self.assertIsNotNone(
            span_received,
            "Telemetry span was not received by the OTLP mock receiver.",
        )

        headers = span_received["headers"]
        val_valid = _get_header_value(headers, "x-valid-header")
        val_content_type = _get_header_value(headers, "content-type")
        val_content_length = _get_header_value(headers, "content-length")
        val_invalid_key = _get_header_value(headers, "x-invalid\nheader")
        val_invalid_val = _get_header_value(headers, "x-invalid-value")

        self.assertIsNotNone(
            val_valid, "x-valid-header was not found in received headers"
        )
        self.assertEqual(val_valid, "ValidValue")

        # Disallowed overriding headers should not be populated
        self.assertNotEqual(val_content_type, "application/json")
        self.assertNotEqual(val_content_length, "123")
        self.assertIsNone(val_invalid_key, "invalid key with LF should be rejected")
        self.assertIsNone(val_invalid_val, "invalid value with NUL should be rejected")

    def test_017_non_retryable_errors(self):
        """Verify that telemetry spans are dropped immediately on non-retryable 4xx errors (e.g., 400)."""
        # Configure telemetry pointing to mock collector with 3 retries, but returning 400 Bad Request
        self._enable_telemetry(headers={"X-Test-Error": "400"}, max_retries=3)

        # Trigger completion - this will queue a telemetry task
        payload = {
            "model": "builtin.nonexistent-model-name-error",
            "messages": [
                {"role": "user", "content": "Trigger trace for non-retryable test."}
            ],
        }
        self._auth_post(f"http://127.0.0.1:{PORT}/v1/chat/completions", payload)

        # Wait to ensure request is made to the mock OTLP collector
        time.sleep(1.0)

        # Drain and count requests received by the mock collector
        requests_received = []
        while not self.mock_server.request_queue.empty():
            requests_received.append(self.mock_server.request_queue.get())

        # Assert that we received exactly 1 request (no retries occurred)
        self.assertEqual(
            len(requests_received),
            1,
            f"Expected exactly 1 request (immediate drop), but got {len(requests_received)}",
        )

    def test_018_queue_head_drop(self):
        """Verify that telemetry queue drops oldest spans (head-drop) when capacity is exceeded."""
        inactive_port = find_free_port()

        # Enable telemetry pointing to this inactive port with 1 retry, queue capacity 3, and 1.0s backoff base
        self._enable_telemetry(
            endpoint=f"http://127.0.0.1:{inactive_port}/v1/traces",
            max_retries=1,
            max_queue_capacity=3,
            retry_backoff_base_s=1.0,
        )

        payloads = [
            {
                "model": "builtin.nonexistent-model-name-error",
                "messages": [{"role": "user", "content": f"Trigger {i}"}],
            }
            for i in range(5)
        ]
        # Trigger 5 completions sequentially to overflow the queue deterministically
        for p in payloads:
            self._auth_post(f"http://127.0.0.1:{PORT}/v1/chat/completions", p)
            time.sleep(0.05)

        # Now start the mock OTLP collector on the inactive port
        temp_collector = MockOTLPServer(("127.0.0.1", inactive_port), MockOTLPHandler)
        temp_thread = threading.Thread(target=temp_collector.serve_forever)
        temp_thread.daemon = True
        temp_thread.start()

        try:
            # We expect to receive Span 0 and Spans 2 to 4.
            received_spans = []
            deadline = time.time() + 5.0
            while time.time() < deadline:
                while not temp_collector.request_queue.empty():
                    received_spans.append(temp_collector.request_queue.get())
                if len(received_spans) >= 4:
                    break
                time.sleep(0.05)

            # Extract prompt values
            input_values = []
            for req in received_spans:
                span, attrs = self._extract_span(req)
                val = attrs.get("input.value", {}).get("stringValue", "")
                input_values.append(val)

            # We should definitely have Span 0 (Trigger 0)
            self.assertTrue(
                any("Trigger 0" in val for val in input_values),
                "Span 0 should be present",
            )
            # We should NOT have Span 1 (Trigger 1)
            self.assertFalse(
                any("Trigger 1" in val for val in input_values),
                "Span 1 should have been dropped",
            )
            # We should have Spans 2 to 4
            for i in range(2, 5):
                self.assertTrue(
                    any(f"Trigger {i}" in val for val in input_values),
                    f"Span {i} should be present",
                )

        finally:
            temp_collector.shutdown()
            temp_collector.server_close()
            temp_thread.join()

    def test_019_telemetry_batching(self):
        """Verify that OTLP telemetry batching triggers on send_batch_size and batch_timeout_s."""
        # Enable telemetry with send_batch_size=3 and batch_timeout_s=2.0
        self._enable_telemetry(
            send_batch_size=3,
            batch_timeout_s=2.0,
        )

        # Trigger 2 completions and verify no requests are delivered to the mock server yet
        self._chat_completion("Hi 1.")
        self._chat_completion("Hi 2.")

        time.sleep(0.5)
        self.assertTrue(
            self.mock_server.request_queue.empty(),
            "Queue should be empty as send_batch_size=3 is not reached yet",
        )

        # Trigger a 3rd completion and verify exactly 1 request containing all 3 spans is delivered
        self._chat_completion("Hi 3.")

        batch_received = self._wait_for_span(timeout=5.0)
        self.assertIsNotNone(
            batch_received, "Batch request was not received by the mock server."
        )

        body = batch_received.get("body", {})
        rs = body.get("resourceSpans", [])
        self.assertEqual(len(rs), 1)
        scope_spans = rs[0].get("scopeSpans", [])
        self.assertEqual(len(scope_spans), 1)
        spans = scope_spans[0].get("spans", [])
        self.assertEqual(
            len(spans), 3, f"Expected 3 spans in the batch, got {len(spans)}"
        )

        # Clean the queue
        while not self.mock_server.request_queue.empty():
            self.mock_server.request_queue.get()

        # Trigger 1 completion, verify no request is delivered immediately
        self._chat_completion("Hi 4.")
        time.sleep(0.5)
        self.assertTrue(
            self.mock_server.request_queue.empty(),
            "Should not be delivered before batch_timeout_s expires",
        )

        # Wait for the timeout to elapse (2.0s + safety buffer) and verify it is delivered
        timeout_received = self._wait_for_span(timeout=3.0)
        self.assertIsNotNone(
            timeout_received, "Span was not delivered after batch_timeout_s."
        )

        body = timeout_received.get("body", {})
        rs = body.get("resourceSpans", [])
        self.assertEqual(len(rs), 1)
        scope_spans = rs[0].get("scopeSpans", [])
        self.assertEqual(len(scope_spans), 1)
        spans = scope_spans[0].get("spans", [])
        self.assertEqual(
            len(spans), 1, f"Expected 1 span in the timeout batch, got {len(spans)}"
        )

    def test_020_telemetry_flush(self):
        """Verify that OTLP telemetry force-flush API immediately dispatches queued spans."""
        # Enable telemetry with send_batch_size=10 and batch_timeout_s=10.0
        self._enable_telemetry(
            send_batch_size=10,
            batch_timeout_s=10.0,
        )

        # Trigger 2 completions
        self._chat_completion("Flush 1.")
        self._chat_completion("Flush 2.")

        time.sleep(0.5)
        # Assert that no requests are received by the mock server
        self.assertTrue(
            self.mock_server.request_queue.empty(),
            "Queue should be empty as batch/timeout triggers are not met yet",
        )

        # Send a POST request to /internal/telemetry/flush and verify it returns 200 OK
        headers = {}
        api_key = os.environ.get("LEMONADE_API_KEY")
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        res = requests.post(
            f"http://localhost:{PORT}/internal/telemetry/flush",
            headers=headers,
            timeout=5.0,
        )
        self.assertEqual(res.status_code, 200, res.text)
        self.assertEqual(res.json(), {"status": "flushed"})

        # Assert that exactly 1 request containing both spans is received by the mock server immediately
        flush_received = self._wait_for_span(timeout=2.0)
        self.assertIsNotNone(
            flush_received, "Flush request was not received by the mock server."
        )

        body = flush_received.get("body", {})
        rs = body.get("resourceSpans", [])
        self.assertEqual(len(rs), 1)
        scope_spans = rs[0].get("scopeSpans", [])
        self.assertEqual(len(scope_spans), 1)
        spans = scope_spans[0].get("spans", [])
        self.assertEqual(
            len(spans), 2, f"Expected 2 spans in the flushed batch, got {len(spans)}"
        )

        # Also double check that the spans match what we sent
        span1_attrs = {a["key"]: a["value"] for a in spans[0]["attributes"]}
        span2_attrs = {a["key"]: a["value"] for a in spans[1]["attributes"]}
        input_values = [
            span1_attrs.get("input.value", {}).get("stringValue", ""),
            span2_attrs.get("input.value", {}).get("stringValue", ""),
        ]
        self.assertTrue(
            any("Flush 1" in val for val in input_values), "Span 1 should be present"
        )
        self.assertTrue(
            any("Flush 2" in val for val in input_values), "Span 2 should be present"
        )


class MultiSemanticTests(TelemetryTestBase):
    """Multi-semantic tracing validation (openinference, otel_genai)."""

    _pull_base_model = True

    def test_021_otel_genai_only_telemetry_span(self):
        """Verify that when only 'otel_genai' is selected, spans only contain OTel GenAI attributes."""
        self._enable_telemetry(semantics=["otel_genai"])
        self._chat_completion("OTel GenAI test.")

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received)

        span, attrs = self._extract_span(span_received)
        self.assertEqual(span["name"], "chat.completions")

        # OTel GenAI attributes must be present
        self.assertEqual(attrs["gen_ai.operation.name"]["stringValue"], "chat")
        self.assertEqual(
            attrs["gen_ai.request.model"]["stringValue"], ENDPOINT_TEST_MODEL
        )
        self.assertEqual(attrs["gen_ai.provider.name"]["stringValue"], "lemonade")
        self.assertEqual(attrs["gen_ai.system"]["stringValue"], "lemonade")
        self.assertIn("gen_ai.usage.input_tokens", attrs)
        self.assertIn("gen_ai.usage.output_tokens", attrs)
        self.assertEqual(attrs["gen_ai.input.messages.0.role"]["stringValue"], "user")
        self.assertEqual(
            attrs["gen_ai.input.messages.0.content"]["stringValue"], "OTel GenAI test."
        )
        self.assertEqual(
            attrs["gen_ai.output.messages.0.role"]["stringValue"], "assistant"
        )
        self.assertTrue(
            len(attrs["gen_ai.output.messages.0.content"]["stringValue"]) > 0
        )

        # OpenInference attributes must NOT be present
        self.assertNotIn("openinference.span.kind", attrs)
        self.assertNotIn("llm.model_name", attrs)
        self.assertNotIn("input.value", attrs)
        self.assertNotIn("output.value", attrs)

    def test_022_multi_semantic_coexistence(self):
        """Verify that when both 'openinference' and 'otel_genai' are selected, both sets of attributes are sent."""
        self._enable_telemetry(semantics=["openinference", "otel_genai"])
        self._chat_completion("Both semantics test.")

        span_received = self._wait_for_span()
        self.assertIsNotNone(span_received)

        span, attrs = self._extract_span(span_received)

        # OTel GenAI attributes present
        self.assertEqual(attrs["gen_ai.operation.name"]["stringValue"], "chat")
        self.assertEqual(
            attrs["gen_ai.request.model"]["stringValue"], ENDPOINT_TEST_MODEL
        )
        self.assertEqual(attrs["gen_ai.input.messages.0.role"]["stringValue"], "user")
        self.assertEqual(
            attrs["gen_ai.input.messages.0.content"]["stringValue"],
            "Both semantics test.",
        )
        self.assertEqual(
            attrs["gen_ai.output.messages.0.role"]["stringValue"], "assistant"
        )
        self.assertTrue(
            len(attrs["gen_ai.output.messages.0.content"]["stringValue"]) > 0
        )

        # OpenInference attributes present
        self.assertEqual(attrs["openinference.span.kind"]["stringValue"], "LLM")
        self.assertEqual(attrs["llm.model_name"]["stringValue"], ENDPOINT_TEST_MODEL)
        self.assertEqual(
            attrs["input.value"]["stringValue"],
            '[{"content":"Both semantics test.","role":"user"}]',
        )


if __name__ == "__main__":
    run_server_tests(
        [
            ConfigTests,
            CoreTracingTests,
            TraceContextTests,
            PrivacyTests,
            ErrorTests,
            ModelTypeTests,
            ReliabilityTests,
            MultiSemanticTests,
        ],
        description="TELEMETRY INTEGRATION TESTS",
    )
