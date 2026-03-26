"""
Deterministic Anthropic integration tests for Lemonade.

This suite intentionally avoids duplicating llama.cpp's deep Anthropic
compatibility matrix. It focuses on Lemonade-owned integration behavior:
- Anthropic routes are reachable and delegate to llama.cpp
- Anthropic stream framing is preserved through Lemonade
- Lemonade-level request validation and error shaping remain stable
"""

import json
import requests

from utils.server_base import ServerTestBase, run_server_tests, parse_args
from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    TIMEOUT_DEFAULT,
    TIMEOUT_MODEL_OPERATION,
)

ANTHROPIC_BASE_URL = f"http://localhost:{PORT}"


class AnthropicIntegrationTests(ServerTestBase):
    """Lean Anthropic integration contract tests."""

    _model_pulled = False

    def ensure_model_pulled(self):
        """Ensure baseline model exists once for this test process."""
        if AnthropicIntegrationTests._model_pulled:
            return

        response = requests.post(
            f"{self.base_url}/pull",
            json={"model_name": ENDPOINT_TEST_MODEL, "stream": False},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        AnthropicIntegrationTests._model_pulled = True

    @staticmethod
    def _collect_sse_events(response):
        """Collect Anthropic SSE events as (event_name, payload_json) tuples."""
        events = []
        current_event = None
        done_markers = 0

        for raw_line in response.iter_lines():
            if not raw_line:
                continue

            line = raw_line.decode("utf-8")
            if line.startswith("event: "):
                current_event = line[len("event: ") :]
                continue

            if line.startswith("data: "):
                payload_text = line[len("data: ") :]
                if payload_text == "[DONE]":
                    done_markers += 1
                    continue
                payload = json.loads(payload_text)
                events.append((current_event, payload))

        return events, done_markers

    def test_001_messages_delegation_smoke(self):
        """/v1/messages should return stable Anthropic response shape."""
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
        self.assertIn("content", body)
        self.assertIsInstance(body["content"], list)
        self.assertIn("usage", body)
        self.assertIn("input_tokens", body["usage"])
        self.assertIn("output_tokens", body["usage"])

        # Guard against accidental OpenAI-schema leakage on Anthropic endpoint.
        self.assertNotIn("choices", body)
        self.assertNotIn("object", body)

    def test_002_count_tokens_delegation_smoke(self):
        """/v1/messages/count_tokens should return Anthropic token-count schema."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "user", "content": "Tokenize this."}],
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

    def test_003_streaming_framing_contract(self):
        """Anthropic streaming contract should survive Lemonade forwarding."""
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
        events, done_markers = self._collect_sse_events(response)

        self.assertGreater(len(events), 0)
        self.assertEqual(done_markers, 0, "Anthropic stream must not emit [DONE]")

        event_names = [name for name, _ in events if name]
        self.assertIn("message_start", event_names)
        self.assertIn("content_block_start", event_names)
        self.assertIn("content_block_delta", event_names)
        self.assertIn("content_block_stop", event_names)
        self.assertIn("message_delta", event_names)
        self.assertIn("message_stop", event_names)

    def test_004_missing_model_validation_contract(self):
        """Lemonade should return Anthropic-shaped client error for missing model."""
        payload = {
            "max_tokens": 16,
            "messages": [{"role": "user", "content": "Hi"}],
        }

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(response.status_code, 400)
        body = response.json()
        self.assertEqual(body.get("type"), "error")
        self.assertIn("error", body)
        self.assertEqual(body["error"].get("type"), "invalid_request_error")

    def test_005_missing_messages_validation_contract(self):
        """Lemonade should return Anthropic-shaped client error for missing messages."""
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 16,
        }

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(response.status_code, 400)
        body = response.json()
        self.assertEqual(body.get("type"), "error")
        self.assertIn("error", body)
        self.assertEqual(body["error"].get("type"), "invalid_request_error")

    def test_006_backend_error_passthrough_messages(self):
        """Backend-origin Anthropic errors should not be wrapped as backend_error."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "max_tokens": 16,
            "messages": [{"role": "invalid_role", "content": "Hi"}],
        }

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertGreaterEqual(response.status_code, 400)
        body = response.json()
        self.assertIn("error", body)
        self.assertIsInstance(body["error"], dict)
        self.assertNotEqual(body["error"].get("type"), "backend_error")

    def test_007_backend_error_passthrough_count_tokens(self):
        """count_tokens backend-origin errors should preserve Anthropic schema."""
        self.ensure_model_pulled()

        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "invalid_role", "content": "Hi"}],
        }

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages/count_tokens",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertGreaterEqual(response.status_code, 400)
        body = response.json()
        self.assertIn("error", body)
        self.assertIsInstance(body["error"], dict)
        self.assertNotEqual(body["error"].get("type"), "backend_error")

    def test_008_messages_updates_stats_telemetry(self):
        """Non-streaming Anthropic messages should update /v1/stats telemetry."""
        self.ensure_model_pulled()

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "max_tokens": 24,
                "messages": [{"role": "user", "content": "Say hello"}],
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        stats = requests.get(f"{self.base_url}/stats", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(stats.status_code, 200)
        data = stats.json()
        self.assertGreater(data.get("input_tokens", 0), 0)
        self.assertGreater(data.get("output_tokens", 0), 0)

    def test_009_responses_updates_stats_telemetry(self):
        """Non-streaming Responses API should update /v1/stats telemetry."""
        self.ensure_model_pulled()

        response = requests.post(
            f"{self.base_url}/responses",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "input": [{"role": "user", "content": "Say hello"}],
                "max_output_tokens": 24,
                "stream": False,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        stats = requests.get(f"{self.base_url}/stats", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(stats.status_code, 200)
        data = stats.json()
        self.assertGreater(data.get("input_tokens", 0), 0)
        self.assertGreater(data.get("output_tokens", 0), 0)

    def test_010_streaming_messages_updates_stats_telemetry(self):
        """Streaming Anthropic messages should update /v1/stats telemetry."""
        self.ensure_model_pulled()

        # Reset runtime state so we validate telemetry from this request only.
        unload = requests.post(
            f"{self.base_url}/unload",
            json={},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertIn(unload.status_code, [200, 404])

        load = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(load.status_code, 200)

        response = requests.post(
            f"{ANTHROPIC_BASE_URL}/v1/messages",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "max_tokens": 24,
                "stream": True,
                "messages": [{"role": "user", "content": "Say hello"}],
            },
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200)

        # Drain stream to completion so telemetry can be finalized.
        for _ in response.iter_lines():
            pass

        stats = requests.get(f"{self.base_url}/stats", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(stats.status_code, 200)
        data = stats.json()
        self.assertGreater(data.get("input_tokens", 0), 0)
        self.assertGreater(data.get("output_tokens", 0), 0)
        self.assertGreater(data.get("time_to_first_token", 0.0), 0.0)
        self.assertGreater(data.get("tokens_per_second", 0.0), 0.0)

    def test_011_streaming_responses_updates_stats_telemetry(self):
        """Streaming Responses API should update /v1/stats telemetry."""
        self.ensure_model_pulled()

        unload = requests.post(
            f"{self.base_url}/unload",
            json={},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertIn(unload.status_code, [200, 404])

        load = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(load.status_code, 200)

        response = requests.post(
            f"{self.base_url}/responses",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "input": [{"role": "user", "content": "Say hello"}],
                "max_output_tokens": 24,
                "stream": True,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200)

        for _ in response.iter_lines():
            pass

        stats = requests.get(f"{self.base_url}/stats", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(stats.status_code, 200)
        data = stats.json()
        self.assertGreater(data.get("input_tokens", 0), 0)
        self.assertGreater(data.get("output_tokens", 0), 0)
        self.assertGreater(data.get("time_to_first_token", 0.0), 0.0)
        self.assertGreater(data.get("tokens_per_second", 0.0), 0.0)


if __name__ == "__main__":
    parse_args(modality="llm")
    run_server_tests(
        AnthropicIntegrationTests,
        "ANTHROPIC INTEGRATION TESTS",
        modality="llm",
        default_wrapped_server="llamacpp",
    )
