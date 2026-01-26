"""
Stable Diffusion image generation tests for Lemonade Server.

Tests the /images/generations endpoint with Stable Diffusion models.

Usage:
    python server_sd.py
    python server_sd.py --server-per-test
    python server_sd.py --server-binary /path/to/lemonade-server

Note: Image generation with CPU backend takes ~2-3 minutes per image at 256x256.
The Vulkan backend is faster but may have compatibility issues with some GPUs.
"""

import base64
import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
)
from utils.test_models import (
    SD_MODEL,
    PORT,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


class StableDiffusionTests(ServerTestBase):
    """Tests for Stable Diffusion image generation."""

    def test_001_basic_image_generation(self):
        """Test basic image generation with SD-Turbo."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A red circle",
            "size": "256x256",  # Smallest practical size for speed
            "steps": 2,  # SD-Turbo works well with few steps
            "n": 1,
            "response_format": "b64_json",
        }

        print(f"[INFO] Sending image generation request with model {SD_MODEL}")
        print(f"[INFO] Using minimal settings (256x256, 2 steps) for CI speed")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
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
                decoded[:4] == b"\x89PNG",
                "Decoded data should be a valid PNG",
            )
            print(f"[OK] Generated valid PNG image ({len(decoded)} bytes)")
        except Exception as e:
            self.fail(f"Failed to decode base64 image: {e}")

        self.assertIn("created", result, "Response should contain 'created' timestamp")
        print(f"[OK] Image generation successful")

    def test_002_missing_prompt_error(self):
        """Test error handling when prompt is missing."""
        payload = {
            "model": SD_MODEL,
            "size": "256x256",
            # No prompt
        }

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        # Should return an error
        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing prompt, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without prompt: {response.status_code}")

    def test_003_invalid_model_error(self):
        """Test error handling with invalid model."""
        payload = {
            "model": "nonexistent-sd-model-xyz-123",
            "prompt": "A cat",
            "size": "256x256",
        }

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        # Should return an error (model not found)
        # Note: Server may return 500 for model not found, ideally should be 404
        self.assertIn(
            response.status_code,
            [400, 404, 422, 500],
            f"Expected error for invalid model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")


if __name__ == "__main__":
    run_server_tests(StableDiffusionTests, "STABLE DIFFUSION TESTS")
