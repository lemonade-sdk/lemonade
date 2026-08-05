"""
VoxCPM2 TTS tests for Lemonade Server.

Tests the /audio/speech endpoint with the VoxCPM2 backend: plain synthesis,
voice design from a parenthetical description, streaming, and format rejection.

Usage:
    python server_tts_voxcpm2.py --wrapped-server voxcpm2 --backend metal
    python server_tts_voxcpm2.py --wrapped-server voxcpm2 --backend vulkan
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
)


class VoxCPM2TTSTests(ServerTestBase):
    """Tests for VoxCPM2 text-to-speech."""

    _model_pulled = False

    @classmethod
    def setUpClass(cls):
        """Verify server, apply runtime config, and pre-pull the TTS model."""
        super().setUpClass()
        cls._ensure_model_pulled()

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return
        model = get_test_model("tts")
        print(f"\n[SETUP] Ensuring {model} is pulled...")
        pull_model_with_retry(model)
        print(f"[SETUP] {model} is ready")
        cls._model_pulled = True

    def _assert_wav_response(self, response, context):
        self.assertEqual(
            response.status_code,
            200,
            f"{context} failed with status {response.status_code}: {response.text[:1000]}",
        )
        self.assertIn(
            "audio/wav",
            response.headers.get("Content-Type", ""),
            f"{context}: response should have audio/wav content type",
        )
        self.assertTrue(
            response.content[:4] == b"RIFF",
            f"{context}: response body should be a valid WAV (RIFF) file",
        )
        self.assertGreater(
            len(response.content), 1000, f"{context}: clip should be substantial"
        )

    def test_001_basic_speech(self):
        """Test basic speech synthesis defaults to the backend's WAV output."""
        payload = {
            "model": get_test_model("tts"),
            "input": "Lemonade can speak with VoxCPM2.",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self._assert_wav_response(response, "Basic speech")
        print(f"[OK] Basic speech produced a clip ({len(response.content)} bytes)")

    def test_002_voice_design(self):
        """VoxCPM2 designs a voice from a parenthetical prefix in the input."""
        payload = {
            "model": get_test_model("tts"),
            "input": "(A calm, deep male narrator voice) Lemonade can speak.",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self._assert_wav_response(response, "Voice design")
        print(f"[OK] Voice design produced a clip ({len(response.content)} bytes)")

    def test_003_streaming(self):
        """Streaming falls back to the backend's WAV output, since it has no PCM."""
        payload = {
            "model": get_test_model("tts"),
            "input": "Lemonade can stream speech with VoxCPM2.",
            "stream_format": "audio",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self._assert_wav_response(response, "Streamed speech")
        print(f"[OK] Streamed speech produced a clip ({len(response.content)} bytes)")

    def test_004_rejects_unsupported_format(self):
        """An explicitly requested format the backend cannot encode is rejected."""
        payload = {
            "model": get_test_model("tts"),
            "input": "Lemonade can speak.",
            "response_format": "mp3",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            400,
            "Requesting mp3 from a wav-only backend should be rejected, "
            f"got {response.status_code}",
        )
        print(f"[OK] Unsupported format rejected")

    def test_005_missing_input_error(self):
        """Test error handling when input is missing."""
        payload = {"model": get_test_model("tts")}

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            400,
            f"Missing input should be rejected, got {response.status_code}",
        )
        print(f"[OK] Missing input rejected")


if __name__ == "__main__":
    run_server_tests(
        VoxCPM2TTSTests,
        "VOXCPM2 TTS TESTS",
        modality="tts",
        default_wrapped_server="voxcpm2",
    )
