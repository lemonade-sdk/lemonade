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

# Import all shared functionality from utils/server_base.py
from utils.server_base import (
    ServerTestingBase,
    run_server_tests_with_class,
    OpenAI,
)


class FlmTesting(ServerTestingBase):
    """Testing class for FLM models that inherits shared functionality."""

    # Endpoint: /api/v1/chat/completions
    def test_001_test_llamacpp_chat_completion_streaming(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        stream = client.chat.completions.create(
            model="Llama-3.2-1B-FLM",
            messages=self.messages,
            stream=True,
            max_completion_tokens=10,
        )

        complete_response = ""
        chunk_count = 0
        for chunk in stream:
            if chunk.choices and chunk.choices[0].delta.content is not None:
                complete_response += chunk.choices[0].delta.content
                print(chunk.choices[0].delta.content, end="")
                chunk_count += 1

        assert chunk_count > 5
        assert len(complete_response) > 5

    # Endpoint: /api/v1/chat/completions
    def test_002_test_llamacpp_chat_completion_non_streaming(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        response = client.chat.completions.create(
            model="Llama-3.2-1B-FLM",
            messages=self.messages,
            stream=False,
            max_completion_tokens=10,
        )

        assert response.choices[0].message.content is not None
        assert len(response.choices[0].message.content) > 5
        print(response.choices[0].message.content)

    
if __name__ == "__main__":
    run_server_tests_with_class(FlmTesting, "SERVER TESTS")

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
