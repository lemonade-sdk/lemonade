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

import base64
import io
import time

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
    load_model,
    unload_model,
)
from utils.capabilities import get_current_config, get_test_model
from utils.test_models import TIMEOUT_DEFAULT, TIMEOUT_MODEL_OPERATION

# A short clip still runs a full diffusion loop per frame.
TIMEOUT_VIDEO_GENERATION = 3600

# Far less than the canceled clip in test_012 would take to finish.
TIMEOUT_CANCELED_CLIP_RELEASED = 180

# WebM/Matroska magic. Enough to prove real container bytes came back rather
# than some other payload.
WEBM_MAGIC = b"\x1a\x45\xdf\xa3"

TERMINAL = {"completed", "failed", "canceled"}

# 1x1 red PNG, enough for the image endpoints to read an input image.
TINY_PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"
)


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

    def _wait_until_generating(self, job_id, model):
        """Return once the clip is on the GPU, past model load and submit."""
        deadline = time.time() + TIMEOUT_MODEL_OPERATION
        while time.time() < deadline:
            payload = requests.get(
                f"{self.base_url}/videos/{job_id}", timeout=TIMEOUT_DEFAULT
            ).json()
            if payload.get("status") in TERMINAL:
                self.fail(f"video finished before it was generating: {payload}")
            health = requests.get(
                f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
            ).json()
            names = [m.get("model_name") for m in health.get("all_models_loaded", [])]
            if payload.get("status") == "running" and model in names:
                break
            time.sleep(1)
        else:
            self.fail(f"video {job_id} never started generating")
        time.sleep(5)

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
        for field in ("created_at", "started_at", "completed_at"):
            self.assertIn(field, payload, f"status missing '{field}'")
        self.assertLessEqual(payload["created_at"], payload["started_at"])
        self.assertLessEqual(payload["started_at"], payload["completed_at"])

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

        # The job store is rewritten on every job transition, so the clip must
        # stay out of it.
        job = requests.get(f"{self.base_url}/jobs/{job_id}", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(job.status_code, 200, job.text[:200])
        self.assertNotIn("b64_json", job.text, "the clip was stored in the job record")
        self.assertLess(len(job.content), 64 * 1024, "the job record carries the clip")
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

    def test_013_duration_sets_the_frame_count(self):
        """A duration in seconds becomes the nearest frame count the model takes.

        One second at 8 fps is 8 frames, which Wan cannot generate; the nearest
        4n+1 count is 9, where sd.cpp on its own would floor 8 down to 5.
        """
        self._ensure_model_pulled()
        job_id = self._create(
            {
                "model": get_test_model("video"),
                "prompt": "a lovely cat walking through tall grass",
                "duration": 1,
                "fps": 8,
                "steps": 4,
                "width": 480,
                "height": 320,
            }
        )
        payload = self._wait(job_id)
        self.assertEqual(
            payload["status"], "completed", f"generation failed: {payload}"
        )
        self.assertEqual(payload["frame_count"], 9)
        self.assertAlmostEqual(payload["duration"], 9 / 8)
        print(
            f"[OK] duration 1s -> {payload['frame_count']} frames, "
            f"{payload['duration']}s"
        )

    def test_012_cancel_releases_a_running_generation(self):
        """Cancelling a clip that is generating stops it on the GPU.

        sd-server refuses to cancel a clip once it produces frames, so Lemonade
        stops the process. The canceled clip would run for many minutes, so a
        short clip that finishes soon after the cancel shows the GPU was freed.
        The model is loaded first so the job does not own it: a job unloads a
        model it loaded itself, which would free the GPU without the stop.
        """
        self._ensure_model_pulled()
        model = get_test_model("video")
        response = load_model(model)
        self.assertEqual(response.status_code, 200, response.text[:300])

        job_id = self._create(
            {
                "model": model,
                "prompt": "a lovely cat walking through tall grass",
                "video_frames": 81,
                "steps": 50,
                "width": 832,
                "height": 480,
            }
        )
        self._wait_until_generating(job_id, model)

        cancel = requests.post(
            f"{self.base_url}/videos/{job_id}/cancel",
            timeout=TIMEOUT_DEFAULT,
        )
        canceled_at = time.time()
        self.assertEqual(cancel.status_code, 200, cancel.text[:200])
        self.assertEqual(cancel.json().get("status"), "canceling")

        payload = self._wait(job_id, timeout=TIMEOUT_MODEL_OPERATION)
        self.assertEqual(
            payload["status"], "canceled", f"cancel did not stop the job: {payload}"
        )

        next_id = self._create(
            {
                "model": model,
                "prompt": "a lovely cat walking through tall grass",
                "video_frames": 9,
                "steps": 4,
                "width": 480,
                "height": 320,
            }
        )
        payload = self._wait(next_id, timeout=TIMEOUT_CANCELED_CLIP_RELEASED)
        self.assertEqual(payload["status"], "completed", f"next clip failed: {payload}")
        elapsed = time.time() - canceled_at
        print(f"[OK] canceled, and the next clip finished {elapsed:.1f}s later")

    def test_014_other_models_keep_serving_during_a_video(self):
        """A chat request on another model answers while a clip is generating."""
        self._ensure_model_pulled()
        _, backend, _ = get_current_config()
        chat_model = get_test_model("llm", "llamacpp", backend, "llm")
        pull_model_with_retry(chat_model)
        response = load_model(chat_model, llamacpp_backend=backend)
        self.assertEqual(response.status_code, 200, response.text[:300])

        video_model = get_test_model("video")
        job_id = self._create(
            {
                "model": video_model,
                "prompt": "a lovely cat walking through tall grass",
                "video_frames": 33,
                "steps": 20,
                "width": 832,
                "height": 480,
            }
        )
        try:
            self._wait_until_generating(job_id, video_model)

            start = time.time()
            chat = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": chat_model,
                    "messages": [{"role": "user", "content": "Say hi."}],
                    "max_tokens": 8,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            elapsed = time.time() - start
            self.assertEqual(chat.status_code, 200, chat.text[:300])

            payload = requests.get(
                f"{self.base_url}/videos/{job_id}", timeout=TIMEOUT_DEFAULT
            ).json()
            self.assertEqual(
                payload.get("status"),
                "running",
                f"chat waited for the video ({elapsed:.1f}s): {payload}",
            )
            print(f"[OK] chat answered in {elapsed:.1f}s while the clip was running")
        finally:
            requests.post(
                f"{self.base_url}/videos/{job_id}/cancel", timeout=TIMEOUT_DEFAULT
            )
            self._wait(job_id, timeout=TIMEOUT_MODEL_OPERATION)
            unload_model(chat_model)

    def test_015_image_endpoints_reject_a_video_model(self):
        """A video model is refused by image generation, edits and variations."""
        self._ensure_model_pulled()
        video_model = get_test_model("video")

        def image_file():
            return {"image": ("in.png", io.BytesIO(TINY_PNG), "image/png")}

        responses = {
            "generations": requests.post(
                f"{self.base_url}/images/generations",
                json={"model": video_model, "prompt": "a kite"},
                timeout=TIMEOUT_MODEL_OPERATION,
            ),
            "edits": requests.post(
                f"{self.base_url}/images/edits",
                files=image_file(),
                data={"model": video_model, "prompt": "a kite"},
                timeout=TIMEOUT_MODEL_OPERATION,
            ),
            "variations": requests.post(
                f"{self.base_url}/images/variations",
                files=image_file(),
                data={"model": video_model},
                timeout=TIMEOUT_MODEL_OPERATION,
            ),
        }
        for endpoint, response in responses.items():
            # Image routes report every backend error with one status, so the
            # message is what tells this refusal apart.
            self.assertNotEqual(
                response.status_code, 200, f"{endpoint}: {response.text[:300]}"
            )
            self.assertIn("is a video model", response.text, endpoint)
        print("[OK] image generations, edits and variations refuse a video model")


if __name__ == "__main__":
    run_server_tests(VideoGenerationTests, description="VIDEO GENERATION TESTS")
