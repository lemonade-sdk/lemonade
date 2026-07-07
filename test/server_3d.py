"""
3D generation tests for Lemonade Server.

Tests the /3d/generations endpoint (image -> textured GLB mesh) with the
Trellis backend.

Usage:
    python server_3d.py --wrapped-server trellis --backend vulkan
    python server_3d.py --wrapped-server trellis --backend rocm

Note: 3D reconstruction is slow (minutes per mesh even at the 512 cascade).
"""

import base64
import struct
import zlib

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
)
from utils.capabilities import get_test_model
from utils.test_models import (
    TIMEOUT_DEFAULT,
)

TIMEOUT_3D_GENERATION = 1800


def make_input_png_b64(size=64):
    """Build a small valid RGB PNG (red square on white) as base64, stdlib only."""

    def make_chunk(chunk_type, data):
        c = chunk_type + data
        return (
            struct.pack(">I", len(data))
            + c
            + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
        )

    ihdr_data = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    rows = bytearray()
    border = size // 8
    for y in range(size):
        rows += b"\x00"
        for x in range(size):
            inside = border <= x < size - border and border <= y < size - border
            rows += b"\xff\x00\x00" if inside else b"\xff\xff\xff"
    idat_data = zlib.compress(bytes(rows))
    png = (
        b"\x89PNG\r\n\x1a\n"
        + make_chunk(b"IHDR", ihdr_data)
        + make_chunk(b"IDAT", idat_data)
        + make_chunk(b"IEND", b"")
    )
    return base64.b64encode(png).decode("ascii")


class Model3DTests(ServerTestBase):
    """Tests for the /3d/generations endpoint."""

    _model_pulled = False

    @classmethod
    def setUpClass(cls):
        """Verify server, apply runtime config, and pre-pull the model."""
        super().setUpClass()
        cls._ensure_model_pulled()

    @classmethod
    def _ensure_model_pulled(cls):
        if cls._model_pulled:
            return
        model = get_test_model("model3d")
        print(f"\n[SETUP] Ensuring {model} is pulled...")
        pull_model_with_retry(model)
        print(f"[SETUP] {model} is ready")
        cls._model_pulled = True

    def _generation_payload(self, **overrides):
        payload = {
            "model": get_test_model("model3d"),
            "image": make_input_png_b64(),
            "resolution": 512,
            "seed": 42,
        }
        payload.update(overrides)
        return payload

    def test_001_basic_3d_generation(self):
        """Test basic image-to-3D generation returns a GLB mesh."""
        payload = self._generation_payload()
        print(f"[INFO] Sending 3D generation request with model {payload['model']}")
        print(f"[INFO] Using the 512 cascade for CI speed; this still takes minutes")

        response = requests.post(
            f"{self.base_url}/3d/generations",
            json=payload,
            timeout=TIMEOUT_3D_GENERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"3D generation failed with status {response.status_code}: {response.text[:1000]}",
        )
        self.assertIn(
            "model/gltf-binary",
            response.headers.get("Content-Type", ""),
            "Response should have model/gltf-binary content type",
        )
        self.assertTrue(
            response.content[:4] == b"glTF",
            "Response body should be a valid GLB (glTF-binary) file",
        )
        self.assertGreater(len(response.content), 10000, "Mesh should be substantial")
        print(f"[OK] Generated valid GLB mesh ({len(response.content)} bytes)")

    def test_002_unsupported_response_format(self):
        """Test that a response_format the backend cannot produce is rejected."""
        payload = self._generation_payload(response_format="obj")

        response = requests.post(
            f"{self.base_url}/3d/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for unsupported response_format, got {response.status_code}: "
            f"{response.text[:1000]}",
        )
        self.assertIn("error", response.json(), "Response should contain 'error' field")
        print(
            f"[OK] Correctly rejected unsupported response_format: {response.status_code}"
        )

    def test_003_missing_image_error(self):
        """Test error handling when image is missing."""
        payload = self._generation_payload()
        del payload["image"]

        response = requests.post(
            f"{self.base_url}/3d/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for missing image, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without image: {response.status_code}")

    def test_004_missing_model_error(self):
        """Test error handling when model is missing."""
        payload = self._generation_payload()
        del payload["model"]

        response = requests.post(
            f"{self.base_url}/3d/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(
            response.status_code,
            400,
            f"Expected 400 for missing model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without model: {response.status_code}")

    def test_005_invalid_model_error(self):
        """Test error handling with a nonexistent model."""
        payload = self._generation_payload(model="nonexistent-3d-model-xyz-123")

        response = requests.post(
            f"{self.base_url}/3d/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertIn(
            response.status_code,
            [400, 404, 422, 500],
            f"Expected error for invalid model, got {response.status_code}",
        )
        self.assertIn("error", response.json(), "Response should contain 'error' field")
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")


if __name__ == "__main__":
    run_server_tests(
        Model3DTests,
        "3D GENERATION TESTS",
        modality="model3d",
        default_wrapped_server="trellis",
    )
