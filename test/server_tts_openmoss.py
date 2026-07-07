"""
OpenMOSS TTS tests for Lemonade Server.

Tests the /audio/speech endpoint with the OpenMOSS backend: plain synthesis,
voice cloning from a reference WAV, and voice design from a text description.

Usage:
    python server_tts_openmoss.py --wrapped-server openmoss --backend vulkan
    python server_tts_openmoss.py --wrapped-server openmoss --backend rocm
"""

import base64
import io
import math
import struct
import wave

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


def make_reference_wav_b64(duration_s=0.5, freq=440, rate=16000):
    """Build a small valid mono WAV (sine tone) as base64, stdlib only."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(int(duration_s * rate)):
            sample = int(20000 * math.sin(2 * math.pi * freq * i / rate))
            frames += struct.pack("<h", sample)
        w.writeframes(bytes(frames))
    return base64.b64encode(buf.getvalue()).decode("ascii")


class OpenMossTTSTests(ServerTestBase):
    """Tests for OpenMOSS text-to-speech."""

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
            "input": "Lemonade can speak with an open voice.",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self._assert_wav_response(response, "Basic speech synthesis")
        print(f"[OK] Generated speech clip ({len(response.content)} bytes)")

    def test_002_explicit_wav_response_format(self):
        """Test that response_format 'wav' is accepted."""
        payload = {
            "model": get_test_model("tts"),
            "input": "Testing the wav response format.",
            "response_format": "wav",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self._assert_wav_response(response, "Speech with response_format=wav")
        print(f"[OK] response_format=wav accepted ({len(response.content)} bytes)")

    def test_003_unsupported_response_format(self):
        """Test that a response_format the backend cannot produce is rejected."""
        payload = {
            "model": get_test_model("tts"),
            "input": "This should be rejected.",
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
            f"Expected 400 for unsupported response_format, got {response.status_code}: "
            f"{response.text[:1000]}",
        )
        self.assertIn("error", response.json(), "Response should contain 'error' field")
        print(
            f"[OK] Correctly rejected unsupported response_format: {response.status_code}"
        )

    def test_004_voice_cloning_with_reference_wav(self):
        """Test speech synthesis with a reference WAV for voice cloning."""
        payload = {
            "model": get_test_model("tts"),
            "input": "Cloning a voice from a reference sample.",
            "reference_wav_b64": make_reference_wav_b64(),
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self._assert_wav_response(response, "Voice cloning")
        print(f"[OK] Voice cloning produced a clip ({len(response.content)} bytes)")

    def test_005_missing_input_error(self):
        """Test error handling when input is missing."""
        payload = {
            "model": get_test_model("tts"),
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing input, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without input: {response.status_code}")

    def _assert_backend_error(self, payload, context, expected_status=400):
        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(
            response.status_code,
            expected_status,
            f"{context}: expected {expected_status}, got {response.status_code}: "
            f"{response.text[:1000]}",
        )
        self.assertIn(
            "application/json",
            response.headers.get("Content-Type", ""),
            f"{context}: error must be JSON, not audio bytes",
        )
        self.assertIn("error", response.json(), f"{context}: missing 'error' field")
        print(f"[OK] {context}: {response.status_code} JSON error")

    def test_006_invalid_reference_wav_b64(self):
        """A reference_wav_b64 that is not valid base64 must be a JSON error, not audio."""
        self._assert_backend_error(
            {
                "model": get_test_model("tts"),
                "input": "This should be rejected.",
                "reference_wav_b64": "!!!not-base64!!!",
            },
            "Invalid base64 reference",
        )

    def test_007_non_wav_reference_data(self):
        """Valid base64 that does not decode to a WAV must be a JSON error, not audio."""
        self._assert_backend_error(
            {
                "model": get_test_model("tts"),
                "input": "This should be rejected.",
                "reference_wav_b64": base64.b64encode(b"definitely not a wav").decode(
                    "ascii"
                ),
            },
            "Non-WAV base64 reference",
        )

    def test_008_invalid_speed(self):
        """A speed outside the supported range must be a JSON error, not audio."""
        self._assert_backend_error(
            {
                "model": get_test_model("tts"),
                "input": "This should be rejected.",
                "speed": 100.0,
            },
            "Out-of-range speed",
        )

    def test_009_non_string_input(self):
        """A non-string input must be a JSON error, not audio."""
        self._assert_backend_error(
            {
                "model": get_test_model("tts"),
                "input": 12345,
            },
            "Non-string input",
        )

    def test_010_voice_design(self):
        """Test voice design: a free-text voice description instead of a fixed voice."""
        model = get_test_model("tts_design")
        print(f"[INFO] Ensuring {model} is pulled...")
        pull_model_with_retry(model)

        payload = {
            "model": model,
            "input": "Designing a brand new voice from a description.",
            "voice": "a calm, deep male narrator voice",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self._assert_wav_response(response, "Voice design")
        print(f"[OK] Voice design produced a clip ({len(response.content)} bytes)")


if __name__ == "__main__":
    run_server_tests(
        OpenMossTTSTests,
        "OPENMOSS TTS TESTS",
        modality="tts",
        default_wrapped_server="openmoss",
    )
