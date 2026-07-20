"""
3D generation tests for Lemonade Server.

Tests the /3d/generations endpoint (image -> textured GLB mesh) with the
Trellis backend.

Usage:
    python server_3d.py --wrapped-server trellis --backend vulkan
    python server_3d.py --wrapped-server trellis --backend rocm

Note: 3D reconstruction is slow (minutes per mesh even at the 512 cascade).
The negative tests run first and never pull the model; only the generation
test downloads it.
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
            "bg_removal": "threshold",
        }
        payload.update(overrides)
        return payload

    def _assert_rejected(self, payload, context, expected_status=400):
        response = requests.post(
            f"{self.base_url}/3d/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            expected_status,
            f"{context}: expected {expected_status}, got {response.status_code}: "
            f"{response.text[:1000]}",
        )
        self.assertIn("error", response.json(), f"{context}: missing 'error' field")
        print(f"[OK] {context}: {response.status_code}")

    def test_001_missing_model_error(self):
        """A request without a model is rejected without loading anything."""
        payload = self._generation_payload()
        del payload["model"]
        self._assert_rejected(payload, "Missing model")

    def test_002_missing_image_error(self):
        """A request without an image is rejected without loading anything."""
        payload = self._generation_payload()
        del payload["image"]
        self._assert_rejected(payload, "Missing image")

    def test_003_non_base64_image_error(self):
        """An image field that is not base64 is rejected without loading anything."""
        self._assert_rejected(
            self._generation_payload(image="!!!not-base64!!!"), "Non-base64 image"
        )

    def test_003a_non_image_payload_error(self):
        """Valid base64 that does not decode to a supported image is rejected without loading anything."""
        self._assert_rejected(
            self._generation_payload(
                image=base64.b64encode(b"not an image").decode("ascii")
            ),
            "Non-image base64 payload",
        )

    def test_004_unsupported_response_format(self):
        """A response_format other than glb is rejected without loading anything."""
        self._assert_rejected(
            self._generation_payload(response_format="obj"),
            "Unsupported response_format",
        )

    def test_005_invalid_resolution_error(self):
        """A resolution outside 512/1024/1536 is rejected without loading anything."""
        self._assert_rejected(
            self._generation_payload(resolution=777), "Invalid resolution"
        )

    def test_006_invalid_bg_removal_error(self):
        """An unknown bg_removal mode is rejected without loading anything."""
        self._assert_rejected(
            self._generation_payload(bg_removal="magic"), "Invalid bg_removal"
        )

    def test_007_non_integer_seed_error(self):
        """A non-integer seed is rejected without loading anything."""
        self._assert_rejected(self._generation_payload(seed="abc"), "Non-integer seed")

    def test_008_invalid_model_error(self):
        """A nonexistent model is a 404 model_not_found, not a download attempt."""
        self._assert_rejected(
            self._generation_payload(model="nonexistent-3d-model-xyz-123"),
            "Invalid model",
            expected_status=404,
        )

    def test_500_basic_3d_generation(self):
        """Test basic image-to-3D generation returns a GLB mesh."""
        self._ensure_model_pulled()
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


if __name__ == "__main__":
    run_server_tests(
        Model3DTests,
        "3D GENERATION TESTS",
        modality="model3d",
        default_wrapped_server="trellis",
    )
