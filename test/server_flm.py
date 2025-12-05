"""
Usage:

1. Install flm
2. flm pull llama3.2:1b
3. python server_flm.py [--offline]

This will launch the lemonade server, query it in openai mode, and make
sure that the response is valid for FLM models.

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
    def test_001_test_flm_chat_completion_streaming(self):
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
        finish_reason = None
        for chunk in stream:
            if (
                chunk.choices
                and chunk.choices[0].delta
                and chunk.choices[0].delta.content is not None
            ):
                complete_response += chunk.choices[0].delta.content
                print(chunk.choices[0].delta.content, end="")
                chunk_count += 1
            # Track finish_reason from final chunk
            if chunk.choices and chunk.choices[0].finish_reason:
                finish_reason = chunk.choices[0].finish_reason

        assert chunk_count > 5, f"Expected >5 content chunks, got {chunk_count}"
        assert (
            len(complete_response) > 5
        ), f"Expected >5 chars in response, got {len(complete_response)}"
        assert finish_reason is not None, "Stream did not complete with finish_reason"


if __name__ == "__main__":
    run_server_tests_with_class(FlmTesting, "SERVER TESTS")

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
