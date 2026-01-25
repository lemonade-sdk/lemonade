"""
Usage: python server_sd.py

This will launch the lemonade server, test image generation with Stable Diffusion,
and make sure that the response is valid.

Examples:
    python server_sd.py
    python server_sd.py --server-binary ./lemonade-server

Note: Image generation with CPU backend takes ~2-3 minutes per image at 256x256 with 1 step.
The Vulkan backend is faster but may have compatibility issues with some GPUs.
"""

import base64

import requests

# Import all shared functionality from utils/server_base.py
from utils.server_base import (
    ServerTestingBase,
    run_server_tests_with_class,
    PORT,
)

SD_MODEL = "SD-Turbo"


class SDServerTesting(ServerTestingBase):
    """Testing class for Stable Diffusion image generation."""

    def setUp(self):
        """Call parent setUp with SD-specific messaging."""
        print(f"\n=== Starting new SD test ===")
        super().setUp()

    # Test 1: Basic image generation (optimized for CI - minimal size and steps)
    def test_001_basic_image_generation(self):
        """Test basic image generation with SD-Turbo."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A red circle",
            "size": "256x256",  # Smallest practical size for speed
            "steps": 2,  # SD-Turbo works well with few steps
            "n": 1,
            "response_format": "b64_json"
        }

        print(f"[INFO] Sending image generation request with model {SD_MODEL}")
        print(f"[INFO] Using minimal settings (256x256, 4 steps) for CI speed")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=600  # 10 minute timeout for CPU inference
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation failed with status {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result, "Response should contain 'data' field")
        self.assertIsInstance(result["data"], list, "Data should be a list")
        self.assertEqual(len(result["data"]), 1, "Should have 1 image")
        self.assertIn("b64_json", result["data"][0], "Should contain base64 image")

        # Verify base64 is valid
        b64_data = result["data"][0]["b64_json"]
        self.assertIsInstance(b64_data, str, "Base64 data should be a string")
        self.assertGreater(len(b64_data), 1000, "Base64 data should be substantial")

        # Try to decode to verify it's valid base64
        try:
            decoded = base64.b64decode(b64_data)
            # PNG files start with specific magic bytes
            self.assertTrue(
                decoded[:4] == b'\x89PNG',
                "Decoded data should be a valid PNG"
            )
            print(f"[OK] Generated valid PNG image ({len(decoded)} bytes)")
        except Exception as e:
            self.fail(f"Failed to decode base64 image: {e}")

        self.assertIn("created", result, "Response should contain 'created' timestamp")
        print(f"[OK] Image generation successful")

    # Test 2: Error handling - missing prompt (fast, no image generation)
    def test_002_missing_prompt(self):
        """Test error handling when prompt is missing."""
        payload = {
            "model": SD_MODEL,
            "size": "256x256"
            # No prompt
        }

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=60
        )

        # Should return an error
        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing prompt, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without prompt: {response.status_code}")

    # Test 3: Error handling - invalid model (fast, no image generation)
    def test_003_invalid_model(self):
        """Test error handling with invalid model."""
        payload = {
            "model": "nonexistent-sd-model",
            "prompt": "A cat",
            "size": "256x256"
        }

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=60
        )

        # Should return an error (model not found)
        # Note: Server returns 500 for model not found, ideally should be 404
        self.assertIn(
            response.status_code,
            [400, 404, 422, 500],
            f"Expected error for invalid model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")

    # Test 4: Image generation with custom steps parameter
    def test_004_image_generation_with_steps(self):
        """Test image generation with custom steps parameter."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A blue square",
            "size": "256x256",
            "steps": 2,
            "response_format": "b64_json"
        }

        print(f"[INFO] Testing image generation with steps=2")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=600
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation with custom steps failed: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])

        # Verify valid PNG
        b64_data = result["data"][0]["b64_json"]
        decoded = base64.b64decode(b64_data)
        self.assertTrue(decoded[:4] == b'\x89PNG', "Should be valid PNG")
        print(f"[OK] Image generation with steps=2 successful ({len(decoded)} bytes)")

    # Test 5: Image generation with custom cfg_scale parameter
    def test_005_image_generation_with_cfg_scale(self):
        """Test image generation with custom cfg_scale parameter."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A green triangle",
            "size": "256x256",
            "steps": 2,
            "cfg_scale": 5.0,
            "response_format": "b64_json"
        }

        print(f"[INFO] Testing image generation with cfg_scale=5.0")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=600
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation with custom cfg_scale failed: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])

        # Verify valid PNG
        b64_data = result["data"][0]["b64_json"]
        decoded = base64.b64decode(b64_data)
        self.assertTrue(decoded[:4] == b'\x89PNG', "Should be valid PNG")
        print(f"[OK] Image generation with cfg_scale=5.0 successful ({len(decoded)} bytes)")

    # Test 6: Image generation with explicit seed parameter
    def test_006_image_generation_with_seed(self):
        """Test image generation with explicit seed parameter."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A yellow star",
            "size": "256x256",
            "steps": 2,
            "seed": 12345,
            "response_format": "b64_json"
        }

        print(f"[INFO] Testing image generation with seed=12345")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=600
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation with seed failed: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])

        # Verify valid PNG
        b64_data = result["data"][0]["b64_json"]
        decoded = base64.b64decode(b64_data)
        self.assertTrue(decoded[:4] == b'\x89PNG', "Should be valid PNG")
        print(f"[OK] Image generation with seed=12345 successful ({len(decoded)} bytes)")

    # Test 7: Models endpoint returns image_defaults for SD-Turbo
    def test_007_models_endpoint_returns_image_defaults(self):
        """Test that /models endpoint returns image_defaults for SD-Turbo."""
        print(f"[INFO] Testing /models endpoint for image_defaults")

        response = requests.get(
            f"{self.base_url}/models?show_all=true",
            timeout=60
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Failed to get models: {response.text}",
        )

        result = response.json()
        models = result.get("data", result) if isinstance(result, dict) else result

        # Find SD-Turbo in the models list
        sd_turbo = None
        for model in models:
            if model.get("id") == SD_MODEL:
                sd_turbo = model
                break

        self.assertIsNotNone(sd_turbo, f"SD-Turbo model not found in /models response")

        # Verify image_defaults exists and has correct values
        self.assertIn("image_defaults", sd_turbo, "SD-Turbo should have image_defaults")
        defaults = sd_turbo["image_defaults"]

        self.assertEqual(defaults.get("steps"), 4, "SD-Turbo steps should be 4")
        self.assertEqual(defaults.get("cfg_scale"), 1.0, "SD-Turbo cfg_scale should be 1.0")
        self.assertEqual(defaults.get("width"), 512, "SD-Turbo width should be 512")
        self.assertEqual(defaults.get("height"), 512, "SD-Turbo height should be 512")

        print(f"[OK] SD-Turbo image_defaults verified: {defaults}")

    # Test 8: Models endpoint returns correct defaults for SDXL-Base-1.0
    def test_008_models_endpoint_sdxl_defaults(self):
        """Test that /models endpoint returns correct image_defaults for SDXL-Base-1.0."""
        print(f"[INFO] Testing /models endpoint for SDXL-Base-1.0 image_defaults")

        response = requests.get(
            f"{self.base_url}/models?show_all=true",
            timeout=60
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Failed to get models: {response.text}",
        )

        result = response.json()
        models = result.get("data", result) if isinstance(result, dict) else result

        # Find SDXL-Base-1.0 in the models list
        sdxl_base = None
        for model in models:
            if model.get("id") == "SDXL-Base-1.0":
                sdxl_base = model
                break

        self.assertIsNotNone(sdxl_base, "SDXL-Base-1.0 model not found in /models response")

        # Verify image_defaults exists and has correct values
        self.assertIn("image_defaults", sdxl_base, "SDXL-Base-1.0 should have image_defaults")
        defaults = sdxl_base["image_defaults"]

        self.assertEqual(defaults.get("steps"), 20, "SDXL-Base-1.0 steps should be 20")
        self.assertEqual(defaults.get("cfg_scale"), 7.5, "SDXL-Base-1.0 cfg_scale should be 7.5")
        self.assertEqual(defaults.get("width"), 1024, "SDXL-Base-1.0 width should be 1024")
        self.assertEqual(defaults.get("height"), 1024, "SDXL-Base-1.0 height should be 1024")

        print(f"[OK] SDXL-Base-1.0 image_defaults verified: {defaults}")


if __name__ == "__main__":
    run_server_tests_with_class(SDServerTesting, "STABLE DIFFUSION SERVER TESTS")
