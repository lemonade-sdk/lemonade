"""
Ollama API compatibility tests for Lemonade Server.

Tests the Ollama translation layer that allows Ollama clients to interact
with Lemonade's inference backends.

Usage:
    python test_ollama.py
    python test_ollama.py --server-per-test
    python test_ollama.py --server-binary /path/to/lemonade-server
"""

import base64
import json
import sys
import requests

try:
    import ollama as ollama_lib
except ImportError:
    ollama_lib = None

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    parse_args,
)
from utils.test_models import (
    PORT,
    ENDPOINT_TEST_MODEL,
    VISION_MODEL,
    SD_MODEL,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)

OLLAMA_BASE_URL = f"http://localhost:{PORT}"


class OllamaTests(ServerTestBase):
    """Tests for Ollama-compatible API endpoints."""

    _model_pulled = False

    @classmethod
    def setUpClass(cls):
        """Set up class - start server and ensure test model is pulled."""
        super().setUpClass()

    def get_ollama_client(self):
        """Get an Ollama client pointed at the test server."""
        if ollama_lib is None:
            self.skipTest("ollama package not installed")
        return ollama_lib.Client(host=OLLAMA_BASE_URL)

    def ensure_model_pulled(self):
        """Ensure the test model is downloaded."""
        if not OllamaTests._model_pulled:
            response = requests.post(
                f"{self.base_url}/pull",
                json={"model_name": ENDPOINT_TEST_MODEL, "stream": False},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(response.status_code, 200)
            OllamaTests._model_pulled = True

    # ========================================================================
    # Basic endpoint tests (no model required)
    # ========================================================================

    def test_001_version(self):
        """Test /api/version returns a version string."""
        response = requests.get(
            f"{OLLAMA_BASE_URL}/api/version",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertIn("version", data)
        self.assertEqual(data["version"], "0.16.1")

    def test_002_root_endpoint(self):
        """Test / is reachable (serves the web app UI)."""
        response = requests.get(
            f"{OLLAMA_BASE_URL}/",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

    def test_003_tags(self):
        """Test /api/tags returns model list."""
        self.ensure_model_pulled()
        response = requests.get(
            f"{OLLAMA_BASE_URL}/api/tags",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertIn("models", data)
        self.assertIsInstance(data["models"], list)

        # Each model should have expected Ollama fields
        if len(data["models"]) > 0:
            model = data["models"][0]
            self.assertIn("name", model)
            self.assertIn("model", model)
            self.assertIn("size", model)
            self.assertIn("details", model)
            self.assertIn("digest", model)

    def test_004_show(self):
        """Test /api/show returns model info."""
        self.ensure_model_pulled()
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/show",
            json={"name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertIn("details", data)
        self.assertIn("modelfile", data)
        self.assertIn("model_info", data)

    def test_005_show_not_found(self):
        """Test /api/show returns 404 for non-existent model."""
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/show",
            json={"name": "nonexistent-model-xyz"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 404)

    def test_006_ps(self):
        """Test /api/ps returns running models with correct Ollama format."""
        self.ensure_model_pulled()

        # Load a model first so /api/ps has something to return
        requests.post(
            f"{OLLAMA_BASE_URL}/api/chat",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "messages": [{"role": "user", "content": "Hi"}],
                "stream": False,
                "options": {"num_predict": 1},
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        response = requests.get(
            f"{OLLAMA_BASE_URL}/api/ps",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertIn("models", data)
        self.assertIsInstance(data["models"], list)

        # Should have at least one loaded model
        self.assertGreater(
            len(data["models"]), 0, "Expected at least one running model"
        )

        model = data["models"][0]
        # Verify required Ollama fields
        self.assertIn("name", model)
        self.assertIn("model", model)
        self.assertIn("expires_at", model)
        self.assertIn("size_vram", model)
        self.assertIn("details", model)

        # Model name must not be empty (regression: was ":latest" due to field mismatch)
        self.assertTrue(
            model["name"].replace(":latest", "") != "",
            f"Model name should not be empty, got: {model['name']}",
        )
        self.assertTrue(
            model["name"].endswith(":latest"),
            f"Model name should end with ':latest', got: {model['name']}",
        )

    def test_007_pull_streaming_progress(self):
        """Test /api/pull streams NDJSON progress with digest field."""
        self.ensure_model_pulled()
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/pull",
            json={"name": ENDPOINT_TEST_MODEL, "stream": True},
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200)

        chunks = []
        for line in response.iter_lines():
            if line:
                chunk = json.loads(line.decode("utf-8"))
                chunks.append(chunk)

        self.assertGreater(len(chunks), 0, "Expected at least one progress chunk")

        # First chunk should be "pulling manifest"
        self.assertEqual(chunks[0]["status"], "pulling manifest")

        # Last chunk should be "success"
        self.assertEqual(chunks[-1]["status"], "success")

        # Any downloading chunk must have digest, completed, and total fields
        download_chunks = [c for c in chunks if "completed" in c]
        for chunk in download_chunks:
            self.assertIn(
                "digest", chunk, "Download progress must include 'digest' field"
            )
            self.assertIn("total", chunk)
            self.assertIn("completed", chunk)

    def test_008_unload_via_generate(self):
        """Test model unload via /api/generate with keep_alive=0 (Ollama convention)."""
        self.ensure_model_pulled()

        # Load the model first
        requests.post(
            f"{OLLAMA_BASE_URL}/api/generate",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "prompt": "Hi",
                "stream": False,
                "options": {"num_predict": 1},
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Verify model is loaded
        ps_response = requests.get(f"{OLLAMA_BASE_URL}/api/ps", timeout=TIMEOUT_DEFAULT)
        loaded_names = [m["name"] for m in ps_response.json()["models"]]
        self.assertTrue(
            any(ENDPOINT_TEST_MODEL in n for n in loaded_names),
            "Model should be loaded before unload test",
        )

        # Unload via empty prompt + keep_alive=0
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/generate",
            json={"model": ENDPOINT_TEST_MODEL, "prompt": "", "keep_alive": 0},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertTrue(data["done"])
        self.assertEqual(data["done_reason"], "unload")

        # Verify model is no longer loaded
        ps_response = requests.get(f"{OLLAMA_BASE_URL}/api/ps", timeout=TIMEOUT_DEFAULT)
        loaded_names = [m["name"] for m in ps_response.json()["models"]]
        self.assertFalse(
            any(ENDPOINT_TEST_MODEL in n for n in loaded_names),
            "Model should be unloaded after keep_alive=0",
        )

    # ========================================================================
    # Chat completion tests
    # ========================================================================

    def test_009_chat_non_streaming(self):
        """Test /api/chat non-streaming."""
        self.ensure_model_pulled()
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/chat",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "messages": [{"role": "user", "content": "Say hello"}],
                "stream": False,
                "options": {"num_predict": 10},
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertIn("message", data)
        self.assertIn("content", data["message"])
        self.assertEqual(data["message"]["role"], "assistant")
        self.assertTrue(data["done"])
        self.assertEqual(data["model"], ENDPOINT_TEST_MODEL)

    def test_010_chat_streaming(self):
        """Test /api/chat streaming returns NDJSON."""
        self.ensure_model_pulled()
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/chat",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "messages": [{"role": "user", "content": "Say hello"}],
                "stream": True,
                "options": {"num_predict": 10},
            },
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200)

        chunks = []
        for line in response.iter_lines():
            if line:
                chunk = json.loads(line.decode("utf-8"))
                chunks.append(chunk)
                self.assertIn("model", chunk)

        # Should have at least one chunk and a final done=true chunk
        self.assertGreater(len(chunks), 0)
        last_chunk = chunks[-1]
        self.assertTrue(last_chunk.get("done", False))

    def test_011_chat_missing_model(self):
        """Test /api/chat returns 400 when model is missing."""
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/chat",
            json={
                "messages": [{"role": "user", "content": "hello"}],
                "stream": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400)

    def test_012_chat_not_found_model(self):
        """Test /api/chat returns 404 for non-existent model."""
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/chat",
            json={
                "model": "nonexistent-model-xyz",
                "messages": [{"role": "user", "content": "hello"}],
                "stream": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 404)

    def test_013_chat_with_latest_suffix(self):
        """Test /api/chat strips :latest suffix from model name."""
        self.ensure_model_pulled()
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/chat",
            json={
                "model": f"{ENDPOINT_TEST_MODEL}:latest",
                "messages": [{"role": "user", "content": "Say hello"}],
                "stream": False,
                "options": {"num_predict": 10},
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertTrue(data["done"])

    # ========================================================================
    # Generate (completion) tests
    # ========================================================================

    def test_014_generate_non_streaming(self):
        """Test /api/generate non-streaming."""
        self.ensure_model_pulled()
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/generate",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "prompt": "Hello, how are you?",
                "stream": False,
                "options": {"num_predict": 10},
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertIn("response", data)
        self.assertTrue(data["done"])
        self.assertEqual(data["model"], ENDPOINT_TEST_MODEL)

    def test_015_generate_streaming(self):
        """Test /api/generate streaming returns NDJSON."""
        self.ensure_model_pulled()
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/generate",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "prompt": "Hello",
                "stream": True,
                "options": {"num_predict": 10},
            },
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200)

        chunks = []
        for line in response.iter_lines():
            if line:
                chunk = json.loads(line.decode("utf-8"))
                chunks.append(chunk)
                self.assertIn("model", chunk)

        self.assertGreater(len(chunks), 0)
        last_chunk = chunks[-1]
        self.assertTrue(last_chunk.get("done", False))

    # ========================================================================
    # 501 stubs
    # ========================================================================

    def test_016_create_returns_501(self):
        """Test /api/create returns 501."""
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/create",
            json={"name": "test"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 501)

    def test_017_copy_returns_501(self):
        """Test /api/copy returns 501."""
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/copy",
            json={"source": "a", "destination": "b"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 501)

    def test_018_push_returns_501(self):
        """Test /api/push returns 501."""
        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/push",
            json={"name": "test"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 501)

    # ========================================================================
    # Ollama Python library tests (if available)
    # ========================================================================

    def test_019_ollama_lib_list(self):
        """Test ollama.list() via Python library."""
        client = self.get_ollama_client()
        self.ensure_model_pulled()
        result = client.list()
        self.assertIsNotNone(result)

    def test_020_ollama_lib_chat(self):
        """Test ollama.chat() via Python library."""
        client = self.get_ollama_client()
        self.ensure_model_pulled()
        result = client.chat(
            model=ENDPOINT_TEST_MODEL,
            messages=[{"role": "user", "content": "Say hello in 5 words"}],
            options={"num_predict": 10},
        )
        self.assertIsNotNone(result)
        self.assertIn("message", result)
        self.assertIn("content", result["message"])

    # ========================================================================
    # Image input/output tests
    # ========================================================================

    def test_021_chat_with_image_input(self):
        """Test /api/chat with image input (vision) using a vision model."""
        if sys.platform == "darwin":
            self.skipTest("Vision model not supported on macOS")
        # Pull the vision model
        response = requests.post(
            f"{self.base_url}/pull",
            json={"model_name": VISION_MODEL, "stream": False},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # 1x1 red PNG (smallest valid PNG)
        png_b64 = (
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4"
            "2mP8/58BAwAI/AL+hc2rNAAAAABJRU5ErkJggg=="
        )

        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/chat",
            json={
                "model": VISION_MODEL,
                "messages": [
                    {
                        "role": "user",
                        "content": "What is in this image?",
                        "images": [png_b64],
                    }
                ],
                "stream": False,
                "options": {"num_predict": 10},
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertIn("message", data)
        self.assertIn("content", data["message"])

    def test_022_generate_image_output(self):
        """Test /api/generate with an image generation model."""
        if sys.platform == "darwin":
            self.skipTest("sd-cpp not supported on macOS")
        # Pull the SD model first
        response = requests.post(
            f"{self.base_url}/pull",
            json={"model_name": SD_MODEL, "stream": False},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        response = requests.post(
            f"{OLLAMA_BASE_URL}/api/generate",
            json={
                "model": SD_MODEL,
                "prompt": "A red circle",
                "stream": False,
                "width": 256,
                "height": 256,
                "steps": 2,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertIn("image", data)
        self.assertTrue(len(data["image"]) > 0, "image field should not be empty")
        self.assertTrue(data["done"])
        self.assertEqual(data["model"], SD_MODEL)

        # Decode base64 and verify PNG magic bytes
        image_bytes = base64.b64decode(data["image"])
        self.assertTrue(
            image_bytes[:4] == b"\x89PNG",
            "Decoded image should start with PNG magic bytes",
        )

    def test_023_ollama_lib_chat_streaming(self):
        """Test ollama.chat() streaming via Python library."""
        client = self.get_ollama_client()
        self.ensure_model_pulled()
        stream = client.chat(
            model=ENDPOINT_TEST_MODEL,
            messages=[{"role": "user", "content": "Say hello"}],
            stream=True,
            options={"num_predict": 10},
        )
        chunks = list(stream)
        self.assertGreater(len(chunks), 0)


if __name__ == "__main__":
    parse_args()
    run_server_tests(OllamaTests, "OLLAMA API TESTS")
