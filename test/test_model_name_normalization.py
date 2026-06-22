"""
Regression tests for model name normalization (stripping :latest suffix).

These tests verify that both /v1/chat/completions and /v1/completions endpoints
correctly handle model names with the :latest suffix (Ollama/Docker convention).

The tests use the ENDPOINT_TEST_MODEL which should already be available in CI.

Usage:
    python test/test_model_name_normalization.py --cli-binary ./build/lemonade
"""

import requests
from utils.server_base import ServerTestBase, run_server_tests, pull_model_with_retry
from utils.test_models import PORT, TIMEOUT_DEFAULT, ENDPOINT_TEST_MODEL


class ModelNameNormalizationTest(ServerTestBase):
    """Test that :latest suffix is stripped from model names."""

    BASE_URL = f"http://localhost:{PORT}"
    TEST_MODEL = ENDPOINT_TEST_MODEL  # Use the standard test model
    TEST_MODEL_WITH_SUFFIX = f"{TEST_MODEL}:latest"

    @classmethod
    def setUpClass(cls):
        """Ensure test model is available."""
        super().setUpClass()
        # ENDPOINT_TEST_MODEL should already be pulled by earlier tests in CI
        # If running standalone, pull it now
        pull_model_with_retry(cls.TEST_MODEL)

    def test_chat_completions_with_latest_suffix(self):
        """Test /v1/chat/completions with model:latest suffix."""
        url = f"{self.BASE_URL}/v1/chat/completions"

        payload = {
            "model": self.TEST_MODEL_WITH_SUFFIX,
            "messages": [{"role": "user", "content": "Say hello"}],
            "max_tokens": 10,
            "stream": False,
        }

        response = requests.post(url, json=payload, timeout=TIMEOUT_DEFAULT)

        # Should succeed (200) and not return model_not_found error
        self.assertEqual(
            response.status_code,
            200,
            f"Expected 200 but got {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertNotIn(
            "error", result, f"Expected successful response but got error: {result}"
        )
        self.assertIn("choices", result, "Response should contain choices")

    def test_chat_completions_streaming_with_latest_suffix(self):
        """Test /v1/chat/completions streaming with model:latest suffix."""
        url = f"{self.BASE_URL}/v1/chat/completions"

        payload = {
            "model": self.TEST_MODEL_WITH_SUFFIX,
            "messages": [{"role": "user", "content": "Say hello"}],
            "max_tokens": 10,
            "stream": True,
        }

        response = requests.post(
            url, json=payload, stream=True, timeout=TIMEOUT_DEFAULT
        )

        # Should start streaming successfully
        self.assertEqual(
            response.status_code, 200, f"Expected 200 but got {response.status_code}"
        )

        # Verify we get SSE data without errors
        chunks = []
        for line in response.iter_lines():
            if line:
                line_str = line.decode("utf-8")
                if line_str.startswith("data: "):
                    chunks.append(line_str)
                    # Check that chunks don't contain error messages
                    self.assertNotIn(
                        "error",
                        line_str.lower(),
                        f"Streaming chunk should not contain error: {line_str}",
                    )
                    if len(chunks) >= 3:  # Get a few chunks
                        break

        self.assertGreater(len(chunks), 0, "Should receive at least one SSE data chunk")

    def test_completions_with_latest_suffix(self):
        """Test /v1/completions with model:latest suffix."""
        url = f"{self.BASE_URL}/v1/completions"

        payload = {
            "model": self.TEST_MODEL_WITH_SUFFIX,
            "prompt": "Hello",
            "max_tokens": 10,
            "stream": False,
        }

        response = requests.post(url, json=payload, timeout=TIMEOUT_DEFAULT)

        # Should succeed (200) and not return model_not_found error
        self.assertEqual(
            response.status_code,
            200,
            f"Expected 200 but got {response.status_code}: {response.text}",
        )

        result = response.json()
        self.assertNotIn(
            "error", result, f"Expected successful response but got error: {result}"
        )
        self.assertIn("choices", result, "Response should contain choices")

    def test_completions_streaming_with_latest_suffix(self):
        """Test /v1/completions streaming with model:latest suffix."""
        url = f"{self.BASE_URL}/v1/completions"

        payload = {
            "model": self.TEST_MODEL_WITH_SUFFIX,
            "prompt": "Hello",
            "max_tokens": 10,
            "stream": True,
        }

        response = requests.post(
            url, json=payload, stream=True, timeout=TIMEOUT_DEFAULT
        )

        # Should start streaming successfully
        self.assertEqual(
            response.status_code, 200, f"Expected 200 but got {response.status_code}"
        )

        # Verify we get SSE data without errors
        chunks = []
        for line in response.iter_lines():
            if line:
                line_str = line.decode("utf-8")
                if line_str.startswith("data: "):
                    chunks.append(line_str)
                    # Check that chunks don't contain error messages
                    self.assertNotIn(
                        "error",
                        line_str.lower(),
                        f"Streaming chunk should not contain error: {line_str}",
                    )
                    if len(chunks) >= 3:
                        break

        self.assertGreater(len(chunks), 0, "Should receive at least one SSE data chunk")

    def test_without_latest_suffix_still_works(self):
        """Verify model without :latest suffix still works (backward compatibility)."""
        url = f"{self.BASE_URL}/v1/chat/completions"

        payload = {
            "model": self.TEST_MODEL,  # No :latest suffix
            "messages": [{"role": "user", "content": "Say hello"}],
            "max_tokens": 10,
            "stream": False,
        }

        response = requests.post(url, json=payload, timeout=TIMEOUT_DEFAULT)

        self.assertEqual(
            response.status_code,
            200,
            f"Model without :latest should still work: {response.text}",
        )

        result = response.json()
        self.assertNotIn("error", result)
        self.assertIn("choices", result)


def main():
    run_server_tests(
        ModelNameNormalizationTest,
        description="MODEL NAME NORMALIZATION TESTS",
    )


if __name__ == "__main__":
    main()
