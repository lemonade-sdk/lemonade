"""
Stable Diffusion NPU image generation tests for Lemonade Server.

Tests the /images/generations and /images/edits endpoints with the
ryzenai-sd (AMD Ryzen AI NPU) backend. These tests follow the same
structure as server_sd.py (sd-cpp), server_whisper.py (whispercpp), and
server_llm.py (ryzenai): a ServerTestBase subclass driven by the shared
harness, gated by the capability catalog, run against an already-running
`lemond` server.

The NPU models are defined in user_models.json with `recipe: "ryzenai-sd"`.
Lemonade downloads the checkpoint (HuggingFace repo id) on first load and
spawns ryzenai-sd-server.exe (path from config.json `ryzenaisd.npu_bin`).

Prerequisites:
    - `lemond` is already running (this harness does not start it).
    - config.json has `ryzenaisd.npu_bin` pointing at ryzenai-sd-server.exe.
    - An AMD Ryzen AI NPU is available.

Usage:
    python server_sd_npu.py --wrapped-server ryzenai-sd-server --backend npu
    python server_sd_npu.py --cli-binary /path/to/lemonade

    # Backward compatible (defaults to ryzenai-sd-server):
    python server_sd_npu.py

Notes:
    - First load can take several minutes (checkpoint download + subprocess
      startup + ONNX/NPU compile), so model operations use a generous timeout.
    - NPU models have fixed compiled shapes; the output size follows the model
      rather than the request when they disagree, so tests validate PNG
      contents rather than exact dimensions.
"""

import base64
import io
import os
import struct
import zlib

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
)
from utils.capabilities import (
    skip_if_unsupported,
    get_test_model,
)
from utils.test_models import (
    PORT,
    TIMEOUT_DEFAULT,
)

# NPU first load (download + subprocess spawn + compile) can be slow; allow a
# generous timeout for model operations. Overridable via env var.
TIMEOUT_NPU_MODEL_OPERATION = int(
    os.environ.get("LEMONADE_TEST_SD_NPU_TIMEOUT", "3600")
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


def _get_image_model():
    """Get the image test model for the current configuration (e.g. SD-Turbo-NPU)."""
    override = os.environ.get("LEMONADE_TEST_SD_NPU_MODEL")
    if override:
        return override
    return get_test_model("image")


class StableDiffusionNPUTests(ServerTestBase):
    """Tests for Stable Diffusion image generation on the AMD Ryzen AI NPU."""

    def _load_model_or_fail(self):
        """Load the configured NPU model, triggering download/spawn on first call.

        Loading the same model again is a no-op on the server side (the running
        ryzenai-sd-server hot-swaps or reuses the pipeline), so calling this at
        the top of each positive test is cheap after the first load.
        """
        model = _get_image_model()
        print(f"[INFO] Loading NPU model {model}")
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": model},
            timeout=TIMEOUT_NPU_MODEL_OPERATION,
        )
        self.assertEqual(
            response.status_code,
            200,
            f"Failed to load model {model}: {response.text}",
        )
        return model

    # =========================================================================
    # IMAGE GENERATION
    # =========================================================================

    @skip_if_unsupported("image_generation_b64")
    def test_001_basic_image_generation(self):
        """Test basic image generation returns a valid base64 PNG."""
        model = self._load_model_or_fail()

        payload = {
            "model": model,
            "prompt": "A red circle",
            "size": "512x512",
            "steps": 1,
            "n": 1,
            "response_format": "b64_json",
        }

        print(f"[INFO] Sending image generation request with model {model}")
        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_NPU_MODEL_OPERATION,
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

        b64_data = result["data"][0]["b64_json"]
        self.assertIsInstance(b64_data, str, "Base64 data should be a string")
        self.assertGreater(len(b64_data), 1000, "Base64 data should be substantial")

        decoded = base64.b64decode(b64_data)
        self.assertTrue(
            decoded[:4] == b"\x89PNG",
            "Decoded data should be a valid PNG",
        )
        self.assertIn("created", result, "Response should contain 'created' timestamp")
        print(f"[OK] Generated valid PNG image ({len(decoded)} bytes)")

    @skip_if_unsupported("image_generation")
    def test_002_missing_prompt_error(self):
        """Test error handling when prompt is missing."""
        payload = {
            "model": _get_image_model(),
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

    @skip_if_unsupported("image_generation")
    def test_003_invalid_model_error(self):
        """Test error handling with an unknown model."""
        payload = {
            "model": "nonexistent-ryzenai-sd-server-model-xyz-123",
            "prompt": "A cat",
            "size": "512x512",
        }

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        # Model not found — server may return 400/404/422/500.
        self.assertIn(
            response.status_code,
            [400, 404, 422, 500],
            f"Expected error for invalid model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")

    @skip_if_unsupported("image_generation_b64")
    def test_004_image_generation_with_steps(self):
        """Test image generation with an explicit steps parameter."""
        model = self._load_model_or_fail()

        payload = {
            "model": model,
            "prompt": "A blue square",
            "size": "512x512",
            "steps": 2,
            "response_format": "b64_json",
        }

        print(f"[INFO] Testing image generation with steps=2")
        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_NPU_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation with custom steps failed: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])
        decoded = base64.b64decode(result["data"][0]["b64_json"])
        self.assertTrue(decoded[:4] == b"\x89PNG", "Should be valid PNG")
        print(f"[OK] Image generation with steps=2 successful ({len(decoded)} bytes)")

    @skip_if_unsupported("image_generation_b64")
    def test_005_image_generation_with_seed(self):
        """Test image generation with an explicit seed parameter."""
        model = self._load_model_or_fail()

        payload = {
            "model": model,
            "prompt": "A yellow star",
            "size": "512x512",
            "steps": 1,
            "seed": 12345,
            "response_format": "b64_json",
        }

        print(f"[INFO] Testing image generation with seed=12345")
        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=TIMEOUT_NPU_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation with seed failed: {response.text}",
        )

        result = response.json()
        self.assertIn("data", result)
        self.assertIn("b64_json", result["data"][0])
        decoded = base64.b64decode(result["data"][0]["b64_json"])
        self.assertTrue(decoded[:4] == b"\x89PNG", "Should be valid PNG")
        print(
            f"[OK] Image generation with seed=12345 successful ({len(decoded)} bytes)"
        )

    @skip_if_unsupported("image_generation")
    def test_006_models_endpoint_returns_image_defaults(self):
        """Test that /models exposes image_defaults for the NPU model."""
        model = _get_image_model()
        print(f"[INFO] Testing /models endpoint for {model} image_defaults")

        response = requests.get(f"{self.base_url}/models?show_all=true", timeout=60)
        self.assertEqual(
            response.status_code,
            200,
            f"Failed to get models: {response.text}",
        )

        result = response.json()
        models = result.get("data", result) if isinstance(result, dict) else result

        entry = None
        for m in models:
            if m.get("id") == model:
                entry = m
                break

        self.assertIsNotNone(entry, f"{model} not found in /models response")
        self.assertIn("image_defaults", entry, f"{model} should have image_defaults")

        defaults = entry["image_defaults"]
        # Values come from user_models.json; assert the shape is present and sane.
        self.assertIn("steps", defaults, "image_defaults should include steps")
        self.assertIn("width", defaults, "image_defaults should include width")
        self.assertIn("height", defaults, "image_defaults should include height")
        self.assertGreater(defaults.get("steps", 0), 0, "steps should be positive")
        self.assertGreater(defaults.get("width", 0), 0, "width should be positive")
        self.assertGreater(defaults.get("height", 0), 0, "height should be positive")
        print(f"[OK] {model} image_defaults verified: {defaults}")

    # =========================================================================
    # IMAGE EDITS (img2img)
    # =========================================================================

    @skip_if_unsupported("image_edits")
    def test_007_image_edit_basic(self):
        """Test basic image edit (img2img) returns a valid PNG."""
        model = self._load_model_or_fail()
        png_bytes = create_minimal_png(512, 512)

        print(f"[INFO] Sending image edit request with model {model}")
        response = requests.post(
            f"{self.base_url}/images/edits",
            files={"image": ("test.png", io.BytesIO(png_bytes), "image/png")},
            data={
                "model": model,
                "prompt": "A red circle",
                "size": "512x512",
                "n": "1",
                "response_format": "b64_json",
            },
            timeout=TIMEOUT_NPU_MODEL_OPERATION,
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
        decoded = base64.b64decode(result["data"][0]["b64_json"])
        self.assertTrue(decoded[:4] == b"\x89PNG", "Result should be a valid PNG")
        print(f"[OK] Image edit successful ({len(decoded)} bytes)")

    @skip_if_unsupported("image_edits")
    def test_008_image_edit_not_multipart_error(self):
        """Test that non-multipart requests to /images/edits return 400."""
        response = requests.post(
            f"{self.base_url}/images/edits",
            json={"model": _get_image_model(), "prompt": "test"},
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

    @skip_if_unsupported("image_edits")
    def test_009_image_edit_missing_image_error(self):
        """Test that /images/edits returns 400 when the image file is missing."""
        response = requests.post(
            f"{self.base_url}/images/edits",
            data={"model": _get_image_model(), "prompt": "test"},
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

    @skip_if_unsupported("image_edits")
    def test_010_image_edit_missing_prompt_error(self):
        """Test that /images/edits returns 400 when the prompt is missing."""
        png_bytes = create_minimal_png()
        response = requests.post(
            f"{self.base_url}/images/edits",
            files={"image": ("test.png", io.BytesIO(png_bytes), "image/png")},
            data={"model": _get_image_model()},
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


if __name__ == "__main__":
    run_server_tests(
        StableDiffusionNPUTests,
        "STABLE DIFFUSION NPU TESTS",
        default_wrapped_server="ryzenai-sd-server",
        modality="stable_diffusion",
    )
