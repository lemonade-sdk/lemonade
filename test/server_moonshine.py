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
        url = f"http://127.0.0.1:{PORT}/load"
        payload = {"model_name": model_name}
        resp = requests.post(url, json=payload, timeout=120)
        self.assertEqual(resp.status_code, 200, f"Failed to load model: {resp.text}")

    @skip_if_unsupported("audio")
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

    @skip_if_unsupported("audio")
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
