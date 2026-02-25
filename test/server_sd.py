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
import io
import struct
import zlib
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


def create_minimal_png(width=8, height=8):
    """Create a minimal valid RGB PNG image as bytes, without external dependencies."""

    def make_chunk(chunk_type, data):
        c = chunk_type + data
        return (
            struct.pack(">I", len(data))
            + c
            + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
        )

    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    row = b"\x00" + b"\xff\x00\x00" * width  # filter byte + red pixels per row
    idat_data = zlib.compress(row * height)
    return (
        b"\x89PNG\r\n\x1a\n"
        + make_chunk(b"IHDR", ihdr_data)
        + make_chunk(b"IDAT", idat_data)
        + make_chunk(b"IEND", b"")
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

    # Test 4: Image generation with custom steps parameter
    def test_004_image_generation_with_steps(self):
        """Test image generation with custom steps parameter."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A blue square",
            "size": "256x256",
            "steps": 2,
            "response_format": "b64_json",
        }

        print(f"[INFO] Testing image generation with steps=2")

        response = requests.post(
            f"{self.base_url}/images/generations", json=payload, timeout=600
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
        self.assertTrue(decoded[:4] == b"\x89PNG", "Should be valid PNG")
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
            "response_format": "b64_json",
        }

        print(f"[INFO] Testing image generation with cfg_scale=5.0")

        response = requests.post(
            f"{self.base_url}/images/generations", json=payload, timeout=600
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
        self.assertTrue(decoded[:4] == b"\x89PNG", "Should be valid PNG")
        print(
            f"[OK] Image generation with cfg_scale=5.0 successful ({len(decoded)} bytes)"
        )

    # Test 6: Image generation with explicit seed parameter
    def test_006_image_generation_with_seed(self):
        """Test image generation with explicit seed parameter."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A yellow star",
            "size": "256x256",
            "steps": 2,
            "seed": 12345,
            "response_format": "b64_json",
        }

        print(f"[INFO] Testing image generation with seed=12345")

        response = requests.post(
            f"{self.base_url}/images/generations", json=payload, timeout=600
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
        self.assertTrue(decoded[:4] == b"\x89PNG", "Should be valid PNG")
        print(
            f"[OK] Image generation with seed=12345 successful ({len(decoded)} bytes)"
        )

    # Test 7: Models endpoint returns image_defaults for SD-Turbo
    def test_007_models_endpoint_returns_image_defaults(self):
        """Test that /models endpoint returns image_defaults for SD-Turbo."""
        print(f"[INFO] Testing /models endpoint for image_defaults")

        response = requests.get(f"{self.base_url}/models?show_all=true", timeout=60)

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
        self.assertEqual(
            defaults.get("cfg_scale"), 1.0, "SD-Turbo cfg_scale should be 1.0"
        )
        self.assertEqual(defaults.get("width"), 512, "SD-Turbo width should be 512")
        self.assertEqual(defaults.get("height"), 512, "SD-Turbo height should be 512")

        print(f"[OK] SD-Turbo image_defaults verified: {defaults}")

    # Test 8: Models endpoint returns correct defaults for SDXL-Base-1.0
    def test_008_models_endpoint_sdxl_defaults(self):
        """Test that /models endpoint returns correct image_defaults for SDXL-Base-1.0."""
        print(f"[INFO] Testing /models endpoint for SDXL-Base-1.0 image_defaults")

        response = requests.get(f"{self.base_url}/models?show_all=true", timeout=60)

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

        self.assertIsNotNone(
            sdxl_base, "SDXL-Base-1.0 model not found in /models response"
        )

        # Verify image_defaults exists and has correct values
        self.assertIn(
            "image_defaults", sdxl_base, "SDXL-Base-1.0 should have image_defaults"
        )
        defaults = sdxl_base["image_defaults"]

        self.assertEqual(defaults.get("steps"), 20, "SDXL-Base-1.0 steps should be 20")
        self.assertEqual(
            defaults.get("cfg_scale"), 7.5, "SDXL-Base-1.0 cfg_scale should be 7.5"
        )
        self.assertEqual(
            defaults.get("width"), 1024, "SDXL-Base-1.0 width should be 1024"
        )
        self.assertEqual(
            defaults.get("height"), 1024, "SDXL-Base-1.0 height should be 1024"
        )

        print(f"[OK] SDXL-Base-1.0 image_defaults verified: {defaults}")

    def test_009_image_edit_not_multipart_error(self):
        """Test that non-multipart requests to /images/edits return 400."""
        response = requests.post(
            f"{self.base_url}/images/edits",
            json={"model": SD_MODEL, "prompt": "test"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for non-multipart request, got {response.status_code}",
        )
        print(
            f"[OK] Correctly rejected non-multipart edit request: {response.status_code}"
        )

    def test_010_image_edit_missing_image_error(self):
        """Test that /images/edits returns 400 when image file is missing."""
        response = requests.post(
            f"{self.base_url}/images/edits",
            data={"model": SD_MODEL, "prompt": "test"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for missing image, got {response.status_code}",
        )
        print(
            f"[OK] Correctly rejected edit request without image: {response.status_code}"
        )

    def test_011_image_edit_missing_prompt_error(self):
        """Test that /images/edits returns 400 when prompt is missing."""
        png_bytes = create_minimal_png()
        response = requests.post(
            f"{self.base_url}/images/edits",
            files={"image": ("test.png", io.BytesIO(png_bytes), "image/png")},
            data={"model": SD_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for missing prompt, got {response.status_code}",
        )
        print(
            f"[OK] Correctly rejected edit request without prompt: {response.status_code}"
        )

    def test_012_image_edit_invalid_n_error(self):
        """Test that /images/edits returns 400 when n is out of valid range."""
        png_bytes = create_minimal_png()
        response = requests.post(
            f"{self.base_url}/images/edits",
            files={"image": ("test.png", io.BytesIO(png_bytes), "image/png")},
            data={"model": SD_MODEL, "prompt": "test", "n": "20"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for n=20, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected edit request with n=20: {response.status_code}")

    def test_013_image_variations_not_multipart_error(self):
        """Test that non-multipart requests to /images/variations return 400."""
        response = requests.post(
            f"{self.base_url}/images/variations",
            json={"model": SD_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for non-multipart request, got {response.status_code}",
        )
        print(
            f"[OK] Correctly rejected non-multipart variations request: {response.status_code}"
        )

    def test_014_image_variations_missing_image_error(self):
        """Test that /images/variations returns 400 when image file is missing."""
        response = requests.post(
            f"{self.base_url}/images/variations",
            data={"model": SD_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for missing image, got {response.status_code}",
        )
        print(
            f"[OK] Correctly rejected variations request without image: {response.status_code}"
        )

    def test_015_image_edit_basic(self):
        """Test basic image edit returns a valid PNG."""
        png_bytes = create_minimal_png(256, 256)
        print(f"[INFO] Sending image edit request with model {SD_MODEL}")

        response = requests.post(
            f"{self.base_url}/images/edits",
            files={"image": ("test.png", io.BytesIO(png_bytes), "image/png")},
            data={
                "model": SD_MODEL,
                "prompt": "A red circle",
                "size": "256x256",
                "n": "1",
                "response_format": "b64_json",
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image edit failed with status {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result, "Response should contain 'data' field")
        self.assertGreater(
            len(result["data"]), 0, "Data should have at least one image"
        )
        b64_data = result["data"][0]["b64_json"]
        decoded = base64.b64decode(b64_data)
        self.assertTrue(decoded[:4] == b"\x89PNG", "Result should be a valid PNG")
        print(f"[OK] Image edit successful ({len(decoded)} bytes)")

    def test_016_image_variations_basic(self):
        """Test basic image variations returns a valid PNG."""
        png_bytes = create_minimal_png(256, 256)
        print(f"[INFO] Sending image variations request with model {SD_MODEL}")

        response = requests.post(
            f"{self.base_url}/images/variations",
            files={"image": ("test.png", io.BytesIO(png_bytes), "image/png")},
            data={
                "model": SD_MODEL,
                "size": "256x256",
                "n": "1",
                "response_format": "b64_json",
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image variations failed with status {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result, "Response should contain 'data' field")
        self.assertGreater(
            len(result["data"]), 0, "Data should have at least one image"
        )
        b64_data = result["data"][0]["b64_json"]
        decoded = base64.b64decode(b64_data)
        self.assertTrue(decoded[:4] == b"\x89PNG", "Result should be a valid PNG")
        print(f"[OK] Image variations successful ({len(decoded)} bytes)")


if __name__ == "__main__":
    run_server_tests(
        StableDiffusionTests,
        "STABLE DIFFUSION TESTS",
        wrapped_server="sd-cpp",
    )
