"""
Moonshine audio transcription tests for Lemonade Server.

Tests the /audio/transcriptions endpoint with Moonshine models.

Usage:
    python server_moonshine.py --wrapped-server moonshine
    python server_moonshine.py --cli-binary /path/to/lemonade
"""

import os
import tempfile
import wave

import requests
import urllib.request

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
    PORT,
    TIMEOUT_DEFAULT,
)


def _get_moonshine_model():
    """Get the Moonshine audio test model from capabilities."""
    return get_test_model("audio")


class MoonshineTests(ServerTestBase):
    """Tests for Moonshine audio transcription."""

    @classmethod
    def setUpClass(cls):
        """Verify server is available."""
        super().setUpClass()

    def _generate_test_audio(self, duration_sec=3.0, sample_rate=16000):
        """Generate a simple sine-wave test audio file."""
        import struct
        import math

        n_samples = int(duration_sec * sample_rate)
        tmp_path = os.path.join(tempfile.gettempdir(), "moonshine_test_audio.wav")

        with wave.open(tmp_path, "w") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(sample_rate)
            for i in range(n_samples):
                sample = int(math.sin(2 * math.pi * 440 * i / sample_rate) * 0.3 * 32767)
                wf.writeframes(struct.pack("<h", sample))

        return tmp_path

    def _load_model(self, model_name):
        """Load a model via the REST API."""
        url = f"http://127.0.0.1:{PORT}/api/v1/load"
        payload = {"model_name": model_name}
        resp = requests.post(url, json=payload, timeout=120)
        self.assertEqual(resp.status_code, 200, f"Failed to load model: {resp.text}")

    @skip_if_unsupported("transcription")
    def test_moonshine_file_transcription(self):
        """Test file-based transcription with Moonshine."""
        model_name = _get_moonshine_model()
        if not model_name or "Moonshine" not in model_name:
            self.skipTest("No Moonshine model configured for testing")

        self._load_model(model_name)

        audio_path = self._generate_test_audio(duration_sec=3.0)
        try:
            url = f"http://127.0.0.1:{PORT}/v1/audio/transcriptions"
            with open(audio_path, "rb") as f:
                files = {"file": ("test.wav", f, "audio/wav")}
                data = {"model": model_name, "response_format": "json"}
                resp = requests.post(url, files=files, data=data, timeout=60)

            self.assertEqual(resp.status_code, 200, f"Transcription failed: {resp.text}")

            result = resp.json()
            self.assertIn("text", result)
            # Sine-wave audio won't produce meaningful text, but the request should succeed
            print(f"[MoonshineTest] Transcript: {result['text']}")

        finally:
            if os.path.exists(audio_path):
                os.remove(audio_path)

    @skip_if_unsupported("transcription")
    def test_moonshine_file_transcription_real_speech(self):
        """Transcribe real speech and check the content."""
        model_name = _get_moonshine_model()
        if not model_name or "Moonshine" not in model_name:
            self.skipTest("No Moonshine model configured for testing")

        wav_path = os.path.join(os.path.dirname(__file__), "test_speech.wav")
        if not os.path.exists(wav_path):
            self.skipTest("test_speech.wav not found")

        self._load_model(model_name)

        url = f"http://127.0.0.1:{PORT}/v1/audio/transcriptions"
        with open(wav_path, "rb") as f:
            files = {"file": ("test_speech.wav", f, "audio/wav")}
            data = {"model": model_name, "response_format": "json"}
            resp = requests.post(url, files=files, data=data, timeout=60)

        self.assertEqual(resp.status_code, 200, f"Transcription failed: {resp.text}")
        text = resp.json().get("text", "")
        print(f"[MoonshineTest] Real speech transcript: {text!r}")
        # test_speech.wav says "Just seeing if this is working."
        self.assertIn("working", text.lower())

    def _stream_wav_over_ws(self, ws_url, model_name, clear_before_streaming=False):
        """Stream test_speech.wav over a realtime WebSocket.

        Returns (events, final_text). With clear_before_streaming, an
        input_audio_buffer.clear is sent first (preceded by a little audio)
        to verify streaming continues after a clear.
        """
        import asyncio
        import base64
        import json as jsonlib

        import websockets

        wav_path = os.path.join(os.path.dirname(__file__), "test_speech.wav")
        chunk_ms = 100

        async def stream() -> tuple[set, str]:
            events = set()
            final_text = ""
            with wave.open(wav_path, "rb") as wf:
                rate = wf.getframerate()
                frames_per_chunk = rate * chunk_ms // 1000

                async with websockets.connect(ws_url) as ws:
                    await ws.send(jsonlib.dumps({
                        "type": "session.update",
                        "session": {"model": model_name},
                    }))

                    async def reader():
                        nonlocal final_text
                        try:
                            async for raw in ws:
                                msg = jsonlib.loads(raw)
                                events.add(msg.get("type"))
                                if msg.get("type") == (
                                    "conversation.item."
                                    "input_audio_transcription.completed"
                                ):
                                    final_text += " " + msg.get("transcript", "")
                        except websockets.exceptions.ConnectionClosed:
                            pass

                    rtask = asyncio.ensure_future(reader())

                    if clear_before_streaming:
                        # Send a few chunks, then clear — streaming must
                        # continue for the audio that follows
                        for _ in range(3):
                            frames = wf.readframes(frames_per_chunk)
                            await ws.send(jsonlib.dumps({
                                "type": "input_audio_buffer.append",
                                "audio": base64.b64encode(frames).decode(),
                            }))
                            await asyncio.sleep(chunk_ms / 1000)
                        wf.rewind()
                        await ws.send(jsonlib.dumps(
                            {"type": "input_audio_buffer.clear"}
                        ))
                        await asyncio.sleep(0.5)

                    while True:
                        frames = wf.readframes(frames_per_chunk)
                        if not frames:
                            break
                        await ws.send(jsonlib.dumps({
                            "type": "input_audio_buffer.append",
                            "audio": base64.b64encode(frames).decode(),
                        }))
                        await asyncio.sleep(chunk_ms / 1000)

                    # Trailing silence lets the streaming model close the line
                    silence = b"\x00\x00" * frames_per_chunk
                    for _ in range(15):
                        await ws.send(jsonlib.dumps({
                            "type": "input_audio_buffer.append",
                            "audio": base64.b64encode(silence).decode(),
                        }))
                        await asyncio.sleep(chunk_ms / 1000)

                    await ws.send(jsonlib.dumps(
                        {"type": "input_audio_buffer.commit"}
                    ))
                    await asyncio.sleep(3)
                    rtask.cancel()
            return events, final_text

        return asyncio.run(stream())

    def _assert_streaming_events(self, events, final_text):
        for expected in (
            "input_audio_buffer.speech_started",
            "conversation.item.input_audio_transcription.delta",
            "input_audio_buffer.speech_stopped",
            "conversation.item.input_audio_transcription.completed",
            "input_audio_buffer.committed",
        ):
            self.assertIn(expected, events, f"missing realtime event: {expected}")
        self.assertIn("working", final_text.lower())

    def _streaming_preflight(self):
        model_name = _get_moonshine_model()
        if not model_name or "Moonshine" not in model_name:
            self.skipTest("No Moonshine model configured for testing")
        wav_path = os.path.join(os.path.dirname(__file__), "test_speech.wav")
        if not os.path.exists(wav_path):
            self.skipTest("test_speech.wav not found")
        self._load_model(model_name)
        return model_name

    @skip_if_unsupported("realtime_websocket")
    def test_moonshine_realtime_streaming_main_port(self):
        """Realtime streaming via the WebSocket upgrade on the main HTTP port."""
        model_name = self._streaming_preflight()
        ws_url = f"ws://127.0.0.1:{PORT}/v1/realtime?model={model_name}"
        events, final_text = self._stream_wav_over_ws(ws_url, model_name)
        print(f"[MoonshineTest] main-port WS events: {sorted(events)}")
        print(f"[MoonshineTest] main-port WS final text: {final_text!r}")
        self._assert_streaming_events(events, final_text)

    @skip_if_unsupported("realtime_websocket")
    def test_moonshine_realtime_streaming_legacy_port(self):
        """Realtime streaming via the dedicated websocket_port (back-compat)."""
        model_name = self._streaming_preflight()
        health = requests.get(
            f"http://127.0.0.1:{PORT}/v1/health", timeout=TIMEOUT_DEFAULT
        ).json()
        ws_port = health.get("websocket_port")
        self.assertTrue(ws_port, "websocket_port missing from /health")
        ws_url = f"ws://127.0.0.1:{ws_port}/realtime?model={model_name}"
        events, final_text = self._stream_wav_over_ws(ws_url, model_name)
        print(f"[MoonshineTest] legacy-port WS events: {sorted(events)}")
        print(f"[MoonshineTest] legacy-port WS final text: {final_text!r}")
        self._assert_streaming_events(events, final_text)

    @skip_if_unsupported("realtime_websocket")
    def test_moonshine_realtime_clear_then_append(self):
        """input_audio_buffer.clear must not end streaming: audio appended
        after a clear still produces deltas and a final transcript."""
        model_name = self._streaming_preflight()
        ws_url = f"ws://127.0.0.1:{PORT}/v1/realtime?model={model_name}"
        events, final_text = self._stream_wav_over_ws(
            ws_url, model_name, clear_before_streaming=True
        )
        print(f"[MoonshineTest] clear-then-append events: {sorted(events)}")
        print(f"[MoonshineTest] clear-then-append final text: {final_text!r}")
        self.assertIn("input_audio_buffer.cleared", events)
        self._assert_streaming_events(events, final_text)

    @skip_if_unsupported("transcription")
    def test_moonshine_thread_count(self):
        """Verify Moonshine backend does not spawn excessive threads."""
        model_name = _get_moonshine_model()
        if not model_name or "Moonshine" not in model_name:
            self.skipTest("No Moonshine model configured for testing")

        self._load_model(model_name)

        # Find the moonshine-server subprocess PID
        import subprocess
        try:
            output = subprocess.check_output(
                ["pgrep", "-f", "moonshine-server"],
                text=True
            ).strip()
            if not output:
                self.skipTest("moonshine-server process not found")

            pid = int(output.splitlines()[0])
            with open(f"/proc/{pid}/status") as f:
                for line in f:
                    if line.startswith("Threads:"):
                        thread_count = int(line.split()[1])
                        self.assertLessEqual(
                            thread_count, 10,
                            f"moonshine-server spawned {thread_count} threads (expected <= 10)"
                        )
                        print(f"[MoonshineTest] Thread count: {thread_count}")
                        break
        except Exception as e:
            self.skipTest(f"Could not check thread count: {e}")


if __name__ == "__main__":
    run_server_tests(MoonshineTests)
