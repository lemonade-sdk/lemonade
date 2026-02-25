"""
Whisper audio transcription tests for Lemonade Server.

Tests the /audio/transcriptions endpoint (HTTP) and the
/api/v1/realtime WebSocket endpoint with Whisper models.

Usage:
    python server_whisper.py
    python server_whisper.py --backend npu
    python server_whisper.py --server-per-test
    python server_whisper.py --server-binary /path/to/lemonade-server
"""

import asyncio
import base64
import os
import struct
import sys
import tempfile
import time
import wave

import requests
import urllib.request
from openai import AsyncOpenAI

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


# Global variable to store the backend to test
WHISPER_BACKEND = None


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

        # Load model with specified backend if provided
        if WHISPER_BACKEND:
            print(f"[INFO] Loading model with {WHISPER_BACKEND} backend")
            load_response = requests.post(
                f"{self.base_url}/load",
                json={
                    "model_name": WHISPER_MODEL,
                    "whispercpp_backend": WHISPER_BACKEND
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            if load_response.status_code != 200:
                self.skipTest(f"{WHISPER_BACKEND} backend not available: {load_response.text}")
                return

        with open(self._test_audio_path, "rb") as audio_file:
            files = {"file": ("test_speech.wav", audio_file, "audio/wav")}
            data = {"model": WHISPER_MODEL, "response_format": "json"}

            backend_msg = f" ({WHISPER_BACKEND} backend)" if WHISPER_BACKEND else ""
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

    def test_005_transcription_npu_backend(self):
        """Test NPU backend with automatic .rai cache download."""
        # Skip if a different backend was specified via CLI
        if WHISPER_BACKEND and WHISPER_BACKEND != "npu":
            self.skipTest(f"Skipping NPU test (testing {WHISPER_BACKEND} backend)")
            return

        print("\n[INFO] Testing NPU backend (requires NPU hardware)")

        # Load model with NPU backend
        load_response = requests.post(
            f"{self.base_url}/load",
            json={
                "model_name": WHISPER_MODEL,
                "whispercpp_backend": "npu"
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        if load_response.status_code != 200:
            print(f"[SKIP] NPU backend not available: {load_response.text}")
            self.skipTest("NPU backend not available (NPU hardware required)")
            return

        print(f"[OK] Model loaded with NPU backend")

        # Verify transcription works with NPU backend
        with open(self._test_audio_path, "rb") as audio_file:
            files = {"file": ("test_speech.wav", audio_file, "audio/wav")}
            data = {"model": WHISPER_MODEL, "response_format": "json"}

            print(f"[INFO] Testing NPU transcription")
            response = requests.post(
                f"{self.base_url}/audio/transcriptions",
                files=files,
                data=data,
                timeout=TIMEOUT_MODEL_OPERATION,
            )

        self.assertEqual(
            response.status_code,
            200,
            f"NPU transcription failed with status {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertIn("text", result, "Response should contain 'text' field")
        self.assertGreater(
            len(result["text"]), 0, "NPU transcription should not be empty"
        )

        print(f"[OK] NPU transcription result: {result['text']}")

    # =========================================================================
    # WebSocket Realtime Transcription Tests
    # =========================================================================

    def _load_pcm16_from_wav(self):
        """
        Read the test WAV file and return raw PCM16 mono 16kHz bytes.

        Handles resampling and channel conversion so the data matches
        what the StreamingAudioBuffer expects (16kHz, mono, int16).
        """
        with wave.open(self._test_audio_path, "rb") as wav:
            n_channels = wav.getnchannels()
            sampwidth = wav.getsampwidth()
            framerate = wav.getframerate()
            n_frames = wav.getnframes()
            raw_data = wav.readframes(n_frames)

        # Decode raw bytes into int16 samples
        if sampwidth == 2:
            samples = list(struct.unpack(f"<{len(raw_data) // 2}h", raw_data))
        elif sampwidth == 1:
            # 8-bit unsigned -> 16-bit signed
            samples = [((b - 128) * 256) for b in raw_data]
        else:
            self.fail(f"Unsupported sample width: {sampwidth}")

        # Convert stereo to mono
        if n_channels == 2:
            samples = [
                (samples[i] + samples[i + 1]) // 2
                for i in range(0, len(samples), 2)
            ]

        # Resample to 16kHz if needed
        target_rate = 16000
        if framerate != target_rate:
            ratio = framerate / target_rate
            new_len = int(len(samples) / ratio)
            samples = [
                samples[min(int(i * ratio), len(samples) - 1)]
                for i in range(new_len)
            ]

        return struct.pack(f"<{len(samples)}h", *samples)

    def _get_websocket_port(self):
        """Fetch WebSocket port from /health endpoint."""
        response = requests.get(f"{self.base_url}/health", timeout=10)
        self.assertEqual(response.status_code, 200, "Failed to fetch /health")
        health = response.json()
        ws_port = health.get("websocket_port")
        self.assertIsNotNone(ws_port, "Server did not provide websocket_port in /health")
        return ws_port

    def _make_openai_client(self):
        """Create an AsyncOpenAI client configured for the local server."""
        ws_port = self._get_websocket_port()
        return AsyncOpenAI(
            api_key="unused",
            base_url=f"http://localhost:{PORT}/api/v1",
            websocket_base_url=f"ws://localhost:{ws_port}",
        )

    def test_006_realtime_websocket_connect(self):
        """Test WebSocket connection and session creation via OpenAI SDK."""
        asyncio.run(self._test_006_realtime_websocket_connect())

    async def _test_006_realtime_websocket_connect(self):
        ws_port = self._get_websocket_port()
        print(f"[INFO] WebSocket port from /health: {ws_port}")

        client = self._make_openai_client()
        print(f"[INFO] Connecting via OpenAI SDK (ws://localhost:{ws_port})")
        async with client.beta.realtime.connect(model=WHISPER_MODEL) as conn:
            # Should receive session.created on connect
            event = await asyncio.wait_for(conn.recv(), timeout=10)
            self.assertEqual(
                event.type,
                "session.created",
                f"Expected session.created, got {event.type}",
            )
            self.assertTrue(
                hasattr(event, "session"),
                "session.created should contain session info",
            )
            print(f"[OK] Session created: {event.session}")

            # Send session update with model
            await conn.session.update(session={"model": WHISPER_MODEL})

            # Should receive session.updated
            event = await asyncio.wait_for(conn.recv(), timeout=10)
            self.assertEqual(
                event.type,
                "session.updated",
                f"Expected session.updated, got {event.type}",
            )
            print(f"[OK] Session updated with model {WHISPER_MODEL}")

        print("[OK] WebSocket connection lifecycle passed")

    def test_007_realtime_websocket_transcription(self):
        """Test full realtime transcription via OpenAI SDK: send audio chunks, receive transcript."""
        asyncio.run(self._test_007_realtime_websocket_transcription())

    async def _test_007_realtime_websocket_transcription(self):
        self.assertIsNotNone(
            self._test_audio_path, "Test audio file not downloaded"
        )

        # Load audio as PCM16 mono 16kHz
        pcm_data = self._load_pcm16_from_wav()
        self.assertGreater(len(pcm_data), 0, "PCM data should not be empty")
        print(
            f"[INFO] Loaded {len(pcm_data)} bytes of PCM16 audio "
            f"({len(pcm_data) // 2} samples, "
            f"{len(pcm_data) // 2 / 16000:.1f}s)"
        )

        # Split into ~256ms chunks (4096 samples * 2 bytes = 8192 bytes)
        chunk_size = 8192
        chunks = [
            pcm_data[i : i + chunk_size]
            for i in range(0, len(pcm_data), chunk_size)
        ]
        print(f"[INFO] Split into {len(chunks)} chunks")

        client = self._make_openai_client()

        async with client.beta.realtime.connect(model=WHISPER_MODEL) as conn:
            # Wait for session created
            event = await asyncio.wait_for(conn.recv(), timeout=10)
            self.assertEqual(event.type, "session.created")

            # Configure model
            await conn.session.update(session={"model": WHISPER_MODEL})
            event = await asyncio.wait_for(conn.recv(), timeout=10)
            self.assertEqual(event.type, "session.updated")

            # Send all audio chunks
            print(f"[INFO] Sending {len(chunks)} audio chunks...")
            for chunk in chunks:
                b64 = base64.b64encode(chunk).decode("ascii")
                await conn.input_audio_buffer.append(audio=b64)
                # Small delay to simulate real-time streaming
                await asyncio.sleep(0.01)

            # Commit the audio buffer to force transcription
            print("[INFO] Committing audio buffer...")
            await conn.input_audio_buffer.commit()

            # Collect messages until we get the transcription result
            transcript = None
            deadline = time.time() + TIMEOUT_MODEL_OPERATION
            while time.time() < deadline:
                try:
                    event = await asyncio.wait_for(conn.recv(), timeout=30)
                    print(f"[INFO] Received message: {event.type}")

                    if (
                        event.type
                        == "conversation.item.input_audio_transcription.completed"
                    ):
                        transcript = getattr(event, "transcript", "")
                        break
                except asyncio.TimeoutError:
                    break

        self.assertIsNotNone(
            transcript, "Should receive a transcription result"
        )
        self.assertGreater(
            len(transcript.strip()),
            0,
            "Transcription should not be empty",
        )
        print(f"[OK] WebSocket transcription result: {transcript}")


if __name__ == "__main__":
    # Parse backend argument if provided
    import argparse
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--backend", type=str, default=None, help="WhisperCpp backend to test (cpu, vulkan, rocm, npu)")
    args, remaining = parser.parse_known_args()
    
    if args.backend:
        WHISPER_BACKEND = args.backend
        print(f"[INFO] Testing WhisperCpp with {WHISPER_BACKEND} backend")
        # Remove --backend from sys.argv so run_server_tests doesn't see it
        sys.argv = [sys.argv[0]] + remaining
    
    run_server_tests(WhisperTests, "WHISPER TRANSCRIPTION TESTS")
