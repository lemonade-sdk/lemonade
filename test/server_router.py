"""
Router end-to-end tests for Lemonade Server (issue #2388).

Registers a `collection.router` collection and drives it with a vanilla OpenAI
client, asserting that requests are dispatched to the right candidate by the
policy's rules. Covers deterministic conditions (keywords / min_chars /
metadata), a model-backed `semantic_similarity` classifier, a cloud candidate,
first-match ordering, fail-open to `default_model`, and the decision surfaced on
the response (`x-lemonade-route` header + `x_lemonade_route` body).

The routing decision is computed server-side before the request is forwarded to
the chosen candidate, so these are true end-to-end runs: a real completion comes
back from the routed model.

Usage:
    python server_router.py
"""

import json as _json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
    get_config,
)
from utils.test_models import PORT, TIMEOUT_DEFAULT


def start_mock_cloud_provider(upstream_ids, marker_content):
    """In-process OpenAI-compatible provider: GET /v1/models + POST
    /v1/chat/completions. The chat reply content is `marker_content` so a test
    can prove a request actually reached this (cloud) provider. Returns
    (base_url ending in /v1, stop_fn)."""

    class _FakeProvider(BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            if self.path.rstrip("/").endswith("/models"):
                data = [{"id": uid, "object": "model"} for uid in upstream_ids]
                payload = _json.dumps({"object": "list", "data": data}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            else:
                self.send_response(404)
                self.end_headers()

        def do_POST(self):  # noqa: N802
            if "/chat/completions" not in self.path:
                self.send_response(404)
                self.end_headers()
                return
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length) if length else b""
            try:
                parsed = _json.loads(body or b"{}")
            except _json.JSONDecodeError:
                parsed = {}
            resp = {
                "id": "cmpl-mock",
                "object": "chat.completion",
                "created": 1,
                "model": parsed.get("model", upstream_ids[0]),
                "choices": [
                    {
                        "index": 0,
                        "message": {"role": "assistant", "content": marker_content},
                        "finish_reason": "stop",
                    }
                ],
                "usage": {
                    "prompt_tokens": 1,
                    "completion_tokens": 1,
                    "total_tokens": 2,
                },
            }
            payload = _json.dumps(resp).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, *_args):
            pass

    httpd = HTTPServer(("127.0.0.1", 0), _FakeProvider)
    port = httpd.server_address[1]
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()

    def stop():
        httpd.shutdown()
        httpd.server_close()

    return f"http://127.0.0.1:{port}/v1", stop


# Two small, distinct local candidates. Model choice is arbitrary for a routing
# test — only which one answers matters.
DEFAULT_MODEL = "Tiny-Test-Model-GGUF"
CAPABLE_MODEL = "Qwen3-0.6B-GGUF"
# Small embedding model that backs the semantic_similarity classifier.
EMBED_MODEL = "nomic-embed-text-v1-GGUF"
# Real encoder classifier (onnxruntime backend) for the `classifier` condition.
CLASSIFIER_MODEL = "Phishing-Email-Detection-ONNX"

# Shared inputs for the semantic_similarity routing tests (test_620/test_621).
# One reference phrase closely mirrors SEMANTIC_CODING_PROMPT so its cosine lands
# far above the threshold; embedding scores carry small run-to-run noise on GPU
# backends, and a borderline example would flip the route intermittently.
# Measured with nomic-embed-text-v1 (Q4_K_S): coding ~0.95, non-coding ~0.50, so
# the 0.70 threshold clears both sides by ~0.2 — well beyond the observed noise.
SEMANTIC_CODING_REFERENCE_PHRASES = [
    "write a function",
    "fix this bug",
    "refactor a recursive function to reduce its time complexity",
    "debug a stack trace",
    "improve the time complexity of an algorithm",
]
SEMANTIC_CODING_PROMPT = (
    "How do I refactor this recursive function to lower its time complexity?"
)
SEMANTIC_NONCODING_PROMPT = (
    "What are some good recipes for a summer picnic by the lake?"
)
SEMANTIC_MIN_SCORE = 0.70

COLLECTION_NAME = "user.Test-Router-Local"

POLICY = {
    "version": "1",
    "model_name": COLLECTION_NAME,
    "recipe": "collection.router",
    "components": [DEFAULT_MODEL, CAPABLE_MODEL],
    "routing": {
        "candidates": [DEFAULT_MODEL, CAPABLE_MODEL],
        "default_model": DEFAULT_MODEL,
        "rules": [
            {
                "id": "sensitive-stays-local",
                "match": {"metadata": {"key": "consent", "equals": "denied"}},
                "route_to": DEFAULT_MODEL,
                "outputs": {"reason": "privacy"},
            },
            {
                "id": "coding-or-long-to-capable",
                "match": {
                    "any": [
                        {"keywords_any": ["def ", "function", "stack trace"]},
                        {"min_chars": 4000},
                    ]
                },
                "route_to": CAPABLE_MODEL,
                "outputs": {"reason": "complex-or-long"},
            },
        ],
    },
}


class RouterTests(ServerTestBase):
    """End-to-end routing through a collection.router collection."""

    _setup_done = False

    @classmethod
    def _ensure_setup(cls):
        if cls._setup_done:
            return
        for model in (DEFAULT_MODEL, CAPABLE_MODEL):
            print(f"\n[SETUP] Ensuring {model} is pulled...")
            pull_model_with_retry(model)
        print(f"[SETUP] Registering {COLLECTION_NAME}...")
        resp = requests.post(
            f"http://localhost:{PORT}/api/v1/pull", json=POLICY, timeout=60
        )
        assert (
            resp.status_code == 200
        ), f"register failed: {resp.status_code} {resp.text}"
        cls._setup_done = True

    @classmethod
    def tearDownClass(cls):
        # Remove the shared collection so no router is left registered on
        # persistent (self-hosted) runners after the suite.
        if cls._setup_done:
            requests.post(
                f"http://localhost:{PORT}/api/v1/delete",
                json={"model_name": COLLECTION_NAME},
                timeout=TIMEOUT_DEFAULT,
            )
        super().tearDownClass()

    def setUp(self):
        super().setUp()
        self._ensure_setup()

    def _route(self, prompt, metadata=None, collection=COLLECTION_NAME):
        """Send a chat request through the router; return (header, decision, body)."""
        body = {
            "model": collection,
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 8,
            "temperature": 0.0,
            "route_trace": True,
        }
        if metadata is not None:
            body["metadata"] = metadata
        resp = requests.post(
            f"{self.base_url}/chat/completions", json=body, timeout=600
        )
        self.assertEqual(
            resp.status_code, 200, f"status {resp.status_code}: {resp.text}"
        )
        header = resp.headers.get("x-lemonade-route", "<missing>")
        decision = resp.json().get("x_lemonade_route", {})
        return header, decision, resp.json()

    def _delete_collection(self, collection):
        """Remove a registered router collection so a stale router isn't left on
        persistent runners (esp. before a cloud provider it references is
        uninstalled). `/delete` is a POST endpoint keyed on `model_name`.

        Runs in `finally`, which also fires when a test fails *before*
        registration; a 404 then just means "nothing to clean up", so accept it —
        asserting only 200 would let the cleanup mask the real failure. Any other
        status is a genuine cleanup problem worth surfacing."""
        resp = requests.post(
            f"{self.base_url}/delete",
            json={"model_name": collection},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertIn(
            resp.status_code,
            (200, 404),
            f"cleanup delete failed: {resp.status_code} {resp.text}",
        )

    def _trace_map(self, decision):
        return {t["condition"]: t["result"] for t in decision.get("trace", [])}

    def test_600_default_fallthrough(self):
        """A prompt matching no rule falls open to default_model."""
        header, decision, _ = self._route("Give me a fun fact about otters.")
        self.assertEqual(decision.get("route_to"), DEFAULT_MODEL)
        self.assertTrue(decision.get("default_used"))
        self.assertEqual(decision.get("matched_rule"), "")
        self.assertEqual(header, "default")
        print(f"[OK] no-match -> {DEFAULT_MODEL} (default)")

    def test_601_keyword_routes_to_capable(self):
        """A coding keyword routes to the capable candidate."""
        header, decision, _ = self._route(
            "Write a Python function to reverse a linked list."
        )
        self.assertEqual(decision.get("route_to"), CAPABLE_MODEL)
        self.assertEqual(decision.get("matched_rule"), "coding-or-long-to-capable")
        self.assertFalse(decision.get("default_used"))
        self.assertEqual(header, "coding-or-long-to-capable")
        self.assertTrue(self._trace_map(decision).get("keywords_any"))
        print(f"[OK] keyword -> {CAPABLE_MODEL}")

    def test_602_min_chars_routes_to_capable(self):
        """A long prompt (no keyword) routes via min_chars."""
        long_prompt = "Summarize this. " + ("data point " * 400)  # > 4000 chars
        self.assertGreater(len(long_prompt), 4000)
        _, decision, _ = self._route(long_prompt)
        self.assertEqual(decision.get("route_to"), CAPABLE_MODEL)
        self.assertEqual(decision.get("matched_rule"), "coding-or-long-to-capable")
        tmap = self._trace_map(decision)
        self.assertFalse(tmap.get("keywords_any"))
        self.assertTrue(tmap.get("min_chars"))
        print(f"[OK] min_chars -> {CAPABLE_MODEL}")

    def test_603_metadata_first_match_wins(self):
        """A coding prompt with consent=denied hits the earlier privacy rule."""
        header, decision, _ = self._route(
            "Write a Python function to reverse a linked list.",
            metadata={"consent": "denied"},
        )
        self.assertEqual(decision.get("route_to"), DEFAULT_MODEL)
        self.assertEqual(decision.get("matched_rule"), "sensitive-stays-local")
        self.assertEqual(decision.get("outputs", {}).get("reason"), "privacy")
        self.assertEqual(header, "sensitive-stays-local")
        print(f"[OK] consent=denied coding prompt -> {DEFAULT_MODEL} (first-match)")

    def test_604_no_trace_when_not_requested(self):
        """Without route_trace the decision omits the per-condition trace."""
        body = {
            "model": COLLECTION_NAME,
            "messages": [{"role": "user", "content": "hello"}],
            "max_tokens": 8,
            "temperature": 0.0,
        }
        resp = requests.post(
            f"{self.base_url}/chat/completions", json=body, timeout=600
        )
        self.assertEqual(resp.status_code, 200, resp.text)
        # Header is always present; the body trace is opt-in.
        self.assertIn("x-lemonade-route", resp.headers)
        decision = resp.json().get("x_lemonade_route", {})
        self.assertEqual(decision.get("trace", []), [])
        print("[OK] route_trace omitted -> no trace array")

    def test_610_cloud_candidate_routing(self):
        """A candidate whose recipe is `cloud` routes to a cloud provider.

        Uses an in-process mock provider (no real key), so this exercises the
        local-vs-cloud split end-to-end: the coding prompt is answered by the
        cloud provider, a casual prompt stays on the local default.
        """
        provider = "testroutercloud"
        upstream_id = "vendor/router-cloud-model"
        marker = "answered-by-cloud-provider"
        collection = "user.Test-Router-Cloud"

        base_url, stop_provider = start_mock_cloud_provider([upstream_id], marker)
        cloud_model = None
        try:
            # Install + auth the provider so its model is discovered. This MUST
            # happen before registering the collection (a cloud candidate only
            # exists in the catalog after discovery).
            resp = requests.post(
                f"{self.base_url}/install",
                json={
                    "backend": "cloud",
                    "provider": provider,
                    "base_url": base_url,
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, f"install failed: {resp.text}")
            resp = requests.post(
                f"{self.base_url}/cloud/auth",
                json={
                    "provider": provider,
                    "api_key": "dummy-key",
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, f"auth failed: {resp.text}")

            # Find the discovered cloud model id (testroutercloud.<cleaned>).
            models = requests.get(
                f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
            ).json()
            cloud_ids = [
                m["id"]
                for m in models.get("data", [])
                if m["id"].startswith(f"{provider}.")
            ]
            self.assertEqual(
                len(cloud_ids), 1, f"expected one cloud model, got {cloud_ids}"
            )
            cloud_model = cloud_ids[0]

            policy = {
                "version": "1",
                "model_name": collection,
                "recipe": "collection.router",
                "components": [DEFAULT_MODEL, cloud_model],
                "routing": {
                    "candidates": [DEFAULT_MODEL, cloud_model],
                    "default_model": DEFAULT_MODEL,
                    "rules": [
                        {
                            "id": "coding-to-cloud",
                            "match": {"keywords_any": ["def ", "function"]},
                            "route_to": cloud_model,
                            "outputs": {"route_category": "cloud"},
                        },
                    ],
                },
            }
            resp = requests.post(
                f"http://localhost:{PORT}/api/v1/pull", json=policy, timeout=60
            )
            self.assertEqual(resp.status_code, 200, f"register failed: {resp.text}")

            # Coding prompt -> cloud candidate, answered by the mock provider.
            body = {
                "model": collection,
                "route_trace": True,
                "max_tokens": 8,
                "messages": [
                    {"role": "user", "content": "Write a function to sort a list."}
                ],
            }
            resp = requests.post(
                f"{self.base_url}/chat/completions", json=body, timeout=600
            )
            self.assertEqual(resp.status_code, 200, resp.text)
            data = resp.json()
            decision = data.get("x_lemonade_route", {})
            self.assertEqual(decision.get("route_to"), cloud_model)
            self.assertEqual(decision.get("matched_rule"), "coding-to-cloud")
            self.assertEqual(
                data["choices"][0]["message"]["content"],
                marker,
                "coding prompt should be answered by the cloud provider",
            )
            print(f"[OK] coding -> {cloud_model} (cloud), answered by mock provider")

            # Casual prompt -> stays local (default).
            body = {
                "model": collection,
                "route_trace": True,
                "max_tokens": 8,
                "messages": [{"role": "user", "content": "Tell me a fun fact."}],
            }
            resp = requests.post(
                f"{self.base_url}/chat/completions", json=body, timeout=600
            )
            self.assertEqual(resp.status_code, 200, resp.text)
            decision = resp.json().get("x_lemonade_route", {})
            self.assertEqual(decision.get("route_to"), DEFAULT_MODEL)
            self.assertTrue(decision.get("default_used"))
            print(f"[OK] casual -> {DEFAULT_MODEL} (local default)")
        finally:
            self._delete_collection(collection)
            requests.delete(
                f"{self.base_url}/cloud/auth/{provider}", timeout=TIMEOUT_DEFAULT
            )
            requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": provider},
                timeout=TIMEOUT_DEFAULT,
            )
            stop_provider()

    def test_620_semantic_similarity_routing(self):
        """A `semantic_similarity` classifier routes by embedding similarity.

        This is the first *model-backed* condition in the suite: it embeds the
        input (via `Router::embeddings`) and scores it against labelled
        reference phrases. Embedding scores carry small run-to-run noise on GPU
        backends, so the reference phrases and threshold are chosen to keep both
        prompts well clear of the boundary; the test asserts the routing
        *outcome* and a healthy separation rather than an exact score.
        """
        pull_model_with_retry(EMBED_MODEL)
        collection = "user.Test-Router-Semantic"
        policy = {
            "version": "1",
            "model_name": collection,
            "recipe": "collection.router",
            "components": [DEFAULT_MODEL, CAPABLE_MODEL, EMBED_MODEL],
            "routing": {
                "candidates": [DEFAULT_MODEL, CAPABLE_MODEL],
                "default_model": DEFAULT_MODEL,
                "classifiers": [
                    {
                        "id": "topic",
                        "type": "semantic_similarity",
                        "model": EMBED_MODEL,
                        "reference_phrases": {
                            "coding": SEMANTIC_CODING_REFERENCE_PHRASES
                        },
                    }
                ],
                "rules": [
                    {
                        "id": "coding-to-capable",
                        "match": {
                            "classifier": "topic",
                            "label": "coding",
                            "min_score": SEMANTIC_MIN_SCORE,
                        },
                        "route_to": CAPABLE_MODEL,
                    }
                ],
            },
        }
        resp = requests.post(
            f"http://localhost:{PORT}/api/v1/pull", json=policy, timeout=60
        )
        self.assertEqual(resp.status_code, 200, f"register failed: {resp.text}")

        try:

            def classify_score(decision):
                for t in decision.get("trace", []):
                    if t["condition"] == "classifier:topic":
                        return t.get("score")
                return None

            # A semantically coding prompt (no literal rule keyword) -> capable.
            _, decision, _ = self._route(SEMANTIC_CODING_PROMPT, collection=collection)
            coding_score = classify_score(decision)
            other_decision = self._route(
                SEMANTIC_NONCODING_PROMPT, collection=collection
            )[1]
            other_score = classify_score(other_decision)
            print(f"semantic scores: coding={coding_score} non-coding={other_score}")

            self.assertEqual(decision.get("route_to"), CAPABLE_MODEL)
            self.assertEqual(decision.get("matched_rule"), "coding-to-capable")
            self.assertIsNotNone(coding_score)
            print(f"[OK] semantic coding ({coding_score:.3f}) -> {CAPABLE_MODEL}")

            # An unrelated prompt scores below threshold -> default.
            self.assertEqual(other_decision.get("route_to"), DEFAULT_MODEL)
            self.assertTrue(other_decision.get("default_used"))
            self.assertIsNotNone(other_score)
            print(f"[OK] semantic non-coding ({other_score:.3f}) -> {DEFAULT_MODEL}")

            # The routing outcomes above already assert both prompts fall on the
            # correct side of the threshold. Additionally require a clear gap
            # between them so a shrinking margin (a regression in the embedding
            # path) fails loudly here rather than intermittently flipping a route.
            self.assertGreaterEqual(
                coding_score - other_score,
                0.25,
                f"semantic separation too small: coding={coding_score:.3f} "
                f"non-coding={other_score:.3f}",
            )
        finally:
            self._delete_collection(collection)

    def test_621_semantic_similarity_cloud_candidate(self):
        """A `semantic_similarity` match routes to a *cloud* candidate.

        The intersection of the two model-backed / cloud paths: an embedding
        classifier decides, and the winning candidate is a `recipe:"cloud"`
        model served by an in-process mock provider (no real key). A coding
        query goes to the cloud model; an unrelated query stays local.
        """
        pull_model_with_retry(EMBED_MODEL)
        provider = "testsemcloud"
        upstream_id = "vendor/sem-cloud-model"
        marker = "answered-by-cloud-provider"
        collection = "user.Test-Router-Semantic-Cloud"

        base_url, stop_provider = start_mock_cloud_provider([upstream_id], marker)
        try:
            resp = requests.post(
                f"{self.base_url}/install",
                json={
                    "backend": "cloud",
                    "provider": provider,
                    "base_url": base_url,
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, f"install failed: {resp.text}")
            resp = requests.post(
                f"{self.base_url}/cloud/auth",
                json={
                    "provider": provider,
                    "api_key": "dummy-key",
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, f"auth failed: {resp.text}")
            models = requests.get(
                f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
            ).json()
            cloud_ids = [
                m["id"]
                for m in models.get("data", [])
                if m["id"].startswith(f"{provider}.")
            ]
            self.assertEqual(
                len(cloud_ids), 1, f"expected one cloud model: {cloud_ids}"
            )
            cloud_model = cloud_ids[0]

            policy = {
                "version": "1",
                "model_name": collection,
                "recipe": "collection.router",
                "components": [DEFAULT_MODEL, cloud_model, EMBED_MODEL],
                "routing": {
                    "candidates": [DEFAULT_MODEL, cloud_model],
                    "default_model": DEFAULT_MODEL,
                    "classifiers": [
                        {
                            "id": "topic",
                            "type": "semantic_similarity",
                            "model": EMBED_MODEL,
                            "reference_phrases": {
                                "coding": SEMANTIC_CODING_REFERENCE_PHRASES
                            },
                        }
                    ],
                    "rules": [
                        {
                            "id": "coding-to-cloud",
                            "match": {
                                "classifier": "topic",
                                "label": "coding",
                                "min_score": SEMANTIC_MIN_SCORE,
                            },
                            "route_to": cloud_model,
                            "outputs": {"route_category": "cloud"},
                        }
                    ],
                },
            }
            resp = requests.post(
                f"http://localhost:{PORT}/api/v1/pull", json=policy, timeout=60
            )
            self.assertEqual(resp.status_code, 200, f"register failed: {resp.text}")

            # Semantically coding -> cloud candidate, answered by the mock provider.
            _, decision, data = self._route(
                SEMANTIC_CODING_PROMPT, collection=collection
            )
            self.assertEqual(decision.get("route_to"), cloud_model)
            self.assertEqual(decision.get("matched_rule"), "coding-to-cloud")
            self.assertEqual(
                data["choices"][0]["message"]["content"],
                marker,
                "coding prompt should be answered by the cloud provider",
            )
            print(f"[OK] semantic coding -> {cloud_model} (cloud), answered by mock")

            # Unrelated -> stays local (default).
            _, decision, _ = self._route(
                SEMANTIC_NONCODING_PROMPT, collection=collection
            )
            self.assertEqual(decision.get("route_to"), DEFAULT_MODEL)
            self.assertTrue(decision.get("default_used"))
            print(f"[OK] semantic non-coding -> {DEFAULT_MODEL} (local default)")
        finally:
            self._delete_collection(collection)
            requests.delete(
                f"{self.base_url}/cloud/auth/{provider}", timeout=TIMEOUT_DEFAULT
            )
            requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": provider},
                timeout=TIMEOUT_DEFAULT,
            )
            stop_provider()

    def test_630_classifier_condition_routing(self):
        """A `classifier` condition routes via a real onnxruntime classifier.

        This exercises the model-backed classifier path end-to-end: the engine
        calls `Router::classify` on an encoder model served by the onnxruntime
        backend (`/v1/classify`), not a chat LLM. Requires that backend, so it
        runs under `--wrapped-server onnxruntime` and is skipped otherwise.

        Scores are deterministic; the two probes separate cleanly on LABEL_1
        (~0.9999 vs ~0.25), so the 0.5 threshold is stable.
        """
        if get_config().get("wrapped_server") != "onnxruntime":
            self.skipTest(
                "classifier-condition e2e needs the onnxruntime backend; "
                "run with --wrapped-server onnxruntime"
            )
        pull_model_with_retry(CLASSIFIER_MODEL)
        collection = "user.Test-Router-Classifier"
        policy = {
            "version": "1",
            "model_name": collection,
            "recipe": "collection.router",
            "components": [DEFAULT_MODEL, CAPABLE_MODEL, CLASSIFIER_MODEL],
            "routing": {
                "candidates": [DEFAULT_MODEL, CAPABLE_MODEL],
                "default_model": DEFAULT_MODEL,
                "classifiers": [
                    {
                        "id": "phishing",
                        "type": "classifier",
                        "model": CLASSIFIER_MODEL,
                        "labels": ["LABEL_0", "LABEL_1", "LABEL_2", "LABEL_3"],
                        "default_label": "LABEL_1",
                        "on_error": "match_false",
                    }
                ],
                "rules": [
                    {
                        "id": "phishing-to-restricted",
                        "match": {
                            "classifier": "phishing",
                            "label": "LABEL_1",
                            "min_score": 0.5,
                        },
                        "route_to": CAPABLE_MODEL,
                    }
                ],
            },
        }
        resp = requests.post(
            f"http://localhost:{PORT}/api/v1/pull", json=policy, timeout=120
        )
        self.assertEqual(resp.status_code, 200, f"register failed: {resp.text}")

        try:

            def clf_score(decision):
                for t in decision.get("trace", []):
                    if t["condition"] == "classifier:phishing":
                        return t.get("score")
                return None

            # High-confidence phishing text -> classifier fires -> restricted.
            _, decision, _ = self._route(
                "Please verify your account at http://secure-login.example now.",
                collection=collection,
            )
            self.assertEqual(decision.get("route_to"), CAPABLE_MODEL)
            self.assertEqual(decision.get("matched_rule"), "phishing-to-restricted")
            hi = clf_score(decision)
            self.assertIsNotNone(hi)
            self.assertGreaterEqual(hi, 0.5)
            print(f"[OK] classifier phishing ({hi:.3f}) -> {CAPABLE_MODEL}")

            # Benign text scores below threshold on LABEL_1 -> default.
            _, decision, _ = self._route(
                "Account notice: sign in to review recent activity.",
                collection=collection,
            )
            self.assertEqual(decision.get("route_to"), DEFAULT_MODEL)
            self.assertTrue(decision.get("default_used"))
            lo = clf_score(decision)
            self.assertIsNotNone(lo)
            self.assertLess(lo, 0.5)
            print(f"[OK] classifier benign ({lo:.3f}) -> {DEFAULT_MODEL}")
        finally:
            self._delete_collection(collection)


if __name__ == "__main__":
    run_server_tests(RouterTests, description="ROUTER TESTS")
