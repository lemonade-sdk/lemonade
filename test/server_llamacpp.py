"""
Usage: python server_llamacpp.py [backend] [--offline]

This will launch the lemonade server with the specified LlamaCPP backend,
query it in openai mode, and make sure that the response is valid for GGUF/LlamaCPP models.

Backend options:
    vulkan  - Use Vulkan backend (python server_llamacpp.py vulkan)
    rocm    - Use ROCm backend (python server_llamacpp.py rocm)

Examples:
    python server_llamacpp.py vulkan
    python server_llamacpp.py rocm --offline

If --offline is provided, tests will run in offline mode to ensure
the server works without network connectivity.

If you get the `ImportError: cannot import name 'TypeIs' from 'typing_extensions'` error:
    1. pip uninstall typing_extensions
    2. pip install openai
"""

import sys
import asyncio
import requests
import numpy as np

# Import all shared functionality from utils/server_base.py
from utils.server_base import (
    ServerTestingBase,
    run_server_tests_with_class,
    OpenAI,
    AsyncOpenAI,
    httpx,
)


class LlamaCppTesting(ServerTestingBase):
    """Testing class for GGUF/LlamaCPP models that inherits shared functionality."""

    # Default llamacpp backend - can be overridden by subclasses or environment variable
    llamacpp_backend = (
        None  # Set to None by default, can be 'vulkan', 'rocm', 'cpu', etc.
    )

    def __init__(self, *args, **kwargs):
        # Allow setting backend via environment variable for easy testing
        import os

        if os.getenv("LLAMACPP_BACKEND"):
            self.llamacpp_backend = os.getenv("LLAMACPP_BACKEND")
        super().__init__(*args, **kwargs)

    def setUp(self):
        """Call parent setUp but with GGUF-specific messaging."""
        backend_msg = (
            f" with {self.llamacpp_backend} backend" if self.llamacpp_backend else ""
        )
        print(f"\n=== Starting new GGUF/LlamaCPP test{backend_msg} ===")
        super().setUp()

    def cleanup_lemonade(self, server_subprocess):
        """Call parent cleanup but with GGUF-specific messaging."""
        print("\n=== Cleaning up GGUF/LlamaCPP test ===")
        super().cleanup_lemonade(server_subprocess)

    def test_000_get_hip_devices_returns_zero(self):
        """ROCm-only: get_hip_devices should report zero devices in CI."""
        if self.llamacpp_backend != "rocm":
            return

        from lemonade.tools.llamacpp.utils import get_hip_devices

        expected_devices = (
            [[0, "AMD Radeon(TM) 8060S Graphics"]]
            if sys.platform.startswith("win")
            else [[0, "AMD Radeon Graphics"]]
        )
        devices = get_hip_devices()
        assert (
            devices == expected_devices
        ), f"Expected {expected_devices} devices, got {devices}"

    # Endpoint: /api/v1/chat/completions
    # def test_001_test_llamacpp_chat_completion_streaming(self):
    #     client = OpenAI(
    #         base_url=self.base_url,
    #         api_key="lemonade",  # required, but unused
    #     )

    #     stream = client.chat.completions.create(
    #         model="Qwen3-0.6B-GGUF",
    #         messages=self.messages,
    #         stream=True,
    #         max_completion_tokens=10,
    #     )

    #     complete_response = ""
    #     chunk_count = 0
    #     for chunk in stream:
    #         if chunk.choices[0].delta.content is not None:
    #             complete_response += chunk.choices[0].delta.content
    #             print(chunk.choices[0].delta.content, end="")
    #             chunk_count += 1

    #     assert chunk_count > 5
    #     assert len(complete_response) > 5

    # # Endpoint: /api/v1/chat/completions
    # def test_002_test_llamacpp_chat_completion_non_streaming(self):
    #     client = OpenAI(
    #         base_url=self.base_url,
    #         api_key="lemonade",  # required, but unused
    #     )

    #     response = client.chat.completions.create(
    #         model="Qwen3-0.6B-GGUF",
    #         messages=self.messages,
    #         stream=False,
    #         max_completion_tokens=10,
    #     )

    #     assert response.choices[0].message.content is not None
    #     assert len(response.choices[0].message.content) > 5
    #     print(response.choices[0].message.content)

    # # Endpoint: /api/v1/embeddings
    # def test_003_test_embeddings_with_gguf(self):
    #     client = OpenAI(
    #         base_url=self.base_url,
    #         api_key="lemonade",  # required, but unused
    #     )
    #     model_id = "nomic-embed-text-v2-moe-GGUF"

    #     # Test 1: Single string
    #     response = client.embeddings.create(
    #         input="Hello, how are you today?",
    #         model=model_id,
    #         encoding_format="float",
    #     )
    #     assert response.data is not None
    #     assert len(response.data) == 1
    #     assert response.data[0].embedding is not None
    #     assert len(response.data[0].embedding) > 0
    #     print(f"Single string embedding dimension: {len(response.data[0].embedding)}")

    #     # Test 2: Array of strings
    #     response = client.embeddings.create(
    #         input=["Hello world", "How are you?", "This is a test"],
    #         model=model_id,
    #         encoding_format="float",
    #     )
    #     assert response.data is not None
    #     assert len(response.data) == 3
    #     for i, embedding in enumerate(response.data):
    #         assert embedding.embedding is not None
    #         assert len(embedding.embedding) > 0
    #         print(f"Array embedding {i+1} dimension: {len(embedding.embedding)}")

    #     # Test 3: Base64 encoding format
    #     response = client.embeddings.create(
    #         input="Test base64 encoding",
    #         model=model_id,
    #         encoding_format="base64",
    #     )
    #     assert response.data is not None
    #     assert len(response.data) == 1
    #     assert response.data[0].embedding is not None
    #     assert len(response.data[0].embedding) > 0
    #     print(f"Base64 embedding length: {len(response.data[0].embedding)}")

    #     # Test 4: Token IDs (if supported by model)
    #     response = client.embeddings.create(
    #         input=[15496, 11, 1268, 527, 499, 3432, 30],
    #         model=model_id,
    #         encoding_format="float",
    #     )
    #     assert response.data is not None
    #     assert len(response.data) == 1
    #     print(f"Token embedding dimension: {len(response.data[0].embedding)}")

    #     # Test 5: Mixed input types (if supported by model)
    #     response = client.embeddings.create(
    #         input=[15496, "hello", 527, "world"],
    #         model=model_id,
    #         encoding_format="float",
    #     )
    #     assert response.data is not None
    #     print(f"Mixed input embedding dimension: {len(response.data[0].embedding)}")

    #     # Test 6: Semantic similarity comparison
    #     texts = [
    #         "The cat sat on the mat",
    #         "A feline rested on the carpet",
    #         "Dogs are loyal animals",
    #         "Python is a programming language",
    #     ]
    #     response = client.embeddings.create(
    #         input=texts,
    #         model=model_id,
    #         encoding_format="float",
    #     )
    #     assert response.data is not None
    #     assert len(response.data) == 4

    #     def cosine_similarity(a, b):
    #         return np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b))

    #     emb1 = np.array(response.data[0].embedding)
    #     emb2 = np.array(response.data[1].embedding)
    #     emb3 = np.array(response.data[2].embedding)

    #     sim_12 = cosine_similarity(emb1, emb2)
    #     sim_13 = cosine_similarity(emb1, emb3)

    #     print(f"Similarity cat/mat vs feline/carpet: {sim_12:.4f}")
    #     print(f"Similarity cat/mat vs dogs: {sim_13:.4f}")
    #     assert (
    #         sim_12 > sim_13
    #     ), f"Semantic similarity test failed: {sim_12:.4f} <= {sim_13:.4f}"

    # # Endpoint: /api/v1/reranking
    # def test_004_test_reranking_with_gguf(self):
    #     query = "A man is eating pasta."
    #     documents = [
    #         "A man is eating food.",  # index 0
    #         "The girl is carrying a baby.",  # index 1
    #         "A man is riding a horse.",  # index 2
    #         "A young girl is playing violin.",  # index 3
    #         "A man is eating a piece of bread.",  # index 4
    #         "A man is eating noodles.",  # index 5
    #     ]

    #     # Make the reranking request
    #     payload = {
    #         "query": query,
    #         "documents": documents,
    #         "model": "jina-reranker-v1-tiny-en-GGUF",
    #     }
    #     response = requests.post(f"{self.base_url}/reranking", json=payload)
    #     response.raise_for_status()
    #     result = response.json()

    #     # Sort results by score
    #     results = result.get("results", [])
    #     results.sort(key=lambda x: x.get("relevance_score", 0), reverse=True)

    #     # Get the indices of the top 3 ranked documents
    #     top_3_indices = [r["index"] for r in results[:3]]

    #     # The food-related documents should be in top 3 (indices 0, 4, and 5)
    #     expected_top_3 = {0, 4, 5}
    #     actual_top_3 = set(top_3_indices)

    #     assert (
    #         actual_top_3 == expected_top_3
    #     ), f"Expected food-related documents (indices {expected_top_3}) to be in top 3, but got {actual_top_3}"

    # def test_005_test_llamacpp_completions_non_streaming(self):
    #     """Test completion endpoint specifically with llamacpp model (non-streaming)"""
    #     client = OpenAI(
    #         base_url=self.base_url,
    #         api_key="lemonade",  # required, but unused
    #     )

    #     completion = client.completions.create(
    #         model="Qwen3-0.6B-GGUF",  # This will use llamacpp recipe
    #         prompt="Hello, how are you?",
    #         stream=False,
    #         max_tokens=20,
    #     )

    #     # Basic validation (same as existing completion tests)
    #     assert len(completion.choices[0].text) > 5
    #     assert completion.usage.prompt_tokens > 0
    #     assert completion.usage.completion_tokens > 0

    #     print(f"LlamaCPP completion: {completion.choices[0].text}")

    # def test_006_test_llamacpp_completions_streaming(self):
    #     """Test streaming completion endpoint specifically with llamacpp model"""
    #     client = OpenAI(
    #         base_url=self.base_url,
    #         api_key="lemonade",  # required, but unused
    #     )

    #     stream = client.completions.create(
    #         model="Qwen3-0.6B-GGUF",  # This will use llamacpp recipe
    #         prompt="def hello_world():",
    #         stream=True,
    #         max_tokens=20,
    #     )

    #     complete_response = ""
    #     chunk_count = 0
    #     for chunk in stream:
    #         if chunk.choices[0].text is not None:
    #             complete_response += chunk.choices[0].text
    #             print(chunk.choices[0].text, end="")
    #             chunk_count += 1

    #     assert chunk_count > 5
    #     assert len(complete_response) > 5

    def test_007_test_generation_parameters_with_llamacpp(self):
        """Test generation parameters across all endpoints with llamacpp models"""
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",
        )

        # Test configuration constants
        TEST_PROMPT = "The weather is sunny and"
        TEST_MESSAGES = [{"role": "user", "content": TEST_PROMPT}]
        MAX_TOKENS = 15
        LLAMACPP_MODEL = "Qwen3-0.6B-GGUF"

        # Base parameter values
        BASE_PARAMS = {
            "temperature": 0.7,
            "top_p": 0.9,
            "repeat_penalty": 1.1,
            "top_k": 40,
        }

        # Alternative parameter values for testing differences
        PARAM_VARIANTS = {
            "temperature": 0.1,
            "top_p": 0.1,
            "repeat_penalty": 2.0,
            "top_k": 1,
        }

        def make_request(endpoint_type, **params):
            """Helper function to make requests to different endpoints with given parameters."""
            extra_body = {
                "repeat_penalty": params.get(
                    "repeat_penalty", BASE_PARAMS["repeat_penalty"]
                ),
                "top_k": params.get("top_k", BASE_PARAMS["top_k"]),
            }

            if endpoint_type == "completions":
                response = client.completions.create(
                    model=LLAMACPP_MODEL,
                    prompt=TEST_PROMPT,
                    max_tokens=MAX_TOKENS,
                    temperature=params.get("temperature", BASE_PARAMS["temperature"]),
                    top_p=params.get("top_p", BASE_PARAMS["top_p"]),
                    extra_body=extra_body,
                )
                return response.choices[0].text

            elif endpoint_type == "chat_completions":
                response = client.chat.completions.create(
                    model=LLAMACPP_MODEL,
                    messages=TEST_MESSAGES,
                    max_completion_tokens=MAX_TOKENS,
                    temperature=params.get("temperature", BASE_PARAMS["temperature"]),
                    top_p=params.get("top_p", BASE_PARAMS["top_p"]),
                    extra_body=extra_body,
                )
                return response.choices[0].message.content

        # Test endpoints (responses endpoint not available for llamacpp)
        endpoints = ["completions", "chat_completions"]

        for endpoint in endpoints:
            print(f"\n--- Testing LlamaCPP {endpoint} endpoint ---")

            # Test 1: Identical parameters should produce identical outputs
            response1 = make_request(endpoint, **BASE_PARAMS)
            response2 = make_request(endpoint, **BASE_PARAMS)

            print(f"Identical params 1: {response1}")
            print(f"Identical params 2: {response2}")

            assert (
                response1 == response2
            ), f"{endpoint}: Identical parameters should produce identical outputs with locked seed"

            # Test 2: Each parameter should affect output when changed
            for param_name, variant_value in PARAM_VARIANTS.items():
                # Create modified parameters
                modified_params = BASE_PARAMS.copy()
                modified_params[param_name] = variant_value

                response_modified = make_request(endpoint, **modified_params)

                print(f"Modified {param_name}: {response_modified}")

                assert (
                    response_modified != response1
                ), f"{endpoint}: Different {param_name} should produce different outputs"


class LlamaCppVulkanTesting(LlamaCppTesting):
    """Testing class for GGUF/LlamaCPP models with Vulkan backend."""

    llamacpp_backend = "vulkan"


class LlamaCppRocmTesting(LlamaCppTesting):
    """Testing class for GGUF/LlamaCPP models with ROCm backend."""

    llamacpp_backend = "rocm"


if __name__ == "__main__":
    import sys
    import os

    # Simple command line parsing without argparse
    if len(sys.argv) < 2:
        print("Usage: python server_llamacpp.py [backend] [--offline]")
        print("Backend options: vulkan, rocm")
        print("Examples:")
        print("    python server_llamacpp.py vulkan")
        print("    python server_llamacpp.py rocm --offline")
        sys.exit(1)

    backend = sys.argv[1].lower()
    offline_mode = "--offline" in sys.argv

    # Select the appropriate test class based on backend
    if backend == "vulkan":
        test_class = LlamaCppVulkanTesting
        description = "GGUF/LLAMACPP VULKAN SERVER TESTS"
    elif backend == "rocm":
        test_class = LlamaCppRocmTesting
        description = "GGUF/LLAMACPP ROCM SERVER TESTS"
    else:
        print(
            f"Error: Unsupported backend '{backend}'. Supported backends: vulkan, rocm"
        )
        sys.exit(1)

    # Pass offline flag to test class if needed
    if offline_mode:
        # You can store this in a class variable or pass it to the test runner
        test_class.offline_mode = True

    # Use the shared test runner with explicit offline parameter to avoid argparse
    run_server_tests_with_class(test_class, description, offline=offline_mode)

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
