"""
Video generation tests for Lemonade Server.

Tests the /videos/generations endpoint (text -> video) with the
stable-diffusion.cpp backend.

Usage:
    python server_video.py --wrapped-server sdcpp --backend vulkan
    python server_video.py --wrapped-server sdcpp --backend rocm

Video generation is slow, so the generation test asks for the shortest clip
that still exercises the frame loop. The negative tests run first and never
pull the model; only the generation test downloads it.
"""

import base64

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
    unload_model,
)
from utils.capabilities import get_test_model
from utils.test_models import TIMEOUT_DEFAULT

# A short clip still runs a full diffusion loop per frame.
TIMEOUT_VIDEO_GENERATION = 3600

# WebM/Matroska magic. Enough to prove real container bytes came back rather
# than a base64 blob of something else.
WEBM_MAGIC = b"\x1a\x45\xdf\xa3"


class VideoGenerationTests(ServerTestBase):
    """Tests for the /videos/generations endpoint."""

    _model_pulled = False

    @classmethod
    def tearDownClass(cls):
        try:
            response = unload_model(get_test_model("video"))
            if response.status_code not in (200, 404):
                print(
                    "Warning: Failed to unload the video backend: "
                    f"{response.status_code} {response.text[:200]}"
                )
        except Exception as e:
            print(f"Warning: Failed to unload the video backend: {e}")
        super().tearDownClass()

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return
        model = get_test_model("video")
        print(f"\n[SETUP] Ensuring {model} is pulled...")
        pull_model_with_retry(model)
        print(f"[SETUP] {model} is ready")
        cls._model_pulled = True

    def _assert_rejected(self, payload, context, expected_status=400):
        response = requests.post(
            f"{self.base_url}/videos/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            expected_status,
            f"{context}: expected {expected_status}, got "
            f"{response.status_code}: {response.text[:200]}",
        )

    # --- negative tests: these never pull the model ---

    def test_001_missing_prompt_rejected(self):
        """A request without a prompt is refused before any model load."""
        self._assert_rejected({"model": get_test_model("video")}, "missing prompt")

    def test_002_missing_model_rejected(self):
        """A request without a model is refused before any model load."""
        self._assert_rejected({"prompt": "a cat"}, "missing model")

    def test_003_unknown_model_rejected(self):
        """An unknown model is refused rather than silently defaulting."""
        response = requests.post(
            f"{self.base_url}/videos/generations",
            json={"model": "user.does-not-exist", "prompt": "a cat"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertNotEqual(
            response.status_code,
            200,
            f"unknown model unexpectedly succeeded: {response.text[:200]}",
        )

    # --- generation ---

    def test_010_generates_a_video(self):
        """A prompt returns a decodable video with the requested frame count.

        Asserts the container bytes and the frame count rather than anything
        about how the clip looks: the point is that parameters reach the
        backend and the async job is polled to completion.
        """
        self._ensure_model_pulled()
        model = get_test_model("video")

        response = requests.post(
            f"{self.base_url}/videos/generations",
            json={
                "model": model,
                "prompt": "a lovely cat walking through tall grass",
                "video_frames": 9,
                "fps": 8,
                "steps": 4,
                "width": 480,
                "height": 320,
            },
            timeout=TIMEOUT_VIDEO_GENERATION,
        )
        self.assertEqual(
            response.status_code,
            200,
            f"generation failed: {response.status_code} {response.text[:300]}",
        )

        payload = response.json()
        for field in ("b64_json", "mime_type", "frame_count", "fps"):
            self.assertIn(field, payload, f"response missing '{field}'")

        self.assertEqual(payload["frame_count"], 9)
        self.assertEqual(payload["fps"], 8)
        self.assertTrue(
            payload["mime_type"].startswith("video/"),
            f"unexpected mime_type: {payload['mime_type']}",
        )

        video = base64.b64decode(payload["b64_json"])
        self.assertGreater(len(video), 1024, "video payload is implausibly small")
        self.assertEqual(
            video[:4],
            WEBM_MAGIC,
            "payload is not a WebM/Matroska container",
        )
        print(
            f"[OK] {len(video)} bytes, {payload['frame_count']} frames "
            f"@ {payload['fps']} fps"
        )


if __name__ == "__main__":
    run_server_tests(VideoGenerationTests, description="VIDEO GENERATION TESTS")
