"""
Hand-tracking tests for Lemonade Server.

Tests the /images/hands endpoint (image -> hands with landmarks) with the
mediapipe backend (MediaPipe hand landmarker on the CPU).

Usage:
    python server_hands.py --wrapped-server mediapipe --backend cpu

The first load builds the backend's Python venv with the pinned mediapipe wheel.
The 0xx tests never pull or load the model.
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

TIMEOUT_HANDS = 900

# MediaPipe's own hand landmarker test image, pinned by hash.
THUMB_URL = "https://storage.googleapis.com/mediapipe-assets/thumb_up.jpg"
THUMB_SHA256 = "5d673c081ab13b8a1812269ff57047066f9c33c07db5f4178089e8cb3fdc0291"

# mediapipe 1.1.1 with hand_landmarker.task (float16 v1) on thumb_up.jpg.
THUMB_REFERENCE = {
    "handedness": "left",
    "box": (128.77, 102.06, 244.04, 283.69),
    "wrist": (244.04, 272.51),
    "thumb_tip": (180.92, 102.06),
}
PX_TOL = 4.0

TINY_PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNoaGgAAAMEAYFL09IQAAAAAElFTkSuQmCC"

_thumb = None


def thumb_b64() -> str:
    global _thumb
    if _thumb is None:
        with urllib.request.urlopen(THUMB_URL, timeout=60) as r:
            data = r.read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != THUMB_SHA256:
            raise RuntimeError(f"{THUMB_URL} changed (sha256 {digest})")
        _thumb = data
    return base64.b64encode(_thumb).decode()


class HandsTests(ServerTestBase):
    """Tests for the /images/hands endpoint."""

    _model_pulled = False

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return
        model = get_test_model("hands")
        print(f"\n[SETUP] Ensuring {model} is pulled...")
        pull_model_with_retry(model)
        cls._model_pulled = True

    @property
    def url(self):
        return f"{self.base_url}/images/hands"

    def _payload(self, **overrides):
        payload = {"model": get_test_model("hands")}
        payload.update(overrides)
        payload.setdefault("image", thumb_b64())
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

    def _hands(self, payload):
        response = requests.post(self.url, json=payload, timeout=TIMEOUT_HANDS)
        self.assertEqual(response.status_code, 200, response.text[:1000])
        return response.json()

    def test_001_missing_model_error(self):
        payload = self._payload(image=TINY_PNG)
        del payload["model"]
        self._assert_rejected(payload, "Missing model", expected_status=400)

    def test_002_invalid_model_error(self):
        self._assert_rejected(
            self._payload(model="nonexistent-hands-xyz-123", image=TINY_PNG),
            "Invalid model",
            expected_status=404,
        )

    def test_003_bad_parameters_rejected(self):
        self._assert_rejected(
            self._payload(image=TINY_PNG, max_hands=17), "max_hands > 16", 400
        )
        self._assert_rejected(
            self._payload(image=TINY_PNG, mirrored="yes"), "mirrored not bool", 400
        )

    def test_500_thumb_up(self):
        """thumb_up.jpg matches the reference hand and landmarks."""
        self._ensure_model_pulled()
        body = self._hands(self._payload())
        image = body["data"][0]
        self.assertEqual(image["object"], "image.hands")
        self.assertEqual(len(image["hands"]), 1, image["hands"])
        hand = image["hands"][0]
        self.assertEqual(hand["object"], "hand")
        self.assertEqual(hand["handedness"], THUMB_REFERENCE["handedness"])
        self.assertGreater(hand["score"], 0.9)
        self.assertEqual(len(hand["landmarks"]), 21)
        self.assertEqual(hand["landmarks"][0]["name"], "wrist")
        for k, v in zip(("x1", "y1", "x2", "y2"), THUMB_REFERENCE["box"]):
            self.assertAlmostEqual(hand["box"][k], v, delta=PX_TOL, msg=k)
        named = {p["name"]: p for p in hand["landmarks"]}
        for name in ("wrist", "thumb_tip"):
            self.assertAlmostEqual(
                named[name]["x"], THUMB_REFERENCE[name][0], delta=PX_TOL
            )
            self.assertAlmostEqual(
                named[name]["y"], THUMB_REFERENCE[name][1], delta=PX_TOL
            )
        print(f"[OK] {hand['handedness']} hand in {body['usage']['inference_ms']} ms")

    def test_501_mirrored_flips_handedness(self):
        self._ensure_model_pulled()
        body = self._hands(self._payload(mirrored=True))
        self.assertEqual(body["data"][0]["hands"][0]["handedness"], "right")

    def test_502_no_hands_and_model_optional(self):
        self._ensure_model_pulled()
        self._hands(self._payload())
        payload = self._payload(image=TINY_PNG)
        del payload["model"]
        body = self._hands(payload)
        self.assertEqual(body["model"], get_test_model("hands"))
        self.assertEqual(body["data"][0]["hands"], [])


if __name__ == "__main__":
    run_server_tests(
        HandsTests,
        "HANDS TESTS",
        modality="hands",
        default_wrapped_server="mediapipe",
    )
