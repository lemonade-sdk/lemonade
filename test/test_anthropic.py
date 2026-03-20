"""
Anthropic Messages API compatibility tests for Lemonade Server.

This suite is intentionally broad and mirrors upstream llama.cpp Anthropic test
coverage patterns: basic messages, streaming event structure, tool use/result
paths, parameter handling, and token counting.

Usage:
    python test_anthropic.py
    python test_anthropic.py --server-per-test
    python test_anthropic.py --wrapped-server llamacpp --backend vulkan
"""

import json
import requests

from utils.server_base import ServerTestBase, run_server_tests, parse_args
from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    SAMPLE_TOOL,
    TIMEOUT_DEFAULT,
    TIMEOUT_MODEL_OPERATION,
)

ANTHROPIC_BASE_URL = f"http://localhost:{PORT}"


class AnthropicApiTests(ServerTestBase):
    """Anthropic-compatible API tests."""

    _model_pulled = False

    def ensure_model_pulled(self):
        """Ensure the baseline test model is available."""
        if not AnthropicApiTests._model_pulled:
            response = requests.post(
                f"{self.base_url}/pull",
                json={"model_name": ENDPOINT_TEST_MODEL, "stream": False},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(response.status_code, 200)
            AnthropicApiTests._model_pulled = True

    @staticmethod
    def _collect_sse_events(response):
        """Parse Anthropic SSE payload into event names + data objects."""
        events = []
        current_event = None

        for raw_line in response.iter_lines():
            if not raw_line:
                continue

            line = raw_line.decode("utf-8")
            if line.startswith("event: "):
                current_event = line[len("event: ") :]
                continue

            if line.startswith("data: "):
                payload = json.loads(line[len("data: ") :])
                events.append((current_event, payload))

        return events

    def test_001_messages_basic(self):
        """Basic non-streaming message response shape."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 32,
            "messages": [{"role": "user", "content": "Say hello"}],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertEqual(body.get("type"), "message")
        self.assertEqual(body.get("role"), "assistant")
        self.assertEqual(body.get("model"), ENDPOINT_TEST_MODEL)
        self.assertIsInstance(body.get("content"), list)
        self.assertGreater(len(body["content"]), 0)
        self.assertIn("usage", body)
        self.assertIn("input_tokens", body["usage"])
        self.assertIn("output_tokens", body["usage"])

    def test_002_messages_system_array(self):
        """System prompt array should be accepted."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 32,
            "system": [{"type": "text", "text": "You are concise."}],
            "messages": [{"role": "user", "content": "Hello"}],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertEqual(body.get("type"), "message")

    def test_003_messages_content_blocks(self):
        """Multipart text blocks should be accepted."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 32,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": "What is"},
                        {"type": "text", "text": " the answer?"},
                    ],
                }
            ],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json().get("type"), "message")

    def test_004_messages_streaming_contract(self):
        """Streaming should emit Anthropic event types and no [DONE] sentinel."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 40,
            "stream": True,
            "messages": [{"role": "user", "content": "Say hello"}],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )

        self.assertEqual(response.status_code, 200)
        events = self._collect_sse_events(response)
        self.assertGreater(len(events), 0)

        event_names = [name for name, _ in events if name]
        self.assertIn("message_start", event_names)
        self.assertIn("content_block_start", event_names)
        self.assertIn("content_block_delta", event_names)
        self.assertIn("content_block_stop", event_names)
        self.assertIn("message_delta", event_names)
        self.assertIn("message_stop", event_names)

    def test_005_messages_streaming_has_message_start_shape(self):
        """message_start payload should include Anthropic message object."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 24,
            "stream": True,
            "messages": [{"role": "user", "content": "Hi"}],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )

        self.assertEqual(response.status_code, 200)
        events = self._collect_sse_events(response)
        message_start_payload = next(
            data for name, data in events if name == "message_start"
        )

        self.assertIn("message", message_start_payload)
        self.assertEqual(message_start_payload["message"].get("role"), "assistant")
        self.assertIn("usage", message_start_payload["message"])

    def test_006_messages_tool_use_request_accepted(self):
        """Tool schema + tool_choice should be accepted and return message response."""
        self.ensure_model_pulled()

        anthropic_tool = {
            "name": SAMPLE_TOOL["function"]["name"],
            "description": SAMPLE_TOOL["function"].get("description", ""),
            "input_schema": SAMPLE_TOOL["function"].get("parameters", {}),
        }

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 80,
            "tools": [anthropic_tool],
            "tool_choice": {"type": "any"},
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "text",
                            "text": "Use calculator_calculate with expression 1+1",
                        }
                    ],
                }
            ],
        }

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertEqual(body.get("type"), "message")
        self.assertIsInstance(body.get("content"), list)

    def test_007_messages_tool_result_input_accepted(self):
        """tool_result content blocks should be accepted without server error."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 80,
            "messages": [
                {
                    "role": "assistant",
                    "content": [
                        {
                            "type": "tool_use",
                            "id": "toolu_1",
                            "name": "calculator_calculate",
                            "input": {"expression": "1+1"},
                        }
                    ],
                },
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "tool_result",
                            "tool_use_id": "toolu_1",
                            "content": [{"type": "text", "text": "2"}],
                        }
                    ],
                },
            ],
        }

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json().get("type"), "message")

    def test_008_messages_missing_model_returns_400(self):
        """Missing model should be rejected."""
        payload = {
            "max_tokens": 16,
            "messages": [{"role": "user", "content": "Hi"}],
        }

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertGreaterEqual(response.status_code, 400)
        self.assertLess(response.status_code, 500)

    def test_009_messages_missing_messages_returns_400(self):
        """Missing messages should be rejected with client error."""
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 16,
        }

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertGreaterEqual(response.status_code, 400)
        self.assertLess(response.status_code, 500)

    def test_010_count_tokens_basic(self):
        """/v1/messages/count_tokens should return input token count only."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "user", "content": "Hello world"}],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages/count_tokens",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertIn("input_tokens", body)
        self.assertIsInstance(body["input_tokens"], int)
        self.assertGreater(body["input_tokens"], 0)
        self.assertNotIn("output_tokens", body)

    def test_011_count_tokens_with_system_prompt(self):
        """/count_tokens should include system prompt tokens."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "system": "You are helpful.",
            "messages": [{"role": "user", "content": "Hello"}],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages/count_tokens",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(response.status_code, 200)
        self.assertGreater(response.json()["input_tokens"], 0)

    def test_012_count_tokens_without_max_tokens(self):
        """/count_tokens must not require max_tokens."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "user", "content": "Tokenize this"}],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages/count_tokens",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(response.status_code, 200)
        self.assertIn("input_tokens", response.json())

    def test_013_streaming_message_delta_contains_stop_reason(self):
        """message_delta should include stop_reason and usage."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 24,
            "stream": True,
            "messages": [{"role": "user", "content": "Say hello"}],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )

        self.assertEqual(response.status_code, 200)
        events = self._collect_sse_events(response)
        delta = next(data for name, data in events if name == "message_delta")

        self.assertIn("delta", delta)
        self.assertIn("stop_reason", delta["delta"])
        self.assertIn("usage", delta)

    def test_014_beta_query_param_is_accepted(self):
        """?beta=true should not fail request."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 16,
            "messages": [{"role": "user", "content": "Hi"}],
        }
        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages?beta=true",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json().get("type"), "message")


if __name__ == "__main__":
    parse_args(modality="llm")
    run_server_tests(
        AnthropicApiTests,
        "ANTHROPIC API TESTS",
        modality="llm",
        default_wrapped_server="llamacpp",
    )
