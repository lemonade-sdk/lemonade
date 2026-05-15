"""
Inference-agnostic endpoint tests for Lemonade Server.

Requires a Lemonade server to already be running on port 13305.
This test module does not start the server, and its inherited
`--server-binary` argument is not used here.

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

import os
import platform
import shutil
import tempfile
import uuid
import requests
from openai import NotFoundError

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    OpenAI,
)
from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    SHARED_REPO_MODEL_B_CHECKPOINT,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
    USER_MODEL_MAIN_CHECKPOINT,
    USER_MODEL_NAME,
    USER_MODEL_TE_CHECKPOINT,
    USER_MODEL_VAE_CHECKPOINT,
)


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
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/pull",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        if response.status_code == 200:
            print(f"[SETUP] {ENDPOINT_TEST_MODEL} is ready")
            cls._model_pulled = True
        else:
            print(f"[SETUP] Warning: pull returned {response.status_code}")

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

    def test_000_endpoints_registered(self):
        """Verify all expected endpoints are registered on both v0 and v1."""
        valid_endpoints = [
            "chat/completions",
            "completions",
            "embeddings",
            "models",
            "responses",
            "pull",
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

        print(
            f"[OK] /health endpoint response: status={data['status']}, models_loaded={len(data['all_models_loaded'])}"
        )

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

    def test_021_pull_multi(self):
        # First delete model if it exists to ensure we're actually testing pull
        delete_response = requests.post(
            f"{self.base_url}/delete",
            json={"model_name": USER_MODEL_NAME},
            timeout=TIMEOUT_DEFAULT,
        )
        # 200 = deleted, 422 = not found (both are acceptable)
        self.assertIn(delete_response.status_code, [200, 422])

        recipe = "sd-cpp"
        ## sd-cpp currently unavailable on MacOS
        if platform.system() == "Darwin":
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
                        # later reruns of test_021_pull_multi in server-per-test CI.
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


if __name__ == "__main__":
    run_server_tests(EndpointTests, "ENDPOINT TESTS")
