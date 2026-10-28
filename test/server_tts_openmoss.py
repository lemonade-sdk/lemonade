"""
OpenMOSS TTS tests for Lemonade Server.

Tests the /audio/speech endpoint with the OpenMOSS backend: plain synthesis,
voice cloning from a reference WAV, and voice design from a text description.

Usage:
    python server_tts_openmoss.py --wrapped-server openmoss --backend vulkan
    python server_tts_openmoss.py --wrapped-server openmoss --backend rocm
"""

import base64
from concurrent.futures import ThreadPoolExecutor
import io
import math
import struct
import time
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
        """Test voice design: an invented voice from a text description.

        Design is opt-in through `voice_design_description` and needs no separate
        model: the voice generator is a component of the speech model. Note this
        request spans two model loads, since the backend swaps the speech model
        out for the generator and back again.
        """
        payload = {
            "model": get_test_model("tts"),
            "input": "Designing a brand new voice from a description.",
            "voice_design_description": "a calm, deep male narrator voice",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self._assert_wav_response(response, "Voice design")
        print(f"[OK] Voice design produced a clip ({len(response.content)} bytes)")

    def _loaded_model_pid(self, model):
        response = requests.get(
            f"{self.base_url}/health",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200, response.text[:1000])
        for loaded in response.json().get("all_models_loaded", []):
            if loaded.get("model_name") == model:
                return loaded.get("pid")
        return None

    def test_011_concurrent_speech_and_voice_design(self):
        """A request arriving after speech is stopped waits for voice design."""

        model = get_test_model("tts")
        voice_design_payload = {
            "model": model,
            "input": "This request designs a voice without interrupting its neighbor.",
            "voice_design_description": (
                "a precise late-night radio narrator with a softly rising cadence"
            ),
        }
        plain_payload = {
            "model": model,
            "input": (
                "A request entering during the model swap must wait and then speak."
            ),
        }

        def send(payload):
            return requests.post(
                f"{self.base_url}/audio/speech",
                json=payload,
                timeout=TIMEOUT_MODEL_OPERATION,
            )

        with ThreadPoolExecutor(max_workers=2) as executor:
            design_future = executor.submit(send, voice_design_payload)
            # The Lemonade /health snapshot keeps the model visible during the
            # intentional swap, but its wrapped speech PID is zero after the
            # speech process was actually stopped. Enter the second request only
            # in that exact window; this is the race the old simultaneous-start
            # test could miss.

            deadline = time.monotonic() + min(TIMEOUT_MODEL_OPERATION, 120)
            while time.monotonic() < deadline:
                if self._loaded_model_pid(model) == 0:
                    break
                if design_future.done():
                    self.fail(
                        "Voice design completed before the test observed the "
                        "speech-process swap window"
                    )
                time.sleep(0.05)
            else:
                self.fail("Timed out waiting for OpenMOSS speech PID to become zero")

            plain_future = executor.submit(send, plain_payload)
            design_response = design_future.result(timeout=TIMEOUT_MODEL_OPERATION)
            plain_response = plain_future.result(timeout=TIMEOUT_MODEL_OPERATION)

        self._assert_wav_response(design_response, "Concurrent voice design")
        self._assert_wav_response(
            plain_response, "Speech entering during voice-design swap"
        )

    def test_012_streaming_pcm(self):
        """OpenMOSS native streaming uses PCM rather than a buffered WAV."""
        model = get_test_model("tts")
        payload = {
            "model": model,
            "input": "Lemonade can stream speech through the OpenMOSS native path.",
            "stream": True,
        }

        print(f"[INFO] Requesting native streamed speech from {model}")

        with requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        ) as response:
            status = response.status_code
            content_type = response.headers.get("Content-Type", "")
            body = b"".join(response.iter_content(chunk_size=8192))

        self.assertEqual(
            status,
            200,
            f"Streamed speech failed with status {status}: {body[:1000]!r}",
        )
        self.assertIn(
            "audio/pcm",
            content_type,
            f"OpenMOSS streaming must use PCM, got '{content_type}'",
        )
        self.assertEqual(response.headers.get("X-MOSS-Sample-Rate"), "24000")
        self.assertEqual(response.headers.get("X-MOSS-Channels"), "1")
        self.assertFalse(
            body[:4] == b"RIFF",
            "Native OpenMOSS streaming should not buffer a WAV container",
        )
        self.assertFalse(
            body.lstrip().startswith(b"{"),
            "A backend JSON error must not be returned as successful audio",
        )
        self.assertGreater(len(body), 1000, "Streamed PCM should be substantial")

        print(f"[OK] Streamed PCM received ({len(body)} bytes)")

    def test_013_voice_field_does_not_trigger_design(self):
        """A plain `voice` value must speak, not design a voice by that name.

        `voice` keeps its OpenAI-compatible meaning and is forwarded as a style
        instruction. A client sending "default" must get speech back rather than
        an attempt to invent a voice literally called "default".
        """
        payload = {
            "model": get_test_model("tts"),
            "input": "A standard voice field should still just speak.",
            "voice": "default",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self._assert_wav_response(response, "Plain voice field")
        print(
            f"[OK] `voice` was treated as an instruction ({len(response.content)} bytes)"
        )

    def test_014_tts_model_rejected_by_audio_generation_endpoint(self):
        """A TTS deployment cannot be driven through /audio/generations."""
        model = get_test_model("tts")
        response = requests.post(
            f"{self.base_url}/audio/generations",
            json={"model": model, "prompt": "This is the wrong endpoint."},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text[:1000])
        self.assertIn("application/json", response.headers.get("Content-Type", ""))
        self.assertEqual(
            response.json().get("error", {}).get("code"), "model_not_applicable"
        )

    def test_015_model_metadata_preserves_local_pcm_layout(self):
        """The live /models object must retain 48 kHz stereo PCM metadata."""
        response = requests.get(
            f"{self.base_url}/models/MOSS-TTS-Local",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200, response.text[:1000])
        model = response.json()
        audio_defaults = model.get("audio_defaults", {})
        self.assertIsInstance(audio_defaults, dict)
        self.assertEqual(
            audio_defaults.get("pcm_sample_rate"),
            48000,
            "MOSS-TTS-Local must export its native 48 kHz PCM rate",
        )
        self.assertEqual(
            audio_defaults.get("pcm_channels"),
            2,
            "MOSS-TTS-Local must export its native stereo PCM layout",
        )
        self.assertNotIn("pcm_sample_rate", model)
        self.assertNotIn("pcm_channels", model)

    def test_016_default_launch_does_not_force_large_context(self):
        """Default TTS launch must leave n_ctx to OpenMOSS v0.3 (8192)."""
        model = get_test_model("tts")

        def loaded_model():
            response = requests.get(
                f"{self.base_url}/health",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(response.status_code, 200, response.text[:1000])
            return next(
                (
                    entry
                    for entry in response.json().get("all_models_loaded", [])
                    if entry.get("model_name") == model
                ),
                None,
            )

        loaded = loaded_model()
        if loaded is None:
            # Keep this test useful when selected on its own. In the full suite
            # an earlier speech test has already loaded the model, so this adds
            # no extra generation work.
            speech = requests.post(
                f"{self.base_url}/audio/speech",
                json={"model": model, "input": "Checking the default OpenMOSS launch."},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self._assert_wav_response(speech, "Context-default launch setup")
            loaded = loaded_model()

        self.assertIsNotNone(loaded, f"{model} should be loaded")
        command = loaded.get("launch_command", [])
        self.assertIsInstance(command, list)
        self.assertNotIn(
            "--n-ctx",
            command,
            f"Lemonade must not override OpenMOSS's default context: {command}",
        )


if __name__ == "__main__":
    run_server_tests(
        OpenMossTTSTests,
        "OPENMOSS TTS TESTS",
        modality="tts",
        default_wrapped_server="openmoss",
    )
