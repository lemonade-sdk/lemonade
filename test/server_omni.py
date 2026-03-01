"""
Integration tests for Lemonade Omni Mode endpoint.

Tests the /omni/chat endpoint which provides a multimodal omni loop
where a tool-calling LLM can orchestrate image generation, vision,
audio transcription, and text-to-speech.

Usage:
    python test/server_omni.py
    python test/server_omni.py --server-per-test
    python test/server_omni.py --server-binary /path/to/lemonade-server
"""

import json
import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    parse_args,
    get_config,
)
from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


# Omni endpoint URL
OMNI_ENDPOINT = f"http://localhost:{PORT}/v1/omni/chat"

# Smallest Qwen3 model for tests that need tool calling support
TOOL_CALLING_MODEL = "Qwen3-0.6B-GGUF"


class OmniEndpointTests(ServerTestBase):
    """Tests for the /omni/chat endpoint."""

    @classmethod
    def setUpClass(cls):
        """Set up class - start server."""
        super().setUpClass()

    def test_omni_chat_non_streaming_no_tools(self):
        """Test non-streaming omni/chat with a simple message (no tool calls expected)."""
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [
                {"role": "user", "content": "Say hello in exactly 3 words."}
            ],
            "stream": False,
            "omni": {
                "max_iterations": 3,
                "tools": [],
            },
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200, f"Response: {response.text}")
        data = response.json()

        # Should have standard OpenAI response fields
        self.assertIn("choices", data)
        self.assertGreater(len(data["choices"]), 0)
        self.assertIn("message", data["choices"][0])
        self.assertIn("content", data["choices"][0]["message"])

        # Should have omni_steps (empty since no tools were called)
        self.assertIn("omni_steps", data)
        self.assertIsInstance(data["omni_steps"], list)

    def test_omni_chat_streaming_basic(self):
        """Test streaming omni/chat returns SSE events."""
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [
                {"role": "user", "content": "Say hello."}
            ],
            "stream": True,
            "omni": {
                "max_iterations": 2,
                "tools": [],
            },
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )

        self.assertEqual(response.status_code, 200, f"Response: {response.text}")
        self.assertIn("text/event-stream", response.headers.get("Content-Type", ""))

        # Parse SSE events
        events = []
        for line in response.iter_lines(decode_unicode=True):
            if line and line.startswith("event: "):
                events.append(line[7:].strip())

        # Should have at least a response delta and done event
        self.assertTrue(
            any(e == "omni.response.delta" for e in events),
            f"Expected omni.response.delta event, got: {events}",
        )
        self.assertTrue(
            any(e == "omni.response.done" for e in events),
            f"Expected omni.response.done event, got: {events}",
        )

    def test_omni_chat_missing_model(self):
        """Test that missing model field returns 400."""
        payload = {
            "messages": [{"role": "user", "content": "Hello"}],
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(response.status_code, 400)
        data = response.json()
        self.assertIn("error", data)

    def test_omni_chat_invalid_json(self):
        """Test that invalid JSON returns 400."""
        response = requests.post(
            OMNI_ENDPOINT,
            data="not valid json",
            headers={"Content-Type": "application/json"},
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(response.status_code, 400)

    def test_omni_chat_invalid_model(self):
        """Test that a nonexistent model returns 404."""
        payload = {
            "model": "nonexistent-model-that-does-not-exist",
            "messages": [{"role": "user", "content": "Hello"}],
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(response.status_code, [404, 500])
        data = response.json()
        self.assertIn("error", data)

    def test_omni_chat_default_config(self):
        """Test that omni config defaults are applied when no omni field is provided."""
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [
                {"role": "user", "content": "Hello"}
            ],
            "stream": False,
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200, f"Response: {response.text}")
        data = response.json()
        self.assertIn("choices", data)
        self.assertIn("omni_steps", data)

    def test_omni_chat_with_all_tools_enabled(self):
        """Test that the omni endpoint accepts all tool types in the config."""
        payload = {
            "model": TOOL_CALLING_MODEL,
            "messages": [
                {"role": "user", "content": "What is 2+2?"}
            ],
            "stream": False,
            "omni": {
                "max_iterations": 2,
                "tools": [
                    "generate_image",
                    "describe_image",
                    "analyze_image",
                    "transcribe_audio",
                    "text_to_speech",
                    "edit_image",
                    "read_file",
                    "write_file",
                    "list_directory",
                    "web_search",
                    "list_models",
                    "load_model",
                ],
                "image_model": "SD-Turbo",
                "audio_model": "Whisper-Large-v3-Turbo-GGUF",
                "tts_model": "kokoro-v1",
            },
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200, f"Response: {response.text}")
        data = response.json()
        self.assertIn("choices", data)

    def test_omni_chat_max_iterations_respected(self):
        """Test that max_iterations limits the number of omni loop iterations."""
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [
                {"role": "user", "content": "Hello"}
            ],
            "stream": False,
            "omni": {
                "max_iterations": 1,
                "tools": [],
            },
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200, f"Response: {response.text}")
        data = response.json()
        self.assertIn("omni_steps", data)
        # With no tools and max_iterations=1, should have 0 steps
        self.assertEqual(len(data["omni_steps"]), 0)

    def test_omni_chat_route_prefixes(self):
        """Test that the omni endpoint is registered under all API prefixes."""
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "user", "content": "Hi"}],
            "stream": False,
            "omni": {"tools": []},
        }

        prefixes = [
            f"http://localhost:{PORT}/v1/omni/chat",
            f"http://localhost:{PORT}/api/v1/omni/chat",
            f"http://localhost:{PORT}/api/v0/omni/chat",
        ]

        for url in prefixes:
            response = requests.post(
                url,
                json=payload,
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(
                response.status_code,
                200,
                f"Failed for prefix {url}: {response.text}",
            )

    def test_omni_chat_custom_system_prompt(self):
        """Test that a custom system_prompt is accepted and replaces the default."""
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "user", "content": "Who are you?"}],
            "stream": False,
            "omni": {
                "system_prompt": "You are Bob, a friendly pirate. Always talk like a pirate.",
                "tools": [],
            },
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200, f"Response: {response.text}")
        data = response.json()
        self.assertIn("choices", data)
        self.assertGreater(len(data["choices"]), 0)
        self.assertIn("message", data["choices"][0])
        self.assertIn("content", data["choices"][0]["message"])

    def test_omni_chat_extra_tools_accepted(self):
        """Test that extra_tools definitions are accepted and merged with native tools."""
        payload = {
            "model": TOOL_CALLING_MODEL,
            "messages": [{"role": "user", "content": "Hello"}],
            "stream": False,
            "omni": {
                "tools": [],
                "extra_tools": [
                    {
                        "type": "function",
                        "function": {
                            "name": "query_database",
                            "description": "Query the customer database",
                            "parameters": {
                                "type": "object",
                                "properties": {
                                    "query": {
                                        "type": "string",
                                        "description": "SQL query to run",
                                    }
                                },
                                "required": ["query"],
                            },
                        },
                    }
                ],
                "tool_callback_url": "http://localhost:9999/tools",
                "tool_callback_timeout": 5,
            },
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200, f"Response: {response.text}")
        data = response.json()
        self.assertIn("choices", data)

    def test_omni_chat_extra_tools_without_callback_url(self):
        """Test that extra_tools without a callback URL still works.

        If the LLM calls the extra tool and no callback URL is set,
        it should get an 'Unknown tool' error in the tool result, but
        the request should not crash.
        """
        payload = {
            "model": TOOL_CALLING_MODEL,
            "messages": [{"role": "user", "content": "Say hello in 3 words."}],
            "stream": False,
            "omni": {
                "tools": [],
                "extra_tools": [
                    {
                        "type": "function",
                        "function": {
                            "name": "noop_tool",
                            "description": "A tool that does nothing",
                            "parameters": {
                                "type": "object",
                                "properties": {},
                            },
                        },
                    }
                ],
            },
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(response.status_code, 200, f"Response: {response.text}")
        data = response.json()
        self.assertIn("choices", data)

    def test_omni_streaming_step_events(self):
        """Test that streaming mode emits proper SSE event types."""
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [
                {"role": "user", "content": "Say hello."}
            ],
            "stream": True,
            "omni": {
                "max_iterations": 2,
                "tools": [],
            },
        }

        response = requests.post(
            OMNI_ENDPOINT,
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )

        self.assertEqual(response.status_code, 200)

        # Collect all SSE events and their data
        events_with_data = []
        current_event = None
        for line in response.iter_lines(decode_unicode=True):
            if not line:
                continue
            if line.startswith("event: "):
                current_event = line[7:].strip()
            elif line.startswith("data: ") and current_event:
                try:
                    data = json.loads(line[6:])
                    events_with_data.append((current_event, data))
                except json.JSONDecodeError:
                    pass

        # Verify we got a response.done event with omni_steps
        done_events = [
            (e, d) for e, d in events_with_data if e == "omni.response.done"
        ]
        self.assertGreater(
            len(done_events), 0, "Expected omni.response.done event"
        )
        self.assertIn("omni_steps", done_events[0][1])


if __name__ == "__main__":
    args = parse_args()
    run_server_tests(OmniEndpointTests)
