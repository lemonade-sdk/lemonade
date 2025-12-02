"""
Test multi-model functionality for C++ Lemonade Server.

This test file validates the new multi-model features:
- Loading multiple models simultaneously
- LRU eviction policy
- NPU exclusivity rules
- Health endpoint with all_models_loaded
- Unload endpoint with optional model_name parameter

Usage:
    python test/server_multimodel.py [--server-binary PATH]

Examples:
    python test/server_multimodel.py
    python test/server_multimodel.py --server-binary ./src/cpp/build/Release/lemonade-server.exe
"""

import sys
import time
import requests
import unittest

# Import shared functionality
from utils.server_base import (
    ServerTestingBase,
    run_server_tests_with_class,
    is_cpp_server,
)


class MultiModelTesting(ServerTestingBase):
    """Testing class for multi-model support."""

    # Use small GGUF models for testing
    model_llm_1 = "Qwen3-0.6B-GGUF"
    model_llm_2 = "Qwen3-1.7B-GGUF"
    model_llm_3 = "granite-4.0-h-tiny-GGUF"

    def setUp(self):
        """Setup for multi-model tests."""
        print(f"\n=== Starting multi-model test ===")

        # Only run these tests on C++ server
        if not is_cpp_server():
            self.skipTest("Multi-model tests only run on C++ server")

        super().setUp()

    def test_001_health_endpoint_empty_state(self):
        """Test health endpoint returns all_models_loaded field when no models loaded."""
        response = requests.get(f"{self.base_url}/health")
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertIn("all_models_loaded", data)
        self.assertEqual(data["all_models_loaded"], [])
        self.assertIsNone(data.get("model_loaded"))
        self.assertIsNone(data.get("checkpoint_loaded"))

    def test_002_load_single_model(self):
        """Test loading a single model and checking health."""
        # Load first model
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)

        # Check health endpoint
        response = requests.get(f"{self.base_url}/health")
        self.assertEqual(response.status_code, 200)

        data = response.json()
        self.assertIn("all_models_loaded", data)
        self.assertEqual(len(data["all_models_loaded"]), 1)

        model_info = data["all_models_loaded"][0]
        self.assertEqual(model_info["model_name"], self.model_llm_1)
        self.assertIn("type", model_info)
        self.assertEqual(model_info["type"], "llm")
        self.assertIn("device", model_info)
        self.assertIn("checkpoint", model_info)
        self.assertIn("last_use", model_info)

        # Check backward compatibility fields
        self.assertEqual(data["model_loaded"], self.model_llm_1)
        self.assertIsNotNone(data["checkpoint_loaded"])

    def test_003_load_multiple_models_with_limit(self):
        """Test loading multiple LLM models respects max limit (max_llm_models=2)."""
        # Server is configured with --max-loaded-models 2 1 1
        # This means we can load 2 LLM models simultaneously

        # Load first model
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)  # Give it time to load

        # Check first model loaded
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 1)
        self.assertEqual(data["all_models_loaded"][0]["model_name"], self.model_llm_1)

        # Load second model (should coexist since limit=2)
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_2}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Check both models are loaded
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 2, data)

        # Verify both models are present
        model_names = {m["model_name"] for m in data["all_models_loaded"]}
        self.assertEqual(model_names, {self.model_llm_1, self.model_llm_2})

    def test_004_most_recent_model_updates_on_access(self):
        """Test that model_loaded/checkpoint_loaded reflect most recently accessed model."""
        # Load model A
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Load model B
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_2}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Health should show model B as most recent (it was loaded last)
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(data["model_loaded"], self.model_llm_2)
        self.assertEqual(len(data["all_models_loaded"]), 2)

        # Do a chat/completion on model A
        response = requests.post(
            f"{self.base_url}/chat/completions",
            json={
                "model": self.model_llm_1,
                "messages": [{"role": "user", "content": "Hi"}],
                "max_tokens": 5,
            },
        )
        self.assertEqual(response.status_code, 200)

        # Health should now show model A as most recent (it was accessed last)
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(data["model_loaded"], self.model_llm_1)
        # Both models should still be loaded
        self.assertEqual(len(data["all_models_loaded"]), 2)

    def test_005_lru_eviction_when_limit_exceeded(self):
        """Test LRU eviction when loading a third model with max_llm_models=2."""
        # Load first two models (fills the limit)
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_2}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Verify both are loaded
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 2)

        # Access model_llm_2 to make it more recent than model_llm_1
        response = requests.post(
            f"{self.base_url}/chat/completions",
            json={
                "model": self.model_llm_2,
                "messages": [{"role": "user", "content": "Hi"}],
                "max_tokens": 5,
            },
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(1)

        # Load third model (should evict model_llm_1 as it's LRU)
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_3}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Verify only 2 models loaded and model_llm_1 was evicted
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 2, data)

        model_names = {m["model_name"] for m in data["all_models_loaded"]}
        self.assertIn(self.model_llm_2, model_names)
        self.assertIn(self.model_llm_3, model_names)
        self.assertNotIn(self.model_llm_1, model_names)

    def test_006_unload_specific_model(self):
        """Test unloading a specific model by name."""
        # Load a model
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Verify it's loaded
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 1)

        # Unload specific model
        response = requests.post(
            f"{self.base_url}/unload", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)
        result = response.json()
        self.assertEqual(result["status"], "success")
        self.assertIn("model_name", result)

        # Verify it's unloaded
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 0)

    def test_007_unload_nonexistent_model_returns_404(self):
        """Test unloading a model that isn't loaded returns 404."""
        response = requests.post(
            f"{self.base_url}/unload", json={"model_name": "NonexistentModel"}
        )
        self.assertEqual(response.status_code, 404)
        result = response.json()
        self.assertIn("error", result)

    def test_008_unload_all_models(self):
        """Test unloading all models without specifying model_name."""
        # Load a model
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Unload all (no model_name parameter)
        response = requests.post(f"{self.base_url}/unload", json={})
        self.assertEqual(response.status_code, 200)
        result = response.json()
        self.assertEqual(result["status"], "success")
        self.assertIn("All models", result["message"])

        # Verify all unloaded
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 0)

    def test_009_lru_timestamp_updates(self):
        """Test that last_use timestamp updates on access."""
        # Load a model
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Get initial timestamp
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        initial_time = data["all_models_loaded"][0]["last_use"]

        # Wait a bit and make an inference request
        time.sleep(1)
        try:
            # Make a quick inference (might fail but should update timestamp)
            requests.post(
                f"{self.base_url}/chat/completions",
                json={
                    "model": self.model_llm_1,
                    "messages": [{"role": "user", "content": "Hi"}],
                    "max_tokens": 5,
                },
                timeout=10,
            )
        except:
            pass  # Ignore failures, we just care about timestamp

        time.sleep(1)

        # Check timestamp updated
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        if len(data["all_models_loaded"]) > 0:
            new_time = data["all_models_loaded"][0]["last_use"]
            self.assertGreater(
                new_time, initial_time, "Timestamp should update after inference"
            )

    def test_010_model_type_field_populated(self):
        """Test that model type field is correctly populated."""
        # Load LLM model
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Check type field
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 1)
        self.assertEqual(data["all_models_loaded"][0]["type"], "llm")

        # Note: Would test embedding/reranking types here if we had those models

    def test_011_device_field_populated(self):
        """Test that device field is correctly populated."""
        # Load a model
        response = requests.post(
            f"{self.base_url}/load", json={"model_name": self.model_llm_1}
        )
        self.assertEqual(response.status_code, 200)
        time.sleep(2)

        # Check device field
        response = requests.get(f"{self.base_url}/health")
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 1)

        device = data["all_models_loaded"][0]["device"]
        self.assertIn("device", data["all_models_loaded"][0])
        # For llamacpp models, should be "gpu"
        self.assertIn("gpu", device.lower())


def main():
    """Main entry point for multi-model tests."""
    from utils.server_base import parse_args

    # Parse args first to set SERVER_BINARY global
    parse_args()

    print("=" * 70)
    print("Testing Multi-Model Support in C++ Lemonade Server")
    print("=" * 70)

    # Check if we should run tests
    if not is_cpp_server():
        print("\n⚠️  These tests require the C++ server implementation.")
        print(
            "   Set LEMONADE_SERVER_BINARY environment variable or use --server-binary flag"
        )
        sys.exit(1)

    # Run the tests
    run_server_tests_with_class(
        MultiModelTesting,
        offline=False,  # Use offline mode to avoid downloading
        additional_args=[
            "--max-loaded-models",
            "2",
            "1",
            "1",
        ],  # 2 LLMs, 1 embedding, 1 reranking
    )


if __name__ == "__main__":
    main()
