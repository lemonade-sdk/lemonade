"""
3D generation tests for Lemonade Server.

Tests the /3d/generations endpoint (image -> GLB mesh) with TRELLIS and TheNoise.

Usage:
    python server_3d.py --wrapped-server trellis --backend vulkan
    python server_3d.py --wrapped-server trellis --backend rocm
    python server_3d.py --wrapped-server thenoise --backend rocm
    python server_3d.py --wrapped-server thenoise --backend cuda

Note: TRELLIS uses its 512 cascade with classifier-free guidance disabled;
TheNoise uses a smaller octree and fewer denoising steps for CI. Negative
tests run first without pulling the model.
"""

import base64
import json
import struct
import zlib

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    pull_model_with_retry,
    load_model,
    unload_model,
)
from utils.capabilities import get_current_config, get_test_model
from utils.test_models import (
    TIMEOUT_DEFAULT,
)

TIMEOUT_3D_GENERATION = 1800

# Loading can install the backend, which downloads and extracts an archive.
TIMEOUT_3D_LOAD = 1800

# Guidance strengths of 1 disable classifier-free guidance, so each flow step
# costs one forward pass instead of two. These tests cover that the pipeline
# returns a valid mesh, not how good the mesh looks.
FAST_GENERATION_ARGS = "--gss 1 --gsh 1"


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
    def tearDownClass(cls):
        # The guidance-free args belong to the backend subprocess this suite
        # started, so retire it rather than let it serve later requests.
        try:
            response = unload_model(get_test_model("model3d"))
            if response.status_code not in (200, 404):
                print(
                    "Warning: Failed to unload the 3D backend: "
                    f"{response.status_code} {response.text[:200]}"
                )
        except Exception as e:
            print(f"Warning: Failed to unload the 3D backend: {e}")
        super().tearDownClass()

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
            "seed": 42,
        }
        if get_current_config()[0] == "thenoise":
            # Cover the public data-URL/base64 normalization as well as native mesh options.
            wrapped = "\r\n".join(
                payload["image"][i : i + 76]
                for i in range(0, len(payload["image"]), 76)
            )
            payload["image"] = "data:image/png;base64," + wrapped
            payload.update(octree_resolution=192, steps=12)
        else:
            payload.update(resolution=512, bg_removal="threshold")
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

    def test_005a_invalid_octree_resolution_error(self):
        """An out-of-range octree resolution is rejected before loading."""
        for value in (0, 769, "192"):
            with self.subTest(value=value):
                self._assert_rejected(
                    self._generation_payload(octree_resolution=value),
                    "Invalid octree resolution",
                )
        self._assert_rejected(self._generation_payload(steps=0), "Invalid mesh steps")

    def test_005b_invalid_triangle_fraction_error(self):
        for value in (0, 0.0009, 1.01, "0.05"):
            with self.subTest(value=value):
                self._assert_rejected(
                    self._generation_payload(triangles=value),
                    "Invalid triangle fraction",
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

        is_trellis = get_current_config()[0] == "trellis"
        # Keep the existing fast TRELLIS load path. TheNoise loads the single
        # Hunyuan checkpoint without TRELLIS-specific launch options.
        load = load_model(
            payload["model"],
            timeout=TIMEOUT_3D_LOAD,
            **(
                {"trellis_args": FAST_GENERATION_ARGS, "merge_args": False}
                if is_trellis
                else {}
            ),
        )
        self.assertEqual(
            load.status_code,
            200,
            f"Loading {payload['model']} failed: " f"{load.text[:1000]}",
        )

        # An option the server does not recognize is dropped silently, which
        # would put the slow full-guidance path back without failing anything.
        health = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT).json()
        loaded = next(
            (
                model
                for model in health.get("all_models_loaded", [])
                if model.get("model_name") == payload["model"]
            ),
            None,
        )
        self.assertIsNotNone(loaded, "The mesh model must be loaded")
        self.assertEqual(loaded.get("type"), "mesh")
        if is_trellis:
            applied = loaded.get("recipe_options", {})
            self.assertEqual(
                applied.get("trellis_args", ""),
                FAST_GENERATION_ARGS,
                f"trellis_args never reached the backend; applied options: {applied}",
            )
            print("[INFO] Using the 512 cascade with guidance disabled for CI speed")
        else:
            # A recipe can expose both capabilities; the loaded mesh must not
            # be sent to TheNoise's image controller.
            image_response = requests.post(
                f"{self.base_url}/images/generations",
                json={"model": payload["model"], "prompt": "a red square"},
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(
                image_response.status_code, 400, image_response.text[:1000]
            )
            self.assertIn("error", image_response.json())
        print(f"[INFO] Sending 3D generation request with model {payload['model']}")

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
        magic, version, length = struct.unpack_from("<4sII", response.content)
        self.assertEqual((magic, version, length), (b"glTF", 2, len(response.content)))
        if is_trellis:
            self.assertGreater(
                len(response.content), 10000, "Mesh should be substantial"
            )
        else:
            # With the same seed and reference, only mesh finishing changes.
            # Compare actual triangle counts, not just the binary file size.
            raw = requests.post(
                f"{self.base_url}/3d/generations",
                json={**payload, "triangles": 1.0},
                timeout=TIMEOUT_3D_GENERATION,
            )
            self.assertEqual(
                raw.status_code, 200, raw.text[:1000] if not raw.ok else ""
            )
            reduced_count = self._glb_triangle_count(response.content)
            raw_count = self._glb_triangle_count(raw.content)
            self.assertGreater(reduced_count, 0)
            self.assertLess(reduced_count, raw_count / 2)
            self.assertLess(len(response.content), len(raw.content))
            print(f"[OK] Triangle reduction: {raw_count} -> {reduced_count}")
        print(f"[OK] Generated valid GLB mesh ({len(response.content)} bytes)")

    def _glb_triangle_count(self, content):
        magic, version, length = struct.unpack_from("<4sII", content)
        self.assertEqual((magic, version, length), (b"glTF", 2, len(content)))
        json_length, chunk_type = struct.unpack_from("<I4s", content, 12)
        self.assertEqual(chunk_type, b"JSON")
        document = json.loads(content[20 : 20 + json_length])
        count = 0
        for mesh in document["meshes"]:
            for primitive in mesh["primitives"]:
                self.assertEqual(primitive.get("mode", 4), 4)
                indices = document["accessors"][primitive["indices"]]["count"]
                self.assertEqual(indices % 3, 0)
                count += indices // 3
        return count


if __name__ == "__main__":
    run_server_tests(
        Model3DTests,
        "3D GENERATION TESTS",
        modality="model3d",
        default_wrapped_server="trellis",
    )
