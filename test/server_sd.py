"""
Usage: python server_sd.py

This will launch the lemonade server, test image generation with Stable Diffusion,
and make sure that the response is valid.

Examples:
    python server_sd.py
    python server_sd.py --server-binary ./lemonade-server
    python server_sd.py --test-save-images  # Run save-images tests

Note: Image generation with CPU backend takes ~2-3 minutes per image at 256x256 with 1 step.
The Vulkan backend is faster but may have compatibility issues with some GPUs.
"""

import os
import sys
import glob
import base64
import tempfile
import argparse

import requests

# Import all shared functionality from utils/server_base.py
from utils.server_base import (
    ServerTestingBase,
    run_server_tests_with_class,
    PORT,
)

SD_MODEL = "SD-Turbo"


class SDServerTesting(ServerTestingBase):
    """Testing class for Stable Diffusion image generation."""

    def setUp(self):
        """Call parent setUp with SD-specific messaging."""
        print(f"\n=== Starting new SD test ===")
        super().setUp()

    # Test 1: Basic image generation (optimized for CI - minimal size and steps)
    def test_001_basic_image_generation(self):
        """Test basic image generation with SD-Turbo."""
        payload = {
            "model": SD_MODEL,
            "prompt": "A red circle",
            "size": "256x256",  # Smallest practical size for speed
            "steps": 1,  # SD-Turbo can work with 1 step
            "n": 1,
            "response_format": "b64_json"
        }

        print(f"[INFO] Sending image generation request with model {SD_MODEL}")
        print(f"[INFO] Using minimal settings (256x256, 1 step) for CI speed")

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

    # Test 2: Error handling - missing prompt (fast, no image generation)
    def test_002_missing_prompt(self):
        """Test error handling when prompt is missing."""
        payload = {
            "model": SD_MODEL,
            "size": "256x256"
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

    # Test 3: Error handling - invalid model (fast, no image generation)
    def test_003_invalid_model(self):
        """Test error handling with invalid model."""
        payload = {
            "model": "nonexistent-sd-model",
            "prompt": "A cat",
            "size": "256x256"
        }

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=60
        )

        # Should return an error (model not found)
        # Note: Server returns 500 for model not found, ideally should be 404
        self.assertIn(
            response.status_code,
            [400, 404, 422, 500],
            f"Expected error for invalid model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")


# Global variable to hold the temp directory for save-images tests
_save_images_temp_dir = None


class SDServerSaveImagesTesting(ServerTestingBase):
    """Testing class for Stable Diffusion --save-images functionality."""

    @classmethod
    def setUpClass(cls):
        """Set up the temp directory for save-images tests."""
        global _save_images_temp_dir
        # Create a temp directory that persists across tests
        _save_images_temp_dir = tempfile.mkdtemp(prefix="lemonade_sd_test_")
        print(f"\n[INFO] Using temp directory for saved images: {_save_images_temp_dir}")
        # Set additional server args to enable save-images
        cls.additional_server_args = ["--save-images", "--images-dir", _save_images_temp_dir]

    @classmethod
    def tearDownClass(cls):
        """Clean up the temp directory."""
        global _save_images_temp_dir
        if _save_images_temp_dir and os.path.exists(_save_images_temp_dir):
            # List any remaining files for debugging
            files = glob.glob(os.path.join(_save_images_temp_dir, "*"))
            if files:
                print(f"[INFO] Cleaning up {len(files)} file(s) from temp directory")
            # Remove all files
            for f in files:
                try:
                    os.remove(f)
                except Exception as e:
                    print(f"[WARN] Failed to remove {f}: {e}")
            # Remove the directory
            try:
                os.rmdir(_save_images_temp_dir)
            except Exception as e:
                print(f"[WARN] Failed to remove temp directory: {e}")

    def setUp(self):
        """Call parent setUp with save-images-specific messaging."""
        print(f"\n=== Starting new SD save-images test ===")
        super().setUp()

    def _count_images_in_dir(self):
        """Count PNG files in the temp directory."""
        global _save_images_temp_dir
        return len(glob.glob(os.path.join(_save_images_temp_dir, "*.png")))

    def _get_latest_image(self):
        """Get the path to the most recently created image."""
        global _save_images_temp_dir
        files = glob.glob(os.path.join(_save_images_temp_dir, "*.png"))
        if not files:
            return None
        # Return the most recently modified file
        return max(files, key=os.path.getmtime)

    # Test 1: Verify image is saved to disk
    def test_001_image_saved_to_disk(self):
        """Test that generated images are saved to disk when --save-images is used."""
        global _save_images_temp_dir

        # Count images before generation
        initial_count = self._count_images_in_dir()
        print(f"[INFO] Initial image count in temp dir: {initial_count}")

        payload = {
            "model": SD_MODEL,
            "prompt": "A blue square",
            "size": "256x256",
            "steps": 1,
            "n": 1,
            "response_format": "b64_json"
        }

        print(f"[INFO] Sending image generation request with --save-images enabled")

        response = requests.post(
            f"{self.base_url}/images/generations",
            json=payload,
            timeout=600
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Image generation failed with status {response.status_code}: {response.text}",
        )

        # Verify response still contains base64 data
        result = response.json()
        self.assertIn("data", result, "Response should contain 'data' field")
        self.assertEqual(len(result["data"]), 1, "Should have 1 image")

        # Check that response contains the file URL (when save-images is enabled)
        # The server should return a URL field pointing to the saved file
        if "url" in result["data"][0]:
            print(f"[OK] Response contains URL: {result['data'][0]['url']}")

        # Verify an image was saved to disk
        final_count = self._count_images_in_dir()
        print(f"[INFO] Final image count in temp dir: {final_count}")

        self.assertGreater(
            final_count,
            initial_count,
            f"Expected at least one new image to be saved. Before: {initial_count}, After: {final_count}"
        )

        # Verify the saved image is a valid PNG
        latest_image = self._get_latest_image()
        self.assertIsNotNone(latest_image, "Should have at least one saved image")
        print(f"[INFO] Latest saved image: {latest_image}")

        # Check file size is reasonable (at least 1KB for a valid PNG)
        file_size = os.path.getsize(latest_image)
        self.assertGreater(file_size, 1000, f"Saved image should be at least 1KB, got {file_size} bytes")

        # Verify PNG magic bytes
        with open(latest_image, "rb") as f:
            magic = f.read(4)
            self.assertEqual(magic, b'\x89PNG', "Saved file should be a valid PNG")

        print(f"[OK] Image saved successfully: {latest_image} ({file_size} bytes)")


if __name__ == "__main__":
    # Parse custom arguments
    parser = argparse.ArgumentParser(description="Stable Diffusion server tests", add_help=False)
    parser.add_argument(
        "--test-save-images",
        action="store_true",
        help="Run save-images tests instead of basic tests"
    )
    args, remaining = parser.parse_known_args()

    # Restore sys.argv for the test framework (remove our custom args)
    sys.argv = [sys.argv[0]] + remaining

    if args.test_save_images:
        run_server_tests_with_class(SDServerSaveImagesTesting, "STABLE DIFFUSION SAVE-IMAGES TESTS")
    else:
        run_server_tests_with_class(SDServerTesting, "STABLE DIFFUSION SERVER TESTS")
