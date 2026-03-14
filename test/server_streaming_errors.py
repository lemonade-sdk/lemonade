"""
Streaming error termination tests for Lemonade Server.

Verifies that streaming responses terminate cleanly (sink.done() called) on
all code paths, preventing "incomplete chunked read" errors on the client.

Usage:
    python server_streaming_errors.py
    python server_streaming_errors.py --server-binary /path/to/lemonade-server
"""

import requests

from utils.server_base import ServerTestBase, run_server_tests
from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    TIMEOUT_DEFAULT,
    TIMEOUT_MODEL_OPERATION,
)


class StreamingErrorTests(ServerTestBase):
    """Tests that streaming responses terminate cleanly on all code paths."""

    def _post_streaming(self, model_name, messages=None, tools=None, timeout=TIMEOUT_DEFAULT):
        """Send a streaming chat/completions request and return the raw response."""
        if messages is None:
            messages = [{"role": "user", "content": "Say hello."}]

        payload = {
            "model": model_name,
            "messages": messages,
            "stream": True,
            "max_tokens": 20,
        }
        if tools is not None:
            payload["tools"] = tools

        return requests.post(
            f"http://localhost:{PORT}/api/v1/chat/completions",
            json=payload,
            stream=True,
            timeout=timeout,
        )

    def _consume_stream(self, response):
        """Consume all SSE lines; fails the test on ChunkedEncodingError."""
        lines = []
        try:
            for raw in response.iter_lines():
                if raw:
                    lines.append(raw.decode("utf-8") if isinstance(raw, bytes) else raw)
        except requests.exceptions.ChunkedEncodingError as exc:
            self.fail(f"Stream not properly terminated (sink.done() missing?): {exc}")
        return lines

    def test_001_nonexistent_model_stream_terminates_cleanly(self):
        """Streaming request for a model not in the catalog terminates cleanly."""
        response = self._post_streaming("NonExistentModel-XYZ-DoesNotExist-12345")
        lines = self._consume_stream(response)
        print(f"[OK] Non-existent model: stream closed cleanly ({len(lines)} line(s))")

    def test_002_invalid_model_name_stream_terminates_cleanly(self):
        """Streaming request with a malformed model name terminates cleanly."""
        response = self._post_streaming("org/repo:invalid-tag-does-not-exist")
        lines = self._consume_stream(response)
        print(f"[OK] Malformed model name: stream closed cleanly ({len(lines)} line(s))")

    def test_003_streaming_after_unload_terminates_cleanly(self):
        """Streaming request after unloading all models terminates cleanly."""
        unload_resp = requests.post(
            f"http://localhost:{PORT}/api/v1/unload",
            json={},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertIn(unload_resp.status_code, [200, 422])

        response = self._post_streaming(ENDPOINT_TEST_MODEL)
        lines = self._consume_stream(response)
        print(f"[OK] Post-unload: stream closed cleanly ({len(lines)} line(s))")

    def test_004_streaming_context_overflow_terminates_cleanly(self):
        """Context-overflow prompt causes backend non-200; stream must still terminate.

        This directly reproduces the original bug: the router starts a chunked
        HTTP response, llama.cpp returns 400 (prompt exceeds n_ctx), and without
        the fix sink.done() is never called, leaving the stream open.
        """
        load_resp = requests.post(
            f"http://localhost:{PORT}/api/v1/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertIn(load_resp.status_code, [200, 409])

        # ~5 000 tokens — well above the 2048-token context of Tiny-Test-Model-GGUF
        overflow_prompt = "The quick brown fox jumps over the lazy dog. " * 600

        response = self._post_streaming(
            ENDPOINT_TEST_MODEL,
            messages=[{"role": "user", "content": overflow_prompt}],
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        lines = self._consume_stream(response)
        print(f"[OK] Context overflow: stream closed cleanly ({len(lines)} line(s))")

    def test_005_streaming_with_many_tools_terminates_cleanly(self):
        """Streaming with 15 tools terminates cleanly (original bug report scenario)."""
        many_tools = [
            {
                "type": "function",
                "function": {
                    "name": f"tool_{i:02d}_action",
                    "description": f"Tool {i} that performs operation {i} on the target system.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "input": {"type": "string", "description": f"Input for tool {i}."},
                            "count": {"type": "integer", "description": f"Repeat count for tool {i}."},
                            "dry_run": {"type": "boolean", "description": "Simulate without executing."},
                        },
                        "required": ["input"],
                    },
                },
            }
            for i in range(15)
        ]

        response = self._post_streaming(
            ENDPOINT_TEST_MODEL,
            messages=[{"role": "user", "content": "List the available tools by name."}],
            tools=many_tools,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        lines = self._consume_stream(response)
        print(f"[OK] 15 tools: stream closed cleanly ({len(lines)} line(s))")

    def test_006_successful_stream_includes_done_marker(self):
        """Successful streaming response includes [DONE] and terminates cleanly."""
        load_resp = requests.post(
            f"http://localhost:{PORT}/api/v1/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertIn(load_resp.status_code, [200, 409])

        response = self._post_streaming(
            ENDPOINT_TEST_MODEL,
            messages=[{"role": "user", "content": "Say exactly: hello"}],
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        lines = self._consume_stream(response)

        self.assertTrue(
            any("[DONE]" in line for line in lines),
            f"[DONE] marker not found in stream. Lines: {lines[:10]}",
        )
        print(f"[OK] Happy path: [DONE] received, stream closed cleanly ({len(lines)} line(s))")


if __name__ == "__main__":
    run_server_tests(StreamingErrorTests, "STREAMING ERROR TERMINATION TESTS")
