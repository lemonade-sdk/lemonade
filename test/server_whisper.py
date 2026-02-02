"""
Whisper audio transcription tests for Lemonade Server.

Tests the /audio/transcriptions endpoint (HTTP) and the
/api/v1/realtime WebSocket endpoint with Whisper models.

Usage:
    python server_whisper.py
    python server_whisper.py --server-per-test
    python server_whisper.py --server-binary /path/to/lemonade-server
"""

import asyncio
import base64
import json
import os
import struct
import tempfile
import time
import wave

import requests
import urllib.request
import websockets

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

# WebSocket server runs on HTTP port + 100
WS_PORT = PORT + 100


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

    async def test_005_realtime_websocket_connect(self):
        """Test WebSocket connection and session creation."""
        ws_url = f"ws://localhost:{WS_PORT}/api/v1/realtime?intent=transcription"

        print(f"[INFO] Connecting to WebSocket at {ws_url}")
        async with websockets.connect(ws_url) as ws:
            # Should receive session.created on connect
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            msg = json.loads(raw)
            self.assertEqual(
                msg["type"],
                "transcription_session.created",
                f"Expected session.created, got {msg['type']}",
            )
            self.assertIn("session", msg, "session.created should contain session info")
            print(f"[OK] Session created: {msg['session']}")

            # Send session update with model
            await ws.send(
                json.dumps(
                    {
                        "type": "transcription_session.update",
                        "session": {"model": WHISPER_MODEL},
                    }
                )
            )

            # Should receive session.updated
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            msg = json.loads(raw)
            self.assertEqual(
                msg["type"],
                "transcription_session.updated",
                f"Expected session.updated, got {msg['type']}",
            )
            print(f"[OK] Session updated with model {WHISPER_MODEL}")

        print("[OK] WebSocket connection lifecycle passed")

    async def test_006_realtime_websocket_transcription(self):
        """Test full realtime transcription: send audio chunks, receive transcript."""
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

        ws_url = f"ws://localhost:{WS_PORT}/api/v1/realtime?intent=transcription"

        async with websockets.connect(ws_url) as ws:
            # Wait for session created
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            msg = json.loads(raw)
            self.assertEqual(msg["type"], "transcription_session.created")

            # Configure model
            await ws.send(
                json.dumps(
                    {
                        "type": "transcription_session.update",
                        "session": {"model": WHISPER_MODEL},
                    }
                )
            )
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            msg = json.loads(raw)
            self.assertEqual(msg["type"], "transcription_session.updated")

            # Send all audio chunks
            print(f"[INFO] Sending {len(chunks)} audio chunks...")
            for chunk in chunks:
                b64 = base64.b64encode(chunk).decode("ascii")
                await ws.send(
                    json.dumps(
                        {
                            "type": "input_audio_buffer.append",
                            "audio": b64,
                        }
                    )
                )
                # Small delay to simulate real-time streaming
                await asyncio.sleep(0.01)

            # Commit the audio buffer to force transcription
            print("[INFO] Committing audio buffer...")
            await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))

            # Collect messages until we get the transcription result
            transcript = None
            deadline = time.time() + TIMEOUT_MODEL_OPERATION
            while time.time() < deadline:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=30)
                    msg = json.loads(raw)
                    print(f"[INFO] Received message: {msg['type']}")

                    if (
                        msg["type"]
                        == "conversation.item.input_audio_transcription.completed"
                    ):
                        transcript = msg.get("transcript", "")
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
    run_server_tests(WhisperTests, "WHISPER TRANSCRIPTION TESTS")
