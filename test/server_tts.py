"""
Kokoro TTS tests for Lemonade Server.

Tests the /audio/speech endpoint.

Usage:
    python server_tts.py
    python server_tts.py --server-per-test
    python server_tts.py --server-binary /path/to/lemonade-server
"""

import base64
import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
)
from utils.test_models import (
    TTS_MODEL,
    PORT,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


class TextToSpeechTests(ServerTestBase):
    """Tests for Text to Speech."""

    def test_001_basic_tts(self):
        """Test basic speech generation with Kokoro."""
        payload = {
            "model": TTS_MODEL,
            "input": "Lemonade can speak",
            "response_format": "mp3",
        }

        print(f"[INFO] Sending speech generation request with model {TTS_MODEL}")

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Speech generation failed with status {response.status_code}: {response.text}",
        )

        # MP3 files start with specific magic bytes
        self.assertTrue(
            response.content[:3] == b"ID3",
            "Decoded data should be a valid MP3",
        )

        print(f"[OK] Speech generation successful")

    def test_002_missing_input_error(self):
        """Test error handling when input is missing."""
        payload = {
            "model": TTS_MODEL,
            # No prompt
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        # Should return an error
        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing prompt, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without prompt: {response.status_code}")

    def test_003_invalid_model_error(self):
        """Test error handling with invalid model."""
        payload = {
            "model": "kokoro-no-junbi-mada-dekite-inai",
            "input": "Lemonade can speak",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        # Should return an error (model not found)
        # Note: Server may return 500 for model not found, ideally should be 404
        self.assertIn(
            response.status_code,
            [400, 404, 422, 500],
            f"Expected error for invalid model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")

    def test_004_tts_voice(self):
        """Test voice selection with Kokoro."""
        payload = {
            "model": TTS_MODEL,
            "input": "Lemonade can speak",
            "voice": "onyx",
            "response_format": "opus",
        }

        print(f"[INFO] Sending speech generation request with model {TTS_MODEL}")

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Speech generation failed with status {response.status_code}: {response.text}",
        )

        # OPUS files start with specific magic bytes
        self.assertTrue(
            response.content[:4] == b"OggS",
            "Decoded data should be a valid Ogg file",
        )

        print(f"[OK] Speech generation successful")

    def test_005_tts_stream(self):
        """Test streaming speech generation with Kokoro."""
        payload = {
            "model": TTS_MODEL,
            "input": "Lemonade can speak",
            "voice": "onyx",
            "stream_format": "audio",
        }

        print(f"[INFO] Sending speech generation request with model {TTS_MODEL}")

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Speech generation failed with status {response.status_code}: {response.text}",
        )

        print(f"[OK] Speech generation successful")

    def test_006_tts_speed(self):
        """Test voice selection with Kokoro."""
        payload = {
            "model": TTS_MODEL,
            "input": "Lemonade can speak",
            "speed": 0.75,
            "response_format": "wav",
        }

        print(f"[INFO] Sending speech generation request with model {TTS_MODEL}")

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Speech generation failed with status {response.status_code}: {response.text}",
        )

        # WAV files start with specific magic bytes
        self.assertTrue(
            response.content[:4] == b"RIFF",
            "Decoded data should be a valid WAV file",
        )

        print(f"[OK] Speech generation successful")


if __name__ == "__main__":
    run_server_tests(TextToSpeechTests, "TTS TESTS")
