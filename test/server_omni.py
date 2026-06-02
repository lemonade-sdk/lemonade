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
and verify that image generation and text-to-speech produce embedded media.
Image editing is out of scope here (the Lite collection's SD-Turbo component
is generation-only).

The wrapped server is the collection's chat (planner) component, llamacpp,
so --backend selects the llamacpp backend.

Usage:
    python server_omni.py --wrapped-server llamacpp --backend vulkan
"""

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


if __name__ == "__main__":
    run_server_tests(OmniTests, "OMNI COLLECTION TESTS", modality="omni")
