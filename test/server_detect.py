"""
Object-detection tests for Lemonade Server.

Tests the /images/detections endpoint (image -> labeled boxes) with the
ryzenai-vision backend (ryzenai_vision_server.py on the Ryzen AI NPU).

Usage:
    LEMONADE_RYZENAIVISION_NPU_BIN=/path/to/ryzen-ai-venv \\
        python server_detect.py --wrapped-server ryzenai-vision --backend npu

The ryzenai-vision backend runs on a separately installed Ryzen AI Software
environment; point the server at it with LEMONADE_RYZENAIVISION_NPU_BIN (or
ryzenaivision.npu_bin in config.json). The first load compiles the model for
the NPU, which can take tens of minutes; later runs reuse the compile cache.

The 0xx tests never pull or load the model.
"""

import base64
import hashlib
import io
import urllib.request

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
)
from utils.capabilities import get_test_model
from utils.test_models import TIMEOUT_DEFAULT

# Covers the first-ever NPU compile of the model.
TIMEOUT_DETECT = 3600

# Ultralytics' canonical test image (810x1080), pinned by hash.
BUS_URL = (
    "https://raw.githubusercontent.com/ultralytics/assets/"
    "53d4e2a8fd15f473c29a148c317a8477be559327/im/bus.jpg"
)
BUS_SHA256 = "c02019c4979c191eb739ddd944445ef408dad5679acab6fd520ef9d434bfbc63"

# Detections of the ORIGINAL Ultralytics yolo26s end-to-end export on CPU (fp32).
# The NPU runs the graph in bf16, so scores and boxes are compared with a
# tolerance rather than exactly.
BUS_REFERENCE = [
    ("bus", 0.970, (6.3, 229.6, 803.1, 741.5)),
    ("person", 0.943, (668.1, 392.2, 810.0, 880.4)),
    ("person", 0.938, (49.4, 398.9, 246.7, 903.0)),
    ("person", 0.911, (222.1, 407.0, 344.2, 861.4)),
]
SCORE_TOL = 0.05
BOX_TOL_PX = 12.0

# 1x1 gray PNG: a valid image for the 0xx tests, which must not touch the network.
TINY_PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNoaGgAAAMEAYFL09IQAAAAAElFTkSuQmCC"

_bus_jpeg = None


def bus_jpeg() -> bytes:
    global _bus_jpeg
    if _bus_jpeg is None:
        with urllib.request.urlopen(BUS_URL, timeout=60) as r:
            data = r.read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != BUS_SHA256:
            raise RuntimeError(f"{BUS_URL} changed (sha256 {digest})")
        _bus_jpeg = data
    return _bus_jpeg


def bus_b64() -> str:
    return base64.b64encode(bus_jpeg()).decode()


class DetectTests(ServerTestBase):
    """Tests for the /images/detections endpoint."""

    _model_pulled = False

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return
        model = get_test_model("detection")
        print(f"\n[SETUP] Ensuring {model} is pulled...")
        pull_model_with_retry(model)
        print(f"[SETUP] {model} is ready")
        cls._model_pulled = True

    @property
    def url(self):
        return f"{self.base_url}/images/detections"

    def _payload(self, **overrides):
        payload = {"model": get_test_model("detection")}
        payload.update(overrides)
        payload.setdefault("image", bus_b64())
        return payload

    def _offline_payload(self, **overrides):
        return self._payload(**{"image": TINY_PNG, **overrides})

    def _detect(self, **kwargs):
        response = requests.post(self.url, timeout=TIMEOUT_DETECT, **kwargs)
        self.assertEqual(
            response.status_code,
            200,
            f"detect failed with status {response.status_code}: {response.text[:1000]}",
        )
        return response.json()

    def _assert_rejected(self, payload, context, expected_status):
        response = requests.post(self.url, json=payload, timeout=TIMEOUT_DEFAULT)
        self.assertEqual(
            response.status_code,
            expected_status,
            f"{context}: expected {expected_status}, got {response.status_code}: "
            f"{response.text[:1000]}",
        )
        self.assertIn("error", response.json(), f"{context}: missing 'error' field")
        print(f"[OK] {context}: {response.status_code} {response.json()['error']}")

    def _assert_matches_reference(self, image_result):
        dets = image_result["detections"]
        for label, score, box in BUS_REFERENCE:
            candidates = [d for d in dets if d["label"] == label]
            self.assertTrue(candidates, f"no '{label}' detected: {dets}")
            best = min(
                candidates,
                key=lambda d: sum(
                    abs(d["box"][k] - v) for k, v in zip(("x1", "y1", "x2", "y2"), box)
                ),
            )
            for k, v in zip(("x1", "y1", "x2", "y2"), box):
                self.assertAlmostEqual(
                    best["box"][k],
                    v,
                    delta=BOX_TOL_PX,
                    msg=f"{label} {k}: {best['box'][k]} vs reference {v}",
                )
            self.assertAlmostEqual(
                best["score"],
                score,
                delta=SCORE_TOL,
                msg=f"{label} score {best['score']} vs reference {score}",
            )

    def test_001_missing_model_error(self):
        """No model and nothing loaded is a 400, without pulling anything."""
        payload = self._offline_payload()
        del payload["model"]
        self._assert_rejected(payload, "Missing model", expected_status=400)

    def test_002_invalid_model_error(self):
        """A nonexistent model is a 404 model_not_found, not a download attempt."""
        self._assert_rejected(
            self._offline_payload(model="nonexistent-detector-xyz-123"),
            "Invalid model",
            expected_status=404,
        )

    def test_003_missing_image_error(self):
        payload = self._offline_payload()
        del payload["image"]
        self._assert_rejected(payload, "Missing image", expected_status=400)

    def test_004_remote_url_rejected(self):
        self._assert_rejected(
            self._offline_payload(image={"image_url": {"url": BUS_URL}}),
            "Remote URL",
            expected_status=400,
        )

    def test_005_not_an_image_rejected(self):
        self._assert_rejected(
            self._offline_payload(
                image=base64.b64encode(b"hello world, not an image").decode()
            ),
            "Non-image payload",
            expected_status=400,
        )

    def test_006_bad_parameters_rejected(self):
        self._assert_rejected(
            self._offline_payload(confidence=1.5), "confidence > 1", expected_status=400
        )
        self._assert_rejected(
            self._offline_payload(max_detections=0),
            "max_detections = 0",
            expected_status=400,
        )
        self._assert_rejected(
            self._offline_payload(classes=[""]), "empty class name", expected_status=400
        )

    def test_007_get_not_allowed(self):
        response = requests.get(self.url, timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 405)

    def test_500_detects_bus_scene(self):
        """bus.jpg matches the reference detections of the original model."""
        self._ensure_model_pulled()
        payload = self._payload(image=f"data:image/jpeg;base64,{bus_b64()}")
        body = self._detect(json=payload)

        self.assertEqual(body.get("object"), "list")
        self.assertEqual(body.get("model"), payload["model"])
        self.assertIsInstance(body.get("created"), int)
        self.assertEqual(len(body["data"]), 1)
        image = body["data"][0]
        self.assertEqual(image["object"], "image.detections")
        self.assertEqual(
            (image["index"], image["width"], image["height"]), (0, 810, 1080)
        )
        scores = [d["score"] for d in image["detections"]]
        self.assertEqual(scores, sorted(scores, reverse=True), "not sorted by score")
        for d in image["detections"]:
            self.assertEqual(d["object"], "detection")
            self.assertGreaterEqual(d["score"], 0.25)
            self.assertLessEqual(d["score"], 1.0)
            b = d["box"]
            self.assertTrue(0 <= b["x1"] <= b["x2"] <= 810, b)
            self.assertTrue(0 <= b["y1"] <= b["y2"] <= 1080, b)
        self.assertEqual(body["usage"]["images"], 1)
        self.assertEqual(body["usage"]["detections"], len(image["detections"]))
        self._assert_matches_reference(image)
        print(
            f"[OK] {len(image['detections'])} detections in "
            f"{body['usage']['inference_ms']} ms: "
            + ", ".join(f"{d['label']} {d['score']:.2f}" for d in image["detections"])
        )

    def test_501_multipart_matches_json(self):
        self._ensure_model_pulled()
        model = get_test_model("detection")
        from_json = self._detect(json=self._payload())
        from_form = self._detect(
            data={"model": model},
            files={"image": ("bus.jpg", io.BytesIO(bus_jpeg()), "image/jpeg")},
        )
        self.assertEqual(
            from_form["data"][0]["detections"], from_json["data"][0]["detections"]
        )

    def test_502_class_filter_and_synonyms(self):
        """Filters by class, maps everyday names, and reports unknown ones."""
        self._ensure_model_pulled()
        body = self._detect(json=self._payload(classes=["bus", "unicorn"]))
        labels = {d["label"] for d in body["data"][0]["detections"]}
        self.assertEqual(labels, {"bus"})
        self.assertEqual(body.get("unresolved_classes"), ["unicorn"])

        body = self._detect(json=self._payload(classes=["the red ball"]))
        self.assertEqual(body["data"][0]["detections"], [])
        self.assertNotIn("unresolved_classes", body, "'ball' maps to 'sports ball'")

    def test_503_batch_and_limits(self):
        self._ensure_model_pulled()
        body = self._detect(
            json=self._payload(
                image=[bus_b64(), {"image_url": f"data:image/jpeg;base64,{bus_b64()}"}],
                max_detections=2,
                confidence=0.5,
            )
        )
        self.assertEqual([d["index"] for d in body["data"]], [0, 1])
        for image in body["data"]:
            self.assertLessEqual(len(image["detections"]), 2)
            self.assertTrue(all(d["score"] >= 0.5 for d in image["detections"]))
        self.assertEqual(body["data"][0]["detections"], body["data"][1]["detections"])

    def test_504_model_optional_when_one_detector_loaded(self):
        self._ensure_model_pulled()
        model = get_test_model("detection")
        self._detect(json=self._payload())
        payload = self._payload()
        del payload["model"]
        body = self._detect(json=payload)
        self.assertEqual(body.get("model"), model)


if __name__ == "__main__":
    run_server_tests(
        DetectTests,
        "DETECT TESTS",
        modality="detection",
        default_wrapped_server="ryzenai-vision",
    )
