"""
Audio generation tests for Lemonade Server.

Tests the /audio/generations endpoint with the ThinkSound (sound effects)
and ACE-Step (music) backends.

Usage:
    python server_audio_generation.py --wrapped-server thinksound --backend vulkan
    python server_audio_generation.py --wrapped-server acestep --backend rocm
"""

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
)
from utils.capabilities import get_test_model
from utils.test_models import (
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


class AudioGenerationTests(ServerTestBase):
    """Tests for the /audio/generations endpoint."""

    _model_pulled = False

    @classmethod
    def setUpClass(cls):
        """Verify server, apply runtime config, and pre-pull the model."""
        super().setUpClass()
        cls._ensure_model_pulled()

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return
        model = get_test_model("audio_generation")
        print(f"\n[SETUP] Ensuring {model} is pulled...")
        pull_model_with_retry(model)
        print(f"[SETUP] {model} is ready")
        cls._model_pulled = True

    def _generation_payload(self, **overrides):
        payload = {
            "model": get_test_model("audio_generation"),
            "prompt": "A short click sound",
            "duration": 3,
            "steps": 4,
            "seed": 42,
        }
        payload.update(overrides)
        return payload

    def test_001_basic_audio_generation(self):
        """Test basic audio generation returns a WAV clip."""
        payload = self._generation_payload()
        print(f"[INFO] Sending audio generation request with model {payload['model']}")

        response = requests.post(
            f"{self.base_url}/audio/generations",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Audio generation failed with status {response.status_code}: {response.text[:1000]}",
        )
        self.assertIn(
            "audio/wav",
            response.headers.get("Content-Type", ""),
            "Response should have audio/wav content type",
        )
        self.assertTrue(
            response.content[:4] == b"RIFF",
            "Response body should be a valid WAV (RIFF) file",
        )
        self.assertGreater(
            len(response.content), 1000, "Audio clip should be substantial"
        )
        print(f"[OK] Generated valid WAV clip ({len(response.content)} bytes)")

    def test_002_explicit_wav_response_format(self):
        """Test that response_format 'wav' is accepted."""
        payload = self._generation_payload(response_format="wav")

        response = requests.post(
            f"{self.base_url}/audio/generations",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Audio generation with response_format=wav failed: {response.text[:1000]}",
        )
        self.assertTrue(response.content[:4] == b"RIFF", "Should be a valid WAV file")
        print(f"[OK] response_format=wav accepted ({len(response.content)} bytes)")

    def test_003_unsupported_response_format(self):
        """Test that a response_format the backend cannot produce is rejected."""
        payload = self._generation_payload(response_format="mp3")

        response = requests.post(
            f"{self.base_url}/audio/generations",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for unsupported response_format, got {response.status_code}: "
            f"{response.text[:1000]}",
        )
        result = response.json()
        self.assertIn("error", result, "Response should contain 'error' field")
        print(
            f"[OK] Correctly rejected unsupported response_format: {response.status_code}"
        )

    def test_004_missing_prompt_error(self):
        """Test error handling when prompt is missing."""
        payload = self._generation_payload()
        del payload["prompt"]

        response = requests.post(
            f"{self.base_url}/audio/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for missing prompt, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without prompt: {response.status_code}")

    def test_005_missing_model_error(self):
        """Test error handling when model is missing."""
        payload = self._generation_payload()
        del payload["model"]

        response = requests.post(
            f"{self.base_url}/audio/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for missing model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without model: {response.status_code}")

    def test_006_invalid_model_error(self):
        """Test error handling with a nonexistent model."""
        payload = self._generation_payload(model="nonexistent-audio-model-xyz-123")

        response = requests.post(
            f"{self.base_url}/audio/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(
            response.status_code,
            [400, 404, 422, 500],
            f"Expected error for invalid model, got {response.status_code}",
        )
        self.assertIn("error", response.json(), "Response should contain 'error' field")
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")


if __name__ == "__main__":
    run_server_tests(
        AudioGenerationTests,
        "AUDIO GENERATION TESTS",
        modality="audio_generation",
        default_wrapped_server="thinksound",
    )
