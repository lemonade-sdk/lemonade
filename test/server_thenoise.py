"""
TheNoise image generation tests for Lemonade Server.

Tests the /images/generations endpoint with TheNoise models (ROCm-only, Strix
Halo/Strix Point iGPU).

Usage:
    python server_thenoise.py
    python server_thenoise.py --cli-binary /path/to/lemonade --backend rocm

Note: TheNoise is an ROCm GPU backend (gfx1150/gfx1151). The first generation
after loading compiles the DiT with torch.compile and can take several minutes.

The individual generation tests (beyond the basic test_001) are heavy AND
non-critical, so they are disabled by default with @skip_heavy. Set
LEMONADE_TEST_HEAVY=1 to run them.
"""

import base64

import requests

from utils.capabilities import (
    skip_heavy,
)
from utils.server_base import (
    ServerTestBase,
    run_server_tests,
)
from utils.test_models import (
    THENOISE_MODEL,
    PORT,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


def assert_valid_png(testcase, b64_data, label=""):
    """Assert that base64 data decodes to a valid PNG image."""
    testcase.assertIsInstance(b64_data, str, "Base64 data should be a string")
    testcase.assertGreater(len(b64_data), 1000, "Base64 data should be substantial")
    decoded = base64.b64decode(b64_data)
    testcase.assertTrue(
        decoded[:4] == b"\x89PNG",
        "Decoded data should be a valid PNG",
    )
    print(f"[OK] {label} produced a valid PNG ({len(decoded)} bytes)")


class TheNoiseTests(ServerTestBase):
    """Tests for TheNoise image generation."""

    def _generate(self, payload):
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
        self.assertIn("created", result, "Response should contain 'created' timestamp")
        return result

    def test_001_basic_image_generation(self):
        """Test basic image generation."""
        payload = {
            "model": THENOISE_MODEL,
            "prompt": "a fox walking in the snow",
            "size": "256x256",
            "steps": 4,
            "n": 1,
            "response_format": "b64_json",
        }

        print(f"[INFO] Sending image generation request with model {THENOISE_MODEL}")
        result = self._generate(payload)

        self.assertEqual(len(result["data"]), 1, "Should have 1 image")
        self.assertIn("b64_json", result["data"][0], "Should contain base64 image")
        assert_valid_png(self, result["data"][0]["b64_json"], "basic generation")
        print(f"[OK] Image generation successful")

    def test_002_missing_prompt_error(self):
        """Test error handling when prompt is missing."""
        payload = {
            "model": THENOISE_MODEL,
            "size": "256x256",
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
            "model": "nonexistent-thenoise-model-xyz-123",
            "prompt": "a cat",
            "size": "256x256",
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

    @skip_heavy
    def test_004_image_generation_with_steps(self):
        """Test image generation with custom steps parameter."""
        result = self._generate(
            {
                "model": THENOISE_MODEL,
                "prompt": "a blue square",
                "size": "256x256",
                "steps": 4,
                "response_format": "b64_json",
            }
        )
        assert_valid_png(self, result["data"][0]["b64_json"], "steps=4")
        print(f"[OK] Image generation with custom steps successful")

    @skip_heavy
    def test_005_image_generation_with_cfg_scale(self):
        """Test image generation with custom cfg_scale."""
        result = self._generate(
            {
                "model": THENOISE_MODEL,
                "prompt": "a green triangle",
                "size": "256x256",
                "steps": 4,
                "cfg_scale": 1.0,
                "response_format": "b64_json",
            }
        )
        assert_valid_png(self, result["data"][0]["b64_json"], "cfg_scale=1.0")
        print(f"[OK] Image generation with custom cfg_scale successful")

    @skip_heavy
    def test_006_image_generation_with_seed(self):
        """Test image generation with explicit seed parameter."""
        result = self._generate(
            {
                "model": THENOISE_MODEL,
                "prompt": "a yellow star",
                "size": "256x256",
                "steps": 4,
                "seed": 12345,
                "response_format": "b64_json",
            }
        )
        assert_valid_png(self, result["data"][0]["b64_json"], "seed=12345")
        print(f"[OK] Image generation with explicit seed successful")

    def test_007_models_endpoint_returns_image_defaults(self):
        """Test that /models returns image_defaults for the TheNoise model."""
        response = requests.get(
            f"{self.base_url}/models?show_all=true",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code,
            200,
            f"Failed to get models: {response.text}",
        )

        result = response.json()
        models = result.get("data", result) if isinstance(result, dict) else result

        model = None
        for m in models:
            if m.get("id") == THENOISE_MODEL:
                model = m
                break

        self.assertIsNotNone(model, f"TheNoise model not found in /models response")
        self.assertIn("image_defaults", model, "Model should have image_defaults")
        defaults = model["image_defaults"]
        self.assertIn("steps", defaults, "image_defaults should have steps")
        self.assertIn("width", defaults, "image_defaults should have width")
        self.assertIn("height", defaults, "image_defaults should have height")
        print(f"[OK] {THENOISE_MODEL} image_defaults verified: {defaults}")

    @skip_heavy
    def test_008_negative_prompt(self):
        """Test image generation with negative_prompt parameter."""
        result = self._generate(
            {
                "model": THENOISE_MODEL,
                "prompt": "a mountain landscape",
                "negative_prompt": "watermark, text, low quality",
                "size": "256x256",
                "steps": 4,
                "response_format": "b64_json",
            }
        )
        assert_valid_png(self, result["data"][0]["b64_json"], "negative_prompt")
        print(f"[OK] Image generation with negative_prompt successful")

    @skip_heavy
    def test_009_sampler(self):
        """Test image generation with sampler parameter."""
        result = self._generate(
            {
                "model": THENOISE_MODEL,
                "prompt": "a red sunset",
                "size": "256x256",
                "steps": 4,
                "sampler": "euler",
                "response_format": "b64_json",
            }
        )
        assert_valid_png(self, result["data"][0]["b64_json"], "sampler=euler")
        print(f"[OK] Image generation with sampler successful")

    @skip_heavy
    def test_010_extra_postprocessing_params(self):
        """Test image generation with the extra postprocessing parameters."""
        result = self._generate(
            {
                "model": THENOISE_MODEL,
                "prompt": "a portrait",
                "size": "256x256",
                "steps": 4,
                "qwen_vae_enhance": True,
                "film_grain": 0.1,
                "sharpening": 0.3,
                "response_format": "b64_json",
            }
        )
        assert_valid_png(self, result["data"][0]["b64_json"], "postprocessing params")
        print(f"[OK] Image generation with postprocessing params successful")

    @skip_heavy
    def test_011_upscale_param(self):
        """Test image generation with the upscale parameter."""
        result = self._generate(
            {
                "model": THENOISE_MODEL,
                "prompt": "a city at night",
                "size": "256x256",
                "steps": 4,
                "upscale": True,
                "response_format": "b64_json",
            }
        )
        assert_valid_png(self, result["data"][0]["b64_json"], "upscale")
        print(f"[OK] Image generation with upscale successful")

    @skip_heavy
    def test_012_multi_image(self):
        """Test that n>1 returns multiple images (looping)."""
        result = self._generate(
            {
                "model": THENOISE_MODEL,
                "prompt": "a field of flowers",
                "size": "256x256",
                "steps": 4,
                "n": 2,
                "response_format": "b64_json",
            }
        )
        self.assertEqual(len(result["data"]), 2, "Should have 2 images")
        for entry in result["data"]:
            self.assertIn("b64_json", entry)
            assert_valid_png(self, entry["b64_json"], "multi-image")
        print(f"[OK] Image generation with n=2 successful")

    def test_013_image_edits_unsupported(self):
        """Test that /images/edits returns an error (TheNoise is text2image only)."""
        response = requests.post(
            f"{self.base_url}/images/edits",
            files={"image": ("test.png", b"\x89PNG\r\n\x1a\n", "image/png")},
            data={
                "model": THENOISE_MODEL,
                "prompt": "a red circle",
                "size": "256x256",
                "response_format": "b64_json",
            },
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(
            response.status_code,
            500,
            f"Expected 500 for unsupported image edit, got {response.status_code}: {response.text}",
        )
        self.assertIn(
            "not supported",
            response.text.lower(),
            f"Error should mention edits are not supported: {response.text}",
        )
        print(
            f"[OK] Correctly rejected image edit as unsupported: {response.status_code}"
        )

    def test_014_image_variations_unsupported(self):
        """Test that /images/variations returns an error (TheNoise is text2image only)."""
        response = requests.post(
            f"{self.base_url}/images/variations",
            files={"image": ("test.png", b"\x89PNG\r\n\x1a\n", "image/png")},
            data={
                "model": THENOISE_MODEL,
                "size": "256x256",
                "response_format": "b64_json",
            },
            timeout=TIMEOUT_DEFAULT,
        )

        self.assertEqual(
            response.status_code,
            500,
            f"Expected 500 for unsupported image variations, got {response.status_code}: {response.text}",
        )
        self.assertIn(
            "not supported",
            response.text.lower(),
            f"Error should mention variations are not supported: {response.text}",
        )
        print(
            f"[OK] Correctly rejected image variations as unsupported: {response.status_code}"
        )


if __name__ == "__main__":
    run_server_tests(
        TheNoiseTests,
        "THENOISE TESTS",
        wrapped_server="thenoise",
        modality="stable_diffusion",
    )
