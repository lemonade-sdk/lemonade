"""
Usage: python server.py [--offline]

This will launch the lemonade server, query it in openai mode,
and make sure that the response is valid.

If --offline is provided, tests will run in offline mode to ensure
the server works without network connectivity.

If you get the `ImportError: cannot import name 'TypeIs' from 'typing_extensions'` error:
    1. pip uninstall typing_extensions
    2. pip install openai
"""

import unittest
import subprocess
import psutil
import asyncio
import socket
import time
from threading import Thread
import sys
import io
import httpx
import argparse
import contextlib
from unittest.mock import patch
import urllib.request
import os
import requests

try:
    from openai import OpenAI, AsyncOpenAI
except ImportError as e:
    raise ImportError("You must `pip install openai` to run this test", e)

# Import huggingface_hub for patching in offline mode
try:
    from huggingface_hub import snapshot_download as original_snapshot_download
except ImportError:
    # If huggingface_hub is not installed, create a dummy function
    def original_snapshot_download(*args, **kwargs):
        raise ImportError("huggingface_hub is not installed")


MODEL_NAME = "Qwen2.5-0.5B-Instruct-CPU"
MODEL_CHECKPOINT = "amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx"
PORT = 8000

# Sample tool schema based on https://github.com/githejie/mcp-server-calculator
sample_tool = {
    "type": "function",
    "function": {
        "name": "calculator_calculate",
        "parameters": {
            "properties": {"expression": {"title": "Expression", "type": "string"}},
            "required": ["expression"],
            "title": "calculateArguments",
            "type": "object",
        },
    },
}


def parse_args():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Test lemonade server")
    parser.add_argument(
        "--offline", action="store_true", help="Run tests in offline mode"
    )
    return parser.parse_args()


@contextlib.contextmanager
def simulate_offline_mode():
    """
    Context manager that simulates a fully offline environment except
    for local connections needed for testing.

    This patches multiple network-related functions to prevent any
    external network access during tests.
    """
    original_create_connection = socket.create_connection

    def mock_create_connection(address, *args, **kwargs):
        host, port = address
        # Allow connections to localhost for testing
        if host == "localhost" or host == "127.0.0.1":
            return original_create_connection(address, *args, **kwargs)
        # Block all other connections
        raise socket.error("Network access disabled for offline testing")

    # Define a function that raises an error for non-local requests
    def block_external_requests(original_func):
        def wrapper(url, *args, **kwargs):
            # Allow localhost requests
            if url.startswith(
                (
                    "http://localhost",
                    "https://localhost",
                    "http://127.0.0.1",
                    "https://127.0.0.1",
                )
            ):
                return original_func(url, *args, **kwargs)
            raise ConnectionError(f"Offline mode: network request blocked to {url}")

        return wrapper

    # Apply all necessary patches to simulate offline mode
    with patch("socket.create_connection", side_effect=mock_create_connection):
        with patch(
            "huggingface_hub.snapshot_download",
            side_effect=lambda *args, **kwargs: (
                kwargs.get("local_files_only", False)
                and original_snapshot_download(*args, **kwargs)
                or (_ for _ in ()).throw(
                    ValueError("Offline mode: network connection attempted")
                )
            ),
        ):
            # Also patch urllib and requests to block external requests
            with patch(
                "urllib.request.urlopen",
                side_effect=block_external_requests(urllib.request.urlopen),
            ):
                with patch(
                    "http.client.HTTPConnection.connect",
                    side_effect=lambda self, *args, **kwargs: (
                        None
                        if self.host in ("localhost", "127.0.0.1")
                        else (_ for _ in ()).throw(
                            ConnectionError("Offline mode: connection blocked")
                        )
                    ),
                ):
                    # Set environment variable to signal offline mode
                    os.environ["LEMONADE_OFFLINE_TEST"] = "1"
                    try:
                        yield
                    finally:
                        # Clean up environment variable
                        if "LEMONADE_OFFLINE_TEST" in os.environ:
                            del os.environ["LEMONADE_OFFLINE_TEST"]


def ensure_model_is_cached():
    """
    Make sure the test model is downloaded and cached locally before running in offline mode.
    """
    try:
        # Call lemonade-server-dev pull to download the model
        subprocess.run(
            ["lemonade-server-dev", "pull", MODEL_NAME],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        print(f"Model {MODEL_NAME} successfully pulled and available in cache")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Failed to download model: {e}")
        return False


def kill_process_on_port(port):
    """Kill any process that is using the specified port."""
    killed = False
    for proc in psutil.process_iter(["pid", "name"]):
        try:
            connections = proc.net_connections()
            for conn in connections:
                if conn.laddr.port == port:
                    proc_name = proc.name()
                    proc_pid = proc.pid
                    proc.kill()
                    print(
                        f"Killed process {proc_name} (PID: {proc_pid}) using port {port}"
                    )
                    killed = True
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            continue

    if not killed:
        print(f"No process found using port {port}")


class Testing(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        """
        Start lemonade server process
        """
        print("\n=== Starting new test ===")
        self.base_url = f"http://localhost:{PORT}/api/v1"
        self.messages = [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "Who won the world series in 2020?"},
            {"role": "assistant", "content": "The LA Dodgers won in 2020."},
            {"role": "user", "content": "In which state was it played?"},
        ]

        # Ensure we kill anything using port 8000
        kill_process_on_port(PORT)

        # The --no-tray option is only available on Windows
        if os.name == "nt":
            cmd = ["lemonade-server-dev", "serve", "--no-tray"]
        else:
            cmd = ["lemonade-server-dev", "serve"]

        # Start the lemonade server
        lemonade_process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

        # Print stdout and stderr in real-time
        def print_output():
            while True:
                stdout = lemonade_process.stdout.readline()
                stderr = lemonade_process.stderr.readline()
                if stdout:
                    print(f"[stdout] {stdout.strip()}")
                if stderr:
                    print(f"[stderr] {stderr.strip()}")
                if not stdout and not stderr and lemonade_process.poll() is not None:
                    break

        output_thread = Thread(target=print_output, daemon=True)
        output_thread.start()

        # Wait for the server to start by checking port 8000
        start_time = time.time()
        while True:
            if time.time() - start_time > 60:
                raise TimeoutError("Server failed to start within 60 seconds")
            try:
                conn = socket.create_connection(("localhost", PORT))
                conn.close()
                break
            except socket.error:
                time.sleep(1)

        # Wait a few other seconds after the port is available
        time.sleep(5)

        print("Server started successfully")

        self.addCleanup(self.cleanup_lemonade, lemonade_process)

        # Ensure stdout can handle Unicode
        if sys.stdout.encoding != "utf-8":
            sys.stdout = io.TextIOWrapper(
                sys.stdout.buffer, encoding="utf-8", errors="replace"
            )
            sys.stderr = io.TextIOWrapper(
                sys.stderr.buffer, encoding="utf-8", errors="replace"
            )

    def cleanup_lemonade(self, server_subprocess: subprocess.Popen):
        """
        Kill the lemonade server and stop the model
        """

        # Kill the server subprocess
        print("\n=== Cleaning up test ===")

        parent = psutil.Process(server_subprocess.pid)
        for child in parent.children(recursive=True):
            child.kill()

        server_subprocess.kill()

        kill_process_on_port(PORT)

    def test_000_endpoints_available(self):
        # List of endpoints to check
        valid_endpoints = [
            "chat/completions",
            "completions",
            "models",
            "responses",
            "pull",
            "delete",
            "load",
            "unload",
            "health",
            "halt",
            "stats",
        ]

        # Ensure that we get a 404 error when the endpoint is not registered
        url = f"http://localhost:{PORT}/api/v0/nonexistent"
        response = requests.get(url)
        assert response.status_code == 404

        # Check that all endpoints are properly registered on both v0 and v1
        for endpoint in valid_endpoints:
            for version in ["v0", "v1"]:
                url = f"http://localhost:{PORT}/api/{version}/{endpoint}"
                response = requests.get(url)
                assert (
                    response.status_code != 404
                ), f"Endpoint {endpoint} is not registered on {version}"

    # Endpoint: /api/v1/chat/completions
    def test_001_test_chat_completion(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        completion = client.chat.completions.create(
            model=MODEL_NAME,
            messages=self.messages,
            max_completion_tokens=10,
        )

        print(completion.choices[0].message.content)
        assert len(completion.choices[0].message.content) > 5

        # Check usage fields
        assert completion.usage.prompt_tokens > 0
        assert completion.usage.completion_tokens > 0
        assert completion.usage.total_tokens > 0
        assert (
            completion.usage.total_tokens
            == completion.usage.prompt_tokens + completion.usage.completion_tokens
        )

    # Endpoint: /api/v1/chat/completions
    def test_002_test_chat_completion_streaming(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        stream = client.chat.completions.create(
            model=MODEL_NAME,
            messages=self.messages,
            stream=True,
            max_tokens=10,
        )
        complete_response = ""
        chunk_count = 0
        for chunk in stream:
            if chunk.choices[0].delta.content is not None:
                complete_response += chunk.choices[0].delta.content
                print(chunk.choices[0].delta.content, end="")
                chunk_count += 1

        assert chunk_count > 5
        assert len(complete_response) > 5

    # Endpoint: /api/v1/chat/completions
    async def test_003_test_chat_completion_streaming_async(self):
        client = AsyncOpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        complete_response = ""
        stream = await client.chat.completions.create(
            model=MODEL_NAME,
            messages=self.messages,
            stream=True,
            max_completion_tokens=10,
        )

        chunk_count = 0
        async for chunk in stream:
            if chunk.choices[0].delta.content is not None:
                complete_response += chunk.choices[0].delta.content
                print(chunk.choices[0].delta.content, end="")
                chunk_count += 1

        assert chunk_count > 5
        assert len(complete_response) > 5

    # Endpoint: /api/v1/models
    def test_004_test_models(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        # Get the list of models
        l = client.models.list()

        # Check that the list is not empty
        assert len(l.data) > 0

        # Check that the list contains a model with both the expected id, checkpoint, and recipe
        assert any(
            model.id == MODEL_NAME
            and model.checkpoint == MODEL_CHECKPOINT
            and model.recipe == "oga-cpu"
            for model in l.data
        )

    # Endpoint: /api/v1/completions
    def test_005_test_completions(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        completion = client.completions.create(
            model=MODEL_NAME,
            prompt="Hello, how are you?",
            stream=False,
            max_tokens=10,
        )

        print(completion.choices[0].text)
        assert len(completion.choices[0].text) > 5

        # Check usage fields
        assert completion.usage.prompt_tokens > 0
        assert completion.usage.completion_tokens > 0
        assert completion.usage.total_tokens > 0
        assert (
            completion.usage.total_tokens
            == completion.usage.prompt_tokens + completion.usage.completion_tokens
        )

    # Endpoint: /api/v1/completions
    def test_006_test_completions_streaming(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        stream = client.completions.create(
            model=MODEL_NAME,
            prompt="Hello, how are you?",
            stream=True,
            max_tokens=10,
        )

        complete_response = ""
        chunk_count = 0
        for chunk in stream:
            if chunk.choices[0].text is not None:
                complete_response += chunk.choices[0].text
                print(chunk.choices[0].text, end="")
                chunk_count += 1

        assert chunk_count > 5
        assert len(complete_response) > 5

    # Endpoint: /api/v1/completions
    async def test_007_test_completions_streaming_async(self):
        client = AsyncOpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        complete_response = ""
        stream = await client.completions.create(
            model=MODEL_NAME,
            prompt="Hello, how are you?",
            stream=True,
            max_tokens=10,
        )

        chunk_count = 0
        async for chunk in stream:
            if chunk.choices[0].text is not None:
                complete_response += chunk.choices[0].text
                print(chunk.choices[0].text, end="")
                chunk_count += 1

        assert chunk_count > 5
        assert len(complete_response) > 5

    # Endpoint: /api/v1/completions with stop parameter
    def test_008_test_completions_with_stop(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        completion = client.completions.create(
            model=MODEL_NAME,
            prompt="Just say 'I am Joe and I like apples'. Here we go: 'I am Joe and",
            stop=["apples"],  # The model will stop generating when it reaches "apples"
            max_tokens=10,
        )

        print(completion.choices[0].text)
        assert len(completion.choices[0].text) > 2
        assert "apples" not in completion.choices[0].text

    # Endpoint: /api/v1/chat/completions with stop parameter
    def test_009_test_chat_completion_with_stop(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        messages = [
            {"role": "system", "content": "Your name is Joe and you like apples."},
            {"role": "user", "content": "What is your name and what do you like?"},
        ]

        completion = client.chat.completions.create(
            model=MODEL_NAME,
            messages=messages,
            stop=["apples"],  # The model will stop generating when it reaches "apples"
            max_completion_tokens=10,
        )

        print(completion.choices[0].message.content)
        assert len(completion.choices[0].message.content) > 2
        assert "apples" not in completion.choices[0].message.content

    # Endpoint: /api/v1/completions with echo parameter
    def test_010_test_completions_with_echo(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        prompt = "Hello, how are you?"
        completion = client.completions.create(
            model=MODEL_NAME,
            prompt=prompt,
            echo=True,
            max_tokens=10,
        )

        print(completion.choices[0].text)
        # Check that the response contains the original prompt
        assert completion.choices[0].text.startswith(prompt)
        # Check that there's additional content beyond the prompt
        assert len(completion.choices[0].text) > len(prompt)

    # Test simultaneous load requests
    async def test_011_test_simultaneous_load_requests(self):
        async with httpx.AsyncClient(base_url=self.base_url, timeout=120.0) as client:
            first_model = "Qwen2.5-0.5B-Instruct-CPU"
            second_model = "Qwen3-0.6B-GGUF"

            # Start two load requests simultaneously
            load_tasks = [
                client.post(
                    "/load",
                    json={
                        "model_name": first_model,
                    },
                ),
                client.post(
                    "/load",
                    json={
                        "model_name": second_model,
                    },
                ),
            ]

            # Execute both requests concurrently
            responses = await asyncio.gather(*load_tasks)

            # Verify both requests completed successfully
            assert responses[0].status_code == 200
            assert responses[1].status_code == 200

            # Verify the final loaded model is the second one
            health_response = await client.get("/health")
            assert health_response.status_code == 200
            health_data = health_response.json()
            assert health_data["model_loaded"] == second_model, health_data[
                "model_loaded"
            ]

    # Test load by model name
    async def test_012_test_load_by_name(self):
        async with httpx.AsyncClient(base_url=self.base_url, timeout=120.0) as client:
            load_response = await client.post("/load", json={"model_name": MODEL_NAME})

            assert load_response.status_code == 200

            # Verify the model loaded
            health_response = await client.get("/health")
            assert health_response.status_code == 200
            health_data = health_response.json()
            assert health_data["model_loaded"] == MODEL_NAME

    # Test pull to register-and-install
    async def test_013_test_load_checkpoint_completion(self):
        async with httpx.AsyncClient(base_url=self.base_url, timeout=120.0) as client:

            load_response = await client.post(
                "/pull",
                json={
                    "model_name": "user.Qwen2.5-0.5B-HF-CPU",
                    "checkpoint": "Qwen/Qwen2.5-0.5B",
                    "recipe": "hf-cpu",
                },
            )

            assert load_response.status_code == 200

            # Verify the model loaded
            health_response = await client.get("/health")
            assert health_response.status_code == 200

            # Run a completions request using the new model
            client = OpenAI(
                base_url=self.base_url,
                api_key="lemonade",  # required, but unused
            )

            completion = client.completions.create(
                model="user.Qwen2.5-0.5B-HF-CPU",
                prompt="Hello, how are you?",
                stream=False,
                max_tokens=10,
            )

            print(completion.choices[0].text)
            assert len(completion.choices[0].text) > 5

    # Endpoint: /api/v1/responses
    def test_014_test_responses(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        response = client.responses.create(
            model=MODEL_NAME,
            input=self.messages,
            stream=False,
            temperature=0.0,
            max_output_tokens=10,
        )

        print(response.output[0].content[0].text)
        assert len(response.output[0].content[0].text) > 5

    # Endpoint: /api/v1/responses
    def test_015_test_responses_streaming(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        stream = client.responses.create(
            model=MODEL_NAME,
            input=self.messages,
            stream=True,
            temperature=0.0,
            max_output_tokens=10,
        )
        complete_response = ""
        event_count = 0
        last_event_type = ""
        for event in stream:

            # Check that the first event is a response.created event
            if event_count == 0:
                assert (
                    event.type == "response.created"
                ), f"Expected first event to be response.created, got {event.type}"

            # Keep track of the streamed response
            elif event.type == "response.output_text.delta":
                complete_response += event.delta
                print(event.delta, end="")

            # Ensure the complete event matches the streamed response
            elif event.type == "response.completed":
                assert (
                    event.response.output[0].content[0].text == complete_response
                ), "Complete response does not match streamed response"

            # Ensure all events we add in the future are also tested
            else:
                assert False, f"Untested event type: {event.type}"
            event_count += 1
            last_event_type = event.type

        assert last_event_type == "response.completed"
        assert len(complete_response) > 5

    # Endpoint: /api/v1/responses
    async def test_016_test_responses_streaming_async(self):
        client = AsyncOpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        stream = await client.responses.create(
            model=MODEL_NAME,
            input=self.messages,
            stream=True,
            temperature=0.0,
            max_output_tokens=10,
        )
        complete_response = ""
        event_count = 0
        last_event_type = ""
        async for event in stream:

            # Check that the first event is a response.created event
            if event_count == 0:
                assert (
                    event.type == "response.created"
                ), f"Expected first event to be response.created, got {event.type}"

            # Keep track of the streamed response
            elif event.type == "response.output_text.delta":
                complete_response += event.delta
                print(event.delta, end="")

            # Ensure the complete event matches the streamed response
            elif event.type == "response.completed":
                assert (
                    event.response.output[0].content[0].text == complete_response
                ), "Complete response does not match streamed response"

            # Ensure all events we add in the future are also tested
            else:
                assert False, f"Untested event type: {event.type}"
            event_count += 1
            last_event_type = event.type

        assert last_event_type == "response.completed"
        assert len(complete_response) > 5

    # Endpoint: /api/v1/chat/completions with tool calls
    def test_017_test_chat_completion_with_tool_calls(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        completion = client.chat.completions.create(
            model=MODEL_NAME,
            messages=[
                {
                    "role": "user",
                    "content": "Run the calculator_calculate tool with expression set to 1+1",
                }
            ],
            tools=[sample_tool],
            max_completion_tokens=50,
        )

        tool_calls = getattr(completion.choices[0].message, "tool_calls", None)
        assert tool_calls is not None
        assert len(tool_calls) == 1

    # Endpoint: /api/v1/chat/completions with tool calls
    def test_018_test_chat_completion_with_tool_calls_streaming(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        stream = client.chat.completions.create(
            model=MODEL_NAME,
            messages=[
                {
                    "role": "user",
                    "content": "Run the calculator_calculate tool with expression set to 1+1",
                }
            ],
            tools=[sample_tool],
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
            if delta.tool_calls:
                for tool_call in delta.tool_calls:
                    print(tool_call)
                    tool_call_count += 1

        assert tool_call_count > 0

    # Endpoint: /api/v1/chat/completions
    def test_019_test_llamacpp_chat_completion_streaming(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        stream = client.chat.completions.create(
            model="Qwen3-0.6B-GGUF",
            messages=self.messages,
            stream=True,
            max_completion_tokens=10,
        )

        complete_response = ""
        chunk_count = 0
        for chunk in stream:
            if chunk.choices[0].delta.content is not None:
                complete_response += chunk.choices[0].delta.content
                print(chunk.choices[0].delta.content, end="")
                chunk_count += 1

        assert chunk_count > 5
        assert len(complete_response) > 5

    # Endpoint: /api/v1/chat/completions
    def test_020_test_llamacpp_chat_completion_non_streaming(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        response = client.chat.completions.create(
            model="Qwen3-0.6B-GGUF",
            messages=self.messages,
            stream=False,
            max_completion_tokens=10,
        )

        assert response.choices[0].message.content is not None
        assert len(response.choices[0].message.content) > 5
        print(response.choices[0].message.content)

    # Endpoint: /api/v1/delete
    async def test_021_test_delete_model(self):
        """Test the delete endpoint functionality"""
        async with httpx.AsyncClient(base_url=self.base_url, timeout=120.0) as client:

            # First, ensure the test model is available by pulling it
            pull_response = await client.post("/pull", json={"model_name": MODEL_NAME})
            assert pull_response.status_code == 200

            # Verify the model is in the models list
            models_response = await client.get("/models")
            assert models_response.status_code == 200
            models_data = models_response.json()
            model_ids = [model["id"] for model in models_data["data"]]
            assert (
                MODEL_NAME in model_ids
            ), f"Model {MODEL_NAME} not found in models list"

            # Test deleting the model
            delete_response = await client.post(
                "/delete", json={"model_name": MODEL_NAME}
            )
            assert delete_response.status_code == 200
            delete_data = delete_response.json()
            assert delete_data["status"] == "success"
            assert MODEL_NAME in delete_data["message"]

            # Verify the model is no longer in the models list
            models_response = await client.get("/models")
            assert models_response.status_code == 200
            models_data = models_response.json()
            model_ids = [model["id"] for model in models_data["data"]]
            assert (
                MODEL_NAME not in model_ids
            ), f"Model {MODEL_NAME} still found in models list after deletion"

            # Test deleting a non-existent model (should return error)
            delete_response = await client.post(
                "/delete", json={"model_name": "NonExistentModel"}
            )
            assert delete_response.status_code == 422  # Unprocessable Entity

            # Re-pull the model for subsequent tests
            pull_response = await client.post("/pull", json={"model_name": MODEL_NAME})
            assert pull_response.status_code == 200


if __name__ == "__main__":
    args = parse_args()

    if args.offline:
        print("\n=== STARTING SERVER TESTS IN OFFLINE MODE ===")

        if not ensure_model_is_cached():
            print("ERROR: Unable to cache the model needed for offline testing")
            sys.exit(1)

        print("Model is cached. Running tests with network access disabled...")

        # Create a new test suite
        test_loader = unittest.TestLoader()
        test_suite = test_loader.loadTestsFromTestCase(Testing)

        # Run the tests in offline mode
        with simulate_offline_mode():
            result = unittest.TextTestRunner().run(test_suite)

        # Set exit code based on test results
        sys.exit(0 if result.wasSuccessful() else 1)
    else:
        print("\n=== STARTING SERVER TESTS IN NORMAL MODE ===")
        unittest.main()

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
