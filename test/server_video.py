"""
Video generation tests for Lemonade Server.

Tests the /videos job API (text -> video) with the stable-diffusion.cpp backend.

Usage:
    python server_video.py --wrapped-server sdcpp --backend vulkan
    python server_video.py --wrapped-server sdcpp --backend rocm

Video generation is slow, so the generation test asks for the shortest clip
that still exercises the frame loop. The negative tests run first and never
pull the model; only the generation tests download it.
"""

import time

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
    unload_model,
)
from utils.capabilities import get_test_model
from utils.test_models import TIMEOUT_DEFAULT, TIMEOUT_MODEL_OPERATION

# A short clip still runs a full diffusion loop per frame.
TIMEOUT_VIDEO_GENERATION = 3600

# WebM/Matroska magic. Enough to prove real container bytes came back rather
# than some other payload.
WEBM_MAGIC = b"\x1a\x45\xdf\xa3"

TERMINAL = {"completed", "failed", "interrupted"}


class VideoGenerationTests(ServerTestBase):
    """Tests for the /videos job API."""

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
            f"{self.base_url}/videos",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            expected_status,
            f"{context}: expected {expected_status}, got "
            f"{response.status_code}: {response.text[:200]}",
        )

    def _create(self, payload):
        response = requests.post(
            f"{self.base_url}/videos",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            202,
            f"create failed: {response.status_code} {response.text[:300]}",
        )
        job = response.json()
        self.assertIn("id", job, "create response carries no id")
        return job["id"]

    def _wait(self, job_id, timeout=TIMEOUT_VIDEO_GENERATION):
        deadline = time.time() + timeout
        status = None
        while time.time() < deadline:
            response = requests.get(
                f"{self.base_url}/videos/{job_id}",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(
                response.status_code,
                200,
                f"poll failed: {response.status_code} {response.text[:200]}",
            )
            payload = response.json()
            status = payload.get("status")
            if status in TERMINAL:
                return payload
            time.sleep(2)
        self.fail(f"video {job_id} did not finish within {timeout}s (last: {status})")

    # --- negative tests: these never pull the model ---

    def test_001_missing_prompt_rejected(self):
        """A request without a prompt is refused before any model load."""
        self._assert_rejected({"model": get_test_model("video")}, "missing prompt")

    def test_002_missing_model_rejected(self):
        """A request without a model is refused before any model load."""
        self._assert_rejected({"prompt": "a cat"}, "missing model")

    def test_003_unknown_video_returns_404(self):
        """Polling an id that was never created is a 404, not a hang."""
        response = requests.get(
            f"{self.base_url}/videos/does-not-exist",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 404, response.text[:200])

    def test_004_unknown_model_fails_the_job(self):
        """An unknown model fails the job rather than silently defaulting."""
        job_id = self._create({"model": "user.does-not-exist", "prompt": "a cat"})
        payload = self._wait(job_id, timeout=TIMEOUT_MODEL_OPERATION)
        self.assertEqual(
            payload["status"],
            "failed",
            f"unknown model unexpectedly succeeded: {payload}",
        )

    def test_005_content_before_completion_is_rejected(self):
        """The content route refuses to serve a clip that does not exist yet."""
        job_id = self._create({"model": "user.does-not-exist", "prompt": "a cat"})
        response = requests.get(
            f"{self.base_url}/videos/{job_id}/content",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 409, response.text[:200])
        self._wait(job_id, timeout=TIMEOUT_MODEL_OPERATION)

    # --- generation ---

    def test_010_generates_a_video(self):
        """A prompt produces a decodable clip with the requested frame count.

        Asserts the container bytes and the frame count rather than anything
        about how the clip looks: the point is that parameters reach the
        backend and the job runs to completion.
        """
        self._ensure_model_pulled()
        model = get_test_model("video")

        job_id = self._create(
            {
                "model": model,
                "prompt": "a lovely cat walking through tall grass",
                "video_frames": 9,
                "fps": 8,
                "steps": 4,
                "width": 480,
                "height": 320,
            }
        )
        payload = self._wait(job_id)
        self.assertEqual(
            payload["status"],
            "completed",
            f"generation failed: {payload}",
        )

        for field in ("mime_type", "frame_count", "fps"):
            self.assertIn(field, payload, f"status missing '{field}'")
        self.assertEqual(payload["frame_count"], 9)
        self.assertEqual(payload["fps"], 8)
        self.assertTrue(
            payload["mime_type"].startswith("video/"),
            f"unexpected mime_type: {payload['mime_type']}",
        )
        self.assertNotIn("b64_json", payload, "the clip leaked into the status payload")

        content = requests.get(
            f"{self.base_url}/videos/{job_id}/content",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(content.status_code, 200, content.text[:200])
        self.assertTrue(
            content.headers.get("Content-Type", "").startswith("video/"),
            f"unexpected Content-Type: {content.headers.get('Content-Type')}",
        )
        video = content.content
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

    def test_011_frame_default_applies_without_recipe_options(self):
        """A video model carrying no recipe options still returns a clip.

        The backend generates a single frame when video_frames is absent, so
        a model registered with nothing but its checkpoints has to pick the
        count up from the recipe descriptor rather than from the entry.
        """
        self._ensure_model_pulled()
        source = get_test_model("video")

        info = requests.get(
            f"{self.base_url}/models/{source}",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            info.status_code,
            200,
            f"could not read {source}: {info.text[:200]}",
        )
        checkpoints = info.json().get("checkpoints")
        self.assertTrue(checkpoints, f"{source} declares no checkpoints to reuse")

        bare = "user.video-no-recipe-options"
        registration = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": bare,
                "recipe": "sd-cpp",
                "labels": ["video"],
                "checkpoints": checkpoints,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(
            registration.status_code,
            200,
            f"could not register {bare}: {registration.text[:200]}",
        )
        self.addCleanup(
            requests.post,
            f"{self.base_url}/delete",
            json={"model_name": bare},
            timeout=TIMEOUT_DEFAULT,
        )

        job_id = self._create(
            {
                "model": bare,
                "prompt": "a lovely cat walking through tall grass",
                "steps": 2,
                "width": 480,
                "height": 320,
            }
        )
        payload = self._wait(job_id)
        self.assertEqual(
            payload["status"], "completed", f"generation failed: {payload}"
        )
        self.assertGreater(
            payload["frame_count"],
            1,
            "a model without recipe options fell through to a single frame",
        )
        print(f"[OK] default frame count reached the backend: {payload['frame_count']}")

    def test_012_cancel_releases_a_running_generation(self):
        """Cancelling ends the job and frees the slot.

        The clip itself may still finish on the GPU: stable-diffusion.cpp
        reports cancel_generating false and refuses to interrupt a job that is
        already producing frames, so this asserts the job state rather than
        that the backend stopped.
        """
        self._ensure_model_pulled()
        model = get_test_model("video")

        job_id = self._create(
            {
                "model": model,
                "prompt": "a lovely cat walking through tall grass",
                "video_frames": 33,
                "steps": 20,
                "width": 832,
                "height": 480,
            }
        )

        # Let it reach the backend before asking it to stop.
        deadline = time.time() + TIMEOUT_MODEL_OPERATION
        while time.time() < deadline:
            payload = requests.get(
                f"{self.base_url}/videos/{job_id}", timeout=TIMEOUT_DEFAULT
            ).json()
            if payload.get("status") == "running":
                break
            if payload.get("status") in TERMINAL:
                self.fail(f"job finished before it could be cancelled: {payload}")
            time.sleep(2)

        cancel = requests.post(
            f"{self.base_url}/videos/{job_id}/cancel",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(cancel.status_code, 200, cancel.text[:200])

        payload = self._wait(job_id, timeout=TIMEOUT_MODEL_OPERATION)
        self.assertIn(
            payload["status"],
            ("interrupted", "failed"),
            f"cancel did not stop the job: {payload}",
        )
        print(f"[OK] cancelled generation ended as {payload['status']}")


if __name__ == "__main__":
    run_server_tests(VideoGenerationTests, description="VIDEO GENERATION TESTS")
