"""
SystemOne decision-model tests for Lemonade Server.

Tests POST /v1/systemone, the /v1/classify zero-shot path, and the router's
`systemone` and `zero_shot` classifier types against a llama.cpp decision model
(llama-server b11361 or later).

Usage:
    python server_systemone.py --wrapped-server llamacpp --backend cpu

The negative tests run first and never pull the model.
"""

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
)
from utils.capabilities import get_test_model
from utils.test_models import PORT, TIMEOUT_DEFAULT

TIMEOUT_DECISION = 600

STATE = (
    "Customer message: I was charged twice for my order last week "
    "and nobody has replied."
)
QUESTIONS = {
    "route": {
        "type": "choice",
        "instructions": "Which team should handle this?",
        "criteria": {"billing": None, "shipping": None, "technical": None},
    },
    "angry": {"type": "noul", "instructions": "Is the customer angry?"},
    "urgency": {
        "type": "score",
        "instructions": "How urgent is this?",
        "criteria": ["can wait", "this week", "today", "right now"],
    },
}
# Long enough to overflow llama-server's default physical batch (512 tokens),
# short of the 8192-token window the model was trained with.
LONG_STATE = (
    "The quarterly report shows revenue growth across all regions, driven by "
    "strong demand for cloud services and improved margins in hardware. "
) * 300


class SystemOneTests(ServerTestBase):
    """Tests for /v1/systemone and its classification paths."""

    _model_pulled = False

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return
        model = get_test_model("systemone")
        print(f"\n[SETUP] Ensuring {model} is pulled...")
        pull_model_with_retry(model)
        print(f"[SETUP] {model} is ready")
        cls._model_pulled = True

    def _post(self, path, payload, timeout=TIMEOUT_DECISION):
        return requests.post(f"{self.base_url}/{path}", json=payload, timeout=timeout)

    def _payload(self, **overrides):
        payload = {
            "model": get_test_model("systemone"),
            "state": STATE,
            "questions": QUESTIONS,
        }
        payload.update(overrides)
        return payload

    def _assert_rejected(self, path, payload, context, expected_status=400):
        response = self._post(path, payload, timeout=TIMEOUT_DEFAULT)
        self.assertEqual(
            response.status_code,
            expected_status,
            f"{context}: expected {expected_status}, got {response.status_code}: "
            f"{response.text[:1000]}",
        )
        self.assertIn("error", response.json(), f"{context}: missing 'error' field")
        print(f"[OK] {context}: {response.status_code}")

    def test_001_missing_questions_error(self):
        """A request without questions is rejected before any model is loaded."""
        payload = self._payload()
        del payload["questions"]
        self._assert_rejected("systemone", payload, "Missing questions")

    def test_002_missing_state_error(self):
        """A request without a state is rejected before any model is loaded."""
        payload = self._payload()
        del payload["state"]
        self._assert_rejected("systemone", payload, "Missing state")

    def test_003_model_without_systemone_label_error(self):
        """A model that is not a decision model is refused without being loaded."""
        self._assert_rejected(
            "systemone",
            self._payload(model=get_test_model("llm")),
            "Model without the systemone label",
        )

    def test_500_typed_answers(self):
        """One request answers a choice, a noul and a score question."""
        self._ensure_model_pulled()
        response = self._post("systemone", self._payload())
        self.assertEqual(response.status_code, 200, response.text[:1000])
        body = response.json()

        self.assertEqual(
            body.get("model"),
            get_test_model("systemone"),
            "Response must name the Lemonade model, not the GGUF path",
        )
        answers = body["answers"]

        route = answers["route"]
        self.assertEqual(
            set(route["probabilities"]), {"billing", "shipping", "technical"}
        )
        self.assertAlmostEqual(sum(route["probabilities"].values()), 1.0, places=3)
        self.assertIn(route["choice"], route["probabilities"])

        self.assertGreaterEqual(answers["angry"]["noul"], 0.0)
        self.assertLessEqual(answers["angry"]["noul"], 1.0)

        urgency = answers["urgency"]
        self.assertGreaterEqual(urgency["score"], 0.0)
        self.assertLessEqual(urgency["score"], 3.0)
        self.assertEqual(len(urgency["probabilities"]), 4)

        self.assertGreater(body["usage"]["input_tokens"], 0)
        print(f"[OK] typed answers: {answers}")

    def test_501_long_state(self):
        """A state far beyond the default physical batch is answered."""
        self._ensure_model_pulled()
        response = self._post(
            "systemone",
            self._payload(
                state=LONG_STATE,
                questions={
                    "topic": {
                        "type": "choice",
                        "instructions": "What is the topic?",
                        "criteria": {"finance": None, "sports": None},
                    }
                },
            ),
        )
        self.assertEqual(response.status_code, 200, response.text[:1000])
        tokens = response.json()["usage"]["input_tokens"]
        self.assertGreater(tokens, 2048)
        print(f"[OK] long state: {tokens} tokens")

    def test_502_classify_with_labels(self):
        """/v1/classify scores exactly the labels it is sent."""
        self._ensure_model_pulled()
        labels = ["coding", "pii", "cooking", "eating"]
        response = self._post(
            "classify",
            {
                "model": get_test_model("systemone"),
                "input": "I want to learn about baking so that I can make a cheesecake",
                "labels": labels,
            },
        )
        self.assertEqual(response.status_code, 200, response.text[:1000])
        scores = response.json()["labels"]
        self.assertEqual(set(scores), set(labels))
        self.assertAlmostEqual(sum(scores.values()), 1.0, places=3)
        self.assertEqual(max(scores, key=scores.get), "cooking")
        print(f"[OK] classify with labels: {scores}")

    def test_503_classify_without_labels_error(self):
        """A decision model has no label set of its own, so labels are required."""
        self._ensure_model_pulled()
        self._assert_rejected(
            "classify",
            {"model": get_test_model("systemone"), "input": "hello"},
            "Classify without labels",
        )

    def test_504_router_policy(self):
        """A router policy uses both the systemone and the zero_shot types."""
        decider = get_test_model("systemone_router")
        pull_model_with_retry(decider)
        default_model = get_test_model("llm")
        capable_model = get_test_model("llm_capable")
        collection = "user.Test-Router-SystemOne"
        policy = {
            "version": "1",
            "model_name": collection,
            "recipe": "collection.router",
            "components": [default_model, capable_model, decider],
            "routing": {
                "candidates": [default_model, capable_model],
                "default_model": default_model,
                "classifiers": [
                    {
                        "id": "pii",
                        "type": "systemone",
                        "model": decider,
                        "default_label": "true",
                        "question": {
                            "type": "noul",
                            "instructions": "Does the message contain personal data?",
                        },
                    },
                    {
                        "id": "topic",
                        "type": "zero_shot",
                        "model": decider,
                        "labels": ["chit-chat", "coding"],
                    },
                ],
                "rules": [
                    {
                        "id": "pii-local",
                        "match": {"classifier": "pii", "min_score": 0.5},
                        "route_to": default_model,
                    },
                    {
                        "id": "code-to-capable",
                        "match": {
                            "classifier": "topic",
                            "label": "coding",
                            "min_score": 0.5,
                        },
                        "route_to": capable_model,
                    },
                ],
            },
        }
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/pull", json=policy, timeout=1800
        )
        self.assertEqual(response.status_code, 200, f"register failed: {response.text}")
        try:
            for prompt, rule in [
                ("Hi, my SSN is 123-45-6789, please update my file.", "pii-local"),
                (
                    "Write a Python function that merges two sorted linked lists.",
                    "code-to-capable",
                ),
            ]:
                response = self._post(
                    "routing/validate",
                    {"policy": policy, "prompt": prompt, "route_trace": True},
                )
                self.assertEqual(response.status_code, 200, response.text[:1000])
                decision = response.json()["decision"]
                self.assertEqual(decision.get("matched_rule"), rule, decision)
                for entry in decision["trace"]:
                    self.assertIsNotNone(
                        entry.get("score"),
                        f"classifier failed instead of scoring: {entry}",
                    )
                print(f"[OK] router: {prompt[:30]!r} -> {rule}")
        finally:
            requests.post(
                f"{self.base_url}/delete",
                json={"model_name": collection},
                timeout=TIMEOUT_DEFAULT,
            )


if __name__ == "__main__":
    run_server_tests(
        SystemOneTests,
        "SYSTEMONE TESTS",
        modality="systemone",
        default_wrapped_server="llamacpp",
    )
