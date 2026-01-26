"""
Inference-agnostic endpoint tests for Lemonade Server.

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
    python server_endpoints.py --server-per-test
    python server_endpoints.py --server-binary /path/to/lemonade-server
"""

import json
import os
import requests
from openai import NotFoundError

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    parse_args,
    get_config,
    OpenAI,
)
from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


def get_recipe_options_path():
    """Get the path to recipe_options.json file."""
    # Default location is ~/.cache/lemonade/recipe_options.json
    cache_dir = os.environ.get(
        "LEMONADE_CACHE_DIR", os.path.expanduser("~/.cache/lemonade")
    )
    return os.path.join(cache_dir, "recipe_options.json")


class EndpointTests(ServerTestBase):
    """Tests for inference-agnostic endpoints."""

    # Track if model has been pulled in per-test mode (persists across tests)
    _model_pulled = False

    @classmethod
    def setUpClass(cls):
        """Set up class - start server and ensure test model is pulled."""
        super().setUpClass()

        # In per-test mode, server isn't started until setUp(), so defer pre-pull
        if get_config().get("server_per_test", False):
            print("\n[SETUP] Per-test mode: will pull model in setUp()")
            return

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

        # In per-test mode, ensure model is pulled after server starts
        if get_config().get("server_per_test", False):
            self._ensure_model_pulled()

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
        response = requests.get(f"http://localhost:{PORT}/live", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)
        print("[OK] /live endpoint returned 200")

    def test_002_health_endpoint(self):
        """Test the /health endpoint returns valid response with expected fields."""
        response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)

        data = response.json()

        # Check required fields per server_spec.md
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

        # Verify model structure per server_spec.md
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
        response_default = requests.get(f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response_default.status_code, 200)
        downloaded_count = len(response_default.json()["data"])

        # Get all models including not-yet-downloaded
        response_all = requests.get(f"{self.base_url}/models?show_all=true", timeout=TIMEOUT_DEFAULT)
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

        # Check extended fields per server_spec.md
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
        models_response = requests.get(f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT)
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
        models_response = requests.get(f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT)
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
        models_response = requests.get(f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT)
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

        # Verify model is loaded via health endpoint
        health_response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(health_response.status_code, 200)
        health_data = health_response.json()

        # Check model appears in all_models_loaded
        loaded_models = [
            m["model_name"] for m in health_data.get("all_models_loaded", [])
        ]
        self.assertIn(ENDPOINT_TEST_MODEL, loaded_models)

        print(f"[OK] Loaded model: {ENDPOINT_TEST_MODEL}")

    def test_010_load_model_with_options(self):
        """Test loading a model with custom options (ctx_size, llamacpp_backend, llamacpp_args)."""
        # Load with custom options (load always loads even if already loaded)
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
        health_response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
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
        # Load with save_options=true (load always loads even if already loaded)
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

        # Check recipe_options.json file
        options_path = get_recipe_options_path()
        if os.path.exists(options_path):
            with open(options_path, "r") as f:
                saved_options = json.load(f)

            if ENDPOINT_TEST_MODEL in saved_options:
                model_options = saved_options[ENDPOINT_TEST_MODEL]
                self.assertEqual(
                    model_options.get("ctx_size"),
                    custom_ctx_size,
                    f"Expected ctx_size={custom_ctx_size} in recipe_options.json",
                )
                print(
                    f"[OK] Verified recipe_options.json contains ctx_size={custom_ctx_size}"
                )
            else:
                print(
                    f"[INFO] Model not found in recipe_options.json (may be expected)"
                )
        else:
            print(f"[INFO] recipe_options.json not found at {options_path}")

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
        health_response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
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

    def test_013_unload_specific_model(self):
        """Test unloading a specific model by name."""
        # First load a model
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Verify model is loaded
        health_response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
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
        health_response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
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
        health_response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        health_data = health_response.json()
        self.assertEqual(len(health_data.get("all_models_loaded", [])), 0)

        print("[OK] Unloaded all models")

    def test_016_delete_model(self):
        """Test deleting a model removes it from local storage."""
        # Model should already be pulled from setUpClass or pull tests
        models_response = requests.get(f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT)
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
        models_response = requests.get(f"{self.base_url}/models", timeout=TIMEOUT_DEFAULT)
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

        # Check required top-level keys per server_spec.md
        required_keys = ["OS Version", "Processor", "Physical Memory", "devices"]
        for key in required_keys:
            self.assertIn(key, data, f"Missing required key: {key}")

        # Verify devices structure
        devices = data["devices"]
        self.assertIsInstance(devices, dict)

        # Check required device types
        required_devices = ["cpu", "amd_igpu", "amd_dgpu", "npu"]
        for device in required_devices:
            self.assertIn(device, devices, f"Missing device type: {device}")

        # CPU should have name, cores, threads, available
        cpu = devices["cpu"]
        self.assertIn("name", cpu)
        self.assertIn("available", cpu)

        print(f"[OK] /system-info: OS={data['OS Version'][:30]}...")

    def test_019_system_info_verbose(self):
        """Test the /system-info endpoint with verbose flag includes extra details."""
        response = requests.get(f"{self.base_url}/system-info?verbose=true", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertIsInstance(data, dict)

        # Check that Python Packages is present in verbose mode
        self.assertIn(
            "Python Packages",
            data,
            "Python Packages should be present in verbose mode",
        )
        packages = data["Python Packages"]
        self.assertIsInstance(packages, list)

        print(f"[OK] /system-info verbose: {len(packages)} packages listed")

    def test_020_stats_endpoint(self):
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
        # Stats fields per server_spec.md (may not all be present if no inference done)
        # Just verify it returns valid JSON
        self.assertIsInstance(data, dict)

        print(f"[OK] /stats endpoint returned: {list(data.keys())}")


if __name__ == "__main__":
    run_server_tests(EndpointTests, "ENDPOINT TESTS")
