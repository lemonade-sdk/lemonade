"""
Whisper audio transcription tests for Lemonade Server.

Tests the /audio/transcriptions endpoint with Whisper models.

Usage:
    python server_whisper.py
    python server_whisper.py --server-per-test
    python server_whisper.py --server-binary /path/to/lemonade-server
"""

import os
import tempfile
import requests
import urllib.request

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
)
from utils.test_models import (
    WHISPER_MODEL,
    TEST_AUDIO_URL,
    PORT,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


class WhisperTests(ServerTestBase):
    """Tests for Whisper audio transcription."""

    # Class-level cache for the test audio file
    _test_audio_path = None

    @classmethod
    def setUpClass(cls):
        """Download test audio file once for all tests."""
        super().setUpClass()

        # Download test audio file to temp directory
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
        """Cleanup test audio file."""
        super().tearDownClass()
        if cls._test_audio_path and os.path.exists(cls._test_audio_path):
            try:
                os.remove(cls._test_audio_path)
                print(f"[INFO] Cleaned up test audio file")
            except Exception:
                pass  # Ignore cleanup errors

    def test_001_transcription_basic(self):
        """Test basic audio transcription with Whisper."""
        self.assertIsNotNone(self._test_audio_path, "Test audio file not downloaded")
        self.assertTrue(
            os.path.exists(self._test_audio_path),
            f"Test audio file not found at {self._test_audio_path}",
        )

        with open(self._test_audio_path, "rb") as audio_file:
            files = {"file": ("test_speech.wav", audio_file, "audio/wav")}
            data = {"model": WHISPER_MODEL, "response_format": "json"}

            print(f"[INFO] Sending transcription request with model {WHISPER_MODEL}")
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
        self.assertIsInstance(
            result["text"], str, "Transcription text should be a string"
        )
        self.assertGreater(len(result["text"]), 0, "Transcription should not be empty")

        print(f"[OK] Transcription result: {result['text']}")

    def test_002_transcription_with_language(self):
        """Test audio transcription with explicit language parameter."""
        self.assertIsNotNone(self._test_audio_path, "Test audio file not downloaded")

        with open(self._test_audio_path, "rb") as audio_file:
            files = {"file": ("test_speech.wav", audio_file, "audio/wav")}
            data = {
                "model": WHISPER_MODEL,
                "language": "en",  # Explicitly set English
                "response_format": "json",
            }

            print(f"[INFO] Sending transcription request with language=en")
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
        self.assertGreater(len(result["text"]), 0, "Transcription should not be empty")

        print(f"[OK] Transcription with language=en: {result['text']}")

    def test_003_transcription_missing_file_error(self):
        """Test error handling when file is missing."""
        data = {"model": WHISPER_MODEL}

        response = requests.post(
            f"{self.base_url}/audio/transcriptions",
            data=data,
            timeout=TIMEOUT_DEFAULT,
        )

        # Should return an error (400 or 422)
        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing file, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without file: {response.status_code}")

    def test_004_transcription_missing_model_error(self):
        """Test error handling when model is missing."""
        with open(self._test_audio_path, "rb") as audio_file:
            files = {"file": ("test_speech.wav", audio_file, "audio/wav")}

            response = requests.post(
                f"{self.base_url}/audio/transcriptions",
                files=files,
                timeout=TIMEOUT_DEFAULT,
            )

        # Should return an error (400 or 422)
        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without model: {response.status_code}")


if __name__ == "__main__":
    run_server_tests(WhisperTests, "WHISPER TRANSCRIPTION TESTS")
