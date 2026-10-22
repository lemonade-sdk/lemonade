"""
Pose-estimation tests for Lemonade Server.

Tests the /images/poses endpoint (image -> people with body keypoints) with the
ryzenai-vision backend (YOLO26-pose on the Ryzen AI NPU).

Usage:
    LEMONADE_RYZENAIVISION_NPU_BIN=/path/to/ryzen-ai-venv \\
        python server_poses.py --wrapped-server ryzenai-vision --backend npu

The first load compiles the model for the NPU, which can take tens of minutes;
later runs reuse the compile cache. The 0xx tests never pull or load the model.
"""

import base64
import hashlib
import urllib.request

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
)
from utils.capabilities import get_test_model
from utils.test_models import TIMEOUT_DEFAULT

TIMEOUT_POSE = 3600

BUS_URL = (
    "https://raw.githubusercontent.com/ultralytics/assets/"
    "53d4e2a8fd15f473c29a148c317a8477be559327/im/bus.jpg"
)
BUS_SHA256 = "c02019c4979c191eb739ddd944445ef408dad5679acab6fd520ef9d434bfbc63"

# The three clear people in bus.jpg from the ORIGINAL Ultralytics yolo26s-pose
# end-to-end export on CPU (fp32): score, box, nose. The NPU runs the graph in
# bf16, so they are compared with a tolerance.
BUS_REFERENCE = [
    (0.920, (52.8, 401.0, 252.8, 903.7), (143.8, 443.3)),
    (0.910, (222.4, 407.5, 345.9, 862.3), (295.3, 450.7)),
    (0.909, (668.8, 397.6, 810.0, 877.2), (787.8, 434.5)),
]
SCORE_TOL = 0.05
PX_TOL = 12.0

COCO_KEYPOINTS = [
    "nose", "left_eye", "right_eye", "left_ear", "right_ear",
    "left_shoulder", "right_shoulder", "left_elbow", "right_elbow",
    "left_wrist", "right_wrist", "left_hip", "right_hip",
    "left_knee", "right_knee", "left_ankle", "right_ankle",
]  # fmt: skip

TINY_PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNoaGgAAAMEAYFL09IQAAAAAElFTkSuQmCC"

_bus_jpeg = None


def bus_b64() -> str:
    global _bus_jpeg
    if _bus_jpeg is None:
        with urllib.request.urlopen(BUS_URL, timeout=60) as r:
            data = r.read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != BUS_SHA256:
            raise RuntimeError(f"{BUS_URL} changed (sha256 {digest})")
        _bus_jpeg = data
    return base64.b64encode(_bus_jpeg).decode()


class PoseTests(ServerTestBase):
    """Tests for the /images/poses endpoint."""

    _model_pulled = False

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return
        model = get_test_model("pose")
        print(f"\n[SETUP] Ensuring {model} is pulled...")
        pull_model_with_retry(model)
        cls._model_pulled = True

    @property
    def url(self):
        return f"{self.base_url}/images/poses"

    def _payload(self, **overrides):
        payload = {"model": get_test_model("pose")}
        payload.update(overrides)
        payload.setdefault("image", bus_b64())
        return payload

    def _assert_rejected(self, payload, context, expected_status):
        response = requests.post(self.url, json=payload, timeout=TIMEOUT_DEFAULT)
        self.assertEqual(
            response.status_code,
            expected_status,
            f"{context}: expected {expected_status}, got {response.status_code}: "
            f"{response.text[:1000]}",
        )
        self.assertIn("error", response.json())
        print(f"[OK] {context}: {response.status_code}")

    def _poses(self, payload):
        response = requests.post(self.url, json=payload, timeout=TIMEOUT_POSE)
        self.assertEqual(response.status_code, 200, response.text[:1000])
        return response.json()

    def test_001_missing_model_error(self):
        payload = self._payload(image=TINY_PNG)
        del payload["model"]
        self._assert_rejected(payload, "Missing model", expected_status=400)

    def test_002_invalid_model_error(self):
        self._assert_rejected(
            self._payload(model="nonexistent-pose-xyz-123", image=TINY_PNG),
            "Invalid model",
            expected_status=404,
        )

    def test_003_bad_parameters_rejected(self):
        self._assert_rejected(
            self._payload(image=TINY_PNG, max_poses=0), "max_poses = 0", 400
        )
        self._assert_rejected(
            self._payload(image=TINY_PNG, confidence=-0.1), "confidence < 0", 400
        )
        self._assert_rejected(
            self._payload(image="not base64 image"), "Non-image payload", 400
        )

    def test_500_bus_people_and_keypoints(self):
        """bus.jpg matches the original model's people and noses."""
        self._ensure_model_pulled()
        body = self._poses(self._payload())
        self.assertEqual(body["object"], "list")
        image = body["data"][0]
        self.assertEqual(image["object"], "image.poses")
        self.assertEqual((image["width"], image["height"]), (810, 1080))
        poses = image["poses"]
        self.assertEqual(body["usage"]["poses"], len(poses))
        for pose in poses:
            self.assertEqual(pose["object"], "pose")
            self.assertEqual([k["name"] for k in pose["keypoints"]], COCO_KEYPOINTS)
        for score, box, nose in BUS_REFERENCE:
            best = min(
                poses,
                key=lambda p: abs(p["box"]["x1"] - box[0])
                + abs(p["box"]["y1"] - box[1]),
            )
            self.assertAlmostEqual(best["score"], score, delta=SCORE_TOL)
            for k, v in zip(("x1", "y1", "x2", "y2"), box):
                self.assertAlmostEqual(best["box"][k], v, delta=PX_TOL, msg=k)
            got = next(k for k in best["keypoints"] if k["name"] == "nose")
            self.assertAlmostEqual(got["x"], nose[0], delta=PX_TOL)
            self.assertAlmostEqual(got["y"], nose[1], delta=PX_TOL)
        print(
            f"[OK] {len(poses)} poses in {body['usage']['inference_ms']} ms: "
            + ", ".join(f"{p['score']:.2f}" for p in poses)
        )

    def test_501_limits_and_model_optional(self):
        self._ensure_model_pulled()
        self._poses(self._payload())
        payload = self._payload(max_poses=1, confidence=0.5)
        del payload["model"]
        body = self._poses(payload)
        self.assertEqual(body["model"], get_test_model("pose"))
        self.assertLessEqual(len(body["data"][0]["poses"]), 1)

    def test_502_detection_model_is_rejected(self):
        """A detection model cannot answer /images/poses."""
        self._ensure_model_pulled()
        pull_model_with_retry("YOLO26s-Detection-NPU")
        response = requests.post(
            self.url,
            json=self._payload(model="YOLO26s-Detection-NPU", image=TINY_PNG),
            timeout=TIMEOUT_POSE,
        )
        self.assertEqual(response.status_code, 400, response.text[:500])


if __name__ == "__main__":
    run_server_tests(
        PoseTests,
        "POSE TESTS",
        modality="pose",
        default_wrapped_server="ryzenai-vision",
    )
