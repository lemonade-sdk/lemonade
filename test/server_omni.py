"""
Server-side Omni collection orchestration tests for Lemonade Server.

Omni "collection" models (recipe "collection.omni") bundle a chat LLM with
image and speech components. When a plain OpenAI /chat/completions request
targets the collection name, the server runs an internal tool-calling loop,
executes the omni tools (image generation, text-to-speech) against the
matching components, and embeds the resulting media into the assistant
message as data-URIs:

- images -> markdown ![generated image](data:image/png;base64,...)
- speech -> <audio>data:audio/mpeg;base64,...</audio>

These tests drive a conversation against the LMX-Omni-5.5B-Lite collection
and verify that image generation and text-to-speech produce embedded media,
and that client-supplied (app) tools are merged with the omni tools and
passed back to the caller rather than executed server-side. Image editing is
out of scope here (the Lite collection's SD-Turbo component is generation-only).

The wrapped server is the collection's chat (planner) component, llamacpp,
so --backend selects the llamacpp backend.

Usage:
    python server_omni.py --wrapped-server llamacpp --backend vulkan
"""

import base64
import struct
import zlib

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    requests,
    PORT,
)
from utils.capabilities import (
    skip_if_unsupported,
    get_test_model,
)
from utils.test_models import (
    TIMEOUT_MODEL_OPERATION,
)


class OmniTests(ServerTestBase):
    """
    Tests for server-side Omni collection orchestration.

    Each test is decorated with @skip_if_unsupported() to skip
    features not supported by the current wrapped server.
    """

    # Track if the collection has been pulled (persists across tests)
    _model_pulled = False

    @classmethod
    def setUpClass(cls):
        """Verify server, apply runtime config, and pre-pull the collection."""
        super().setUpClass()

        # Download the whole collection up front so /chat/completions does not
        # trigger a download mid-inference. A bare pull of a registered
        # collection cascades to all of its components.
        cls._ensure_model_pulled()

    @classmethod
    def _ensure_model_pulled(cls):
        """Ensure the collection (and its components) are pulled (once)."""
        if cls._model_pulled:
            return

        model = get_test_model("omni")
        print(f"\n[SETUP] Ensuring {model} and its components are pulled...")
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/pull",
            json={"model_name": model},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        if response.status_code == 200:
            print(f"[SETUP] {model} is ready")
            cls._model_pulled = True
        else:
            print(f"[SETUP] Warning: pull returned {response.status_code}")

    @staticmethod
    def _solid_color_png_data_uri(r, g, b, size=256):
        """Build a data-URI for a solid RGB PNG (stdlib only, no Pillow).

        Used to upload a known image to the collection planner so a test can
        assert the planner actually saw it (by naming the color back).
        """

        def _chunk(tag, data):
            return (
                struct.pack(">I", len(data))
                + tag
                + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
            )

        ihdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)  # 8-bit RGB
        row = b"\x00" + bytes((r, g, b)) * size  # filter byte + pixels
        raw = row * size
        png = (
            b"\x89PNG\r\n\x1a\n"
            + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", zlib.compress(raw, 9))
            + _chunk(b"IEND", b"")
        )
        return "data:image/png;base64," + base64.b64encode(png).decode()

    @staticmethod
    def _assert_contains(testcase, content, needle, label):
        """Assert embedded media of the given kind is present in the content."""
        print(f"Response ({label}): {content[:200]}")
        testcase.assertIsNotNone(content, "Response should have content")
        testcase.assertIn(
            needle,
            content,
            f"Expected an embedded {label} data-URI ('{needle}') in the response",
        )

    # =========================================================================
    # CONVERSATION TESTS
    # =========================================================================

    @skip_if_unsupported("collection_chat")
    def test_001_image_then_speech_conversation(self):
        """A multi-turn conversation produces an image, then spoken audio.

        Turn 1 asks for a picture and expects an inline image data-URI. The
        assistant turn (including the embedded image) is fed back as history —
        mirroring how a frontend echoes media — and turn 2 asks the model to
        speak, expecting an inline audio data-URI.
        """
        client = self.get_openai_client()
        model = self.get_test_model("omni")

        messages = [{"role": "user", "content": "Draw a red apple on a table."}]
        completion = client.chat.completions.create(
            model=model,
            messages=messages,
            stream=False,
        )
        image_content = completion.choices[0].message.content
        self._assert_contains(self, image_content, "data:image/", "image")

        # Echo the assistant turn back as history (frontends re-send full
        # history including the embedded media), then ask for speech.
        messages.append({"role": "assistant", "content": image_content})
        messages.append(
            {"role": "user", "content": "Now say 'here is your apple' out loud."}
        )
        completion = client.chat.completions.create(
            model=model,
            messages=messages,
            stream=False,
        )
        speech_content = completion.choices[0].message.content
        self._assert_contains(self, speech_content, "data:audio/", "audio")

    @skip_if_unsupported("collection_chat_streaming")
    def test_002_image_generation_streaming(self):
        """Streaming image generation delivers the image as a content delta."""
        client = self.get_openai_client()
        model = self.get_test_model("omni")

        stream = client.chat.completions.create(
            model=model,
            messages=[{"role": "user", "content": "Draw a red apple on a table."}],
            stream=True,
        )

        complete_response = ""
        chunk_count = 0
        for chunk in stream:
            if (
                chunk.choices
                and chunk.choices[0].delta
                and chunk.choices[0].delta.content is not None
            ):
                complete_response += chunk.choices[0].delta.content
                chunk_count += 1

        print(f"Received {chunk_count} content chunks")
        self.assertGreater(
            chunk_count, 1, f"Should have multiple chunks, got {chunk_count}"
        )
        self._assert_contains(self, complete_response, "data:image/", "image")

    @skip_if_unsupported("collection_chat")
    def test_003_mixed_omni_and_app_tools(self):
        """A mixed turn runs an omni tool server-side AND hands an app tool back.

        A collection request may carry the client's own `tools` alongside the
        server-injected omni tools. The server must do both things in the same
        request: resolve omni tool calls internally (embedding the media in the
        assistant content) AND return any app tool call — which it knows nothing
        about — to the client as a finish_reason: "tool_calls" response to
        execute and resume.

        We prompt for both an image (the omni `generate_image` tool, reliably
        exercised by the other tests) and a weather lookup (an app-only
        `get_current_weather` tool with no matching component). The orchestrator
        accumulates artifacts across loop iterations, so regardless of whether
        the planner emits both calls in one turn or across turns, the final
        response should carry the embedded image and the passed-back app call.
        """
        client = self.get_openai_client()
        model = self.get_test_model("omni")

        app_tools = [
            {
                "type": "function",
                "function": {
                    "name": "get_current_weather",
                    "description": "Get the current weather for a city.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "city": {
                                "type": "string",
                                "description": "The city to look up.",
                            },
                        },
                        "required": ["city"],
                    },
                },
            }
        ]

        completion = client.chat.completions.create(
            model=model,
            messages=[
                {
                    "role": "user",
                    "content": (
                        "Draw a red apple on a table, and also call the "
                        "get_current_weather tool for Paris."
                    ),
                }
            ],
            tools=app_tools,
            stream=False,
        )

        choice = completion.choices[0]
        print(f"finish_reason: {choice.finish_reason}")

        # The omni tool was executed server-side: its image is embedded in the
        # assistant content.
        self._assert_contains(self, choice.message.content, "data:image/", "image")

        # The app tool was NOT executed: it is handed back for the client to run.
        self.assertEqual(
            choice.finish_reason,
            "tool_calls",
            "App tool calls must be returned for the client to execute",
        )
        tool_calls = choice.message.tool_calls
        self.assertTrue(tool_calls, "Expected the app tool call to be passed back")
        called_names = [tc.function.name for tc in tool_calls]
        print(f"Returned tool calls: {called_names}")
        self.assertIn(
            "get_current_weather",
            called_names,
            "The server must pass the app tool call back unexecuted",
        )

    @skip_if_unsupported("collection_chat_streaming")
    def test_004_mixed_omni_and_app_tools_streaming(self):
        """Streaming variant of the mixed omni + app-tool turn.

        Same contract as test_003 but with stream: true. The omni image arrives
        as a content delta and the app tool call is returned in the streamed
        deltas. Streaming tool-call deltas MUST carry a per-call integer
        `index` (OpenAI streaming shape) so clients/SDKs can merge them; a
        passthrough of the non-streaming message.tool_calls objects omits it and
        breaks reconstruction. This test asserts that index is present and that
        the call reconstructs correctly.
        """
        client = self.get_openai_client()
        model = self.get_test_model("omni")

        app_tools = [
            {
                "type": "function",
                "function": {
                    "name": "get_current_weather",
                    "description": "Get the current weather for a city.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "city": {
                                "type": "string",
                                "description": "The city to look up.",
                            },
                        },
                        "required": ["city"],
                    },
                },
            }
        ]

        stream = client.chat.completions.create(
            model=model,
            messages=[
                {
                    "role": "user",
                    "content": (
                        "Draw a red apple on a table, and also call the "
                        "get_current_weather tool for Paris."
                    ),
                }
            ],
            tools=app_tools,
            stream=True,
        )

        content = ""
        finish_reason = None
        # Reconstruct tool calls from streamed deltas, keyed by their index.
        tool_calls_by_index = {}
        for chunk in stream:
            if not chunk.choices:
                continue
            choice = chunk.choices[0]
            if choice.finish_reason:
                finish_reason = choice.finish_reason
            delta = choice.delta
            if not delta:
                continue
            if delta.content:
                content += delta.content
            for tc in delta.tool_calls or []:
                # The streaming contract: every tool-call delta must carry an
                # integer index so the client can merge fragments by slot.
                self.assertIsInstance(
                    tc.index,
                    int,
                    "Streamed tool-call deltas must include an integer 'index'",
                )
                slot = tool_calls_by_index.setdefault(
                    tc.index, {"name": "", "arguments": ""}
                )
                if tc.function and tc.function.name:
                    slot["name"] += tc.function.name
                if tc.function and tc.function.arguments:
                    slot["arguments"] += tc.function.arguments

        print(f"finish_reason: {finish_reason}")
        print(f"Reconstructed tool calls: {tool_calls_by_index}")

        # Omni tool ran server-side: its image arrived as a content delta.
        self._assert_contains(self, content, "data:image/", "image")

        # App tool was handed back, reconstructable from the streamed deltas.
        self.assertEqual(
            finish_reason,
            "tool_calls",
            "App tool calls must be returned for the client to execute",
        )
        called_names = [slot["name"] for slot in tool_calls_by_index.values()]
        self.assertIn(
            "get_current_weather",
            called_names,
            "The server must pass the app tool call back unexecuted",
        )

    @skip_if_unsupported("collection_chat")
    def test_005_image_input_passthrough(self):
        """An uploaded image must reach the vision-capable planner.

        Regression test for the collection path stripping user `image_url`
        parts to a text placeholder ("[User provided image #N]") and only
        keeping the base64 as an edit source. With that bug a plain
        "what's in this image?" question never reached the planner's vision
        encoder, so only edit-style flows could use an uploaded image.

        We upload a solid-green image and ask for its dominant color. The
        planner (Qwen3.5-4B-MTP, a vision model) can only answer "green" if
        the image_url was passed through; with the placeholder it is blind to
        the pixels and cannot name the color.
        """
        client = self.get_openai_client()
        model = self.get_test_model("omni")

        image_uri = self._solid_color_png_data_uri(0, 255, 0)
        completion = client.chat.completions.create(
            model=model,
            messages=[
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "text",
                            "text": (
                                "What is the dominant color of this image? "
                                "Answer with a single color word."
                            ),
                        },
                        {"type": "image_url", "image_url": {"url": image_uri}},
                    ],
                }
            ],
            stream=False,
        )

        content = completion.choices[0].message.content or ""
        print(f"Response (image input): {content[:200]}")
        self.assertIn(
            "green",
            content.lower(),
            "Planner must receive the uploaded image_url and identify it as green; "
            "a text placeholder leaves it blind to the pixels",
        )


if __name__ == "__main__":
    run_server_tests(OmniTests, "OMNI COLLECTION TESTS", modality="omni")
