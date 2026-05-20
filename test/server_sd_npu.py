"""
SD NPU image generation tests for Lemonade Server.

Tests the /images/generations, /images/edits, and /images/variations endpoints
with SD NPU models (ONNX-based Stable Diffusion on AMD Ryzen AI NPU).

Usage:
    python server_sd_npu.py
    python server_sd_npu.py --server-binary /path/to/lemond
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
    SD_NPU_MODEL,
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


class SDNPUTests(ServerTestBase):
    """Tests for SD NPU image generation."""

    def test_001_basic_image_generation(self):
        """Test basic image generation with SD NPU model."""
        payload = {
            "model": SD_NPU_MODEL,
            "prompt": "A red circle",
            "size": "512x512",
            "n": 1,
            "response_format": "b64_json",
        }

        print(f"[INFO] Sending image generation request with model {SD_NPU_MODEL}")

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
            self.assertTrue(
                decoded[:4] == b"\x89PNG",
                "Decoded data should be a valid PNG",
            )
            print(f"[OK] Generated valid PNG image ({len(decoded)} bytes)")
        except Exception as e:
            self.fail(f"Failed to decode base64 image: {e}")

        self.assertIn("created", result, "Response should contain 'created' timestamp")
        print("[OK] Image generation successful")

    def test_002_missing_prompt_error(self):
        """Test error handling when prompt is missing."""
        payload = {
            "model": SD_NPU_MODEL,
            "size": "512x512",
            # No prompt
        }

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing prompt, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without prompt: {response.status_code}")

    def test_003_invalid_model_error(self):
        """Test error handling with invalid model."""
        payload = {
            "model": "nonexistent-sd-npu-model-xyz-123",
            "prompt": "A cat",
            "size": "512x512",
        }

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(
            response.status_code,
            [400, 404, 422, 500],
            f"Expected error for invalid model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")

    def test_004_image_generation_with_steps(self):
        """Test image generation with custom steps parameter."""
        payload = {
            "model": SD_NPU_MODEL,
            "prompt": "A blue square",
            "size": "512x512",
            "steps": 1,
            "response_format": "b64_json",
        }

        print("[INFO] Testing image generation with steps=1")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation with custom steps failed: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])

        b64_data = result["data"][0]["b64_json"]
        decoded = base64.b64decode(b64_data)
        self.assertTrue(decoded[:4] == b"\x89PNG", "Should be valid PNG")
        print(f"[OK] Image generation with steps=1 successful ({len(decoded)} bytes)")

    def test_005_image_generation_with_cfg_scale(self):
        """Test image generation with custom cfg_scale parameter.
        SD-Turbo is a distilled model that operates without classifier-free
        guidance, so only cfg_scale <= 1.0 is valid for the NPU model."""
        payload = {
            "model": SD_NPU_MODEL,
            "prompt": "A green triangle",
            "size": "512x512",
            "steps": 1,
            "cfg_scale": 1.0,
            "response_format": "b64_json",
        }

        print("[INFO] Testing image generation with cfg_scale=1.0")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation with custom cfg_scale failed: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])

        b64_data = result["data"][0]["b64_json"]
        decoded = base64.b64decode(b64_data)
        self.assertTrue(decoded[:4] == b"\x89PNG", "Should be valid PNG")
        print(
            f"[OK] Image generation with cfg_scale=1.0 successful ({len(decoded)} bytes)"
        )

    def test_006_image_generation_with_seed(self):
        """Test image generation with explicit seed parameter."""
        payload = {
            "model": SD_NPU_MODEL,
            "prompt": "A yellow star",
            "size": "512x512",
            "steps": 1,
            "seed": 12345,
            "response_format": "b64_json",
        }

        print("[INFO] Testing image generation with seed=12345")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation with seed failed: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])

        b64_data = result["data"][0]["b64_json"]
        decoded = base64.b64decode(b64_data)
        self.assertTrue(decoded[:4] == b"\x89PNG", "Should be valid PNG")
        print(
            f"[OK] Image generation with seed=12345 successful ({len(decoded)} bytes)"
        )

    def test_007_models_endpoint_returns_image_defaults(self):
        """Test that /models endpoint returns image_defaults for SD NPU model."""
        print("[INFO] Testing /models endpoint for image_defaults")

        response = requests.get(f"{self.base_url}/models?show_all=true", timeout=60)

        self.assertEqual(
            response.status_code,
            200,
            f"Failed to get models: {response.text}",
        )

        result = response.json()
        models = result.get("data", result) if isinstance(result, dict) else result

        # Find the NPU model in the models list
        sd_npu = None
        for model in models:
            if model.get("id") == SD_NPU_MODEL:
                sd_npu = model
                break

        self.assertIsNotNone(sd_npu, f"{SD_NPU_MODEL} not found in /models response")

        # Verify image_defaults exists
        self.assertIn(
            "image_defaults", sd_npu, f"{SD_NPU_MODEL} should have image_defaults"
        )
        defaults = sd_npu["image_defaults"]

        self.assertIn("steps", defaults, "image_defaults should have steps")
        self.assertIn("cfg_scale", defaults, "image_defaults should have cfg_scale")
        self.assertIn("width", defaults, "image_defaults should have width")
        self.assertIn("height", defaults, "image_defaults should have height")

        print(f"[OK] {SD_NPU_MODEL} image_defaults verified: {defaults}")

    def test_008_image_edit_not_multipart_error(self):
        """Test that non-multipart requests to /images/edits return 400."""
        response = requests.post(
            f"{self.base_url}/images/edits",
            json={"model": SD_NPU_MODEL, "prompt": "test"},
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

    def test_009_image_edit_missing_image_error(self):
        """Test that /images/edits returns 400 when image file is missing."""
        response = requests.post(
            f"{self.base_url}/images/edits",
            data={"model": SD_NPU_MODEL, "prompt": "test"},
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

    def test_010_image_edit_basic(self):
        """Test basic image edit returns a valid PNG."""
        png_bytes = create_minimal_png(512, 512)
        print(f"[INFO] Sending image edit request with model {SD_NPU_MODEL}")

        response = requests.post(
            f"{self.base_url}/images/edits",
            files={"image": ("test.png", io.BytesIO(png_bytes), "image/png")},
            data={
                "model": SD_NPU_MODEL,
                "prompt": "A red circle",
                "size": "512x512",
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

    def test_011_image_variations_not_multipart_error(self):
        """Test that non-multipart requests to /images/variations return 400."""
        response = requests.post(
            f"{self.base_url}/images/variations",
            json={"model": SD_NPU_MODEL},
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

    def test_012_image_variations_basic(self):
        """Test basic image variations returns a valid PNG."""
        png_bytes = create_minimal_png(512, 512)
        print(f"[INFO] Sending image variations request with model {SD_NPU_MODEL}")

        response = requests.post(
            f"{self.base_url}/images/variations",
            files={"image": ("test.png", io.BytesIO(png_bytes), "image/png")},
            data={
                "model": SD_NPU_MODEL,
                "size": "512x512",
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
        SDNPUTests,
        "SD NPU TESTS",
        wrapped_server="sd-npu",
        modality="stable_diffusion",
    )
