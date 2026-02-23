"""
Real-ESRGAN image upscaling tests for Lemonade Server.

Tests the /images/upscale endpoint with ESRGAN upscale models.

Usage:
    python server_esrgan.py
    python server_esrgan.py --server-per-test
    python server_esrgan.py --server-binary /path/to/lemonade-server
    python server_esrgan.py --backend rocm
    python server_esrgan.py --backend vulkan
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
    ESRGAN_MODEL,
    ESRGAN_ANIME_MODEL,
    PORT,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


def create_test_png(width=64, height=64):
    """Create a minimal valid PNG image in memory and return its base64 encoding."""

    def make_chunk(chunk_type, data):
        chunk = chunk_type + data
        return struct.pack(">I", len(data)) + chunk + struct.pack(">I", zlib.crc32(chunk) & 0xFFFFFFFF)

    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    raw_data = b""
    for _ in range(height):
        raw_data += b"\x00" + b"\xff\x00\x00" * width  # red pixels

    idat_data = zlib.compress(raw_data)

    png = b"\x89PNG\r\n\x1a\n"
    png += make_chunk(b"IHDR", ihdr_data)
    png += make_chunk(b"IDAT", idat_data)
    png += make_chunk(b"IEND", b"")

    return base64.b64encode(png).decode("utf-8")


class ESRGANUpscaleTests(ServerTestBase):
    """Tests for Real-ESRGAN image upscaling."""

    def test_001_basic_upscale(self):
        """Test basic 4x upscale with RealESRGAN-x4plus."""
        test_image_b64 = create_test_png(64, 64)

        payload = {
            "model": ESRGAN_MODEL,
            "image": test_image_b64,
        }

        print(f"[INFO] Sending upscale request with model {ESRGAN_MODEL}")

        response = requests.post(
            f"{self.base_url}/images/upscale",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Upscale failed with status {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result, "Response should contain 'data' field")
        self.assertIsInstance(result["data"], list, "Data should be a list")
        self.assertEqual(len(result["data"]), 1, "Should have 1 image")
        self.assertIn("b64_json", result["data"][0], "Should contain base64 image")

        b64_data = result["data"][0]["b64_json"]
        self.assertIsInstance(b64_data, str, "Base64 data should be a string")
        self.assertGreater(len(b64_data), 1000, "Base64 data should be substantial")

        decoded = base64.b64decode(b64_data)
        self.assertTrue(
            decoded[:4] == b"\x89PNG",
            "Decoded data should be a valid PNG",
        )
        print(f"[OK] Upscaled valid PNG image ({len(decoded)} bytes)")

        self.assertIn("created", result, "Response should contain 'created' timestamp")
        print(f"[OK] Basic upscale successful")

    def test_002_upscale_anime_model(self):
        """Test upscale with RealESRGAN-x4plus-anime model."""
        test_image_b64 = create_test_png(32, 32)

        payload = {
            "model": ESRGAN_ANIME_MODEL,
            "image": test_image_b64,
        }

        print(f"[INFO] Sending upscale request with model {ESRGAN_ANIME_MODEL}")

        response = requests.post(
            f"{self.base_url}/images/upscale",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Anime upscale failed with status {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])

        decoded = base64.b64decode(result["data"][0]["b64_json"])
        self.assertTrue(decoded[:4] == b"\x89PNG", "Should be valid PNG")
        print(f"[OK] Anime model upscale successful ({len(decoded)} bytes)")

    def test_003_missing_image_error(self):
        """Test error handling when image field is missing."""
        payload = {
            "model": ESRGAN_MODEL,
        }

        response = requests.post(
            f"{self.base_url}/images/upscale",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing image, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without image: {response.status_code}")

    def test_004_missing_model_error(self):
        """Test error handling when model field is missing."""
        test_image_b64 = create_test_png(16, 16)

        payload = {
            "image": test_image_b64,
        }

        response = requests.post(
            f"{self.base_url}/images/upscale",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(
            response.status_code,
            [400, 422, 500],
            f"Expected error for missing model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without model: {response.status_code}")

    def test_005_invalid_base64_error(self):
        """Test error handling with invalid base64 image data."""
        payload = {
            "model": ESRGAN_MODEL,
            "image": "not-valid-base64!!!",
        }

        response = requests.post(
            f"{self.base_url}/images/upscale",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(
            response.status_code,
            [400, 422, 500],
            f"Expected error for invalid base64, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected invalid base64: {response.status_code}")

    def test_006_models_endpoint_lists_esrgan(self):
        """Test that /models endpoint lists ESRGAN models with upscale label."""
        print(f"[INFO] Testing /models endpoint for ESRGAN models")

        response = requests.get(f"{self.base_url}/models?show_all=true", timeout=60)

        self.assertEqual(
            response.status_code,
            200,
            f"Failed to get models: {response.text}",
        )

        result = response.json()
        models = result.get("data", result) if isinstance(result, dict) else result

        esrgan_found = False
        for model in models:
            if model.get("id") == ESRGAN_MODEL:
                esrgan_found = True
                labels = model.get("labels", [])
                self.assertIn("upscale", labels, "ESRGAN model should have 'upscale' label")
                self.assertIn("image", labels, "ESRGAN model should have 'image' label")
                break

        self.assertTrue(esrgan_found, f"{ESRGAN_MODEL} not found in /models response")
        print(f"[OK] ESRGAN model found with correct labels")


if __name__ == "__main__":
    run_server_tests(
        ESRGANUpscaleTests,
        "ESRGAN UPSCALE TESTS",
        wrapped_server="sd-cpp",
    )
