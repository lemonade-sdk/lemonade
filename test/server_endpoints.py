"""
Inference-agnostic endpoint tests for Lemonade Server.

Requires a Lemonade server to already be running on port 13305.
This test module does not start the server, and its inherited
`--cli-binary` argument is not used here.

Tests endpoints that don't require specific inference backends:
- /health
- /models
- /pull (including streaming mode)
- /delete
- /load (including save_options and recipe_options.json)
- /unload
- /system-info
- /stats
- /live

Usage:
    python server_endpoints.py
"""

import json
import os
import platform
import socket
import subprocess
import time
import unittest
import shutil
import tempfile
import uuid
import requests
from openai import NotFoundError
from prometheus_client.parser import text_string_to_metric_families

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    OpenAI,
    pull_model_with_retry,
)
from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    get_default_lemond_binary,
    SHARED_REPO_MODEL_A_NAME,
    SHARED_REPO_MODEL_A_CHECKPOINT,
    SHARED_REPO_MODEL_B_NAME,
    SHARED_REPO_MODEL_B_CHECKPOINT,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
    USER_MODEL_MAIN_CHECKPOINT,
    USER_MODEL_NAME,
    USER_MODEL_TE_CHECKPOINT,
    USER_MODEL_VAE_CHECKPOINT,
    get_hf_cache_dir,
    get_hf_cache_dir_candidates,
)


def _resolve_lemond_binary():
    """Locate the lemond daemon binary for the duplicate-port test.

    Prefers the binary built alongside this checkout; falls back to whatever is
    on PATH. Returns None if neither exists so the test can skip cleanly rather
    than fail on a machine without a built daemon.
    """
    candidate = get_default_lemond_binary()
    if candidate and os.path.exists(candidate):
        return candidate
    return shutil.which("lemond")


def _pick_free_port():
    """Return an unused TCP port assigned by the OS on the IPv4 loopback.

    Binding to port 0 lets the kernel pick a free port; we read it back and
    close the socket immediately. Both lemond instances in the duplicate-port
    test target this port, which is independent of the suite's server on PORT.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


def _lemond_health_ok(port, headers):
    """True if lemond answers a 200 on /api/v1/health at the given port."""
    try:
        response = requests.get(
            f"http://localhost:{port}/api/v1/health",
            headers=headers,
            timeout=2,
        )
        return response.status_code == 200
    except requests.RequestException:
        return False


class EndpointTests(ServerTestBase):
    """Tests for inference-agnostic endpoints."""

    # Track if model has been pulled (persists across tests)
    _model_pulled = False

    @classmethod
    def setUpClass(cls):
        """Set up class - verify server and ensure test model is pulled."""
        super().setUpClass()

        # Ensure the test model is pulled once for all tests
        cls._ensure_model_pulled()

    @classmethod
    def _ensure_model_pulled(cls):
        """Ensure the test model is pulled (only does work once)."""
        if cls._model_pulled:
            return

        print(f"\n[SETUP] Ensuring {ENDPOINT_TEST_MODEL} is pulled...")
        pull_model_with_retry(ENDPOINT_TEST_MODEL)
        print(f"[SETUP] {ENDPOINT_TEST_MODEL} is ready")
        cls._model_pulled = True

    def setUp(self):
        """Set up each test."""
        super().setUp()

    def _get_loaded_model_info(self, model_name):
        """Return loaded model info from /health for a model, or None."""
        health = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT).json()
        for model in health.get("all_models_loaded", []):
            if model["model_name"] == model_name:
                return model
        return None

    def _assert_loaded_model_pid(self, model_info):
        """Assert /health exposes a usable wrapped backend process ID."""
        self.assertIsNotNone(model_info, "Model should appear in /health")
        self.assertIn("pid", model_info)
        self.assertIsInstance(model_info["pid"], int)
        self.assertGreater(model_info["pid"], 0)

    def _parse_prometheus_text(self, body):
        """Validate Prometheus text format and return sample labels by metric name."""
        samples = {}
        for family in text_string_to_metric_families(body):
            self.assertTrue(family.name, "Metric family name should not be empty")
            self.assertTrue(family.documentation is not None)
            self.assertTrue(family.type, f"{family.name} should have a metric type")
            for sample in family.samples:
                float(sample.value)
                samples.setdefault(sample.name, []).append(sample.labels)

        return samples

    def test_000_endpoints_registered(self):
        """Verify all expected endpoints are registered on both v0 and v1."""
        valid_endpoints = [
            "chat/completions",
            "completions",
            "embeddings",
            "models",
            "responses",
            "pull",
            "registry/search",
            "pull/variants",
            "delete",
            "load",
            "unload",
            "health",
            "stats",
            "system-info",
            "reranking",
            "audio/transcriptions",
            "images/generations",
            "install",
            "uninstall",
        ]

        session = requests.Session()

        # Ensure 404 for non-existent endpoint
        url = f"http://localhost:{PORT}/api/v0/nonexistent"
        response = session.head(url, timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 404)

        # Check that all endpoints are properly registered on both v0 and v1
        for endpoint in valid_endpoints:
            for version in ["v0", "v1"]:
                url = f"http://localhost:{PORT}/api/{version}/{endpoint}"
                response = session.head(url, timeout=TIMEOUT_DEFAULT)
                self.assertNotEqual(
                    response.status_code,
                    404,
                    f"Endpoint {endpoint} is not registered on {version}",
                )

        session.close()

    def test_001_live_endpoint(self):
        """Test the /live endpoint for load balancer health checks."""
        response = requests.get(
            f"http://localhost:{PORT}/live", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(response.status_code, 200)
        print("[OK] /live endpoint returned 200")

    def test_002_health_endpoint(self):
        """Test the /health endpoint returns valid response with expected fields."""
        response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)

        data = response.json()

        # Check required fields per docs/api/lemonade.md
        self.assertIn("status", data)
        self.assertEqual(data["status"], "ok")
        self.assertIn("all_models_loaded", data)
        self.assertIsInstance(data["all_models_loaded"], list)
        self.assertIn("max_models", data)

        # max_models should have llm, embedding, reranking keys
        max_models = data["max_models"]
        self.assertIn("llm", max_models)
        self.assertIn("embedding", max_models)
        self.assertIn("reranking", max_models)

        # telemetry should have enabled, and captures iff enabled is True
        self.assertIn("telemetry", data)
        telemetry = data["telemetry"]
        self.assertIn("enabled", telemetry)
        self.assertIsInstance(telemetry["enabled"], bool)
        if telemetry["enabled"]:
            self.assertIn("captures", telemetry)
            self.assertIsInstance(telemetry["captures"], list)
            for capture in telemetry["captures"]:
                self.assertIn(capture, ["inputs", "outputs", "thinking"])
        else:
            self.assertNotIn("captures", telemetry)

        print(
            f"[OK] /health endpoint response: status={data['status']}, models_loaded={len(data['all_models_loaded'])}"
        )

    def test_002a_metrics_endpoint(self):
        """Test root-level /metrics returns Prometheus text and loaded model samples."""
        response = requests.get(
            f"http://localhost:{PORT}/metrics", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(response.status_code, 200)
        self.assertIn("text/plain", response.headers.get("Content-Type", ""))
        body = response.text
        self.assertIn("# HELP lemonade_server_up", body)
        self.assertIn("# TYPE lemonade_server_up gauge", body)
        self.assertRegex(body, r"(?m)^lemonade_server_up 1(?:\.0+)?$")

        samples = self._parse_prometheus_text(body)
        self.assertIn("lemonade_server_up", samples)
        self.assertIn("lemonade_loaded_models", samples)
        self.assertIn("lemonade_max_loaded_models", samples)

        head_response = requests.head(
            f"http://localhost:{PORT}/metrics", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(head_response.status_code, 200)

        load_response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(load_response.status_code, 200)

        loaded_response = requests.get(
            f"http://localhost:{PORT}/metrics", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(loaded_response.status_code, 200)
        loaded_samples = self._parse_prometheus_text(loaded_response.text)
        self.assertIn("lemonade_model_info", loaded_samples)
        self.assertTrue(
            any(
                labels.get("model_name") == ENDPOINT_TEST_MODEL
                for labels in loaded_samples["lemonade_model_info"]
            ),
            "Loaded model should be exposed in lemonade_model_info",
        )
        print("[OK] /metrics returned Prometheus text with loaded model samples")

    def test_003_models_list(self):
        """Test listing available models via /models endpoint."""
        # Model is already pulled in setUpClass
        response = requests.get(f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertIn("data", data)
        self.assertGreater(
            len(data["data"]),
            0,
            "Models list should not be empty after pulling a model",
        )

        # Verify model structure per docs/api/openai.md
        model = data["data"][0]
        self.assertIn("id", model)
        self.assertIn("object", model)
        self.assertEqual(model["object"], "model")

        # Verify our pulled model is in the list
        model_ids = [m["id"] for m in data["data"]]
        self.assertIn(ENDPOINT_TEST_MODEL, model_ids)

        print(f"[OK] /models returned {len(data['data'])} downloaded models")

    def test_004_models_list_show_all(self):
        """Test that show_all=true returns more models than default."""
        # Get only downloaded models (default)
        response_default = requests.get(
            f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(response_default.status_code, 200)
        downloaded_count = len(response_default.json()["data"])

        # Get all models including not-yet-downloaded
        response_all = requests.get(
            f"{self.base_url}/models?show_all=true", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(response_all.status_code, 200)
        all_count = len(response_all.json()["data"])

        # show_all should return more models than default (catalog is larger than downloaded)
        self.assertGreater(
            all_count,
            downloaded_count,
            "Catalog should have more models than downloaded",
        )
        print(f"[OK] /models: downloaded={downloaded_count}, catalog={all_count}")

    def test_004a_registry_search_validation(self):
        # Registry search validates locally without contacting a provider.
        missing_query = requests.get(
            f"{self.base_url}/registry/search", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(missing_query.status_code, 400)

        bad_source = requests.get(
            f"{self.base_url}/registry/search",
            params={"query": "qwen", "source": "unknown"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(bad_source.status_code, 400)
        self.assertIn("Unsupported model source", bad_source.text)

        bad_limit = requests.get(
            f"{self.base_url}/registry/search",
            params={"query": "qwen", "source": "modelscope", "limit": 0},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(bad_limit.status_code, 400)
        self.assertIn("limit", bad_limit.text)

        malformed_limit = requests.get(
            f"{self.base_url}/registry/search",
            params={"query": "qwen", "source": "modelscope", "limit": "12x"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(malformed_limit.status_code, 400)
        self.assertIn("limit", malformed_limit.text)

        bad_format = requests.get(
            f"{self.base_url}/registry/search",
            params={"query": "qwen", "source": "modelscope", "format": "safetensors"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(bad_format.status_code, 400)
        self.assertIn("format", bad_format.text)

    def test_005_models_retrieve(self):
        """Test retrieving a specific model by ID with extended fields."""
        client = self.get_openai_client()

        # Get a model from the list first
        models = client.models.list()
        self.assertGreater(len(models.data), 0)

        test_model = models.data[0]
        model = client.models.retrieve(test_model.id)

        self.assertEqual(model.id, test_model.id)

        # Check extended fields per docs/api/openai.md
        self.assertTrue(hasattr(model, "checkpoint") or "checkpoint" in str(model))

        print(f"[OK] Retrieved model: {model.id}")

    def test_006_models_retrieve_not_found(self):
        """Test that retrieving non-existent model returns NotFoundError."""
        client = self.get_openai_client()

        with self.assertRaises(NotFoundError):
            client.models.retrieve("non-existent-model-xyz-123")

        print("[OK] NotFoundError raised for non-existent model")

    def test_007_pull_model_non_streaming(self):
        """Test pulling/downloading a model (non-streaming mode)."""
        # First delete model if it exists to ensure we're actually testing pull
        delete_response = requests.post(
            f"{self.base_url}/delete",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )
        # 200 = deleted, 422 = not found (both are acceptable)
        self.assertIn(delete_response.status_code, [200, 422])

        # Verify model is not in downloaded list
        models_response = requests.get(
            f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
        )
        models_data = models_response.json()
        model_ids = [m["id"] for m in models_data["data"]]
        self.assertNotIn(
            ENDPOINT_TEST_MODEL, model_ids, "Model should be deleted before pull test"
        )

        # Now pull the model
        response = requests.post(
            f"{self.base_url}/pull",
            json={"model_name": ENDPOINT_TEST_MODEL, "stream": False},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertIn("status", data)
        self.assertEqual(data["status"], "success")

        # Verify model is now in downloaded list
        models_response = requests.get(
            f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
        )
        models_data = models_response.json()
        model_ids = [m["id"] for m in models_data["data"]]
        self.assertIn(
            ENDPOINT_TEST_MODEL, model_ids, "Model should be downloaded after pull"
        )

        print(f"[OK] Pull (non-streaming): model={ENDPOINT_TEST_MODEL}")

    def test_008_pull_model_streaming(self):
        """Test pulling a model with streaming progress events."""
        # First delete model to ensure we're actually testing pull
        delete_response = requests.post(
            f"{self.base_url}/delete",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertIn(delete_response.status_code, [200, 422])

        # Pull with streaming
        response = requests.post(
            f"{self.base_url}/pull",
            json={"model_name": ENDPOINT_TEST_MODEL, "stream": True},
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200)

        # Parse SSE events
        events_received = []
        complete_received = False

        for line in response.iter_lines():
            if line:
                line_str = line.decode("utf-8")
                if line_str.startswith("event:"):
                    event_type = line_str.split(":", 1)[1].strip()
                    events_received.append(event_type)
                    if event_type == "complete":
                        complete_received = True

        # Should have received progress and complete events
        self.assertTrue(
            complete_received
            or "progress" in events_received
            or len(events_received) > 0,
            f"Expected streaming events, got: {events_received}",
        )

        # Verify model is now in downloaded list
        models_response = requests.get(
            f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
        )
        models_data = models_response.json()
        model_ids = [m["id"] for m in models_data["data"]]
        self.assertIn(
            ENDPOINT_TEST_MODEL,
            model_ids,
            "Model should be downloaded after streaming pull",
        )

        print(f"[OK] Pull (streaming): received events: {set(events_received)}")

    def test_009_load_model_basic(self):
        """Test loading a model into memory."""
        # Model is already pulled (setUpClass or previous pull tests)
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertEqual(data["status"], "success")

        # Verify model is loaded via health endpoint and exposes backend PID
        loaded_model = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self._assert_loaded_model_pid(loaded_model)

        print(f"[OK] Loaded model: {ENDPOINT_TEST_MODEL}")

    def test_010_load_model_with_options(self):
        """Test loading a model with custom options (ctx_size, llamacpp_backend, llamacpp_args)."""
        # Load with custom options (reloads only if options differ from current)
        custom_ctx_size = 2048
        response = requests.post(
            f"{self.base_url}/load",
            json={
                "model_name": ENDPOINT_TEST_MODEL,
                "ctx_size": custom_ctx_size,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # Verify options were applied via health endpoint
        health_response = requests.get(
            f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
        )
        health_data = health_response.json()

        # Find our model in loaded models
        loaded_model = None
        for m in health_data.get("all_models_loaded", []):
            if m["model_name"] == ENDPOINT_TEST_MODEL:
                loaded_model = m
                break

        self.assertIsNotNone(
            loaded_model, f"Model {ENDPOINT_TEST_MODEL} should be loaded"
        )

        # Check recipe_options contains our ctx_size
        recipe_options = loaded_model.get("recipe_options", {})
        if "ctx_size" in recipe_options:
            self.assertEqual(recipe_options["ctx_size"], custom_ctx_size)

        print(f"[OK] Loaded model with ctx_size={custom_ctx_size}")

    def test_011_load_model_save_options(self):
        """Test save_options=true saves settings to recipe_options.json."""
        custom_ctx_size = 4096
        response = requests.post(
            f"{self.base_url}/load",
            json={
                "model_name": ENDPOINT_TEST_MODEL,
                "ctx_size": custom_ctx_size,
                "save_options": True,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        model_info_response = requests.get(
            f"{self.base_url}/models/{ENDPOINT_TEST_MODEL}",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(model_info_response.status_code, 200)

        model_info = model_info_response.json()
        self.assertIn("recipe_options", model_info)
        self.assertEqual(
            model_info["recipe_options"].get("ctx_size"),
            custom_ctx_size,
            f"Expected saved ctx_size={custom_ctx_size} in model info recipe_options",
        )
        print(f"[OK] Verified saved ctx_size={custom_ctx_size} via model info")

    def test_012_load_uses_saved_options(self):
        """Test that load reads previously saved options from recipe_options.json."""
        # First, save options with a specific ctx_size
        custom_ctx_size = 3072
        requests.post(
            f"{self.base_url}/load",
            json={
                "model_name": ENDPOINT_TEST_MODEL,
                "ctx_size": custom_ctx_size,
                "save_options": True,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Unload the model so we can reload it fresh
        requests.post(
            f"{self.base_url}/unload",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )

        # Load again WITHOUT specifying ctx_size - should use saved value from recipe_options.json
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # Verify via health
        health_response = requests.get(
            f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
        )
        health_data = health_response.json()

        for m in health_data.get("all_models_loaded", []):
            if m["model_name"] == ENDPOINT_TEST_MODEL:
                recipe_options = m.get("recipe_options", {})
                if "ctx_size" in recipe_options:
                    self.assertEqual(
                        recipe_options["ctx_size"],
                        custom_ctx_size,
                        "Should use saved ctx_size from recipe_options.json",
                    )
                    print(f"[OK] Load used saved ctx_size={custom_ctx_size}")
                break

    def test_012a_load_idempotent_same_options(self):
        """Test that /load is idempotent: loading an already-loaded model with
        the same options is a no-op (no eviction or reload).

        Uses the wrapped backend process ID as the proof signal: a no-op
        /load keeps the same backend process, while an eviction/reload starts
        a different process."""
        # Ensure model is loaded (this may take seconds for the initial load)
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        loaded_before = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self._assert_loaded_model_pid(loaded_before)

        # Second /load with the same options — should be a no-op
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        loaded_after = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self._assert_loaded_model_pid(loaded_after)

        self.assertEqual(
            loaded_after["pid"],
            loaded_before["pid"],
            "Idempotent /load should keep the same wrapped backend process",
        )
        print(
            f"[OK] Idempotent /load with same options kept PID "
            f"{loaded_after['pid']}"
        )

    def test_012b_load_reloads_on_option_change(self):
        """Test that /load evicts and reloads when options differ.

        The changed PID proves the wrapped backend process was replaced."""
        # Ensure model is loaded with default options (no ctx_size override)
        requests.post(
            f"{self.base_url}/unload",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        loaded_before = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self._assert_loaded_model_pid(loaded_before)
        opts_before = loaded_before.get("recipe_options", {})
        self.assertNotEqual(
            opts_before.get("ctx_size"),
            2048,
            "Precondition: model should not already have ctx_size=2048",
        )

        # Load again with different options
        custom_ctx = 2048
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL, "ctx_size": custom_ctx},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        loaded_after = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self._assert_loaded_model_pid(loaded_after)
        opts_after = loaded_after.get("recipe_options", {})
        self.assertEqual(
            opts_after.get("ctx_size"),
            custom_ctx,
            "Option-change /load should reload with new options",
        )
        self.assertNotEqual(
            loaded_after["pid"],
            loaded_before["pid"],
            "Option-change /load should replace the wrapped backend process",
        )

        print(
            f"[OK] /load with different options replaced PID "
            f"{loaded_before['pid']} -> {loaded_after['pid']} "
            f"(ctx_size={custom_ctx})"
        )

    def test_012c_load_noop_when_already_loaded_by_inference(self):
        """Regression test for #1603: /load after an inference-triggered
        auto-load should no-op, not evict and reload the model.

        The old code did is_model_loaded() → unload → load as separate
        mutex acquisitions in handle_load, so a /load arriving after
        auto-load completed would always evict and reload (~90s for large
        models). The fix makes this decision atomic inside load_mutex_.

        We make this deterministic by loading via inference first (wait
        for completion), then calling /load. The wrapped backend process ID
        proves whether a reload occurred."""
        # Ensure clean slate
        requests.post(
            f"{self.base_url}/unload",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )

        # Load the model via inference (triggers auto_load_model_if_needed)
        inference_response = requests.post(
            f"{self.base_url}/chat/completions",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "messages": [{"role": "user", "content": "hi"}],
                "max_tokens": 5,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(inference_response.status_code, 200)
        loaded_before = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self._assert_loaded_model_pid(loaded_before)

        # Now /load the same model — should no-op, not evict+reload
        load_response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(load_response.status_code, 200)

        loaded_after = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self._assert_loaded_model_pid(loaded_after)
        self.assertEqual(
            loaded_after["pid"],
            loaded_before["pid"],
            "/load after auto-load should keep the same wrapped backend process",
        )

        print(
            f"[OK] /load after auto-load was a no-op and kept PID "
            f"{loaded_after['pid']}"
        )

    def test_013_auto_load_forwards_only_allowlisted_options(self):
        """Regression for #2663 / PR #2664 review: request-scoped params must NOT leak
        into recipe_options on auto-load.

        When the server auto-loads a model via an inference endpoint (e.g.
        /v1/chat/completions) it must only forward an explicit allowlist
        of load-level fields (currently only ctx_size).  Request-scoped
        fields like temperature, max_tokens, stream, messages, model, etc. must
        remain invisible to RecipeOptions so they cannot affect subsequent requests.

        Steps:
          1. Unload the test model.
          2. Call /v1/chat/completions with a large mix of request-scoped params
             AND a custom ctx_size.
          3. Auto-load should only apply ctx_size.
          4. Verify recipe_options on the loaded model contains ctx_size but
             none of the request-scoped or recipe-level fields sent alongside."""
        # Ensure clean slate
        requests.post(
            f"{self.base_url}/unload",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )

        try:
            # Send an inference request with both load-level and request-scoped params.
            # Only ctx_size should be forwarded to the RecipeOptions constructor.
            custom_ctx_size = 8192
            inference_response = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": ENDPOINT_TEST_MODEL,
                    "messages": [{"role": "user", "content": "Hello, world!"}],
                    "max_tokens": 5,
                    "temperature": 0.99,
                    "top_p": 0.88,
                    "top_k": 77,
                    "stream": False,
                    "presence_penalty": -0.5,
                    "frequency_penalty": 1.2,
                    "seed": 42,
                    "pinned": True,
                    "llamacpp_args": "--foo-bar",
                    "auto_evict": True,
                    "evict_idle_timeout": 1,
                    "ctx_size": custom_ctx_size,
                    "max_completion_tokens": 10,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(
                inference_response.status_code,
                200,
                f"Chat completions should succeed: {inference_response.text[:500]}",
            )

            # Verify the loaded model's recipe_options
            health_response = requests.get(
                f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
            )
            health_data = health_response.json()

            loaded_model = None
            for m in health_data.get("all_models_loaded", []):
                if m["model_name"] == ENDPOINT_TEST_MODEL:
                    loaded_model = m
                    break

            self.assertIsNotNone(
                loaded_model,
                f"Model {ENDPOINT_TEST_MODEL} should be loaded after auto-load",
            )

            recipe_options = loaded_model.get("recipe_options", {})

            # ---- Allowlisted: ctx_size MUST be present ----
            self.assertIn(
                "ctx_size",
                recipe_options,
                "ctx_size from inference request should be forwarded to recipe_options",
            )
            self.assertEqual(
                recipe_options["ctx_size"],
                custom_ctx_size,
                f"ctx_size should match request value {custom_ctx_size}",
            )

            # ---- Denied: request-scoped params must NOT be in recipe_options ----
            forbidden = [
                "temperature",
                "max_tokens",
                "stream",
                "messages",
                "top_p",
                "top_k",
                "presence_penalty",
                "frequency_penalty",
                "seed",
                "max_completion_tokens",
                "model",
                "pinned",
                "llamacpp_args",
                "auto_evict",
                "evict_idle_timeout",
            ]
            for field in forbidden:
                self.assertNotIn(
                    field,
                    recipe_options,
                    f"Request-scoped field '{field}' must NOT leak into recipe_options "
                    f"on auto-load (found: {recipe_options.get(field)})",
                )

            print(
                f"[OK] Auto-load forwarded only ctx_size={custom_ctx_size}; "
                f"request-scoped params correctly excluded"
            )
        finally:
            requests.post(
                f"{self.base_url}/unload",
                json={"model_name": ENDPOINT_TEST_MODEL},
                timeout=TIMEOUT_DEFAULT,
            )

    def _start_mock_cloud_provider(
        self, upstream_ids, chat_handler=None, sse_chunks=None
    ):
        """Spin up an in-process OpenAI-compatible mock provider.

        Serves GET /v1/models with the given ids and (optionally) POST
        /v1/chat/completions. When `sse_chunks` is provided, the chat
        endpoint emits each chunk as an SSE `data:` line (the caller is
        responsible for shaping each chunk as OpenAI-compat JSON) and
        terminates with `data: [DONE]\\n\\n`. Otherwise it falls back to
        the non-streaming chat_handler(body) -> dict shape. Returns
        (base_url, stop_fn). The base URL ends with /v1.
        """
        import json as _json
        import threading
        from http.server import BaseHTTPRequestHandler, HTTPServer

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
                if sse_chunks is not None and parsed.get("stream") is True:
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Cache-Control", "no-cache")
                    self.end_headers()
                    for chunk in sse_chunks:
                        line = f"data: {_json.dumps(chunk)}\n\n".encode()
                        self.wfile.write(line)
                        self.wfile.flush()
                    self.wfile.write(b"data: [DONE]\n\n")
                    self.wfile.flush()
                    return
                if chat_handler is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                resp = chat_handler(parsed)
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

    def test_012d_cloud_install_then_auth_then_chat(self):
        """End-to-end cloud workflow on the refactored server-side path.

        Verifies:
          (1) /v1/install with backend=cloud registers a provider.
          (2) /v1/system-info reports the provider with auth_state.runtime_key_set=false.
          (3) /v1/cloud/auth stores a runtime key and triggers discovery.
          (4) /v1/models lists the discovered cloud model.
          (5) /v1/chat/completions round-trips through the mock provider.
          (6) /v1/cloud/auth (DELETE) clears the runtime key and evicts models.
          (7) /v1/uninstall removes the provider entirely.
        """
        provider = "testcloud"
        upstream_id = "vendor/regression-model"
        public_name = f"{provider}.{upstream_id}"

        def chat_response(req):
            return {
                "id": "cmpl-1",
                "object": "chat.completion",
                "created": 1,
                "model": req.get("model", upstream_id),
                "choices": [
                    {
                        "index": 0,
                        "message": {"role": "assistant", "content": "pong"},
                        "finish_reason": "stop",
                    }
                ],
                "usage": {
                    "prompt_tokens": 1,
                    "completion_tokens": 1,
                    "total_tokens": 2,
                },
            }

        base_url, stop_provider = self._start_mock_cloud_provider(
            [upstream_id],
            chat_handler=chat_response,
        )

        try:
            # (1) Install with no api_key — provider is registered, no discovery
            # happens yet (no resolvable key).
            resp = requests.post(
                f"{self.base_url}/install",
                json={
                    "backend": "cloud",
                    "provider": provider,
                    "base_url": base_url,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, f"install failed: {resp.text}")
            data = resp.json()
            self.assertEqual(data["status"], "success")
            self.assertEqual(data["provider"], provider)
            self.assertEqual(
                data["models_discovered"],
                0,
                "No key supplied — discovery should yield zero models",
            )

            # (2) system-info reports the new provider with no auth.
            info = requests.get(
                f"{self.base_url}/system-info",
                timeout=TIMEOUT_DEFAULT,
            ).json()
            entries = [
                p
                for p in info.get("cloud", {}).get("providers", [])
                if p["name"] == provider
            ]
            self.assertEqual(
                len(entries), 1, "Provider should be listed in system-info"
            )
            self.assertFalse(entries[0]["env_var_set"])
            self.assertFalse(entries[0]["runtime_key_set"])

            # (3) /cloud/auth stores the runtime key and triggers discovery.
            resp = requests.post(
                f"{self.base_url}/cloud/auth",
                json={
                    "provider": provider,
                    "api_key": "dummy-key",
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, f"auth set failed: {resp.text}")
            auth_data = resp.json()
            self.assertTrue(auth_data["auth_state"]["runtime_key_set"])
            self.assertEqual(auth_data["models_discovered"], 1)

            # (4) /models now lists the discovered cloud model.
            models = requests.get(
                f"{self.base_url}/models",
                timeout=TIMEOUT_DEFAULT,
            ).json()
            ids = [m["id"] for m in models.get("data", [])]
            self.assertIn(
                public_name,
                ids,
                f"Discovered cloud model should appear in /models; got {ids}",
            )

            # (5) Round-trip chat completion through the mock.
            resp = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_name,
                    "messages": [{"role": "user", "content": "ping"}],
                    "max_tokens": 5,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(resp.status_code, 200, f"chat failed: {resp.text}")
            reply = resp.json()["choices"][0]["message"]["content"]
            self.assertEqual(reply, "pong")

            # (6) DELETE /cloud/auth clears the runtime key and evicts models.
            resp = requests.delete(
                f"{self.base_url}/cloud/auth/{provider}",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, resp.text)
            cleared = resp.json()
            self.assertTrue(cleared["cleared_runtime_key"])
            self.assertFalse(cleared["auth_state"]["runtime_key_set"])
            # Without a key, the model should be gone from /models.
            models = requests.get(
                f"{self.base_url}/models",
                timeout=TIMEOUT_DEFAULT,
            ).json()
            ids = [m["id"] for m in models.get("data", [])]
            self.assertNotIn(
                public_name,
                ids,
                "Clearing the runtime key must evict the provider's models",
            )

            # (7) /uninstall removes the provider record from the registry.
            resp = requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": provider},
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, resp.text)
            info = requests.get(
                f"{self.base_url}/system-info",
                timeout=TIMEOUT_DEFAULT,
            ).json()
            entries = [
                p
                for p in info.get("cloud", {}).get("providers", [])
                if p["name"] == provider
            ]
            self.assertEqual(
                len(entries), 0, "Uninstalled provider must disappear from system-info"
            )
        finally:
            stop_provider()

        print("[OK] Cloud install -> auth -> chat -> clear -> uninstall round-trip")

    def test_012e_cloud_auth_unknown_provider_returns_404(self):
        """/cloud/auth refuses to set a key for an unknown provider — keeps
        the registry honest (no implicit-install) and gives the CLI/UI a
        precise error to surface."""
        resp = requests.post(
            f"{self.base_url}/cloud/auth",
            json={"provider": "never-installed", "api_key": "k"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(resp.status_code, 404, resp.text)
        body = resp.json()
        self.assertEqual(body["error"]["type"], "invalid_request_error")
        print("[OK] /cloud/auth on unknown provider returns 404")

    def test_012f_chat_against_evicted_cloud_model_returns_404(self):
        """When DELETE /cloud/auth/{provider} clears the runtime key it also
        evicts that provider's discovered models from the cache. A chat call
        referring to one of those models must surface the standard model
        not-found 404, not a stack-trace 500 — eviction has to be visible
        to the chat endpoint."""
        provider = "testevicted"
        upstream_id = "vendor/evicted-model"
        public_name = f"{provider}.{upstream_id}"
        base_url, stop_provider = self._start_mock_cloud_provider([upstream_id])
        try:
            requests.post(
                f"{self.base_url}/install",
                json={"backend": "cloud", "provider": provider, "base_url": base_url},
                timeout=TIMEOUT_DEFAULT,
            )
            requests.post(
                f"{self.base_url}/cloud/auth",
                json={
                    "provider": provider,
                    "api_key": "k",
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            requests.delete(
                f"{self.base_url}/cloud/auth/{provider}",
                timeout=TIMEOUT_DEFAULT,
            )
            resp = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_name,
                    "messages": [{"role": "user", "content": "hi"}],
                    "max_tokens": 1,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 404, resp.text)
            requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": provider},
                timeout=TIMEOUT_DEFAULT,
            )
        finally:
            stop_provider()
        print("[OK] Chat against an evicted cloud model returns a clean 404")

    def test_012j_chat_with_loaded_model_but_cleared_key_returns_missing_creds(self):
        """The real missing-creds path: load a cloud model (router holds an
        active CloudServer instance), then clear the runtime key. Subsequent
        chat calls reuse the already-loaded server — they bypass model-not-
        found and hit resolve_creds() at request time, which must return
        the structured missing_creds_error() instead of crashing or 500."""
        provider = "testmissingkey"
        upstream_id = "vendor/needs-creds"
        public_name = f"{provider}.{upstream_id}"

        def chat_response(req):
            # Should never be called — creds are cleared before chat.
            return {"error": "mock should not have been reached"}

        base_url, stop_provider = self._start_mock_cloud_provider(
            [upstream_id],
            chat_handler=chat_response,
        )
        try:
            requests.post(
                f"{self.base_url}/install",
                json={"backend": "cloud", "provider": provider, "base_url": base_url},
                timeout=TIMEOUT_DEFAULT,
            )
            requests.post(
                f"{self.base_url}/cloud/auth",
                json={
                    "provider": provider,
                    "api_key": "k",
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            # Load the model so the router holds a live CloudServer instance.
            load_resp = requests.post(
                f"{self.base_url}/load",
                json={"model_name": public_name},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(load_resp.status_code, 200, load_resp.text)

            # Clear the runtime key. evict_cloud_models drops the cache entry
            # but the router's loaded CloudServer instance keeps loaded_=true,
            # which is exactly the state that exercises missing_creds_error().
            clear_resp = requests.delete(
                f"{self.base_url}/cloud/auth/{provider}",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(clear_resp.status_code, 200, clear_resp.text)

            chat_resp = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_name,
                    "messages": [{"role": "user", "content": "hi"}],
                    "max_tokens": 1,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            # Whichever specific status the structured error uses, the
            # contract is: it must not be 200 (mock should never run) and
            # the body must be JSON with an `error` envelope that names
            # the missing-credentials condition — never an HTML stack-trace
            # or empty body — so a UI/CLI can route the user to /cloud/auth.
            self.assertNotEqual(
                chat_resp.status_code, 200, "Chat must not succeed without creds"
            )
            body = chat_resp.json()
            self.assertIn(
                "error", body, f"Missing structured error envelope: {chat_resp.text}"
            )
            err = body["error"]
            self.assertIn("message", err, f"Error envelope missing message: {body}")
            self.assertIn("type", err, f"Error envelope missing type: {body}")
            msg = err["message"].lower()
            self.assertTrue(
                "api key" in msg or "credential" in msg or "auth" in msg,
                f"Error message should reference missing credentials: {err['message']}",
            )
            # The provider name should appear so multi-provider setups know
            # which one to authenticate.
            self.assertIn(
                provider,
                err.get("details", {}).get("provider", "") + err["message"],
                f"Error should name the offending provider: {body}",
            )
        finally:
            stop_provider()
            requests.post(
                f"{self.base_url}/unload",
                json={"model_name": public_name},
                timeout=TIMEOUT_DEFAULT,
            )
            requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": provider},
                timeout=TIMEOUT_DEFAULT,
            )
        print("[OK] Loaded cloud model + cleared key returns missing_creds_error()")

    def test_012k_streaming_chat_through_cloud_provider(self):
        """End-to-end SSE through a cloud-routed model: the upstream provider
        emits OpenAI-shape `data:` chunks, CloudServer streams them through to
        the client unchanged, and the client sees `[DONE]` as the terminator."""
        provider = "teststream"
        upstream_id = "vendor/streamer"
        public_name = f"{provider}.{upstream_id}"

        sse_chunks = [
            {
                "id": "cmpl-stream-1",
                "object": "chat.completion.chunk",
                "choices": [{"index": 0, "delta": {"content": "Hel"}}],
            },
            {
                "id": "cmpl-stream-1",
                "object": "chat.completion.chunk",
                "choices": [{"index": 0, "delta": {"content": "lo"}}],
            },
            {
                "id": "cmpl-stream-1",
                "object": "chat.completion.chunk",
                "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
            },
        ]
        base_url, stop_provider = self._start_mock_cloud_provider(
            [upstream_id],
            sse_chunks=sse_chunks,
        )
        try:
            requests.post(
                f"{self.base_url}/install",
                json={"backend": "cloud", "provider": provider, "base_url": base_url},
                timeout=TIMEOUT_DEFAULT,
            )
            requests.post(
                f"{self.base_url}/cloud/auth",
                json={
                    "provider": provider,
                    "api_key": "k",
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )

            with requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_name,
                    "messages": [{"role": "user", "content": "hi"}],
                    "stream": True,
                    "max_tokens": 5,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
                stream=True,
            ) as resp:
                self.assertEqual(resp.status_code, 200, resp.text)
                deltas = []
                saw_done = False
                for raw in resp.iter_lines():
                    if not raw:
                        continue
                    line = raw.decode("utf-8") if isinstance(raw, bytes) else raw
                    if not line.startswith("data:"):
                        continue
                    payload = line[len("data:") :].strip()
                    if payload == "[DONE]":
                        saw_done = True
                        break
                    obj = json.loads(payload)
                    delta = obj.get("choices", [{}])[0].get("delta", {})
                    if "content" in delta:
                        deltas.append(delta["content"])
                self.assertTrue(saw_done, "Stream must end with data: [DONE]")
                self.assertEqual(
                    "".join(deltas),
                    "Hello",
                    f"Streamed chunks did not assemble correctly: {deltas}",
                )
        finally:
            stop_provider()
            requests.delete(
                f"{self.base_url}/cloud/auth/{provider}",
                timeout=TIMEOUT_DEFAULT,
            )
            requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": provider},
                timeout=TIMEOUT_DEFAULT,
            )
        print("[OK] Streaming chat through cloud provider round-trips SSE")

    def test_012g_install_rejects_bad_provider_name(self):
        """Provider names must be [a-z0-9_-]+ lowercase. Uppercase ('Fireworks'
        vs 'fireworks') would resolve the same env var but be distinct registry
        records — registry-level confusion the install path now refuses."""
        for bad_name in ["Fireworks", "with space", "vendor/x", "with.dot", ""]:
            resp = requests.post(
                f"{self.base_url}/install",
                json={
                    "backend": "cloud",
                    "provider": bad_name,
                    "base_url": "https://example.com/v1",
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(
                resp.status_code,
                400,
                f"Install accepted bad provider name {bad_name!r}: {resp.text}",
            )
            body = resp.json()
            self.assertEqual(body["error"]["type"], "invalid_request_error")
        print("[OK] /install rejects non-[a-z0-9_-]+ provider names with 400")

    def test_012h_http_base_url_requires_opt_in_for_keys(self):
        """Custom OpenAI-compatible backends may be on trusted LAN HTTP. Do
        not block keyless URLs, but require explicit opt-in before Lemonade
        stores or uses an API key over plaintext HTTP."""
        installed = []

        def cleanup(provider):
            requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": provider},
                timeout=TIMEOUT_DEFAULT,
            )

        try:
            # http:// to a non-loopback host: accepted with a transport warning.
            provider = "httpguard"
            resp = requests.post(
                f"{self.base_url}/install",
                json={
                    "backend": "cloud",
                    "provider": provider,
                    "base_url": "http://api.example.com/v1",
                },
                timeout=TIMEOUT_DEFAULT,
            )
            installed.append(provider)
            self.assertEqual(resp.status_code, 200, resp.text)
            body = resp.json()
            self.assertEqual(body["status"], "success")
            warnings = body.get("warnings", [])
            self.assertTrue(any("http://" in w for w in warnings), body)
            self.assertFalse(any("Bearer token" in w for w in warnings), body)
            self.assertIn("warning", body)

            info = requests.get(
                f"{self.base_url}/system-info",
                timeout=TIMEOUT_DEFAULT,
            ).json()
            entry = next(
                p
                for p in info.get("cloud", {}).get("providers", [])
                if p["name"] == provider
            )
            self.assertFalse(entry["allow_insecure_http"])
            self.assertTrue(any("http://" in w for w in entry.get("warnings", [])))

            # gopher:// (any non-http(s) scheme): still rejected.
            resp = requests.post(
                f"{self.base_url}/install",
                json={
                    "backend": "cloud",
                    "provider": "schemeguard",
                    "base_url": "gopher://example.com/v1",
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 400, resp.text)

            # Bare http(s) schemes without hosts are rejected.
            for bad_url in ["http://", "https://"]:
                resp = requests.post(
                    f"{self.base_url}/install",
                    json={
                        "backend": "cloud",
                        "provider": "bareurl",
                        "base_url": bad_url,
                    },
                    timeout=TIMEOUT_DEFAULT,
                )
                self.assertEqual(resp.status_code, 400, resp.text)
                self.assertIn("host", resp.json()["error"]["message"])

            # Install + api_key in one request is rejected by default, then
            # accepted with explicit allow_insecure_http opt-in.
            provider = "httpkeyinstall"
            base_url, stop_provider = self._start_mock_cloud_provider(
                ["vendor/http-key"]
            )
            try:
                resp = requests.post(
                    f"{self.base_url}/install",
                    json={
                        "backend": "cloud",
                        "provider": provider,
                        "base_url": base_url,
                        "api_key": "dummy-key",
                    },
                    timeout=TIMEOUT_DEFAULT,
                )
                self.assertEqual(resp.status_code, 400, resp.text)
                self.assertEqual(
                    resp.json()["error"]["code"], "insecure_http_requires_opt_in"
                )
                resp = requests.post(
                    f"{self.base_url}/install",
                    json={
                        "backend": "cloud",
                        "provider": provider,
                        "base_url": base_url,
                        "api_key": "dummy-key",
                        "allow_insecure_http": True,
                    },
                    timeout=TIMEOUT_DEFAULT,
                )
                installed.append(provider)
                self.assertEqual(resp.status_code, 200, resp.text)
                self.assertTrue(resp.json()["allow_insecure_http"])
                warnings = resp.json().get("warnings", [])
                self.assertTrue(any("http://" in w for w in warnings), resp.text)
                self.assertTrue(any("Bearer token" in w for w in warnings), resp.text)
            finally:
                stop_provider()

            # Auth after an HTTP install is rejected by default, then accepted
            # with the same explicit opt-in.
            provider = "httpkeyauth"
            base_url, stop_provider = self._start_mock_cloud_provider(
                ["vendor/http-auth"]
            )
            try:
                resp = requests.post(
                    f"{self.base_url}/install",
                    json={
                        "backend": "cloud",
                        "provider": provider,
                        "base_url": base_url,
                    },
                    timeout=TIMEOUT_DEFAULT,
                )
                installed.append(provider)
                self.assertEqual(resp.status_code, 200, resp.text)
                resp = requests.post(
                    f"{self.base_url}/cloud/auth",
                    json={"provider": provider, "api_key": "dummy-key"},
                    timeout=TIMEOUT_DEFAULT,
                )
                self.assertEqual(resp.status_code, 400, resp.text)
                self.assertEqual(
                    resp.json()["error"]["code"], "insecure_http_requires_opt_in"
                )
                resp = requests.post(
                    f"{self.base_url}/cloud/auth",
                    json={
                        "provider": provider,
                        "api_key": "dummy-key",
                        "allow_insecure_http": True,
                    },
                    timeout=TIMEOUT_DEFAULT,
                )
                self.assertEqual(resp.status_code, 200, resp.text)
                self.assertTrue(resp.json()["allow_insecure_http"])
                warnings = resp.json().get("warnings", [])
                self.assertTrue(any("http://" in w for w in warnings), resp.text)
                self.assertTrue(any("Bearer token" in w for w in warnings), resp.text)
            finally:
                stop_provider()
        finally:
            for provider in installed:
                cleanup(provider)

        print("[OK] http:// cloud keys require explicit opt-in")

    def test_012i_cloud_refresh_is_idempotent_no_duplicates(self):
        """refresh_cloud_models must evict-then-emplace this provider's prior
        entries on every call. Asymmetry with build_cache() (overwrite instead
        of emplace) would not be visible on a clean cache, but would surface
        as duplicate or stale entries after a second /cloud/auth — so we
        re-auth the same provider with the same key twice and verify the
        same set of models is present, exactly once each."""
        provider = "idempotent"
        upstream_ids = ["vendor/a", "vendor/b"]
        base_url, stop_provider = self._start_mock_cloud_provider(upstream_ids)
        try:
            requests.post(
                f"{self.base_url}/install",
                json={"backend": "cloud", "provider": provider, "base_url": base_url},
                timeout=TIMEOUT_DEFAULT,
            )

            # First auth: discover both upstream ids.
            resp = requests.post(
                f"{self.base_url}/cloud/auth",
                json={
                    "provider": provider,
                    "api_key": "k1",
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, resp.text)
            self.assertEqual(resp.json()["models_discovered"], 2)

            # Second auth with the same key: must still report exactly 2 — the
            # eviction step removes the previous entries before re-emplacing.
            resp = requests.post(
                f"{self.base_url}/cloud/auth",
                json={
                    "provider": provider,
                    "api_key": "k1",
                    "allow_insecure_http": True,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(resp.status_code, 200, resp.text)
            self.assertEqual(
                resp.json()["models_discovered"],
                2,
                "Re-auth must report the same count — refresh is supposed to "
                "evict the provider's prior entries before re-emplacing",
            )

            # /models lists each discovered id exactly once.
            models = requests.get(
                f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
            ).json()
            ids = [m["id"] for m in models.get("data", [])]
            for uid in upstream_ids:
                expected = f"{provider}.{uid}"
                self.assertEqual(
                    ids.count(expected),
                    1,
                    f"Expected {expected} exactly once in /models, ids={ids}",
                )
        finally:
            stop_provider()
            requests.delete(
                f"{self.base_url}/cloud/auth/{provider}",
                timeout=TIMEOUT_DEFAULT,
            )
            requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": provider},
                timeout=TIMEOUT_DEFAULT,
            )
        print("[OK] Cloud refresh is idempotent — re-auth produces no duplicates")

    def test_013_unload_specific_model(self):
        """Test unloading a specific model by name."""
        # First load a model
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Verify model is loaded
        health_response = requests.get(
            f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
        )
        health_data = health_response.json()
        loaded_models = [
            m["model_name"] for m in health_data.get("all_models_loaded", [])
        ]
        self.assertIn(
            ENDPOINT_TEST_MODEL,
            loaded_models,
            "Model should be loaded before unload test",
        )

        # Unload the specific model
        response = requests.post(
            f"{self.base_url}/unload",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertEqual(data["status"], "success")

        # Verify model is actually unloaded via health endpoint
        health_response = requests.get(
            f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
        )
        health_data = health_response.json()
        loaded_models = [
            m["model_name"] for m in health_data.get("all_models_loaded", [])
        ]
        self.assertNotIn(
            ENDPOINT_TEST_MODEL,
            loaded_models,
            "Model should be unloaded after unload request",
        )

        print(f"[OK] Unloaded specific model: {ENDPOINT_TEST_MODEL}")

    def test_014_unload_nonexistent_model(self):
        """Test that unloading a model that isn't loaded returns 404."""
        response = requests.post(
            f"{self.base_url}/unload",
            json={"model_name": "NonexistentModel-XYZ-123"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 404)

        print("[OK] 404 returned for unloading non-existent model")

    def test_015_unload_all_models(self):
        """Test unloading all models without specifying model_name."""
        # First load a model
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Unload all (no model_name parameter)
        response = requests.post(
            f"{self.base_url}/unload",
            json={},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertEqual(data["status"], "success")

        # Verify all models are unloaded
        health_response = requests.get(
            f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
        )
        health_data = health_response.json()
        self.assertEqual(len(health_data.get("all_models_loaded", [])), 0)

        print("[OK] Unloaded all models")

    def test_016_delete_model(self):
        """Test deleting a model removes it from local storage."""
        # Model should already be pulled from setUpClass or pull tests
        models_response = requests.get(
            f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
        )
        models_data = models_response.json()
        model_ids = [m["id"] for m in models_data["data"]]
        self.assertIn(
            ENDPOINT_TEST_MODEL, model_ids, "Model should exist before delete test"
        )

        # Delete the model
        response = requests.post(
            f"{self.base_url}/delete",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertEqual(data["status"], "success")

        # Verify model is no longer in the list
        models_response = requests.get(
            f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
        )
        models_data = models_response.json()
        model_ids = [m["id"] for m in models_data["data"]]
        self.assertNotIn(ENDPOINT_TEST_MODEL, model_ids)

        # Re-pull for subsequent tests (stats test needs a model)
        requests.post(
            f"{self.base_url}/pull",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        print(f"[OK] Deleted and re-pulled model: {ENDPOINT_TEST_MODEL}")

    def test_017_delete_nonexistent_model(self):
        """Test that deleting a non-existent model returns error."""
        response = requests.post(
            f"{self.base_url}/delete",
            json={"model_name": "NonExistentModel-XYZ-123"},
            timeout=TIMEOUT_DEFAULT,
        )
        # Should return 422 Unprocessable Entity
        self.assertEqual(response.status_code, 422)

        print("[OK] 422 returned for deleting non-existent model")

    def test_018_system_info(self):
        """Test the /system-info endpoint returns required fields."""
        response = requests.get(f"{self.base_url}/system-info", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertIsInstance(data, dict)

        # Check required top-level keys per docs/api/lemonade.md
        required_keys = [
            "OS Version",
            "Processor",
            "Physical Memory",
            "devices",
            "recipes",
        ]
        for key in required_keys:
            self.assertIn(key, data, f"Missing required key: {key}")

        # Verify devices structure
        devices = data["devices"]
        self.assertIsInstance(devices, dict)

        # Check required device types
        required_devices = ["cpu", "amd_gpu", "amd_npu"]
        for device in required_devices:
            self.assertIn(device, devices, f"Missing device type: {device}")

        # CPU should have name, cores, threads, available
        cpu = devices["cpu"]
        self.assertIn("name", cpu)
        self.assertIn("available", cpu)

        # Verify recipes structure per docs/api/lemonade.md
        recipes = data["recipes"]
        self.assertIsInstance(recipes, dict)

        # Should contain known recipes
        known_recipes = [
            "llamacpp",
            "whispercpp",
            "sd-cpp",
            "flm",
            "ryzenai-llm",
        ]
        for recipe in known_recipes:
            self.assertIn(recipe, recipes, f"Missing recipe: {recipe}")

        # Each recipe should have backends
        for recipe_name, recipe_data in recipes.items():
            self.assertIn(
                "backends", recipe_data, f"Recipe {recipe_name} missing 'backends'"
            )
            backends = recipe_data["backends"]
            self.assertIsInstance(
                backends, dict, f"Recipe {recipe_name} backends should be dict"
            )
            has_supported_backend = any(
                backend_data.get("state") != "unsupported"
                for backend_data in backends.values()
            )
            if has_supported_backend:
                self.assertIn(
                    "default_backend",
                    recipe_data,
                    f"Recipe {recipe_name} missing 'default_backend'",
                )
                self.assertIn(
                    recipe_data["default_backend"],
                    backends,
                    f"Recipe {recipe_name} default_backend must exist in backends map",
                )

            # Each backend should have required fields
            for backend_name, backend_data in backends.items():
                self.assertIn(
                    "devices",
                    backend_data,
                    f"Backend {recipe_name}/{backend_name} missing 'devices'",
                )
                self.assertIn(
                    "state",
                    backend_data,
                    f"Backend {recipe_name}/{backend_name} missing 'state'",
                )
                self.assertIn(
                    "message",
                    backend_data,
                    f"Backend {recipe_name}/{backend_name} missing 'message'",
                )
                self.assertIn(
                    "action",
                    backend_data,
                    f"Backend {recipe_name}/{backend_name} missing 'action'",
                )
                self.assertIsInstance(
                    backend_data["devices"],
                    list,
                    f"Backend {recipe_name}/{backend_name} devices should be list",
                )
                self.assertIsInstance(
                    backend_data["state"],
                    str,
                    f"Backend {recipe_name}/{backend_name} state should be string",
                )
                self.assertIsInstance(
                    backend_data["message"],
                    str,
                    f"Backend {recipe_name}/{backend_name} message should be string",
                )
                self.assertIsInstance(
                    backend_data["action"],
                    str,
                    f"Backend {recipe_name}/{backend_name} action should be string",
                )
                self.assertIn(
                    backend_data["state"],
                    {
                        "unsupported",
                        "installable",
                        "update_required",
                        "action_required",
                        "installed",
                    },
                    f"Backend {recipe_name}/{backend_name} has invalid state: {backend_data['state']}",
                )

                # If available, may have version field (optional)
                # version is optional, so we just check it's a string if present
                if "version" in backend_data:
                    self.assertIsInstance(
                        backend_data["version"],
                        str,
                        f"Backend {recipe_name}/{backend_name} version should be string",
                    )

        print(
            f"[OK] /system-info: OS={data['OS Version'][:30]}..., recipes={len(recipes)}"
        )

    def test_020_web_app_root(self):
        """Test that GET / returns HTML for the web app (browser-accessible UI)."""
        response = requests.get(f"http://localhost:{PORT}/", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)
        content_type = response.headers.get("Content-Type", "")
        self.assertIn(
            "text/html",
            content_type,
            f"Expected text/html at /, got: {content_type}",
        )
        body = response.text
        self.assertIn(
            "<html",
            body.lower(),
            "Response body does not look like HTML",
        )
        print(f"[OK] GET / returned HTML ({len(body)} bytes)")

    def test_021_stats_endpoint(self):
        """Test the /stats endpoint returns performance metrics."""
        # First, make an inference request to populate stats
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Make a simple completion to populate stats
        try:
            client = self.get_openai_client()
            client.chat.completions.create(
                model=ENDPOINT_TEST_MODEL,
                messages=[{"role": "user", "content": "Hi"}],
                max_completion_tokens=5,
            )
        except Exception:
            pass  # Stats may still be populated even if inference fails

        response = requests.get(f"{self.base_url}/stats", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)

        data = response.json()
        # Stats fields per docs/api/lemonade.md (may not all be present if no inference done)
        # Just verify it returns valid JSON
        self.assertIsInstance(data, dict)

        print(f"[OK] /stats endpoint returned: {list(data.keys())}")

    def test_021s_pull_multi(self):
        # First delete model if it exists to ensure we're actually testing pull
        delete_response = requests.post(
            f"{self.base_url}/delete",
            json={"model_name": USER_MODEL_NAME},
            timeout=TIMEOUT_DEFAULT,
        )
        # 200 = deleted, 422 = not found (both are acceptable)
        self.assertIn(delete_response.status_code, [200, 422])

        recipe = "sd-cpp"
        ## sd-cpp currently unavailable on MacOS or Linux ARM64
        if platform.system() == "Darwin" or (
            platform.system() == "Linux" and platform.machine() == "aarch64"
        ):
            recipe = "llamacpp"
        recipe_backend = f"{recipe}_backend"

        # Bare-name alias for a unique user.* registration — what `/v1/models` emits.
        public_name = USER_MODEL_NAME.split(".", 1)[1]

        # Verify model is not in downloaded list
        models_response = requests.get(
            f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT
        )
        models_data = models_response.json()
        model_ids = [m["id"] for m in models_data["data"]]
        self.assertNotIn(
            public_name, model_ids, "Model should be deleted before pull test"
        )

        # Now pull the model
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": USER_MODEL_NAME,
                "checkpoints": {
                    "main": USER_MODEL_MAIN_CHECKPOINT,
                    "text_encoder": USER_MODEL_TE_CHECKPOINT,
                    "vae": USER_MODEL_VAE_CHECKPOINT,
                },
                "recipe": recipe,
                "recipe_options": {recipe_backend: "cpu"},
                "stream": False,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertIn("status", data)
        self.assertEqual(data["status"], "success")

        # Verify model is now in downloaded list. Under the model-naming spec the
        # API emits a unique user.* registration as its bare name; both forms are
        # accepted as input and resolve to the same record.
        models_response = requests.get(
            f"{self.base_url}/models/" + USER_MODEL_NAME, timeout=TIMEOUT_DEFAULT
        )
        model_data = models_response.json()
        self.assertIn("id", model_data)
        self.assertEqual(
            model_data["id"], public_name, "Model should be downloaded after pull"
        )
        self.assertIn("checkpoints", model_data)
        self.assertIn("main", model_data["checkpoints"])
        self.assertEqual(
            model_data["checkpoints"]["main"],
            USER_MODEL_MAIN_CHECKPOINT,
            "Main checkpoint not matching",
        )
        self.assertIn("text_encoder", model_data["checkpoints"])
        self.assertEqual(
            model_data["checkpoints"]["text_encoder"],
            USER_MODEL_TE_CHECKPOINT,
            "Text encoder checkpoint not matching",
        )
        self.assertIn("vae", model_data["checkpoints"])
        self.assertEqual(
            model_data["checkpoints"]["vae"],
            USER_MODEL_VAE_CHECKPOINT,
            "VAE checkpoint not matching",
        )
        self.assertIn("recipe", model_data)
        self.assertEqual(
            model_data["recipe"], recipe, f"Model recipe should be {recipe}"
        )

        self.assertIn("labels", model_data)
        self.assertIn("custom", model_data["labels"])

        if recipe == "sd-cpp":
            self.assertIn("image", model_data["labels"])

        self.assertIn("recipe_options", model_data)
        self.assertIn(recipe_backend, model_data["recipe_options"])
        self.assertEqual(
            model_data["recipe_options"][recipe_backend],
            "cpu",
            f"{recipe_backend} should be cpu",
        )

        print(f"[OK] Pull (multicheckpoint): model={USER_MODEL_NAME}")

    def test_021a_pull_sdcpp_import_preserves_merged_recipe_options(self):
        """Test /pull keeps image_defaults + recipe_options visible immediately.

        This exercises the import/warm-cache path for user models:
        add_model_to_cache() builds merged recipe options from image_defaults and
        JSON recipe_options, and download_model() must not overwrite that merged
        state with only the import recipe_options payload.
        """
        if platform.system() == "Darwin":
            self.skipTest("sd-cpp pull tests are skipped on macOS in this suite")
        if platform.system() == "Linux" and platform.machine() == "aarch64":
            self.skipTest("sd-cpp not supported on Linux ARM64")

        model_name = f"user.Pull-Merge-Regression-{uuid.uuid4().hex[:8]}"
        image_defaults = {
            "steps": 33,
            "cfg_scale": 8.5,
            "width": 640,
            "height": 768,
            "sampling_method": "euler",
            "flow_shift": 1.25,
        }
        recipe_options = {
            "sd-cpp_backend": "cpu",
            "sdcpp_args": "--diffusion-fa 1 --offload-to-cpu 1",
        }

        try:
            response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": model_name,
                    "checkpoints": {
                        # Use a different main quant than USER_MODEL_NAME so this test's
                        # cleanup does not delete the same shared main file and poison
                        # later reruns of test_021s_pull_multi.
                        "main": SHARED_REPO_MODEL_B_CHECKPOINT,
                        "text_encoder": USER_MODEL_TE_CHECKPOINT,
                        "vae": USER_MODEL_VAE_CHECKPOINT,
                    },
                    "recipe": "sd-cpp",
                    "image_defaults": image_defaults,
                    "recipe_options": recipe_options,
                    "stream": False,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(response.status_code, 200)

            model_info_response = requests.get(
                f"{self.base_url}/models/{model_name}",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(model_info_response.status_code, 200)

            model_data = model_info_response.json()
            self.assertIn("recipe_options", model_data)

            actual_options = model_data["recipe_options"]
            for key, value in image_defaults.items():
                self.assertIn(
                    key,
                    actual_options,
                    f"Expected image_defaults key '{key}' in recipe_options after pull",
                )
                self.assertEqual(
                    actual_options[key],
                    value,
                    f"Expected recipe_options['{key}']={value!r} after pull",
                )

            for key, value in recipe_options.items():
                self.assertIn(
                    key,
                    actual_options,
                    f"Expected recipe_options key '{key}' after pull",
                )
                self.assertEqual(
                    actual_options[key],
                    value,
                    f"Expected recipe_options['{key}']={value!r} after pull",
                )

            print(
                f"[OK] Pull preserved merged image_defaults + recipe_options for {model_name}"
            )
        finally:
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": model_name},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_021c_naming_spec_pull_rejects_reserved_prefixes(self):
        """Naming spec: /pull rejects extra.* / builtin.* model names, including
        as the bare-name part of a user.* alias (e.g. user.builtin.Foo)."""
        for reserved in [
            f"extra.Rejected-{uuid.uuid4().hex[:6]}",
            f"builtin.Rejected-{uuid.uuid4().hex[:6]}",
            # user.<reserved>.<bare> must also be rejected — otherwise it would
            # hijack the builtin.<bare> / extra.<bare> alias slot.
            f"user.builtin.Hijack-{uuid.uuid4().hex[:6]}",
            f"user.extra.Hijack-{uuid.uuid4().hex[:6]}",
        ]:
            response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": reserved,
                    "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
                    "recipe": "llamacpp",
                    "stream": False,
                },
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(
                response.status_code,
                400,
                f"Expected 400 for reserved prefix '{reserved}', got "
                f"{response.status_code}: {response.text}",
            )
            self.assertIn("reserved", response.text.lower())
        print(
            "[OK] /pull rejects extra.*/builtin.* and user.extra.*/user.builtin.* names"
        )

    def test_021d_naming_spec_builtin_canonical_alias(self):
        """Naming spec: builtin.<name> resolves to the same model as the bare name."""
        bare_response = requests.get(
            f"{self.base_url}/models/{ENDPOINT_TEST_MODEL}",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(bare_response.status_code, 200)
        self.assertEqual(bare_response.json()["id"], ENDPOINT_TEST_MODEL)

        canonical_response = requests.get(
            f"{self.base_url}/models/builtin.{ENDPOINT_TEST_MODEL}",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(canonical_response.status_code, 200)
        # When the built-in is the precedence-winner (no shadowing), the API
        # emits the bare id regardless of which form the client requested.
        self.assertEqual(canonical_response.json()["id"], ENDPOINT_TEST_MODEL)
        self.assertEqual(
            canonical_response.json()["checkpoint"],
            bare_response.json()["checkpoint"],
        )
        print(f"[OK] builtin.{ENDPOINT_TEST_MODEL} alias resolves to bare id")

    def test_021e_naming_spec_user_shadows_builtin(self):
        """Naming spec: a user.X registration shadows a built-in X.

        The user model wins precedence and emits as the bare name; the built-in
        is shadowed and emits as builtin.X. Both must remain visible.
        """
        user_canonical = f"user.{ENDPOINT_TEST_MODEL}"
        shadowed_id = f"builtin.{ENDPOINT_TEST_MODEL}"

        try:
            pull_response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": user_canonical,
                    "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
                    "recipe": "llamacpp",
                    "stream": False,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(pull_response.status_code, 200)

            models_response = requests.get(
                f"{self.base_url}/models?show_all=true",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(models_response.status_code, 200)
            model_ids = {m["id"] for m in models_response.json()["data"]}

            self.assertIn(
                ENDPOINT_TEST_MODEL,
                model_ids,
                "Bare id should be present (user model winner)",
            )
            self.assertIn(
                shadowed_id,
                model_ids,
                "Shadowed built-in should expose its builtin.<name> id",
            )
            self.assertNotIn(
                user_canonical,
                model_ids,
                "Winning user model should NOT also appear under user.<name>",
            )

            # All four input forms must resolve.
            for input_id in [ENDPOINT_TEST_MODEL, user_canonical, shadowed_id]:
                r = requests.get(
                    f"{self.base_url}/models/{input_id}",
                    timeout=TIMEOUT_DEFAULT,
                )
                self.assertEqual(r.status_code, 200, f"Failed to resolve {input_id}")

            # Bare and user.* should resolve to the same record (the user model).
            bare_info = requests.get(
                f"{self.base_url}/models/{ENDPOINT_TEST_MODEL}",
                timeout=TIMEOUT_DEFAULT,
            ).json()
            user_info = requests.get(
                f"{self.base_url}/models/{user_canonical}",
                timeout=TIMEOUT_DEFAULT,
            ).json()
            self.assertEqual(bare_info["checkpoint"], user_info["checkpoint"])
            self.assertEqual(bare_info["id"], user_info["id"])  # both emit bare

            # builtin.* should resolve to a different record (the built-in).
            builtin_info = requests.get(
                f"{self.base_url}/models/{shadowed_id}",
                timeout=TIMEOUT_DEFAULT,
            ).json()
            self.assertEqual(builtin_info["id"], shadowed_id)
            self.assertNotEqual(builtin_info["checkpoint"], bare_info["checkpoint"])

            print(f"[OK] user.{ENDPOINT_TEST_MODEL} shadows built-in cleanly")
        finally:
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": user_canonical},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_021j_register_user_collection(self):
        """Register a user-defined collection via POST /pull."""
        canonical_name = f"user.TestColl-{uuid.uuid4().hex[:8]}"
        # Unique `user.<name>` entries are exposed under the bare public name.
        public_name = canonical_name[5:]

        try:
            response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": canonical_name,
                    "recipe": "collection.omni",
                    "components": [ENDPOINT_TEST_MODEL],
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(response.status_code, 200, response.text)
            self.assertEqual(response.json()["status"], "success")

            # Show all so user.* models are visible
            models_response = requests.get(
                f"{self.base_url}/models?show_all=true",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(models_response.status_code, 200)
            entry = next(
                (m for m in models_response.json()["data"] if m["id"] == public_name),
                None,
            )
            self.assertIsNotNone(entry, f"{public_name} should appear in /models")
            self.assertEqual(entry.get("recipe"), "collection.omni")
            self.assertEqual(entry.get("components"), [ENDPOINT_TEST_MODEL])
            self.assertTrue(
                entry.get("downloaded"),
                "Collection should report downloaded=true when all components are downloaded",
            )

            print(f"[OK] Registered omni collection: {public_name}")
        finally:
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": canonical_name},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_021j_register_user_collection_with_system_prompt(self):
        """A registered user collection round-trips an optional system_prompt.

        Verifies the per-collection override path documented in
        docs/dev/lemonade-omni.md: a custom omni model can ship its own
        system_prompt template; the global default in toolDefinitions.json is
        the fallback. The wire surface must echo the field on GET /models/{id}
        and on /models?show_all=true so the desktop app can read it back when
        re-opening the Omni Model editor.
        """
        canonical_name = f"user.PromptColl-{uuid.uuid4().hex[:8]}"
        public_name = canonical_name[5:]
        prompt_template = (
            "You are a focused tester. Tools available:\n\n"
            "{tool_list}\n\n"
            "Use them sparingly.{tool_guidance}"
        )

        try:
            response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": canonical_name,
                    "recipe": "collection.omni",
                    "components": [ENDPOINT_TEST_MODEL],
                    "system_prompt": prompt_template,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(response.status_code, 200, response.text)

            single = requests.get(
                f"{self.base_url}/models/{public_name}",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(single.status_code, 200)
            self.assertEqual(
                single.json().get("system_prompt"),
                prompt_template,
                "GET /models/{id} must echo the registered system_prompt verbatim.",
            )

            listing = requests.get(
                f"{self.base_url}/models?show_all=true",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(listing.status_code, 200)
            entry = next(
                (m for m in listing.json()["data"] if m["id"] == public_name),
                None,
            )
            self.assertIsNotNone(entry)
            self.assertEqual(entry.get("system_prompt"), prompt_template)

            print(f"[OK] system_prompt round-tripped for {public_name}")
        finally:
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": canonical_name},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_021k_register_collection_missing_components(self):
        """Collections referencing unknown components are rejected with 400."""
        canonical_name = f"user.BadColl-{uuid.uuid4().hex[:8]}"
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": canonical_name,
                "recipe": "collection.omni",
                "components": [f"user.does-not-exist-{uuid.uuid4().hex[:6]}"],
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("not registered", response.json().get("error", "").lower())
        print("[OK] Unknown component rejected with 400")

    def test_021l_register_collection_empty_array(self):
        """Empty components is rejected with 400."""
        canonical_name = f"user.EmptyColl-{uuid.uuid4().hex[:8]}"
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": canonical_name,
                "recipe": "collection.omni",
                "components": [],
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("components", response.json().get("error", ""))
        print("[OK] Empty components rejected with 400")

    def test_021m_register_collection_no_user_prefix(self):
        """Collection name without user. prefix is rejected with 400."""
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": f"NoPrefixColl-{uuid.uuid4().hex[:8]}",
                "recipe": "collection.omni",
                "components": [ENDPOINT_TEST_MODEL],
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("user.", response.json().get("error", ""))
        print("[OK] Missing user. prefix rejected with 400")

    def test_021n_register_collection_self_reference(self):
        """A collection that lists itself in components is rejected."""
        canonical_name = f"user.SelfRef-{uuid.uuid4().hex[:8]}"
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": canonical_name,
                "recipe": "collection.omni",
                "components": [canonical_name],
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("itself", response.json().get("error", "").lower())
        print("[OK] Self-reference rejected with 400")

    def test_021p_collection_components_canonicalized(self):
        """Client may register a collection using a component's public alias.
        Storage must canonicalize so downstream cache-key lookups
        (check_component_downloaded / update_model_in_cache) match; the wire
        format then re-emits components under their public names for
        consistency with the `id` field."""
        suffix = uuid.uuid4().hex[:8]
        component_canonical = f"user.AliasComp-{suffix}"
        # Unique user.<name> entries surface under the bare public alias.
        component_alias = component_canonical[5:]
        collection_name = f"user.AliasColl-{suffix}"
        try:
            pull_response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": component_canonical,
                    "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
                    "recipe": "llamacpp",
                    "stream": False,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(pull_response.status_code, 200, pull_response.text)

            # Register collection using the bare alias for the component.
            coll_response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": collection_name,
                    "recipe": "collection.omni",
                    "components": [component_alias],
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(coll_response.status_code, 200, coll_response.text)

            models_response = requests.get(
                f"{self.base_url}/models?show_all=true",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(models_response.status_code, 200)
            entry = next(
                (
                    m
                    for m in models_response.json()["data"]
                    if m["id"] == collection_name[5:]
                ),
                None,
            )
            self.assertIsNotNone(entry)
            self.assertEqual(
                entry.get("components"),
                [component_alias],
                "Wire-format components must use public names (same namespace as `id`)",
            )
            # `downloaded` can only be True if the internal cache-key lookup
            # found the component under its canonical name — this is the real
            # proof that storage canonicalized the aliased input.
            self.assertTrue(
                entry.get("downloaded"),
                "Cache-key lookups must find the canonically-stored component",
            )
            print("[OK] Aliased component canonicalized in storage, public on wire")
        finally:
            for name in (collection_name, component_canonical):
                try:
                    requests.post(
                        f"{self.base_url}/delete",
                        json={"model_name": name},
                        timeout=TIMEOUT_DEFAULT,
                    )
                except Exception:
                    pass

    def test_021t_inline_collection_missing_def_rejected(self):
        """Inline collection imports fail closed: a component with no matching
        definition in `models` (and not already registered) must be rejected,
        not silently dropped into a smaller, different collection."""
        suffix = uuid.uuid4().hex[:8]
        collection_name = f"user.InlineColl-{suffix}"
        defined = f"InlineComp-{suffix}"
        missing = f"MissingComp-{suffix}"
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": collection_name,
                "recipe": "collection.omni",
                # `components` lists two, but `models` defines only one and the
                # other is not a registered model -> the import must be rejected.
                "components": [defined, missing],
                "models": [
                    {
                        "model_name": defined,
                        "recipe": "llamacpp",
                        "checkpoints": {"main": USER_MODEL_MAIN_CHECKPOINT},
                    }
                ],
                "stream": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("matching definition", response.json().get("error", "").lower())

        # Fail-closed: the rejected collection must not have been persisted.
        models_response = requests.get(
            f"{self.base_url}/models?show_all=true",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(models_response.status_code, 200)
        ids = {m["id"] for m in models_response.json()["data"]}
        self.assertNotIn(
            collection_name[5:],
            ids,
            "Rejected inline collection must not be persisted",
        )
        print("[OK] Inline collection with missing component def rejected with 400")

    def test_021u_inline_collection_invalid_def_rejected(self):
        """Inline collection imports fail closed on a *malformed* component def:
        a `models` entry whose name matches but is missing the minimum a real
        registration needs (recipe + checkpoint) must be rejected up front, not
        registered as a half-defined user.* model that fails later mid-download."""
        suffix = uuid.uuid4().hex[:8]
        collection_name = f"user.InvalidColl-{suffix}"
        comp = f"InvalidComp-{suffix}"
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": collection_name,
                "recipe": "collection.omni",
                "components": [comp],
                # Name matches `components`, but the def has no recipe and no
                # checkpoint -> not a usable registration -> must be rejected.
                "models": [{"model_name": comp}],
                "stream": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("incomplete definition", response.json().get("error", "").lower())

        # Fail-closed: neither the collection nor the half-defined component
        # may have been persisted as a side effect.
        models_response = requests.get(
            f"{self.base_url}/models?show_all=true",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(models_response.status_code, 200)
        ids = {m["id"] for m in models_response.json()["data"]}
        self.assertNotIn(
            collection_name[5:], ids, "Rejected collection must not persist"
        )
        self.assertNotIn(comp, ids, "Half-defined component must not be registered")
        print("[OK] Inline collection with invalid component def rejected with 400")

    def test_021v_collection_self_reference_bare_name_rejected(self):
        """A collection that lists itself as a component by its *bare* name (e.g.
        `user.MyCol` with components ["MyCol"]) must be rejected, not just the
        exact `user.`-qualified spelling — otherwise it resolves back to itself."""
        suffix = uuid.uuid4().hex[:8]
        bare = f"SelfRefColl-{suffix}"
        collection_name = f"user.{bare}"
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": collection_name,
                "recipe": "collection.omni",
                # Bare self-reference: must be caught by the bare-form comparison.
                "components": [bare],
                "models": [
                    {
                        "model_name": bare,
                        "recipe": "collection.omni",
                        "components": [ENDPOINT_TEST_MODEL],
                    }
                ],
                "stream": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("reference itself", response.json().get("error", "").lower())

        models_response = requests.get(
            f"{self.base_url}/models?show_all=true",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(models_response.status_code, 200)
        ids = {m["id"] for m in models_response.json()["data"]}
        self.assertNotIn(bare, ids, "Self-referential collection must not persist")
        print("[OK] Bare-name self-referential collection rejected with 400")

    def _server_hf_cache_root(self, probe_repo_dir):
        """Return the HF cache root the *server* actually uses, verified by
        locating a repo dir the server already downloaded (`probe_repo_dir`), or
        None if it can't be located from this process.

        The server's cache may live somewhere the test process can't compute or
        read — e.g. a config.json `models_dir` override, or a packaged server
        (macOS .pkg) running under a different user/HOME. We probe candidates
        (config models_dir, then env/platform defaults) for a known-downloaded
        repo; if none match, the test can't stage a manifest where the server
        will read it, so the caller should skip."""
        candidates = []
        try:
            cfg = requests.get(
                f"http://localhost:{PORT}/internal/config", timeout=TIMEOUT_DEFAULT
            ).json()
            models_dir = cfg.get("models_dir", "") or ""
            if models_dir and models_dir != "auto" and os.path.isabs(models_dir):
                candidates.append(models_dir)
        except Exception:
            pass
        candidates.extend(get_hf_cache_dir_candidates())
        for root in candidates:
            if os.path.isdir(os.path.join(root, probe_repo_dir)):
                return root
        return None

    def _write_collection_manifest(self, cache_root, repo_id, components, models):
        """Write a fake HF-cached collection manifest for `repo_id` into the HF
        cache (refs/main + a snapshot dir), mimicking a repo pulled by
        `lemonade pull <org>/<repo>`. Returns the repo cache dir path."""
        repo_dir = os.path.join(cache_root, "models--" + repo_id.replace("/", "--"))
        snapshot = os.path.join(repo_dir, "snapshots", "rev1")
        os.makedirs(snapshot, exist_ok=True)
        os.makedirs(os.path.join(repo_dir, "refs"), exist_ok=True)
        with open(os.path.join(repo_dir, "refs", "main"), "w", encoding="utf-8") as f:
            f.write("rev1")
        manifest = {
            "model_name": repo_id.split("/")[-1],
            "recipe": "collection.omni",
            "checkpoints": {"main": ""},
            "components": components,
            "models": models,
        }
        # Content-based discovery: the filename is not load-bearing for the cache
        # reader, but use the documented <RepoName>.json convention anyway.
        with open(
            os.path.join(snapshot, repo_id.split("/")[-1] + ".json"),
            "w",
            encoding="utf-8",
        ) as f:
            json.dump(manifest, f)
        return repo_dir

    def _collection_components(self, model_id):
        r = requests.get(f"{self.base_url}/models/{model_id}", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(r.status_code, 200, r.text)
        return r.json().get("components", [])

    def test_021w_hf_backed_collection_refresh_is_pointer_only(self):
        """HF-backed collections are pointer-only: the pull body is just a repo
        pointer (the real `lemonade pull <org>/<repo>` shape — no inline
        components/models), /pull resolves components from the manifest on disk,
        nothing is persisted in user_models.json, and a changed manifest is
        reflected on re-pull (the Codex/fl0rianr staleness scenario). The staged
        manifest stands in for what /pull's own download step writes to disk;
        the real network download of the manifest is exercised by server_omni.py.
        Uses already-downloaded components so the refresh needs no network."""
        suffix = uuid.uuid4().hex[:8]
        repo_id = f"lemontest/RefreshKit-{suffix}"
        collection = f"user.RefreshKit-{suffix}"
        # Component A: the always-present built-in test model.
        comp_a = ENDPOINT_TEST_MODEL
        a_def = {
            "model_name": comp_a,
            "recipe": "llamacpp",
            "checkpoints": {"main": USER_MODEL_MAIN_CHECKPOINT},
        }
        # Component B: a user model we pre-pull so it is already downloaded; the
        # refresh that adds it must not require a network fetch.
        comp_b = f"RefreshB-{suffix}"
        b_def = {
            "model_name": comp_b,
            "recipe": "llamacpp",
            "checkpoints": {"main": USER_MODEL_MAIN_CHECKPOINT},
        }
        repo_dir = None
        try:
            # Pre-download component B as a standalone user model.
            pull_b = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": f"user.{comp_b}",
                    "recipe": b_def["recipe"],
                    "checkpoints": b_def["checkpoints"],
                    "stream": False,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(pull_b.status_code, 200, pull_b.text)

            # Discover the server's real HF cache root by locating the repo it
            # just downloaded for component B (config models_dir overrides can put
            # it where the test side wouldn't compute, e.g. macOS .pkg installs).
            b_repo_dir = "models--" + USER_MODEL_MAIN_CHECKPOINT.split(":")[0].replace(
                "/", "--"
            )
            cache_root = self._server_hf_cache_root(b_repo_dir)
            if cache_root is None:
                self.skipTest(
                    "Cannot locate the server's HF cache from the test process "
                    "(e.g. packaged server under a different user); the HF-backed "
                    "refresh path is covered end-to-end by server_omni.py."
                )

            # Manifest v1 on disk (stands in for /pull's own manifest download).
            repo_dir = self._write_collection_manifest(
                cache_root, repo_id, [comp_a], [a_def]
            )

            # Register the HF-backed collection with the real hf_pull POINTER body:
            # model name + recipe + the repo as the checkpoint. No inline
            # components/models — /pull resolves them from the manifest on disk.
            reg = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": collection,
                    "recipe": "collection.omni",
                    "checkpoints": {"main": repo_id},
                    "stream": False,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(reg.status_code, 200, reg.text)

            # Pointer-only: components must NOT be persisted in the registry.
            self.assertEqual(
                sorted(self._collection_components(collection)),
                sorted([comp_a]),
                "Collection should expose the manifest's single component",
            )

            # Manifest v2: add component B upstream, then refresh via re-pull.
            self._write_collection_manifest(
                cache_root, repo_id, [comp_a, comp_b], [a_def, b_def]
            )
            refresh = requests.post(
                f"{self.base_url}/pull",
                json={"model_name": collection, "stream": False},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(refresh.status_code, 200, refresh.text)

            # The new component must now be reflected — proving the registry did
            # not shadow the refreshed manifest with a stale persisted list. A
            # unique user.* component surfaces under its bare public name.
            components = self._collection_components(collection)
            self.assertIn(comp_a, components)
            self.assertIn(
                comp_b,
                components,
                "Refreshed manifest's added component must appear after re-pull",
            )
            print("[OK] HF-backed collection refresh reflects changed manifest")
        finally:
            for name in (collection, f"user.{comp_b}"):
                try:
                    requests.post(
                        f"{self.base_url}/delete",
                        json={"model_name": name},
                        timeout=TIMEOUT_DEFAULT,
                    )
                except Exception:
                    pass
            if repo_dir and os.path.isdir(repo_dir):
                shutil.rmtree(repo_dir, ignore_errors=True)

    def test_021x_reject_nested_collection_by_name(self):
        """Nested collections are not supported: a collection whose component
        names an already-registered collection (here the built-in
        LMX-Omni-5.5B-Lite) must be rejected — components must be leaf models."""
        collection_name = f"user.NestByName-{uuid.uuid4().hex[:8]}"
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": collection_name,
                "recipe": "collection.omni",
                "components": ["LMX-Omni-5.5B-Lite"],  # a built-in collection
                "stream": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("not collections", response.json().get("error", "").lower())
        ids = {
            m["id"]
            for m in requests.get(
                f"{self.base_url}/models?show_all=true", timeout=TIMEOUT_DEFAULT
            ).json()["data"]
        }
        self.assertNotIn(
            collection_name[5:], ids, "Rejected collection must not persist"
        )
        print("[OK] Nested collection (component is a registered collection) rejected")

    def test_021y_reject_nested_collection_inline_def(self):
        """Nested collections are not supported: a component whose inline `models`
        definition is itself a collection (recipe collection.omni) must be
        rejected, not registered."""
        suffix = uuid.uuid4().hex[:8]
        collection_name = f"user.NestInline-{suffix}"
        child = f"NestChild-{suffix}"
        response = requests.post(
            f"{self.base_url}/pull",
            json={
                "model_name": collection_name,
                "recipe": "collection.omni",
                "components": [child],
                "models": [
                    {
                        "model_name": child,
                        "recipe": "collection.omni",  # nested → must be rejected
                        "components": [ENDPOINT_TEST_MODEL],
                    }
                ],
                "stream": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("not collections", response.json().get("error", "").lower())
        ids = {
            m["id"]
            for m in requests.get(
                f"{self.base_url}/models?show_all=true", timeout=TIMEOUT_DEFAULT
            ).json()["data"]
        }
        self.assertNotIn(
            collection_name[5:], ids, "Rejected collection must not persist"
        )
        self.assertNotIn(child, ids, "Nested child collection must not be registered")
        print("[OK] Nested collection (inline collection component def) rejected")

    def test_021o_load_collection_routes_through_component_branch(self):
        """POST /load on a collection must not route the collection itself
        through the generic HF download path (collections have no checkpoint).
        Component cascading is the only legitimate download path."""
        canonical_name = f"user.LoadColl-{uuid.uuid4().hex[:8]}"
        try:
            pull_response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": canonical_name,
                    "recipe": "collection.omni",
                    "components": [ENDPOINT_TEST_MODEL],
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(pull_response.status_code, 200, pull_response.text)

            load_response = requests.post(
                f"{self.base_url}/load",
                json={"model_name": canonical_name},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(load_response.status_code, 200, load_response.text)
            self.assertEqual(load_response.json().get("recipe"), "collection.omni")
            print("[OK] Load on collection succeeded via component branch")
        finally:
            try:
                requests.post(
                    f"{self.base_url}/unload",
                    json={"model_name": ENDPOINT_TEST_MODEL},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": canonical_name},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_021z_router_collection_chat_dispatch(self):
        """A collection.router model flips /chat/completions into engine mode
        (#2385): the recipe is the trigger — no "auto", no /v1/route. The
        routing engine selects a candidate and the request is dispatched to it,
        returning a real completion produced by the engine-selected model."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterColl-{suffix}"
        public_name = canonical_name[5:]
        try:
            # Register a collection.router whose only candidate is the test
            # model. Both the keyword rule and the fail-open default resolve to
            # ENDPOINT_TEST_MODEL, so any input dispatches there.
            pull_response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": canonical_name,
                    "version": "1",
                    "recipe": "collection.router",
                    "components": [ENDPOINT_TEST_MODEL],
                    "routing": {
                        "candidates": [ENDPOINT_TEST_MODEL],
                        "default_model": ENDPOINT_TEST_MODEL,
                        "rules": [
                            {
                                "id": "code-to-test-model",
                                "match": {"keywords_any": ["code", "def "]},
                                "route_to": ENDPOINT_TEST_MODEL,
                            }
                        ],
                    },
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(pull_response.status_code, 200, pull_response.text)
            self.assertEqual(pull_response.json()["status"], "success")

            # Addressing the collection.router model by name must return a real
            # completion produced by the engine-selected candidate.
            chat_response = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_name,
                    "messages": [
                        {"role": "user", "content": "Please write code for me"}
                    ],
                    "max_tokens": 8,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(chat_response.status_code, 200, chat_response.text)
            body = chat_response.json()
            self.assertIn("choices", body)
            self.assertTrue(
                body["choices"], "engine-routed completion must have choices"
            )
            message = body["choices"][0].get("message", {})
            self.assertIsInstance(message.get("content"), str)
            # The response reflects the engine-selected candidate, not the
            # collection.router alias that was addressed.
            self.assertNotEqual(
                body.get("model"),
                public_name,
                "response model must be the routed candidate, not the router alias",
            )
            route = body.get("x_lemonade_route")
            self.assertIsInstance(route, dict)
            self.assertEqual(route.get("version"), "1")
            self.assertEqual(route.get("route_to"), ENDPOINT_TEST_MODEL)
            self.assertEqual(route.get("matched_rule"), "code-to-test-model")
            self.assertEqual(route.get("default_used"), False)
            self.assertEqual(route.get("outputs"), {})
            self.assertNotIn("trace", route, "trace must be opt-in via route_trace")
            self.assertEqual(
                chat_response.headers.get("x-lemonade-route"),
                "code-to-test-model",
            )

            default_response = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_name,
                    "messages": [{"role": "user", "content": "Hello there"}],
                    "max_tokens": 8,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(default_response.status_code, 200, default_response.text)
            default_body = default_response.json()
            default_route = default_body.get("x_lemonade_route")
            self.assertIsInstance(default_route, dict)
            self.assertEqual(default_route.get("route_to"), ENDPOINT_TEST_MODEL)
            self.assertEqual(default_route.get("matched_rule"), "")
            self.assertEqual(default_route.get("default_used"), True)
            self.assertEqual(default_route.get("outputs"), {})
            self.assertNotIn("trace", default_route)
            self.assertEqual(
                default_response.headers.get("x-lemonade-route"), "default"
            )
            print(f"[OK] collection.router dispatched {public_name} -> completion")
        finally:
            try:
                requests.post(
                    f"{self.base_url}/unload",
                    json={"model_name": ENDPOINT_TEST_MODEL},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": canonical_name},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_021zh_router_collection_repull_overwrite(self):
        """Re-pulling an already-registered collection.router under the same
        name must succeed (#2703). On overwrite the registration data is
        enriched with the persisted registry source; that internal field must
        not reach the strict routing-policy parser, which would otherwise reject
        it as an unknown root key."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterRepull-{suffix}"
        collection_body = {
            "model_name": canonical_name,
            "version": "1",
            "recipe": "collection.router",
            "components": [ENDPOINT_TEST_MODEL],
            "routing": {
                "candidates": [ENDPOINT_TEST_MODEL],
                "default_model": ENDPOINT_TEST_MODEL,
                "rules": [
                    {
                        "id": "always-test-model",
                        "match": {"keywords_any": ["code"]},
                        "route_to": ENDPOINT_TEST_MODEL,
                    }
                ],
            },
        }
        try:
            # Initial registration.
            first = requests.post(
                f"{self.base_url}/pull",
                json=collection_body,
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(first.status_code, 200, first.text)
            self.assertEqual(first.json()["status"], "success")

            # Re-pull the identical body (no explicit source/registry_source).
            # The overwrite path injects the persisted registry source into the
            # registration data; validating that enriched object used to 500
            # with "collection contains unknown key 'source'".
            second = requests.post(
                f"{self.base_url}/pull",
                json=collection_body,
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(second.status_code, 200, second.text)
            self.assertEqual(second.json()["status"], "success")
            print(f"[OK] collection.router re-pull overwrite: {canonical_name}")
        finally:
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": canonical_name},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_021zi_router_collection_trace_and_outputs(self):
        """route_trace=true returns the full Decision trace and copies rule
        outputs verbatim without interpreting them (#2386)."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterTrace-{suffix}"
        public_name = canonical_name[5:]
        routing = {
            "candidates": [ENDPOINT_TEST_MODEL],
            "default_model": ENDPOINT_TEST_MODEL,
            "rules": [
                {
                    "id": "code-to-test-model",
                    "match": {"keywords_any": ["code", "def "]},
                    "route_to": ENDPOINT_TEST_MODEL,
                    "outputs": {"verdict": "warn"},
                }
            ],
        }
        try:
            pull_response = self._pull_router_collection(
                canonical_name, routing=routing
            )
            self.assertEqual(pull_response.status_code, 200, pull_response.text)
            self.assertEqual(pull_response.json()["status"], "success")

            chat_response = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_name,
                    "messages": [
                        {"role": "user", "content": "Please write code for me"}
                    ],
                    "route_trace": True,
                    "max_tokens": 8,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(chat_response.status_code, 200, chat_response.text)
            body = chat_response.json()
            self.assertIn("choices", body)
            route = body.get("x_lemonade_route")
            self.assertIsInstance(route, dict)
            self.assertEqual(route.get("outputs"), {"verdict": "warn"})
            self.assertEqual(route.get("matched_rule"), "code-to-test-model")
            trace = route.get("trace")
            self.assertIsInstance(trace, list)
            self.assertTrue(trace)
            self.assertTrue(
                any(
                    entry.get("condition") == "keywords_any"
                    and entry.get("result") is True
                    for entry in trace
                ),
                f"route trace must include the matched keywords condition: {trace}",
            )
            # Core must not interpret trust outputs as content-filter behavior.
            choice = body["choices"][0]
            self.assertNotEqual(choice.get("finish_reason"), "content_filter")
            print(f"[OK] collection.router route_trace returned Decision trace")
        finally:
            self._cleanup_router_collection(canonical_name)

    def test_021zj_router_llm_l0a_live(self):
        """L0a live path (#2405), deterministic: the router component is a mock
        cloud model (via _start_mock_cloud_provider) that returns a fixed valid
        {model, rationale} reply, while the candidate is the real local
        ENDPOINT_TEST_MODEL. This exercises the complete production path —
        collection.router -> LlmClassifier -> ClassifierServices::chat ->
        Router::chat_completion -> CloudServer -> strict structured-reply
        parser -> rule evaluation and trace -> real candidate auto-load ->
        candidate completion — without depending on a small local LLM obeying
        a formatting prompt on every platform. Exhaustive parser behavior is
        covered by the C++ RoutingPolicyLlmRouterTest suite; this test
        validates wiring and backend integration, and asserts the live
        adapter constraints on the captured router request."""
        provider = "routercloud"
        upstream_id = "mock/l0a-router"
        router_model = f"{provider}.{upstream_id}"
        candidate_model = ENDPOINT_TEST_MODEL
        pull_model_with_retry(candidate_model)

        fixed_rationale = "The configured candidate handles this request."
        captured = {}

        def chat_response(req):
            captured["request"] = req
            return {
                "id": "cmpl-l0a-router",
                "object": "chat.completion",
                "created": 1,
                "model": req.get("model", upstream_id),
                "choices": [
                    {
                        "index": 0,
                        "message": {
                            "role": "assistant",
                            "content": json.dumps(
                                {
                                    "model": candidate_model,
                                    "rationale": fixed_rationale,
                                }
                            ),
                        },
                        "finish_reason": "stop",
                    }
                ],
                "usage": {
                    "prompt_tokens": 1,
                    "completion_tokens": 1,
                    "total_tokens": 2,
                },
            }

        base_url, stop_provider = self._start_mock_cloud_provider(
            [upstream_id], chat_handler=chat_response
        )

        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterL0a-{suffix}"
        public_name = canonical_name[5:]
        try:
            # Register + authenticate the mock provider so the router model is
            # discoverable (same flow as the cloud endpoint tests).
            resp = requests.post(
                f"{self.base_url}/install",
                json={
                    "backend": "cloud",
                    "provider": provider,
                    "base_url": base_url,
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
            self.assertEqual(resp.json()["models_discovered"], 1)

            requests.post(
                f"{self.base_url}/unload", json={}, timeout=TIMEOUT_DEFAULT
            )

            routing = {
                "candidates": [candidate_model],
                "default_model": candidate_model,
                "router": {
                    "type": "llm",
                    "model": router_model,
                    "prompt": "You are a model router. Pick the best model "
                    "for the user's request.",
                },
            }
            pull_response = self._pull_router_collection(
                canonical_name,
                routing=routing,
                overrides={"components": [router_model, candidate_model]},
            )
            self.assertEqual(pull_response.status_code, 200, pull_response.text)
            self.assertEqual(pull_response.json()["status"], "success")

            user_text = "Explain gradient descent."
            chat_response_http = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_name,
                    "messages": [{"role": "user", "content": user_text}],
                    "max_tokens": 16,
                    "route_trace": True,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(
                chat_response_http.status_code, 200, chat_response_http.text
            )
            body = chat_response_http.json()
            self.assertNotIn("error", body, body)

            # Lemonade's route envelope is the backend-independent source of
            # truth for the selected candidate. The OpenAI `model` field remains
            # backend-owned and may contain a checkpoint/path or be omitted.
            route = body.get("x_lemonade_route")
            self.assertIsInstance(route, dict)
            self.assertEqual(route.get("route_to"), candidate_model)

            # The trace carries the structured choice: the winning
            # classifier:__router entry names the candidate as its label and
            # records the mock's exact rationale.
            trace = route.get("trace")
            self.assertIsInstance(trace, list)
            router_entry = next(
                (
                    e
                    for e in trace
                    if e.get("condition") == "classifier:__router"
                    and e.get("result") is True
                ),
                None,
            )
            self.assertIsNotNone(
                router_entry, f"no winning __router trace entry: {trace}"
            )
            self.assertEqual(router_entry.get("label"), candidate_model)
            self.assertEqual(router_entry.get("rationale"), fixed_rationale)

            # Live adapter constraints, asserted on the request the mock
            # provider actually received from Router::chat_completion.
            router_request = captured.get("request")
            self.assertIsNotNone(
                router_request, "mock provider never received the router call"
            )
            self.assertFalse(router_request["stream"])
            self.assertEqual(router_request["temperature"], 0.0)
            self.assertLessEqual(router_request["max_tokens"], 256)
            self.assertTrue(
                router_request["messages"][-1]["content"].startswith("/no_think\n")
            )
            # The user message after the /no_think prefix is the structured
            # routing-context payload.
            payload = json.loads(
                router_request["messages"][-1]["content"][len("/no_think\n") :]
            )
            self.assertEqual(payload.get("text"), user_text)
            self.assertIn("has_tools", payload)
            self.assertIn("has_images", payload)
            print(
                "[OK] L0a live (deterministic): structured choice routed "
                "through Router::chat_completion -> CloudServer with label + "
                "rationale in the trace and constrained adapter request"
            )
        finally:
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": canonical_name},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass
            try:
                requests.post(
                    f"{self.base_url}/unload",
                    json={"model_name": candidate_model},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass
            try:
                requests.post(
                    f"{self.base_url}/uninstall",
                    json={"backend": "cloud", "provider": provider},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass
            stop_provider()

    def _pull_router_collection(self, canonical_name, routing=None, overrides=None):
        """Register a collection.router whose single candidate is
        ENDPOINT_TEST_MODEL. `overrides` is merged into the top-level pull
        payload (e.g. to drop "version" for the negative tests)."""
        if routing is None:
            routing = {
                "candidates": [ENDPOINT_TEST_MODEL],
                "default_model": ENDPOINT_TEST_MODEL,
                "rules": [
                    {
                        "id": "code-to-test-model",
                        "match": {"keywords_any": ["code", "def "]},
                        "route_to": ENDPOINT_TEST_MODEL,
                    }
                ],
            }
        payload = {
            "model_name": canonical_name,
            "version": "1",
            "recipe": "collection.router",
            "components": [ENDPOINT_TEST_MODEL],
            "routing": routing,
        }
        if overrides is not None:
            payload.update(overrides)
            # Allow negative tests to remove a required key entirely.
            for key, value in list(payload.items()):
                if value is None:
                    del payload[key]
        return requests.post(
            f"{self.base_url}/pull", json=payload, timeout=TIMEOUT_MODEL_OPERATION
        )

    def _cleanup_router_collection(self, canonical_name):
        for endpoint, body in (
            ("/unload", {"model_name": ENDPOINT_TEST_MODEL}),
            ("/delete", {"model_name": canonical_name}),
        ):
            try:
                requests.post(
                    f"{self.base_url}{endpoint}", json=body, timeout=TIMEOUT_DEFAULT
                )
            except Exception:
                pass

    def _collect_sse_data_events(self, resp):
        data_events = []
        for raw_line in resp.iter_lines():
            if not raw_line:
                continue
            line = raw_line.decode("utf-8", errors="replace")
            if line.startswith("data:"):
                data_events.append(line[len("data:") :].strip())
        return data_events

    def _assert_stream_route_decision(self, resp, endpoint_name):
        if resp.status_code != 200:
            self.fail(
                f"streaming {endpoint_name} returned {resp.status_code}: {resp.text}"
            )
        self.assertEqual(resp.headers.get("x-lemonade-route"), "code-to-test-model")
        data_events = self._collect_sse_data_events(resp)
        self.assertTrue(
            data_events,
            f"streaming {endpoint_name} must emit at least one SSE data event",
        )
        blob = "\n".join(data_events)
        self.assertNotIn(
            '"error"',
            blob,
            f"streaming {endpoint_name} must not error: {blob[:500]}",
        )
        route_chunks = []
        for event in data_events:
            if event == "[DONE]":
                continue
            try:
                payload = json.loads(event)
            except Exception:
                continue
            route = payload.get("x_lemonade_route")
            if route:
                route_chunks.append(route)
        self.assertTrue(
            route_chunks,
            f"streaming {endpoint_name} must attach x_lemonade_route to a chunk",
        )
        route = route_chunks[0]
        self.assertEqual(route.get("route_to"), ENDPOINT_TEST_MODEL)
        self.assertEqual(route.get("matched_rule"), "code-to-test-model")
        self.assertIsInstance(route.get("trace"), list)
        return data_events

    def test_021zj_router_collection_chat_streaming_route_decision(self):
        """/chat/completions streaming attaches additive route metadata."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterChatStream-{suffix}"
        public_name = canonical_name[5:]
        try:
            pull_response = self._pull_router_collection(canonical_name)
            self.assertEqual(pull_response.status_code, 200, pull_response.text)
            self.assertEqual(pull_response.json()["status"], "success")

            with requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_name,
                    "messages": [
                        {"role": "user", "content": "Please write code for me"}
                    ],
                    "max_tokens": 8,
                    "stream": True,
                    "route_trace": True,
                },
                stream=True,
                timeout=TIMEOUT_MODEL_OPERATION,
            ) as resp:
                self._assert_stream_route_decision(resp, "/chat/completions")
            print(
                f"[OK] collection.router /chat/completions (streaming) attached route decision"
            )
        finally:
            self._cleanup_router_collection(canonical_name)

    def test_021zk_router_collection_completions_streaming_route_decision(self):
        """/completions streaming attaches additive route metadata."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterComplStream-{suffix}"
        public_name = canonical_name[5:]
        try:
            pull_response = self._pull_router_collection(canonical_name)
            self.assertEqual(pull_response.status_code, 200, pull_response.text)
            self.assertEqual(pull_response.json()["status"], "success")

            with requests.post(
                f"{self.base_url}/completions",
                json={
                    "model": public_name,
                    "prompt": "Please write code for me",
                    "max_tokens": 8,
                    "stream": True,
                    "route_trace": True,
                },
                stream=True,
                timeout=TIMEOUT_MODEL_OPERATION,
            ) as resp:
                self._assert_stream_route_decision(resp, "/completions")
            print(
                f"[OK] collection.router /completions (streaming) attached route decision"
            )
        finally:
            self._cleanup_router_collection(canonical_name)

    def test_021za_router_collection_completions_dispatch(self):
        """/completions dispatches a collection.router request to the
        engine-selected candidate (#2385), same as /chat/completions."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterCompl-{suffix}"
        public_name = canonical_name[5:]
        try:
            pull_response = self._pull_router_collection(canonical_name)
            self.assertEqual(pull_response.status_code, 200, pull_response.text)
            self.assertEqual(pull_response.json()["status"], "success")

            resp = requests.post(
                f"{self.base_url}/completions",
                json={
                    "model": public_name,
                    "prompt": "Please write code for me",
                    "max_tokens": 8,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(resp.status_code, 200, resp.text)
            body = resp.json()
            self.assertIn("choices", body)
            self.assertTrue(
                body["choices"], "engine-routed completion must have choices"
            )
            self.assertIsInstance(body["choices"][0].get("text"), str)
            self.assertNotEqual(
                body.get("model"),
                public_name,
                "response model must be the routed candidate, not the router alias",
            )
            print(f"[OK] collection.router /completions dispatched {public_name}")
        finally:
            self._cleanup_router_collection(canonical_name)

    def test_021zb_router_collection_responses_dispatch(self):
        """/responses (non-streaming) dispatches a collection.router request to
        the engine-selected candidate (#2385)."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterResp-{suffix}"
        public_name = canonical_name[5:]
        try:
            pull_response = self._pull_router_collection(canonical_name)
            self.assertEqual(pull_response.status_code, 200, pull_response.text)
            self.assertEqual(pull_response.json()["status"], "success")

            resp = requests.post(
                f"{self.base_url}/responses",
                json={
                    "model": public_name,
                    "input": "Please write code for me",
                    "max_output_tokens": 16,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(resp.status_code, 200, resp.text)
            body = resp.json()
            self.assertNotIn("error", body, resp.text)
            # The response reflects the routed candidate, not the router alias.
            if "model" in body:
                self.assertNotEqual(body.get("model"), public_name)
            print(f"[OK] collection.router /responses dispatched {public_name}")
        finally:
            self._cleanup_router_collection(canonical_name)

    def test_021zc_router_collection_responses_streaming_dispatch(self):
        """/responses with stream=true dispatches a collection.router request to
        the engine-selected candidate and streams SSE events (#2385). Exercises
        the request re-serialization that carries the rewritten model to the
        backend on the streaming path."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterRespStream-{suffix}"
        public_name = canonical_name[5:]
        try:
            pull_response = self._pull_router_collection(canonical_name)
            self.assertEqual(pull_response.status_code, 200, pull_response.text)
            self.assertEqual(pull_response.json()["status"], "success")

            with requests.post(
                f"{self.base_url}/responses",
                json={
                    "model": public_name,
                    "input": "Please write code for me",
                    "max_output_tokens": 16,
                    "stream": True,
                    "route_trace": True,
                },
                stream=True,
                timeout=TIMEOUT_MODEL_OPERATION,
            ) as resp:
                self._assert_stream_route_decision(resp, "/responses")
            print(
                f"[OK] collection.router /responses (streaming) dispatched {public_name}"
            )
        finally:
            self._cleanup_router_collection(canonical_name)

    def test_021zd_router_collection_survives_cache_rebuild(self):
        """The parsed routing policy survives a models-cache rebuild (#2385):
        the source-declared version and routing block are persisted and
        re-parsed, so dispatch still works after the cache is invalidated by an
        unrelated /pull."""
        suffix = uuid.uuid4().hex[:8]
        canonical_a = f"user.RouterRebuildA-{suffix}"
        canonical_b = f"user.RouterRebuildB-{suffix}"
        public_a = canonical_a[5:]
        try:
            resp_a = self._pull_router_collection(canonical_a)
            self.assertEqual(resp_a.status_code, 200, resp_a.text)
            # A second /pull invalidates the models cache; the next request that
            # touches the cache rebuilds it and must re-parse collection A's
            # policy from its persisted version + routing block.
            resp_b = self._pull_router_collection(canonical_b)
            self.assertEqual(resp_b.status_code, 200, resp_b.text)

            chat_response = requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": public_a,
                    "messages": [{"role": "user", "content": "Please write code"}],
                    "max_tokens": 8,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(chat_response.status_code, 200, chat_response.text)
            body = chat_response.json()
            self.assertTrue(body.get("choices"))
            self.assertNotEqual(
                body.get("model"),
                public_a,
                "policy must still dispatch after a cache rebuild",
            )
            print(f"[OK] collection.router policy survived cache rebuild: {public_a}")
        finally:
            self._cleanup_router_collection(canonical_a)
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": canonical_b},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_021ze_router_collection_invalid_policy_rejected(self):
        """A collection.router /pull with a broken routing policy is rejected at
        registration (#2385): the parser gate runs before the model is stored,
        so bad policies never reach dispatch. Covers a missing schema version
        and a default_model that is not a declared candidate."""
        suffix = uuid.uuid4().hex[:8]

        # Missing required schema version.
        no_version = f"user.RouterNoVer-{suffix}"
        try:
            resp = self._pull_router_collection(no_version, overrides={"version": None})
            self.assertNotEqual(
                resp.status_code,
                200,
                f"router pull without version must be rejected: {resp.text}",
            )
        finally:
            self._cleanup_router_collection(no_version)

        # default_model is not one of the candidates.
        bad_default = f"user.RouterBadDefault-{suffix}"
        try:
            resp = self._pull_router_collection(
                bad_default,
                routing={
                    "candidates": [ENDPOINT_TEST_MODEL],
                    "default_model": "Not-A-Candidate-Model",
                    "rules": [],
                },
            )
            self.assertNotEqual(
                resp.status_code,
                200,
                f"router pull with non-candidate default_model must be rejected: {resp.text}",
            )
        finally:
            self._cleanup_router_collection(bad_default)
        print("[OK] collection.router invalid policies rejected at registration")

    def test_021zf_router_collection_load_is_virtual_noop(self):
        """/load on a collection.router acknowledges success without bringing up
        a backend (#2385): router collections are virtual, so /load must not
        fall through to the normal backend-load path."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterLoad-{suffix}"
        public_name = canonical_name[5:]
        try:
            pull_response = self._pull_router_collection(canonical_name)
            self.assertEqual(pull_response.status_code, 200, pull_response.text)

            load_response = requests.post(
                f"{self.base_url}/load",
                json={"model_name": public_name},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(load_response.status_code, 200, load_response.text)
            load_body = load_response.json()
            self.assertEqual(load_body.get("status"), "success")
            self.assertEqual(load_body.get("recipe"), "collection.router")

            # The virtual collection must not appear as a loaded backend.
            health = requests.get(
                f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
            ).json()
            loaded = health.get("all_models_loaded", []) or []
            self.assertNotIn(public_name, loaded)
            self.assertNotIn(canonical_name, loaded)
            print(f"[OK] collection.router /load was a virtual no-op: {public_name}")
        finally:
            self._cleanup_router_collection(canonical_name)

    def test_021zg_router_collection_export_roundtrip(self):
        """A router collection exported from /models surfaces its schema
        "version" (not just "routing") and can be re-imported through /pull
        (#2385). Guards the import/export round-trip so the required version
        isn't dropped on export and rejected on re-import."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterExport-{suffix}"
        public_name = canonical_name[5:]
        reimport_name = f"user.RouterReimport-{suffix}"
        try:
            pull_response = self._pull_router_collection(canonical_name)
            self.assertEqual(pull_response.status_code, 200, pull_response.text)

            models = requests.get(
                f"{self.base_url}/models?show_all=true", timeout=TIMEOUT_DEFAULT
            ).json()
            exported = next(
                (m for m in models.get("data", []) if m.get("id") == public_name),
                None,
            )
            self.assertIsNotNone(exported, f"{public_name} missing from /models export")
            self.assertEqual(exported.get("recipe"), "collection.router")
            self.assertIn("routing", exported)
            self.assertEqual(
                exported.get("version"),
                "1",
                "exported router collection must surface its schema version",
            )

            # The exported object must be re-importable verbatim (modulo name).
            reimport_response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": reimport_name,
                    "version": exported["version"],
                    "recipe": exported["recipe"],
                    "components": exported["components"],
                    "routing": exported["routing"],
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(reimport_response.status_code, 200, reimport_response.text)
            self.assertEqual(reimport_response.json().get("status"), "success")
            print(f"[OK] collection.router export round-trip preserved version")
        finally:
            self._cleanup_router_collection(canonical_name)
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": reimport_name},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def test_021zh_router_collection_responses_typed_input_dispatch(self):
        """/responses dispatch works when the input uses typed content parts
        (message with input_text parts) rather than a plain string (#2385),
        exercising the RouteContext extraction for structured Responses input."""
        suffix = uuid.uuid4().hex[:8]
        canonical_name = f"user.RouterTyped-{suffix}"
        public_name = canonical_name[5:]
        try:
            pull_response = self._pull_router_collection(canonical_name)
            self.assertEqual(pull_response.status_code, 200, pull_response.text)

            resp = requests.post(
                f"{self.base_url}/responses",
                json={
                    "model": public_name,
                    "input": [
                        {
                            "role": "user",
                            "content": [
                                {"type": "input_text", "text": "Please write code"}
                            ],
                        }
                    ],
                    "max_output_tokens": 16,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(resp.status_code, 200, resp.text)
            body = resp.json()
            self.assertNotIn("error", body, resp.text)
            if "model" in body:
                self.assertNotEqual(body.get("model"), public_name)
            print(
                f"[OK] collection.router /responses typed input dispatched {public_name}"
            )
        finally:
            self._cleanup_router_collection(canonical_name)

    def test_021q_collection_repull_overwrites_components(self):
        """Re-pulling an existing collection with a new components array must
        overwrite the stored entry, not silently reuse the old components."""
        suffix = uuid.uuid4().hex[:8]
        extra_component = f"user.RepullExtra-{suffix}"
        # Unique user.<name> entries surface under the bare public alias on the wire.
        extra_component_alias = extra_component[5:]
        collection_name = f"user.RepullColl-{suffix}"
        try:
            extra_pull = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": extra_component,
                    "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
                    "recipe": "llamacpp",
                    "stream": False,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(extra_pull.status_code, 200, extra_pull.text)

            first = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": collection_name,
                    "recipe": "collection.omni",
                    "components": [ENDPOINT_TEST_MODEL],
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(first.status_code, 200, first.text)

            second = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": collection_name,
                    "recipe": "collection.omni",
                    "components": [ENDPOINT_TEST_MODEL, extra_component],
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(second.status_code, 200, second.text)

            models_response = requests.get(
                f"{self.base_url}/models?show_all=true",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(models_response.status_code, 200)
            entry = next(
                (
                    m
                    for m in models_response.json()["data"]
                    if m["id"] == collection_name[5:]
                ),
                None,
            )
            self.assertIsNotNone(entry)
            self.assertEqual(
                sorted(entry.get("components", [])),
                sorted([ENDPOINT_TEST_MODEL, extra_component_alias]),
                "Re-pull must persist the new components list",
            )
            print("[OK] Collection re-pull overwrote components")
        finally:
            for name in (collection_name, extra_component):
                try:
                    requests.post(
                        f"{self.base_url}/delete",
                        json={"model_name": name},
                        timeout=TIMEOUT_DEFAULT,
                    )
                except Exception:
                    pass

    def test_021f_naming_spec_unique_registered(self):
        """Naming spec: a unique user.<name> with no built-in collision emits as bare."""
        bare = f"NameSpec-Unique-{uuid.uuid4().hex[:8]}"
        canonical = f"user.{bare}"

        try:
            pull_response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": canonical,
                    "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
                    "recipe": "llamacpp",
                    "stream": False,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(pull_response.status_code, 200)

            models_response = requests.get(
                f"{self.base_url}/models?show_all=true",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(models_response.status_code, 200)
            model_ids = {m["id"] for m in models_response.json()["data"]}
            self.assertIn(bare, model_ids)
            self.assertNotIn(canonical, model_ids)
            self.assertNotIn(f"builtin.{bare}", model_ids)

            # Bare and user.* both resolve; builtin.* must 404.
            for ok_id in [bare, canonical]:
                r = requests.get(
                    f"{self.base_url}/models/{ok_id}",
                    timeout=TIMEOUT_DEFAULT,
                )
                self.assertEqual(r.status_code, 200)
                self.assertEqual(r.json()["id"], bare)

            builtin_response = requests.get(
                f"{self.base_url}/models/builtin.{bare}",
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(builtin_response.status_code, 404)

            print(f"[OK] unique user.{bare} emits as bare id with no collision")
        finally:
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": canonical},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def _set_extra_models_dir(self, value):
        """Swap extra_models_dir via /internal/set; returns the prior value."""
        prior = (
            requests.get(
                f"http://localhost:{PORT}/internal/config", timeout=TIMEOUT_DEFAULT
            )
            .json()
            .get("extra_models_dir", "")
        )
        response = requests.post(
            f"http://localhost:{PORT}/internal/set",
            json={"extra_models_dir": value},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code, 200, f"/internal/set failed: {response.text}"
        )
        return prior

    def _write_stub_gguf(self, directory, bare_name):
        """Drop a stub GGUF in a subdir so extras discovery emits extra.<bare_name>."""
        import struct

        sub_dir = os.path.join(directory, bare_name)
        os.makedirs(sub_dir, exist_ok=True)
        with open(os.path.join(sub_dir, "model.gguf"), "wb") as f:
            f.write(b"GGUF")
            f.write(struct.pack("<I", 3))  # version
            f.write(struct.pack("<Q", 0))  # tensor_count
            f.write(struct.pack("<Q", 0))  # kv_count

    def _write_root_stub_gguf(self, directory, filename):
        """Drop a stub GGUF directly in extra_models_dir."""
        import struct

        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(directory, filename), "wb") as f:
            f.write(b"GGUF")
            f.write(struct.pack("<I", 3))  # version
            f.write(struct.pack("<Q", 0))  # tensor_count
            f.write(struct.pack("<Q", 0))  # kv_count

    def test_021g_naming_spec_three_way_collision(self):
        """Naming spec: built-in + user.* + extra.* all sharing a bare name.

        The user.* wins precedence; the other two appear under their canonical IDs.
        """
        bare = ENDPOINT_TEST_MODEL  # known built-in
        user_canonical = f"user.{bare}"
        extra_dir = tempfile.mkdtemp(prefix="lemon_extra_3way_")
        self._write_stub_gguf(extra_dir, bare)

        prior_dir = self._set_extra_models_dir(extra_dir)
        try:
            pull_response = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": user_canonical,
                    "checkpoint": USER_MODEL_MAIN_CHECKPOINT,
                    "recipe": "llamacpp",
                    "stream": False,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(pull_response.status_code, 200)

            models_response = requests.get(
                f"{self.base_url}/models?show_all=true", timeout=TIMEOUT_DEFAULT
            )
            self.assertEqual(models_response.status_code, 200)
            ids = {m["id"] for m in models_response.json()["data"]}

            self.assertIn(bare, ids, "Winner emits bare id")
            self.assertIn(f"extra.{bare}", ids, "Imported source under canonical id")
            self.assertIn(f"builtin.{bare}", ids, "Built-in under canonical id")
            self.assertNotIn(
                user_canonical,
                ids,
                "Winning user.* not also emitted under canonical id",
            )

            bare_resp = requests.get(
                f"{self.base_url}/models/{bare}", timeout=TIMEOUT_DEFAULT
            ).json()
            user_resp = requests.get(
                f"{self.base_url}/models/{user_canonical}", timeout=TIMEOUT_DEFAULT
            ).json()
            extra_resp = requests.get(
                f"{self.base_url}/models/extra.{bare}", timeout=TIMEOUT_DEFAULT
            ).json()
            builtin_resp = requests.get(
                f"{self.base_url}/models/builtin.{bare}", timeout=TIMEOUT_DEFAULT
            ).json()

            self.assertEqual(bare_resp["checkpoint"], user_resp["checkpoint"])
            self.assertNotEqual(extra_resp["checkpoint"], bare_resp["checkpoint"])
            self.assertNotEqual(builtin_resp["checkpoint"], bare_resp["checkpoint"])
            self.assertNotEqual(extra_resp["checkpoint"], builtin_resp["checkpoint"])

            print(
                f"[OK] three-way collision: bare/{bare}, extra.{bare}, builtin.{bare}"
            )
        finally:
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": user_canonical},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass
            self._set_extra_models_dir(prior_dir)
            shutil.rmtree(extra_dir, ignore_errors=True)

    def test_021h_naming_spec_extra_shadows_builtin(self):
        """Naming spec: extra.* + built-in (no user.*); extra wins precedence."""
        bare = ENDPOINT_TEST_MODEL
        extra_dir = tempfile.mkdtemp(prefix="lemon_extra_shadow_")
        self._write_stub_gguf(extra_dir, bare)

        prior_dir = self._set_extra_models_dir(extra_dir)
        try:
            models_response = requests.get(
                f"{self.base_url}/models?show_all=true", timeout=TIMEOUT_DEFAULT
            )
            self.assertEqual(models_response.status_code, 200)
            ids = {m["id"] for m in models_response.json()["data"]}

            self.assertIn(
                bare, ids, "extra wins precedence over builtin; emits bare id"
            )
            self.assertIn(f"builtin.{bare}", ids, "shadowed builtin under canonical id")
            self.assertNotIn(
                f"extra.{bare}",
                ids,
                "winning extra.* not also emitted under canonical id",
            )

            bare_resp = requests.get(
                f"{self.base_url}/models/{bare}", timeout=TIMEOUT_DEFAULT
            ).json()
            extra_resp = requests.get(
                f"{self.base_url}/models/extra.{bare}", timeout=TIMEOUT_DEFAULT
            ).json()
            builtin_resp = requests.get(
                f"{self.base_url}/models/builtin.{bare}", timeout=TIMEOUT_DEFAULT
            ).json()

            self.assertEqual(bare_resp["checkpoint"], extra_resp["checkpoint"])
            self.assertNotEqual(bare_resp["checkpoint"], builtin_resp["checkpoint"])

            print(
                f"[OK] extra shadows built-in: bare/{bare} -> extra, builtin.{bare} -> built-in"
            )
        finally:
            self._set_extra_models_dir(prior_dir)
            shutil.rmtree(extra_dir, ignore_errors=True)

    def test_021i_extra_root_gguf_emits_stem_name(self):
        """Root-level extra_models_dir GGUF files emit the filename stem."""
        bare = "Qwen3.5-4B-UD-Q4_K_XL"
        extra_dir = tempfile.mkdtemp(prefix="lemon_extra_root_")
        self._write_root_stub_gguf(extra_dir, f"{bare}.gguf")

        prior_dir = self._set_extra_models_dir(extra_dir)
        try:
            models_response = requests.get(
                f"{self.base_url}/models?show_all=true", timeout=TIMEOUT_DEFAULT
            )
            self.assertEqual(models_response.status_code, 200)
            ids = {m["id"] for m in models_response.json()["data"]}

            self.assertIn(bare, ids)
            self.assertNotIn(f"{bare}.gguf", ids)

            bare_resp = requests.get(
                f"{self.base_url}/models/{bare}", timeout=TIMEOUT_DEFAULT
            )
            self.assertEqual(bare_resp.status_code, 200)
            self.assertEqual(bare_resp.json()["id"], bare)
            self.assertEqual(
                bare_resp.json()["checkpoint"],
                os.path.join(extra_dir, f"{bare}.gguf"),
            )

            print(f"[OK] root GGUF emits stem: {bare}")
        finally:
            self._set_extra_models_dir(prior_dir)
            shutil.rmtree(extra_dir, ignore_errors=True)

    def test_021r_openai_chat_extra_models_precedence(self):
        """Regression test for #2014: OpenAI API resolves aliases to local files, shadowing built-ins."""
        # Use a built-in model name to prove precedence and alias resolution simultaneously
        bare = ENDPOINT_TEST_MODEL
        extra_dir = tempfile.mkdtemp(prefix="lemon_extra_regression_")
        self._write_root_stub_gguf(extra_dir, f"{bare}.gguf")

        prior_dir = self._set_extra_models_dir(extra_dir)
        try:
            # 500 (Failed to load) proves it resolved to our local stub instead of the real built-in.
            payload = {"model": bare, "messages": [{"role": "user", "content": "hi"}]}
            resp = requests.post(
                f"http://localhost:{PORT}/v1/chat/completions",
                json=payload,
                timeout=TIMEOUT_DEFAULT,
            )

            self.assertEqual(resp.status_code, 500)
            self.assertIn(
                "Failed to load model", resp.json().get("error", {}).get("message", "")
            )

            print(f"[OK] OpenAI API correctly resolves local shadowing for: {bare}")
        finally:
            self._set_extra_models_dir(prior_dir)
            shutil.rmtree(extra_dir, ignore_errors=True)

    def _get_test_backend(self):
        """Get a lightweight test backend based on platform."""
        import sys

        if sys.platform == "darwin":
            return "llamacpp", "metal"
        else:
            return "llamacpp", "cpu"

    def test_022_install_backend_non_streaming(self):
        """Test installing a backend via /install endpoint (non-streaming)."""
        recipe, backend = self._get_test_backend()

        # First uninstall to get clean state
        requests.post(
            f"{self.base_url}/uninstall",
            json={"recipe": recipe, "backend": backend},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Install (non-streaming)
        response = requests.post(
            f"{self.base_url}/install",
            json={"recipe": recipe, "backend": backend, "stream": False},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertEqual(data["status"], "success")
        self.assertEqual(data["recipe"], recipe)
        self.assertEqual(data["backend"], backend)
        print(f"[OK] Non-streaming install of {recipe}:{backend}")

    def test_023_install_backend_streaming(self):
        """Test installing a backend with SSE streaming progress."""
        recipe, backend = self._get_test_backend()

        # Uninstall first to force a fresh download
        requests.post(
            f"{self.base_url}/uninstall",
            json={"recipe": recipe, "backend": backend},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Install with streaming
        response = requests.post(
            f"{self.base_url}/install",
            json={"recipe": recipe, "backend": backend, "stream": True},
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200)

        # Parse SSE events
        got_progress = False
        got_complete = False
        for line in response.iter_lines(decode_unicode=True):
            if not line:
                continue
            if line.startswith("event: progress"):
                got_progress = True
            elif line.startswith("event: complete"):
                got_complete = True
            elif line.startswith("event: error"):
                self.fail(f"Received error event: {line}")

        self.assertTrue(got_complete, "Expected 'complete' SSE event")
        print(
            f"[OK] Streaming install of {recipe}:{backend} (progress events: {got_progress})"
        )

    def test_024_install_already_installed(self):
        """Test that installing an already-installed backend returns quickly."""
        recipe, backend = self._get_test_backend()

        response = requests.post(
            f"{self.base_url}/install",
            json={"recipe": recipe, "backend": backend, "stream": False},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertEqual(data["status"], "success")
        print(
            f"[OK] Re-install of already-installed {recipe}:{backend} returned quickly"
        )

    def test_025_uninstall_backend(self):
        """Test uninstalling a backend via /uninstall endpoint."""
        recipe, backend = self._get_test_backend()

        # Ensure installed first
        requests.post(
            f"{self.base_url}/install",
            json={"recipe": recipe, "backend": backend, "stream": False},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Verify via system-info
        response = requests.get(f"{self.base_url}/system-info", timeout=TIMEOUT_DEFAULT)
        info = response.json()
        self.assertTrue(
            info["recipes"][recipe]["backends"][backend].get("state", "")
            in {"installed", "update_required"},
            f"Expected {recipe}:{backend} to be installed before uninstall",
        )

        # Uninstall
        response = requests.post(
            f"{self.base_url}/uninstall",
            json={"recipe": recipe, "backend": backend},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertEqual(data["status"], "success")
        print(f"[OK] Uninstalled {recipe}:{backend}")

    def test_026_uninstall_not_installed(self):
        """Test uninstalling a backend that isn't installed."""
        recipe, backend = self._get_test_backend()

        # Uninstall twice - second time should still return 200
        requests.post(
            f"{self.base_url}/uninstall",
            json={"recipe": recipe, "backend": backend},
            timeout=TIMEOUT_DEFAULT,
        )
        response = requests.post(
            f"{self.base_url}/uninstall",
            json={"recipe": recipe, "backend": backend},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        print(f"[OK] Uninstalling non-installed {recipe}:{backend} returns 200")

    def test_027_reinstall_after_uninstall(self):
        """Test full cycle: install, verify, uninstall, verify, reinstall."""
        recipe, backend = self._get_test_backend()

        # Re-install to leave system in clean state for other tests
        response = requests.post(
            f"{self.base_url}/install",
            json={"recipe": recipe, "backend": backend, "stream": False},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        print(f"[OK] Reinstalled {recipe}:{backend} - system in clean state")

    def test_028_install_missing_params(self):
        """Test that /install returns 400 for missing parameters."""
        response = requests.post(
            f"{self.base_url}/install",
            json={"recipe": "llamacpp"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400)
        print("[OK] /install returns 400 for missing 'backend' parameter")

    def test_029_system_info_release_url(self):
        """Test that system-info includes release_url for backends."""
        response = requests.get(f"{self.base_url}/system-info", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)
        data = response.json()

        # Check that at least one backend has a release_url
        found_url = False
        if "recipes" in data:
            for recipe_name, recipe_info in data["recipes"].items():
                if "backends" in recipe_info:
                    for backend_name, backend_info in recipe_info["backends"].items():
                        if "release_url" in backend_info:
                            found_url = True
                            url = backend_info["release_url"]
                            self.assertTrue(
                                url.startswith("https://github.com/"),
                                f"Expected GitHub URL, got: {url}",
                            )
                            break
                if found_url:
                    break

        self.assertTrue(
            found_url, "Expected at least one backend with release_url in system-info"
        )
        print("[OK] system-info contains release_url for backends")

    # =========================================================================
    # PULL/VARIANTS TESTS
    # The two error-only tests (030, 031) run in every CI environment because
    # they never touch the network — the server rejects the request before any
    # HuggingFace call is made.
    #
    # The live-network tests (032, 033) are gated behind the env var
    # LEMONADE_INTEGRATION_TESTS=1 so they are opt-in and do not cause
    # failures due to HF rate limits, network policy, or HF outages in
    # standard CI runs.
    # =========================================================================

    def test_030_pull_variants_missing_checkpoint_returns_400(self):
        """GET /pull/variants without checkpoint param returns 400 with exact error message."""
        response = requests.get(
            f"{self.base_url}/pull/variants",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for missing checkpoint, got {response.status_code}: {response.text}",
        )
        data = response.json()
        self.assertIn("error", data)
        self.assertIn(
            "Missing required query parameter 'checkpoint'",
            data["error"],
            f"Unexpected error message: {data['error']}",
        )
        print("[OK] Missing checkpoint param returns 400 with descriptive error")

    def test_031_pull_variants_malformed_checkpoint_returns_400(self):
        """GET /pull/variants with checkpoint missing '/' returns 400 with exact error message."""
        response = requests.get(
            f"{self.base_url}/pull/variants",
            params={"checkpoint": "noslashrepo"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for malformed checkpoint, got {response.status_code}: {response.text}",
        )
        data = response.json()
        self.assertIn("error", data)
        self.assertIn(
            "owner/name",
            data["error"],
            f"Expected 'owner/name' format hint in error message, got: {data['error']}",
        )
        print(
            "[OK] Malformed checkpoint (no slash) returns 400 with owner/name format hint"
        )

    @unittest.skipUnless(
        os.environ.get("LEMONADE_INTEGRATION_TESTS") == "1",
        "Skipped: set LEMONADE_INTEGRATION_TESTS=1 to run live HuggingFace tests",
    )
    def test_032_pull_variants_nonexistent_checkpoint_returns_404(self):
        """GET /pull/variants for a repo that does not exist on HuggingFace returns 404.

        Requires LEMONADE_INTEGRATION_TESTS=1 — makes a live HuggingFace API call.
        """
        checkpoint = "lemonade-nonexistent-owner/lemonade-nonexistent-repo-xyz"
        response = requests.get(
            f"{self.base_url}/pull/variants",
            params={"checkpoint": checkpoint},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            404,
            f"Expected 404 for nonexistent HF repo, got {response.status_code}: {response.text}",
        )
        data = response.json()
        self.assertIn("error", data)
        self.assertIn(
            checkpoint,
            data["error"],
            f"Expected checkpoint name in 404 error message, got: {data['error']}",
        )
        self.assertIn(
            "not found on Hugging Face",
            data["error"],
            f"Unexpected 404 error message: {data['error']}",
        )
        print(
            "[OK] Nonexistent HuggingFace checkpoint returns 404 with descriptive error"
        )

    @unittest.skipUnless(
        os.environ.get("LEMONADE_INTEGRATION_TESTS") == "1",
        "Skipped: set LEMONADE_INTEGRATION_TESTS=1 to run live HuggingFace tests",
    )
    def test_033_pull_variants_valid_checkpoint_returns_variant_list(self):
        """GET /pull/variants for a known public GGUF repo returns a valid variant list.

        Requires LEMONADE_INTEGRATION_TESTS=1 — makes a live HuggingFace API call.
        Uses TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF as a stable, small public fixture.
        """
        checkpoint = "TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF"
        response = requests.get(
            f"{self.base_url}/pull/variants",
            params={"checkpoint": checkpoint},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            200,
            f"Expected 200 for valid checkpoint, got {response.status_code}: {response.text}",
        )
        data = response.json()

        # Top-level fields per documented API contract
        self.assertIn("checkpoint", data)
        self.assertIn("recipe", data)
        self.assertIn("suggested_name", data)
        self.assertIn("variants", data)

        # checkpoint must echo the input value exactly
        self.assertEqual(
            data["checkpoint"],
            checkpoint,
            f"Expected checkpoint to echo input '{checkpoint}', got '{data['checkpoint']}'",
        )

        # recipe must be a non-empty string
        self.assertIsInstance(data["recipe"], str)
        self.assertGreater(len(data["recipe"]), 0, "Expected non-empty recipe string")

        # variants must be a non-empty list
        variants = data["variants"]
        self.assertIsInstance(variants, list)
        self.assertGreater(
            len(variants), 0, "Expected at least one variant for TinyLlama GGUF repo"
        )

        # every variant must carry all documented fields including size_bytes
        for v in variants:
            self.assertIn("name", v)
            self.assertIn("primary_file", v)
            self.assertIn("files", v)
            self.assertIn("sharded", v)
            self.assertIn(
                "size_bytes",
                v,
                f"Variant '{v.get('name')}' is missing 'size_bytes' field",
            )
            self.assertIsInstance(v["files"], list)
            self.assertGreater(
                len(v["files"]), 0, f"Variant '{v.get('name')}' has empty files list"
            )
            self.assertIsInstance(v["sharded"], bool)
            self.assertIsInstance(v["size_bytes"], int)

        print(
            f"[OK] Valid checkpoint returned {len(variants)} variant(s): "
            f"{[v['name'] for v in variants]}"
        )

    def test_035_second_lemond_on_busy_port_exits_nonzero(self):
        """A second lemond on an in-use port must refuse to start and exit non-zero.

        Regression test for the duplicate-port guard: lemond preflight-probes the
        listen port and if another server already holds it, prints a clear error
        to stderr and exits non-zero instead of silently failing to bind. This
        test starts a real lemond on a fresh port, launches a second lemond on the
        same port, and asserts the second one (a) exits non-zero, (b) reports the
        port is already in use, and (c) leaves the first server healthy.
        """
        lemond_binary = _resolve_lemond_binary()
        if not lemond_binary:
            self.skipTest("lemond binary not found (build it or add it to PATH)")

        headers = {}
        api_key = os.environ.get("LEMONADE_API_KEY")
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"

        port = _pick_free_port()
        cache_dir = tempfile.mkdtemp(prefix="lemond_dupport_")
        first_log_path = os.path.join(cache_dir, "first_lemond.log")
        cmd = [lemond_binary, cache_dir, "--port", str(port)]

        first = None
        second = None
        try:
            # --- Start the first lemond and wait until it is healthy ---
            with open(first_log_path, "w", encoding="utf-8") as first_log:
                first = subprocess.Popen(
                    cmd,
                    stdout=first_log,
                    stderr=subprocess.STDOUT,
                    env=os.environ.copy(),
                )

            deadline = time.time() + 60
            first_healthy = False
            while time.time() < deadline:
                if first.poll() is not None:
                    break  # exited early; surface the log below
                if _lemond_health_ok(port, headers):
                    first_healthy = True
                    break
                time.sleep(1)

            if not first_healthy:
                with open(first_log_path, "r", encoding="utf-8", errors="replace") as f:
                    log = f.read()
                self.fail(
                    f"First lemond never became healthy on port {port}.\n"
                    f"=== lemond log ===\n{log}"
                )

            # --- Start the second lemond on the SAME port; it must refuse ---
            second = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=os.environ.copy(),
            )
            try:
                out, err = second.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                second.kill()
                out, err = second.communicate()
                self.fail(
                    "Second lemond did not exit; it should fail fast on the "
                    f"in-use port {port}."
                )

            combined = f"{out or ''}\n{err or ''}"

            self.assertNotEqual(
                second.returncode,
                0,
                "Second lemond on an in-use port must exit non-zero, "
                f"got exit code 0.\n=== output ===\n{combined}",
            )
            self.assertIn(
                "already in use",
                combined.lower(),
                "Second lemond should report the port is already in use.\n"
                f"=== output ===\n{combined}",
            )

            # --- The original server must still be healthy ---
            self.assertTrue(
                _lemond_health_ok(port, headers),
                "First lemond should remain healthy after the duplicate was "
                "rejected.",
            )

            print(
                f"[OK] Second lemond on in-use port {port} exited "
                f"{second.returncode} and first server stayed healthy"
            )
        finally:
            for proc in (second, first):
                if proc is not None and proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=10)
            shutil.rmtree(cache_dir, ignore_errors=True)

    def test_034_shared_repo_variant_resolves_after_refs_main_advances(self):
        """Regression for #2300: two models sharing one HF repo with different
        quants must both stay resolvable after refs/main advances past one of them.

        HF refs/main is a single sticky per-repo pointer (advanced only on a
        successful pull). When a sibling variant is pulled/updated, refs/main moves
        to a snapshot that contains only that variant; the other variant stays in
        the previous snapshot, so refs/main no longer covers both. On the next
        models-cache build (i.e. after a lemond restart) the GGUF resolver, which
        searches only the refs/main snapshot, then reports the variant not covered
        by refs/main as not downloaded even though its file is still cached. The fix
        broadens the resolver to fall back to all snapshots when the active one
        lacks the requested variant.

        Repro without waiting for a real upstream commit: pull both variants (they
        land in one snapshot under the current commit), then move one variant into a
        fresh snapshot and repoint refs/main at it, so the two variants live in
        different snapshots. The models cache is then rebuilt by pulling an
        unrelated model from a *different* repo (re-pulling a shared-repo model would
        query HF and repair refs/main, masking the bug).
        """
        a_name = SHARED_REPO_MODEL_A_NAME
        b_name = SHARED_REPO_MODEL_B_NAME
        repo_id, a_file = SHARED_REPO_MODEL_A_CHECKPOINT.split(":", 1)
        b_file = SHARED_REPO_MODEL_B_CHECKPOINT.split(":", 1)[1]
        repo_cache_dir = "models--" + repo_id.replace("/", "--")
        throwaway = f"user.OrphanRebuild-{uuid.uuid4().hex[:8]}"

        def _pull(model_name, checkpoint):
            r = requests.post(
                f"{self.base_url}/pull",
                json={
                    "model_name": model_name,
                    "recipe": "llamacpp",
                    "checkpoints": {"main": checkpoint},
                    "stream": False,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(r.status_code, 200, r.text)

        def _delete(model_name):
            # Best-effort cleanup; ignore status so one failure does not mask others.
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": model_name},
                    timeout=TIMEOUT_MODEL_OPERATION,
                )
            except requests.RequestException:
                pass

        def _downloaded(model_name):
            # GET /models/{id} -> get_model_info() -> build_cache(), so this re-runs
            # the on-disk resolver against the staged cache state.
            r = requests.get(
                f"{self.base_url}/models/{model_name}", timeout=TIMEOUT_DEFAULT
            )
            self.assertEqual(r.status_code, 200, r.text)
            return r.json().get("downloaded", False)

        try:
            # 1. Pull both variants of the shared repo. With a single upstream commit
            #    both files land in the same snapshots/<commit>/ directory.
            _pull(a_name, SHARED_REPO_MODEL_A_CHECKPOINT)
            _pull(b_name, SHARED_REPO_MODEL_B_CHECKPOINT)
            self.assertTrue(_downloaded(a_name), "Variant A should download")
            self.assertTrue(_downloaded(b_name), "Variant B should download")

            # Locate the server's real HF cache (handles config models_dir overrides
            # and packaged servers running under a different user/HOME).
            cache_root = self._server_hf_cache_root(repo_cache_dir)
            if cache_root is None:
                self.skipTest(
                    "Cannot locate the server's HF cache from the test process; "
                    "the shared-repo resolution path needs on-disk snapshot access."
                )

            repo_dir = os.path.join(cache_root, repo_cache_dir)
            snapshots_dir = os.path.join(repo_dir, "snapshots")
            refs_main = os.path.join(repo_dir, "refs", "main")

            with open(refs_main, encoding="utf-8") as f:
                cur_rev = f.read().strip()
            cur_snapshot = os.path.join(snapshots_dir, cur_rev)
            a_in_cur = os.path.join(cur_snapshot, a_file)
            b_in_cur = os.path.join(cur_snapshot, b_file)
            if not (os.path.exists(a_in_cur) and os.path.exists(b_in_cur)):
                self.skipTest(
                    "Shared-repo variants are not co-located in the active snapshot "
                    "(upstream layout changed); cannot stage the orphan."
                )

            # 2. Simulate an upstream commit being pulled for one variant: move B
            #    into a fresh snapshot and advance refs/main to it. This mirrors what
            #    a real pull does — only the freshly pulled file lands in the new
            #    snapshot, so the two variants no longer share one snapshot and
            #    refs/main no longer covers both of them. (The repo here stores real
            #    files in the snapshot dirs, so the file is relocated directly.)
            new_rev = "0" * 40
            new_snapshot = os.path.join(snapshots_dir, new_rev)
            os.makedirs(new_snapshot, exist_ok=True)
            os.rename(b_in_cur, os.path.join(new_snapshot, b_file))
            os.makedirs(os.path.dirname(refs_main), exist_ok=True)
            with open(refs_main, "w", encoding="utf-8") as f:
                f.write(new_rev)

            # Sanity: each variant now lives in a different snapshot, and refs/main
            # points at the one that holds only B.
            self.assertTrue(
                os.path.exists(os.path.join(new_snapshot, b_file)),
                "Setup error: B must be in the new refs/main snapshot",
            )
            self.assertFalse(
                os.path.exists(os.path.join(new_snapshot, a_file)),
                "Setup error: A must not be in the new refs/main snapshot",
            )
            self.assertTrue(
                os.path.exists(a_in_cur),
                "Setup error: A must remain in the previous snapshot",
            )

            # 3. Force a models-cache rebuild without touching the shared repo (this
            #    is what a lemond restart would do). Pulling an unrelated model from a
            #    different repo invalidates the cache so the resolver re-runs.
            _pull(throwaway, USER_MODEL_MAIN_CHECKPOINT)

            # 4. #2300: both variants are still physically cached (one in each
            #    snapshot), so both must resolve as downloaded. Before the fix the
            #    resolver searched only the refs/main snapshot, so the variant not
            #    covered by refs/main was reported missing.
            self.assertTrue(
                _downloaded(b_name),
                "#2300: variant B must remain downloaded after refs/main advances "
                "(its file is still cached, in the new snapshot)",
            )
            self.assertTrue(
                _downloaded(a_name),
                "#2300: variant A must remain downloaded after refs/main advances "
                "(its file is still cached, in the previous snapshot)",
            )
            print(
                "[OK] #2300 shared-repo variants both resolve after refs/main advance"
            )
        finally:
            _delete(a_name)
            _delete(b_name)
            _delete(throwaway)

    def test_036_lemond_restart_with_lingering_connections_succeeds(self):
        """A new lemond instance must be able to start on a port that has lingering
        client connections in FIN_WAIT / TIME_WAIT states.

        This test starts lemond, connects a client socket, shuts down the first
        lemond, and attempts to start a second lemond on the same port while the
        client socket is kept open (which creates a lingering server-side connection
        in the TCP stack). The second lemond should start successfully.
        """
        lemond_binary = _resolve_lemond_binary()
        if not lemond_binary:
            self.skipTest("lemond binary not found")

        headers = {}
        api_key = os.environ.get("LEMONADE_API_KEY")
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"

        port = _pick_free_port()
        cache_dir = tempfile.mkdtemp(prefix="lemond_lingering_")
        first_log_path = os.path.join(cache_dir, "first_lemond.log")
        second_log_path = os.path.join(cache_dir, "second_lemond.log")
        cmd = [lemond_binary, cache_dir, "--port", str(port)]

        first = None
        second = None
        client_sock = None
        try:
            # 1. Start the first lemond
            with open(first_log_path, "w", encoding="utf-8") as first_log:
                first = subprocess.Popen(
                    cmd,
                    stdout=first_log,
                    stderr=subprocess.STDOUT,
                    env=os.environ.copy(),
                )

            # Wait for it to be healthy
            deadline = time.time() + 30
            first_healthy = False
            while time.time() < deadline:
                if first.poll() is not None:
                    break
                if _lemond_health_ok(port, headers):
                    first_healthy = True
                    break
                time.sleep(1)

            self.assertTrue(first_healthy, "First lemond failed to start")

            # 2. Establish a TCP connection from a client socket and keep it open
            client_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            client_sock.connect(("127.0.0.1", port))
            client_sock.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")

            # 3. Shutdown the first lemond
            first.terminate()
            try:
                first.wait(timeout=10)
            except subprocess.TimeoutExpired:
                first.kill()
                first.wait(timeout=10)

            # 4. Attempt to start a second lemond on the SAME port while client_sock is still active.
            # Without the fix, the second lemond would fail to start with EADDRINUSE (port already in use).
            with open(second_log_path, "w", encoding="utf-8") as second_log:
                second = subprocess.Popen(
                    cmd,
                    stdout=second_log,
                    stderr=subprocess.STDOUT,
                    env=os.environ.copy(),
                )

            # Assert that the second lemond starts and becomes healthy
            deadline = time.time() + 30
            second_healthy = False
            while time.time() < deadline:
                if second.poll() is not None:
                    break
                if _lemond_health_ok(port, headers):
                    second_healthy = True
                    break
                time.sleep(1)

            if not second_healthy:
                with open(
                    second_log_path, "r", encoding="utf-8", errors="replace"
                ) as f:
                    log = f.read()
                self.fail(
                    f"Second lemond failed to start on port {port} with lingering connection.\n"
                    f"=== second lemond log ===\n{log}"
                )

            print(
                f"[OK] Second lemond started successfully on port {port} with lingering connections"
            )

        finally:
            if client_sock:
                try:
                    client_sock.close()
                except Exception:
                    pass
            for proc in (second, first):
                if proc is not None and proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=10)
            shutil.rmtree(cache_dir, ignore_errors=True)

    def test_050_telemetry_trust_incoming_trace_context_config(self):
        """The opt-in W3C trace-context flag round-trips and validates as boolean."""
        config_url = f"http://localhost:{PORT}/internal/config"
        set_url = f"http://localhost:{PORT}/internal/set"

        prior = (
            requests.get(config_url, timeout=TIMEOUT_DEFAULT)
            .json()
            .get("telemetry", {})
            .get("trust_incoming_trace_context", False)
        )
        try:
            # Enable, then confirm it reads back as True.
            resp = requests.post(
                set_url,
                json={"telemetry": {"trust_incoming_trace_context": True}},
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(
                resp.status_code, 200, f"/internal/set failed: {resp.text}"
            )
            read_back = (
                requests.get(config_url, timeout=TIMEOUT_DEFAULT)
                .json()
                .get("telemetry", {})
                .get("trust_incoming_trace_context")
            )
            self.assertTrue(read_back)

            # A non-boolean value must be rejected by config validation.
            bad = requests.post(
                set_url,
                json={"telemetry": {"trust_incoming_trace_context": "yes"}},
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(
                bad.status_code,
                400,
                f"expected 400 for non-boolean value, got {bad.status_code}: {bad.text}",
            )
        finally:
            requests.post(
                set_url,
                json={"telemetry": {"trust_incoming_trace_context": bool(prior)}},
                timeout=TIMEOUT_DEFAULT,
            )


if __name__ == "__main__":
    run_server_tests(EndpointTests, "ENDPOINT TESTS")
