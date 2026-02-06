"""
LLM, Embedding, and Reranking tests for Lemonade Server.

Consolidates tests from:
- test/server.py (ryzenai-server tests)
- test/server_llamacpp.py (llamacpp tests)
- test/server_multimodel.py (multi-model tests)
- test/server_flm.py (FLM tests)

Tests are decorated with @skip_if_unsupported() to skip features
not supported by the current wrapped server.

Usage:
    python server_llm.py --wrapped-server llamacpp --backend vulkan
    python server_llm.py --wrapped-server llamacpp --backend rocm
    python server_llm.py --wrapped-server ryzenai --backend cpu
    python server_llm.py --wrapped-server ryzenai --backend hybrid
    python server_llm.py --wrapped-server ryzenai --backend npu
    python server_llm.py --wrapped-server flm
"""

import asyncio
import time
import requests
import numpy as np

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    OpenAI,
    AsyncOpenAI,
    httpx,
)
from utils.capabilities import (
    skip_if_unsupported,
    get_test_model,
    get_capabilities,
    supports,
)
from utils.test_models import (
    PORT,
    STANDARD_MESSAGES,
    RESPONSES_MESSAGES,
    SIMPLE_MESSAGES,
    TEST_PROMPT,
    SAMPLE_TOOL,
    MULTI_MODEL_SECONDARY,
    MULTI_MODEL_TERTIARY,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


class LLMTests(ServerTestBase):
    """
    Tests for LLM inference, embeddings, and reranking.

    Each test is decorated with @skip_if_unsupported() to skip
    features not supported by the current wrapped server.
    """

    # Enable multi-model support (2 LLMs, 1 embedding, 1 reranking, 1 other)
    additional_server_args = ["--max-loaded-models", "2", "1", "1", "1"]

    # =========================================================================
    # CHAT COMPLETIONS TESTS
    # =========================================================================

    @skip_if_unsupported("chat_completions")
    def test_001_chat_completions_non_streaming(self):
        """Test non-streaming chat completion."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        completion = client.chat.completions.create(
            model=model,
            messages=self.messages,
            max_completion_tokens=10,
            stream=False,
        )

        print(f"Response: {completion.choices[0].message.content}")
        self.assertGreater(
            len(completion.choices[0].message.content),
            5,
            "Response should have content",
        )

        # Check usage fields
        self.assertGreater(completion.usage.prompt_tokens, 0)
        self.assertGreater(completion.usage.completion_tokens, 0)
        self.assertGreater(completion.usage.total_tokens, 0)
        self.assertEqual(
            completion.usage.total_tokens,
            completion.usage.prompt_tokens + completion.usage.completion_tokens,
        )

    @skip_if_unsupported("chat_completions_streaming")
    def test_002_chat_completions_streaming(self):
        """Test streaming chat completion."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        stream = client.chat.completions.create(
            model=model,
            messages=self.messages,
            stream=True,
            max_completion_tokens=10,
        )

        complete_response = ""
        chunk_count = 0
        for chunk in stream:
            if (
                chunk.choices
                and chunk.choices[0].delta
                and chunk.choices[0].delta.content is not None
            ):
                complete_response += chunk.choices[0].delta.content
                print(chunk.choices[0].delta.content, end="")
                chunk_count += 1

        print()  # newline after streaming output
        self.assertGreater(
            chunk_count, 2, f"Should have multiple chunks, got {chunk_count}"
        )
        self.assertGreater(len(complete_response), 5, "Response should have content")

    @skip_if_unsupported("chat_completions_async")
    async def test_003_chat_completions_streaming_async(self):
        """Test async streaming chat completion."""
        client = self.get_async_openai_client()
        model = self.get_test_model("llm")

        stream = await client.chat.completions.create(
            model=model,
            messages=self.messages,
            stream=True,
            max_completion_tokens=10,
        )

        complete_response = ""
        chunk_count = 0
        async for chunk in stream:
            if (
                chunk.choices
                and chunk.choices[0].delta
                and chunk.choices[0].delta.content is not None
            ):
                complete_response += chunk.choices[0].delta.content
                print(chunk.choices[0].delta.content, end="")
                chunk_count += 1

        print()
        self.assertGreater(chunk_count, 2)
        self.assertGreater(len(complete_response), 5)

    # =========================================================================
    # COMPLETIONS TESTS
    # =========================================================================

    @skip_if_unsupported("completions")
    def test_004_completions_non_streaming(self):
        """Test non-streaming completions endpoint."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        completion = client.completions.create(
            model=model,
            prompt=TEST_PROMPT,
            stream=False,
            max_tokens=10,
        )

        print(f"Response: {completion.choices[0].text}")
        self.assertGreater(len(completion.choices[0].text), 5)

        # Check usage fields
        self.assertGreater(completion.usage.prompt_tokens, 0)
        self.assertGreater(completion.usage.completion_tokens, 0)
        self.assertGreater(completion.usage.total_tokens, 0)

    @skip_if_unsupported("completions_streaming")
    def test_005_completions_streaming(self):
        """Test streaming completions endpoint."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        stream = client.completions.create(
            model=model,
            prompt=TEST_PROMPT,
            stream=True,
            max_tokens=10,
        )

        complete_response = ""
        chunk_count = 0
        for chunk in stream:
            if chunk.choices and chunk.choices[0].text is not None:
                complete_response += chunk.choices[0].text
                print(chunk.choices[0].text, end="")
                chunk_count += 1

        print()
        self.assertGreater(chunk_count, 2)
        self.assertGreater(len(complete_response), 5)

    @skip_if_unsupported("completions_async")
    async def test_006_completions_streaming_async(self):
        """Test async streaming completions endpoint."""
        client = self.get_async_openai_client()
        model = self.get_test_model("llm")

        stream = await client.completions.create(
            model=model,
            prompt=TEST_PROMPT,
            stream=True,
            max_tokens=10,
        )

        complete_response = ""
        chunk_count = 0
        async for chunk in stream:
            if chunk.choices and chunk.choices[0].text is not None:
                complete_response += chunk.choices[0].text
                print(chunk.choices[0].text, end="")
                chunk_count += 1

        print()
        self.assertGreater(chunk_count, 2)
        self.assertGreater(len(complete_response), 5)

    # =========================================================================
    # RESPONSES API TESTS
    # =========================================================================

    @skip_if_unsupported("responses_api")
    def test_007_responses_api(self):
        """Test the Responses API endpoint."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        response = client.responses.create(
            model=model,
            input=RESPONSES_MESSAGES,
            stream=False,
            temperature=0.0,
            max_output_tokens=10,
        )

        print(f"Response: {response.output[0].content[0].text}")
        self.assertGreater(len(response.output[0].content[0].text), 5)

    @skip_if_unsupported("responses_api_streaming")
    def test_008_responses_api_streaming(self):
        """Test the Responses API endpoint with streaming."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        stream = client.responses.create(
            model=model,
            input=RESPONSES_MESSAGES,
            stream=True,
            temperature=0.0,
            max_output_tokens=10,
        )

        complete_response = ""
        event_count = 0
        last_event_type = ""

        for event in stream:
            if event_count == 0:
                self.assertEqual(
                    event.type,
                    "response.created",
                    f"Expected first event to be response.created, got {event.type}",
                )
            elif event.type == "response.output_text.delta":
                complete_response += event.delta
                print(event.delta, end="")
            elif event.type == "response.completed":
                self.assertEqual(
                    event.response.output[0].content[0].text,
                    complete_response,
                    "Complete response should match streamed response",
                )

            event_count += 1
            last_event_type = event.type

        print()
        self.assertEqual(last_event_type, "response.completed")
        self.assertGreater(len(complete_response), 5)

    # =========================================================================
    # PARAMETER TESTS
    # =========================================================================

    @skip_if_unsupported("stop_parameter")
    def test_009_completions_with_stop(self):
        """Test completions with stop parameter."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        completion = client.completions.create(
            model=model,
            prompt="Just say 'I am Joe and I like apples'. Here we go: 'I am Joe and",
            stop=["apples"],
            max_tokens=10,
        )

        print(f"Response: {completion.choices[0].text}")
        self.assertGreater(len(completion.choices[0].text), 2)
        self.assertNotIn("apples", completion.choices[0].text)

    @skip_if_unsupported("stop_parameter")
    def test_010_chat_completions_with_stop(self):
        """Test chat completions with stop parameter."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        # Ask for a numbered list - stop on "2" to get just the first item
        messages = [
            {"role": "user", "content": "List 5 colors, one per line, numbered 1-5."},
        ]

        completion = client.chat.completions.create(
            model=model,
            messages=messages,
            stop=["2."],  # Stop before the second item
            max_completion_tokens=50,
        )

        response = completion.choices[0].message.content
        print(f"Response: {response}")

        # Should have some content (at least "1. <color>")
        self.assertGreater(len(response), 2)
        # Should not contain "2." since we stopped there
        self.assertNotIn("2.", response)

    @skip_if_unsupported("echo_parameter")
    def test_011_completions_with_echo(self):
        """Test completions with echo parameter."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        prompt = "Hello, how are you?"
        completion = client.completions.create(
            model=model,
            prompt=prompt,
            echo=True,
            max_tokens=10,
        )

        print(f"Response: {completion.choices[0].text}")
        self.assertTrue(
            completion.choices[0].text.startswith(prompt),
            "Response should start with prompt when echo=True",
        )
        self.assertGreater(len(completion.choices[0].text), len(prompt))

    # =========================================================================
    # TOOL CALLS TESTS
    # =========================================================================

    @skip_if_unsupported("tool_calls")
    def test_012_chat_completions_with_tool_calls(self):
        """Test chat completions with tool calls."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        completion = client.chat.completions.create(
            model=model,
            messages=[
                {
                    "role": "user",
                    "content": "Run the calculator_calculate tool with expression set to 1+1",
                }
            ],
            tools=[SAMPLE_TOOL],
            max_completion_tokens=50,
        )

        tool_calls = getattr(completion.choices[0].message, "tool_calls", None)
        self.assertIsNotNone(tool_calls, "Response should have tool_calls")
        self.assertEqual(len(tool_calls), 1)

    @skip_if_unsupported("tool_calls_streaming")
    def test_013_chat_completions_with_tool_calls_streaming(self):
        """Test streaming chat completions with tool calls."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        stream = client.chat.completions.create(
            model=model,
            messages=[
                {
                    "role": "user",
                    "content": "Run the calculator_calculate tool with expression set to 1+1",
                }
            ],
            tools=[SAMPLE_TOOL],
            max_completion_tokens=50,
            stream=True,
        )

        tool_call_count = 0
        for chunk in stream:
            delta = (
                chunk.choices[0].delta
                if chunk.choices and len(chunk.choices) > 0
                else None
            )
            if delta and delta.tool_calls:
                for tool_call in delta.tool_calls:
                    print(tool_call)
                    tool_call_count += 1

        self.assertGreater(tool_call_count, 0, "Should receive tool call chunks")

    # =========================================================================
    # GENERATION PARAMETERS TESTS
    # =========================================================================

    @skip_if_unsupported("generation_parameters")
    def test_014_generation_parameters(self):
        """Test that generation parameters affect output."""
        client = self.get_openai_client()
        model = self.get_test_model("llm")

        test_prompt = "The weather is sunny and"
        test_messages = [{"role": "user", "content": test_prompt}]
        max_tokens = 15

        base_params = {
            "temperature": 0.7,
            "top_p": 0.9,
            "repeat_penalty": 1.1,
            "top_k": 40,
        }

        param_variants = {
            "temperature": 0.1,
            "top_p": 0.1,
            "repeat_penalty": 2.0,
            "top_k": 1,
        }

        def make_request(**params):
            extra_body = {
                "repeat_penalty": params.get(
                    "repeat_penalty", base_params["repeat_penalty"]
                ),
                "top_k": params.get("top_k", base_params["top_k"]),
            }
            response = client.chat.completions.create(
                model=model,
                messages=test_messages,
                max_completion_tokens=max_tokens,
                temperature=params.get("temperature", base_params["temperature"]),
                top_p=params.get("top_p", base_params["top_p"]),
                extra_body=extra_body,
            )
            return response.choices[0].message.content

        # Test identical params produce identical outputs
        response1 = make_request(**base_params)
        response2 = make_request(**base_params)

        print(f"Identical params 1: {response1}")
        print(f"Identical params 2: {response2}")

        self.assertEqual(
            response1,
            response2,
            "Identical parameters should produce identical outputs",
        )

        # Test that changing params produces different outputs
        for param_name, variant_value in param_variants.items():
            modified_params = base_params.copy()
            modified_params[param_name] = variant_value

            response_modified = make_request(**modified_params)
            print(f"Modified {param_name}: {response_modified}")

            self.assertNotEqual(
                response_modified,
                response1,
                f"Different {param_name} should produce different outputs",
            )

    # =========================================================================
    # EMBEDDINGS TESTS
    # =========================================================================

    @skip_if_unsupported("embeddings")
    def test_015_embeddings_single_string(self):
        """Test embeddings with a single string input."""
        client = self.get_openai_client()
        model = self.get_test_model("embedding")

        response = client.embeddings.create(
            input="Hello, how are you today?",
            model=model,
            encoding_format="float",
        )

        self.assertIsNotNone(response.data)
        self.assertEqual(len(response.data), 1)
        self.assertIsNotNone(response.data[0].embedding)
        self.assertGreater(len(response.data[0].embedding), 0)
        print(f"Embedding dimension: {len(response.data[0].embedding)}")

    @skip_if_unsupported("embeddings")
    def test_016_embeddings_array_of_strings(self):
        """Test embeddings with array of strings."""
        client = self.get_openai_client()
        model = self.get_test_model("embedding")

        response = client.embeddings.create(
            input=["Hello world", "How are you?", "This is a test"],
            model=model,
            encoding_format="float",
        )

        self.assertIsNotNone(response.data)
        self.assertEqual(len(response.data), 3)
        for i, embedding in enumerate(response.data):
            self.assertIsNotNone(embedding.embedding)
            self.assertGreater(len(embedding.embedding), 0)
            print(f"Embedding {i+1} dimension: {len(embedding.embedding)}")

    @skip_if_unsupported("embeddings")
    def test_017_embeddings_semantic_similarity(self):
        """Test that semantically similar texts have similar embeddings."""
        client = self.get_openai_client()
        model = self.get_test_model("embedding")

        texts = [
            "The cat sat on the mat",
            "A feline rested on the carpet",
            "Dogs are loyal animals",
            "Python is a programming language",
        ]

        response = client.embeddings.create(
            input=texts,
            model=model,
            encoding_format="float",
        )

        self.assertEqual(len(response.data), 4)

        def cosine_similarity(a, b):
            return np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b))

        emb1 = np.array(response.data[0].embedding)
        emb2 = np.array(response.data[1].embedding)
        emb3 = np.array(response.data[2].embedding)

        sim_12 = cosine_similarity(emb1, emb2)
        sim_13 = cosine_similarity(emb1, emb3)

        print(f"Similarity cat/mat vs feline/carpet: {sim_12:.4f}")
        print(f"Similarity cat/mat vs dogs: {sim_13:.4f}")

        self.assertGreater(
            sim_12,
            sim_13,
            f"Semantic similarity test failed: {sim_12:.4f} <= {sim_13:.4f}",
        )

    # =========================================================================
    # RERANKING TESTS
    # =========================================================================

    @skip_if_unsupported("reranking")
    def test_018_reranking(self):
        """Test document reranking."""
        model = self.get_test_model("reranking")

        query = "A man is eating pasta."
        documents = [
            "A man is eating food.",
            "The girl is carrying a baby.",
            "A man is riding a horse.",
            "A young girl is playing violin.",
            "A man is eating a piece of bread.",
            "A man is eating noodles.",
        ]

        payload = {
            "query": query,
            "documents": documents,
            "model": model,
        }

        response = requests.post(
            f"{self.base_url}/reranking", json=payload, timeout=TIMEOUT_MODEL_OPERATION
        )
        response.raise_for_status()
        result = response.json()

        results = result.get("results", [])
        results.sort(key=lambda x: x.get("relevance_score", 0), reverse=True)

        top_3_indices = [r["index"] for r in results[:3]]
        expected_top_3 = {0, 4, 5}  # Food-related documents
        actual_top_3 = set(top_3_indices)

        print(f"Top 3 indices: {top_3_indices}")
        self.assertEqual(
            actual_top_3,
            expected_top_3,
            f"Expected food-related documents {expected_top_3} in top 3, got {actual_top_3}",
        )

    # =========================================================================
    # MULTI-MODEL TESTS
    # =========================================================================

    @skip_if_unsupported("multi_model")
    def test_019_multi_model_load(self):
        """Test loading multiple models simultaneously."""
        requests.post(f"{self.base_url}/unload", json={}, timeout=TIMEOUT_DEFAULT)

        model1 = self.get_test_model("llm")
        model2 = MULTI_MODEL_SECONDARY

        # Load first model
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": model1},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # Load second model
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": model2},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # Check health shows both models loaded
        health_response = requests.get(
            f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(health_response.status_code, 200)
        health_data = health_response.json()

        self.assertIn("all_models_loaded", health_data)
        self.assertEqual(len(health_data["all_models_loaded"]), 2)

        # Verify both models are present
        model_names = {m["model_name"] for m in health_data["all_models_loaded"]}
        self.assertIn(model1, model_names)
        self.assertIn(model2, model_names)

    @skip_if_unsupported("multi_model")
    def test_020_multi_model_unload_specific(self):
        """Test unloading a specific model by name."""
        requests.post(f"{self.base_url}/unload", json={}, timeout=TIMEOUT_DEFAULT)

        model = self.get_test_model("llm")

        # Load a model
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": model},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Unload specific model
        response = requests.post(
            f"{self.base_url}/unload",
            json={"model_name": model},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

        result = response.json()
        self.assertEqual(result["status"], "success")

    @skip_if_unsupported("multi_model")
    def test_021_lru_eviction(self):
        """Test LRU eviction when loading a third model with max_llm_models=2."""
        requests.post(f"{self.base_url}/unload", json={}, timeout=TIMEOUT_DEFAULT)

        model1 = self.get_test_model("llm")
        model2 = MULTI_MODEL_SECONDARY
        model3 = MULTI_MODEL_TERTIARY

        # Load first two models (fills the limit)
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": model1},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        time.sleep(1)
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": model2},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        time.sleep(1)

        # Verify both are loaded
        response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 2)

        # Access model2 to make it more recent than model1
        requests.post(
            f"{self.base_url}/chat/completions",
            json={
                "model": model2,
                "messages": [{"role": "user", "content": "Hi"}],
                "max_tokens": 5,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        time.sleep(1)

        # Load third model (should evict model1 as it's LRU)
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": model3},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        time.sleep(1)

        # Verify only 2 models loaded and model1 was evicted
        response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 2)

        model_names = {m["model_name"] for m in data["all_models_loaded"]}
        self.assertIn(model2, model_names)
        self.assertIn(model3, model_names)
        self.assertNotIn(model1, model_names)

    @skip_if_unsupported("multi_model")
    def test_022_unload_all_models(self):
        """Test unloading all models without specifying model_name."""
        requests.post(f"{self.base_url}/unload", json={}, timeout=TIMEOUT_DEFAULT)

        model = self.get_test_model("llm")

        # Load a model
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": model},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Unload all (no model_name parameter)
        response = requests.post(
            f"{self.base_url}/unload", json={}, timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(response.status_code, 200)
        result = response.json()
        self.assertEqual(result["status"], "success")

        # Verify all unloaded
        response = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        data = response.json()
        self.assertEqual(len(data["all_models_loaded"]), 0)


if __name__ == "__main__":
    run_server_tests(LLMTests, "LLM/EMBEDDING/RERANKING TESTS")
