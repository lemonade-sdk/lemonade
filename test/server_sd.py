"""
Usage: python server_sd.py

This will launch the lemonade server, test image generation with Stable Diffusion,
and make sure that the response is valid.

Examples:
    python server_sd.py
    python server_sd.py --server-binary ./lemonade-server

Note: Image generation with CPU backend takes ~4-5 minutes per image.
The Vulkan backend may have compatibility issues with some AMD GPUs.
"""

import os
import base64
import tempfile

import requests

# Import all shared functionality from utils/server_base.py
from utils.server_base import (
    ServerTestingBase,
    run_server_tests_with_class,
    PORT,
)

SD_MODEL = "user.SD-Turbo"


class SDServerTesting(ServerTestingBase):
    """Testing class for Stable Diffusion image generation."""

    def setUp(self):
        """Call parent setUp with SD-specific messaging."""
        print(f"\n=== Starting new SD test ===")
        super().setUp()

    # Test 1: Basic image generation
    def test_001_basic_image_generation(self):
        """Test basic image generation with SD-Turbo."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A simple red circle on white background",
            "size": "512x512",
            "steps": 4,  # SD-Turbo works with few steps
            "n": 1,
            "response_format": "b64_json"
        }

        print(f"[INFO] Sending image generation request with model {SD_MODEL}")
        print(f"[INFO] This may take several minutes with CPU backend...")

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

    # Test 2: Test with different size
    def test_002_image_generation_custom_size(self):
        """Test image generation with custom size."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A blue square",
            "size": "256x256",  # Smaller for faster test
            "steps": 4,
            "response_format": "b64_json"
        }

        print(f"[INFO] Testing image generation with size 256x256")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=600
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation failed: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])
        print(f"[OK] Custom size image generation successful")

    # Test 3: Error handling - missing prompt
    def test_003_missing_prompt(self):
        """Test error handling when prompt is missing."""
        payload = {
            "model": SD_MODEL,
            "size": "512x512"
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

    # Test 4: Error handling - invalid model
    def test_004_invalid_model(self):
        """Test error handling with invalid model."""
        payload = {
            "model": "nonexistent-model",
            "prompt": "A cat",
            "size": "512x512"
        }

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=60
        )

        # Should return an error (model not found)
        self.assertIn(
            response.status_code,
            [400, 404, 422],
            f"Expected error for invalid model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")

    # Test 5: Test with seed for reproducibility
    def test_005_image_generation_with_seed(self):
        """Test image generation with explicit seed."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A green triangle",
            "size": "256x256",
            "steps": 4,
            "seed": 42,
            "response_format": "b64_json"
        }

        print(f"[INFO] Testing image generation with seed=42")

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
        print(f"[OK] Seeded image generation successful")


if __name__ == "__main__":
    run_server_tests_with_class(SDServerTesting, "STABLE DIFFUSION SERVER TESTS")
