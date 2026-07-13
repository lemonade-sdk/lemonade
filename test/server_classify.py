"""
Text-classification tests for Lemonade Server.

Tests the /classify endpoint (text -> {label: score}) with the onnxruntime
backend (ort-server subprocess, CPU EP).

Usage:
    python server_classify.py --wrapped-server onnxruntime --backend cpu

The negative tests run first and never pull the model; only the classification
test downloads it.
"""

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
)
from utils.capabilities import get_test_model
from utils.test_models import TIMEOUT_DEFAULT

TIMEOUT_CLASSIFY = 600

# Reference scores from the ORIGINAL PyTorch model
# (cybersectony/phishing-email-detection-distilbert_v2.4.1) via transformers.
# The probe is deliberately non-saturated so tokenization errors are loud:
# serving without [CLS]/[SEP] moves LABEL_0 from 0.747 to ~0.999.
HF_REFERENCE_TEXT = "Account notice: sign in to review recent activity."
HF_REFERENCE = {
    "LABEL_0": 0.747434,
    "LABEL_1": 0.252449,
    "LABEL_2": 0.000045,
    "LABEL_3": 0.000072,
}
HF_REFERENCE_TOL = 5e-3


class ClassifyTests(ServerTestBase):
    """Tests for the /classify endpoint."""

    _model_pulled = False

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return
        model = get_test_model("classification")
        print(f"\n[SETUP] Ensuring {model} is pulled...")
        pull_model_with_retry(model)
        print(f"[SETUP] {model} is ready")
        cls._model_pulled = True

    def _payload(self, **overrides):
        payload = {
            "model": get_test_model("classification"),
            "input": "Please verify your account at http://secure-login.example now.",
        }
        payload.update(overrides)
        return payload

    def _assert_rejected(self, payload, context, expected_status):
        response = requests.post(
            f"{self.base_url}/classify",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            expected_status,
            f"{context}: expected {expected_status}, got {response.status_code}: "
            f"{response.text[:1000]}",
        )
        self.assertIn("error", response.json(), f"{context}: missing 'error' field")
        print(f"[OK] {context}: {response.status_code}")

    def test_001_missing_model_error(self):
        """A request without a model (and nothing loaded) is rejected without pulling."""
        payload = self._payload()
        del payload["model"]
        self._assert_rejected(payload, "Missing model", expected_status=400)

    def test_002_invalid_model_error(self):
        """A nonexistent model is a 404 model_not_found, not a download attempt."""
        self._assert_rejected(
            self._payload(model="nonexistent-classifier-xyz-123"),
            "Invalid model",
            expected_status=404,
        )

    def test_500_basic_classification(self):
        """A classify request returns per-label scores in [0, 1]."""
        self._ensure_model_pulled()
        payload = self._payload()
        print(f"[INFO] Classifying with model {payload['model']}")

        response = requests.post(
            f"{self.base_url}/classify",
            json=payload,
            timeout=TIMEOUT_CLASSIFY,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"classify failed with status {response.status_code}: {response.text[:1000]}",
        )
        body = response.json()
        self.assertEqual(
            body.get("object"),
            "classification",
            "Response must have object=classification",
        )
        self.assertEqual(
            body.get("model"), payload["model"], "Response must echo the model id"
        )
        self.assertIn("labels", body, "Response must contain a 'labels' object")
        labels = body["labels"]
        self.assertIsInstance(labels, dict, "'labels' must be an object")
        self.assertTrue(labels, "'labels' must be non-empty")
        for label, score in labels.items():
            self.assertIsInstance(label, str, f"label key must be a string: {label!r}")
            self.assertIsInstance(
                score, (int, float), f"score for {label} must be numeric"
            )
            self.assertGreaterEqual(score, 0.0, f"score for {label} below 0")
            self.assertLessEqual(score, 1.0, f"score for {label} above 1")
        print(f"[OK] classify returned {len(labels)} labels: {labels}")

    def test_003_wrong_type_input_error(self):
        """Non-string input is rejected 400 before any model is loaded."""
        self._assert_rejected(
            self._payload(input=12345), "Non-string input", expected_status=400
        )

    def test_004_wrong_type_top_k_error(self):
        """Non-positive / non-integer top_k is rejected 400 before any model is loaded."""
        self._assert_rejected(
            self._payload(top_k="two"), "Non-integer top_k", expected_status=400
        )
        self._assert_rejected(self._payload(top_k=0), "Zero top_k", expected_status=400)

    def test_501_missing_input_error(self):
        """A request with a valid model but no input text is a 400 error."""
        self._ensure_model_pulled()
        payload = self._payload()
        del payload["input"]
        self._assert_rejected(payload, "Missing input", expected_status=400)

    def test_502_top_k_limits_labels(self):
        """top_k=1 returns exactly one label through the whole stack."""
        self._ensure_model_pulled()
        response = requests.post(
            f"{self.base_url}/classify",
            json=self._payload(top_k=1),
            timeout=TIMEOUT_CLASSIFY,
        )
        self.assertEqual(response.status_code, 200, response.text[:500])
        labels = response.json().get("labels", {})
        self.assertEqual(len(labels), 1, f"expected 1 label with top_k=1: {labels}")

    def test_503_model_optional_when_one_classifier_loaded(self):
        """With a classifier loaded, 'model' may be omitted and is echoed back."""
        self._ensure_model_pulled()
        model = get_test_model("classification")
        # Ensure it is the loaded classifier.
        requests.post(
            f"{self.base_url}/classify",
            json=self._payload(),
            timeout=TIMEOUT_CLASSIFY,
        )

        payload = self._payload()
        del payload["model"]
        response = requests.post(
            f"{self.base_url}/classify", json=payload, timeout=TIMEOUT_CLASSIFY
        )
        self.assertEqual(
            response.status_code,
            200,
            f"model-optional classify failed: {response.status_code} {response.text[:500]}",
        )
        body = response.json()
        self.assertEqual(
            body.get("model"), model, "response must echo the resolved loaded model"
        )
        self.assertTrue(body.get("labels"), "labels must be present")
        print(f"[OK] model-optional classify resolved to {body.get('model')}")

    def test_504_matches_huggingface_reference(self):
        """End-to-end scores must match the original PyTorch model.

        Structural checks (labels present, scores in [0,1]) pass even when
        tokenization is wrong. HF_REFERENCE was computed with transformers on
        the source model; the probe text is deliberately NON-saturated (top
        score ~0.75), so a tokenization regression moves it by ~0.25 instead of
        hiding in the 1e-5 noise of a confident prediction.
        """
        self._ensure_model_pulled()
        response = requests.post(
            f"{self.base_url}/classify",
            json=self._payload(input=HF_REFERENCE_TEXT),
            timeout=TIMEOUT_CLASSIFY,
        )
        self.assertEqual(response.status_code, 200, response.text[:500])
        labels = response.json()["labels"]
        self.assertEqual(
            set(labels), set(HF_REFERENCE), f"label set mismatch: {sorted(labels)}"
        )
        for label, expected in HF_REFERENCE.items():
            self.assertAlmostEqual(
                labels[label],
                expected,
                delta=HF_REFERENCE_TOL,
                msg=(
                    f"{label}: {labels[label]:.6f} != HuggingFace reference "
                    f"{expected:.6f} (tolerance {HF_REFERENCE_TOL}). A delta this "
                    "large means the served scores diverge from the original model."
                ),
            )
        print(f"[OK] scores match the HuggingFace reference within {HF_REFERENCE_TOL}")


if __name__ == "__main__":
    run_server_tests(
        ClassifyTests,
        "CLASSIFY TESTS",
        modality="classification",
        default_wrapped_server="onnxruntime",
    )
