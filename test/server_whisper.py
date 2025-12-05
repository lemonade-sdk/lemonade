"""
Usage: python server_whisper.py

This will launch the lemonade server, test audio transcription with Whisper,
and make sure that the response is valid.

Examples:
    python server_whisper.py
    python server_whisper.py --server-binary ./lemonade-server

If you get the `ImportError: cannot import name 'TypeIs' from 'typing_extensions'` error:
    1. pip uninstall typing_extensions
    2. pip install openai
"""

import os
import tempfile
import requests
import urllib.request

# Import all shared functionality from utils/server_base.py
from utils.server_base import (
    ServerTestingBase,
    run_server_tests_with_class,
    PORT,
)

# Test audio file URL from lemonade-sdk assets repository
TEST_AUDIO_URL = "https://raw.githubusercontent.com/lemonade-sdk/assets/main/audio/test_speech.wav"
WHISPER_MODEL = "Whisper-Tiny"


class WhisperTesting(ServerTestingBase):
    """Testing class for Whisper audio transcription."""

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

    def setUp(self):
        """Call parent setUp with Whisper-specific messaging."""
        print(f"\n=== Starting new Whisper test ===")
        super().setUp()

    # Test 1: Basic transcription
    def test_001_test_whisper_transcription(self):
        """Test basic audio transcription with Whisper."""
        self.assertIsNotNone(
            self._test_audio_path, "Test audio file not downloaded"
        )
        self.assertTrue(
            os.path.exists(self._test_audio_path),
            f"Test audio file not found at {self._test_audio_path}",
        )

        # Prepare multipart form data
        with open(self._test_audio_path, "rb") as audio_file:
            files = {
                "file": ("test_speech.wav", audio_file, "audio/wav")
            }
            data = {
                "model": WHISPER_MODEL,
                "response_format": "json"
            }

            print(f"[INFO] Sending transcription request with model {WHISPER_MODEL}")
            response = requests.post(
                f"{self.base_url}/audio/transcriptions",
                files=files,
                data=data,
                timeout=300  # 5 minute timeout for model loading + transcription
            )

        self.assertEqual(
            response.status_code,
            200,
            f"Transcription failed with status {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertIn("text", result, "Response should contain 'text' field")
        self.assertIsInstance(result["text"], str, "Transcription text should be a string")
        self.assertGreater(len(result["text"]), 0, "Transcription should not be empty")

        print(f"[OK] Transcription result: {result['text']}")

    # Test 2: Transcription with language parameter
    def test_002_test_whisper_transcription_with_language(self):
        """Test audio transcription with explicit language parameter."""
        self.assertIsNotNone(
            self._test_audio_path, "Test audio file not downloaded"
        )

        with open(self._test_audio_path, "rb") as audio_file:
            files = {
                "file": ("test_speech.wav", audio_file, "audio/wav")
            }
            data = {
                "model": WHISPER_MODEL,
                "language": "en",  # Explicitly set English
                "response_format": "json"
            }

            print(f"[INFO] Sending transcription request with language=en")
            response = requests.post(
                f"{self.base_url}/audio/transcriptions",
                files=files,
                data=data,
                timeout=300
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

    # Test 3: Error handling - missing file
    def test_003_test_transcription_missing_file(self):
        """Test error handling when file is missing."""
        data = {
            "model": WHISPER_MODEL,
        }

        response = requests.post(
            f"{self.base_url}/audio/transcriptions",
            data=data,
            timeout=60
        )

        # Should return an error (400 or 422)
        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing file, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without file: {response.status_code}")

    # Test 4: Error handling - missing model
    def test_004_test_transcription_missing_model(self):
        """Test error handling when model is missing."""
        with open(self._test_audio_path, "rb") as audio_file:
            files = {
                "file": ("test_speech.wav", audio_file, "audio/wav")
            }

            response = requests.post(
                f"{self.base_url}/audio/transcriptions",
                files=files,
                timeout=60
            )

        # Should return an error (400 or 422)
        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without model: {response.status_code}")


if __name__ == "__main__":
    run_server_tests_with_class(WhisperTesting, "WHISPER SERVER TESTS")
