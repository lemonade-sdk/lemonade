"""
Parakeet audio transcription tests for Lemonade Server.

Tests the /audio/transcriptions endpoint with parakeet.cpp models.

Usage:
    python test/server_parakeet.py --wrapped-server parakeetcpp --backend cpu
    python test/server_parakeet.py --wrapped-server parakeetcpp --backend vulkan
    python test/server_parakeet.py --cli-binary /path/to/lemonade
"""

import os
import tempfile
import urllib.request

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    get_config,
)
from utils.capabilities import (
    skip_if_unsupported,
    get_test_model,
)
from utils.test_models import (
    TEST_AUDIO_URL,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


def _get_parakeet_model():
    return get_test_model("audio")


def _get_parakeetcpp_backend():
    config = get_config()
    wrapped_server = config.get("wrapped_server")
    backend = config.get("backend")
    if wrapped_server == "parakeetcpp" and backend:
        return backend
    return None


class ParakeetTests(ServerTestBase):
    """Tests for Parakeet audio transcription via parakeet-server."""

    _test_audio_path = None

    @classmethod
    def setUpClass(cls):
        super().setUpClass()

        cls._test_audio_path = os.path.join(tempfile.gettempdir(), "test_speech.wav")

        if not os.path.exists(cls._test_audio_path):
            print(f"\n[INFO] Downloading test audio file from {TEST_AUDIO_URL}")
            try:
                urllib.request.urlretrieve(TEST_AUDIO_URL, cls._test_audio_path)
                print(f"[OK] Downloaded to {cls._test_audio_path}")
            except Exception as e:
                print(f"[ERROR] Failed to download test audio: {e}")
                raise

    @classmethod
    def tearDownClass(cls):
        if cls._test_audio_path and os.path.exists(cls._test_audio_path):
            try:
                os.remove(cls._test_audio_path)
            except Exception:
                pass
        super().tearDownClass()

    def _load_parakeet_model_or_fail(self):
        model = _get_parakeet_model()
        backend = _get_parakeetcpp_backend()

        if backend:
            print(f"[INFO] Loading model with {backend} backend")
            load_response = requests.post(
                f"{self.base_url}/load",
                json={"model_name": model, "parakeetcpp_backend": backend},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(
                load_response.status_code,
                200,
                f"Failed to load {model} with {backend} backend: {load_response.text}",
            )

        return model

    @skip_if_unsupported("transcription")
    def test_001_transcription_basic(self):
        """Test basic audio transcription with Parakeet."""
        self.assertTrue(
            os.path.exists(self._test_audio_path),
            f"Test audio file not found at {self._test_audio_path}",
        )

        model = self._load_parakeet_model_or_fail()
        backend = _get_parakeetcpp_backend()

        with open(self._test_audio_path, "rb") as audio_file:
            files = {"file": ("test_speech.wav", audio_file, "audio/wav")}
            data = {"model": model, "response_format": "json"}

            backend_msg = f" ({backend} backend)" if backend else ""
            print(f"[INFO] Sending transcription request{backend_msg}")
            response = requests.post(
                f"{self.base_url}/audio/transcriptions",
                files=files,
                data=data,
                timeout=TIMEOUT_MODEL_OPERATION,
            )

        self.assertEqual(
            response.status_code,
            200,
            f"Transcription failed with status {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertIn("text", result, "Response should contain 'text' field")
        self.assertIsInstance(result["text"], str)
        self.assertGreater(len(result["text"]), 0, "Transcription should not be empty")

        print(f"[OK] Transcription result: {result['text']}")

    @skip_if_unsupported("transcription")
    def test_002_transcription_verbose_json(self):
        """Test verbose_json response format."""
        model = self._load_parakeet_model_or_fail()

        with open(self._test_audio_path, "rb") as audio_file:
            files = {"file": ("test_speech.wav", audio_file, "audio/wav")}
            data = {"model": model, "response_format": "verbose_json"}

            response = requests.post(
                f"{self.base_url}/audio/transcriptions",
                files=files,
                data=data,
                timeout=TIMEOUT_MODEL_OPERATION,
            )

        self.assertEqual(
            response.status_code,
            200,
            f"verbose_json transcription failed: {response.text}",
        )

        result = response.json()
        self.assertIn("text", result)
        self.assertGreater(len(result["text"]), 0)
        print(f"[OK] verbose_json result: {result['text']}")

    def test_003_transcription_missing_file_error(self):
        """Test error handling when file is missing."""
        model = _get_parakeet_model()

        response = requests.post(
            f"{self.base_url}/audio/transcriptions",
            data={"model": model},
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing file, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without file: {response.status_code}")

    def test_004_transcription_missing_model_error(self):
        """Test error handling when model is missing."""
        with open(self._test_audio_path, "rb") as audio_file:
            response = requests.post(
                f"{self.base_url}/audio/transcriptions",
                files={"file": ("test_speech.wav", audio_file, "audio/wav")},
                timeout=TIMEOUT_DEFAULT,
            )

        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without model: {response.status_code}")

    @skip_if_unsupported("transcription")
    def test_005_chat_completion_unsupported(self):
        """Parakeet models must reject chat completion requests."""
        model = self._load_parakeet_model_or_fail()

        response = requests.post(
            f"{self.base_url}/chat/completions",
            json={"model": model, "messages": [{"role": "user", "content": "Hello"}]},
            timeout=TIMEOUT_DEFAULT,
        )

        result = response.json()
        self.assertIn(
            "error",
            result,
            "Chat completion on a Parakeet model should return an error",
        )
        print(f"[OK] chat/completions correctly rejected: {result['error']['message']}")


if __name__ == "__main__":
    run_server_tests(
        ParakeetTests,
        "PARAKEET / AUDIO TRANSCRIPTION TESTS",
        modality="whisper",
        default_wrapped_server="parakeetcpp",
    )
