import logging
import time
import subprocess
import json
import asyncio
from abc import ABC, abstractmethod

import requests
from tabulate import tabulate
from fastapi import HTTPException, status
from fastapi.responses import StreamingResponse

from openai import OpenAI, AsyncOpenAI, OpenAIError
from openai.types.chat import ChatCompletionChunk
from openai.types.chat.chat_completion_chunk import (
    Choice,
    ChoiceDelta,
    ChoiceDeltaToolCall,
    ChoiceDeltaToolCallFunction,
)

from lemonade_server.pydantic_models import (
    ChatCompletionRequest,
    CompletionRequest,
    PullConfig,
    EmbeddingsRequest,
    RerankingRequest,
)
from lemonade_server.model_manager import ModelManager
from lemonade.tools.server.utils.port import find_free_port


def create_progress_tool_call_chunk(progress: float) -> ChatCompletionChunk:
    """
    Create an OpenAI-compatible ChatCompletionChunk for progress updates.

    Args:
        progress: Progress value between 0.0 and 1.0

    Returns:
        ChatCompletionChunk object containing the progress tool call
    """
    tool_call = ChoiceDeltaToolCall(
        index=0,
        id="progress_update",
        type="function",
        function=ChoiceDeltaToolCallFunction(
            name="update_progress", arguments=json.dumps({"progress": progress})
        ),
    )

    return ChatCompletionChunk(
        id="progress",
        object="chat.completion.chunk",
        created=int(time.time()),
        model="progress",
        choices=[
            Choice(
                index=0, delta=ChoiceDelta(tool_calls=[tool_call]), finish_reason=None
            )
        ],
    )


class WrappedServerTelemetry(ABC):
    """
    Manages telemetry data collection and display for wrapped server.
    """

    def __init__(self):
        self.input_tokens = None
        self.output_tokens = None
        self.time_to_first_token = None
        self.tokens_per_second = None
        self.prompt_eval_time = None
        self.eval_time = None

    @abstractmethod
    def parse_telemetry_line(self, line: str):
        """
        Parse telemetry data from wrapped server output lines.
        """

    def get_telemetry_data(self):
        return {
            "input_tokens": self.input_tokens,
            "output_tokens": self.output_tokens,
            "time_to_first_token": self.time_to_first_token,
            "tokens_per_second": self.tokens_per_second,
            "decode_token_times": None,
        }

    def show_telemetry(self):
        # Check if debug logging is enabled
        if not logging.getLogger().isEnabledFor(logging.DEBUG):
            return

        # Prepare telemetry data (transposed format)
        telemetry = [
            ["Input tokens", self.input_tokens],
            ["Output tokens", self.output_tokens],
            ["TTFT (s)", f"{self.time_to_first_token:.2f}"],
            ["TPS", f"{self.tokens_per_second:.2f}"],
        ]

        table = tabulate(
            telemetry, headers=["Metric", "Value"], tablefmt="fancy_grid"
        ).split("\n")

        # Show telemetry in debug while complying with uvicorn's log indentation
        logging.debug("\n          ".join(table))


class WrappedServer(ABC):
    """
    Abstract base class that defines the interface for Lemonade to "wrap" a server
    like llama-server.
    """

    def __init__(self, server_name: str, telemetry: WrappedServerTelemetry, prefill_progress: bool = False):
        self.port: int = None
        self.process: subprocess.Popen = None
        self.server_name: str = server_name
        self.telemetry: WrappedServerTelemetry = telemetry
        self.log_thread_exception = None
        self.prefill_progress = prefill_progress

    def choose_port(self):
        """
        Users probably don't care what port we start the wrapped server on, so let's
        search for an empty port
        """

        self.port = find_free_port()

        if self.port is None:
            msg = f"Failed to find an empty port to start {self.server_name} on"
            logging.error(msg)
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=msg,
            )

    def address(self) -> str:
        """
        Generate the base URL for the server.

        Returns:
            The base URL for the wrapped server
        """
        return f"http://127.0.0.1:{self.port}/v1"

    def _separate_openai_params(
        self,
        request_dict: dict,
        endpoint_type: str = "chat",
    ) -> dict:
        """
        Separate standard OpenAI parameters from custom wrapped server parameters.

        Args:
            request_dict: Dictionary of all request parameters
            endpoint_type: Type of endpoint ("chat" or "completion")

        Returns:
            Dictionary with parameters properly separated for OpenAI client
        """
        openai_client_params = {}
        extra_params = {}

        # Common OpenAI parameters for both endpoint types
        common_params = {
            "model",
            "frequency_penalty",
            "logit_bias",
            "logprobs",
            "max_tokens",
            "n",
            "presence_penalty",
            "seed",
            "stop",
            "stream",
            "temperature",
            "top_p",
            "user",
        }

        # Standard OpenAI parameters by endpoint type
        if endpoint_type == "chat":
            chat_specific_params = {
                "messages",
                "top_logprobs",
                "response_format",
                "service_tier",
                "stream_options",
                "tools",
                "tool_choice",
                "parallel_tool_calls",
            }
            openai_params = common_params | chat_specific_params
        else:  # completion
            completion_specific_params = {
                "prompt",
                "best_of",
                "echo",
                "suffix",
            }
            openai_params = common_params | completion_specific_params

        for key, value in request_dict.items():
            if key in openai_params:
                openai_client_params[key] = value
            else:
                extra_params[key] = value

        # If there are custom parameters, use extra_body to pass them through
        if extra_params:
            openai_client_params["extra_body"] = extra_params

        return openai_client_params

    def _log_subprocess_output(self, prefix: str):
        """
        Read subprocess output line by line, log to debug, and parse telemetry
        """

        if self.process.stdout:
            try:
                for line in iter(self.process.stdout.readline, ""):
                    if line:
                        line_stripped = line.strip()
                        logging.debug("%s: %s", prefix, line_stripped)

                        self.telemetry.parse_telemetry_line(line_stripped)

                    if self.process.poll() is not None:
                        break
            except HTTPException as e:
                self.log_thread_exception = e
            except UnicodeDecodeError as e:
                logging.debug(
                    "Unicode decode error reading subprocess output: %s", str(e)
                )
            except Exception as e:  # pylint: disable=broad-exception-caught
                logging.error("Unexpected error reading subprocess output: %s", str(e))

    def _wait_for_load(self):
        status_code = None
        while not self.process.poll() and status_code != 200:
            health_url = f"http://localhost:{self.port}/health"
            try:
                health_response = requests.get(health_url)
            except requests.exceptions.ConnectionError:
                logging.debug(
                    f"Not able to connect to {self.server_name} yet, will retry"
                )
            else:
                status_code = health_response.status_code
                logging.debug(
                    f"Testing {self.server_name} readiness (will retry until ready), "
                    f"result: {health_response.json()}"
                )
            time.sleep(1)

        if self.log_thread_exception:
            e = self.log_thread_exception
            self.log_thread_exception = None
            raise e

    @abstractmethod
    def _launch_server_subprocess(
        self,
        model_config: PullConfig,
        snapshot_files: dict,
        ctx_size: int,
        supports_embeddings: bool = False,
        supports_reranking: bool = False,
    ):
        """
        Launch wrapped server subprocess with appropriate configuration.

        Args:
            snapshot_files: Dictionary of model files to load
            supports_embeddings: Whether the model supports embeddings
            supports_reranking: Whether the model supports reranking
        """

    @abstractmethod
    def install_server(self, backend=None):
        """
        Install the wrapped server
        """

    @abstractmethod
    def download_model(
        self, config_checkpoint, config_mmproj=None, do_not_upgrade=False
    ) -> dict:
        """
        Download a model for the wrapper server
        """

    def load(
        self,
        model_config: PullConfig,
        ctx_size: int,
        do_not_upgrade: bool = False,
    ):
        # Install and/or update the wrapped server if needed
        try:
            self.install_server()
        except NotImplementedError as e:
            raise HTTPException(
                status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail=str(e)
            )

        # Download the model to the hugging face cache
        snapshot_files = self.download_model(
            model_config.checkpoint, model_config.mmproj, do_not_upgrade=do_not_upgrade
        )
        logging.debug(f"Model file paths: {snapshot_files}")

        # Check if model supports embeddings
        supported_models = ModelManager().supported_models
        model_info = supported_models.get(model_config.model_name, {})
        supports_embeddings = "embeddings" in model_info.get("labels", [])
        supports_reranking = "reranking" in model_info.get("labels", [])

        self._launch_server_subprocess(
            model_config=model_config,
            snapshot_files=snapshot_files,
            ctx_size=ctx_size,
            supports_embeddings=supports_embeddings,
            supports_reranking=supports_reranking,
        )

        # Check the /health endpoint until server is ready
        self._wait_for_load()

        if self.process.poll():
            raise HTTPException(
                status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                detail=f"Failed to load {model_config.model_name} with {self.server_name}",
            )

    def _supports_prefill_progress(self) -> bool:
        """
        Check if telemetry supports prefill progress tracking.
        """
        return (
            hasattr(self.telemetry, "should_report_progress")
            and hasattr(self.telemetry, "prefill_progress")
            and hasattr(self.telemetry, "prefill_complete")
        )

    def _get_current_progress(self) -> float:
        """Get current prefill progress from telemetry."""
        if hasattr(self.telemetry, "prefill_progress"):
            return self.telemetry.prefill_progress
        return 0.0

    def _is_prefill_complete(self) -> bool:
        """Check if prefill phase is complete."""
        return (
            hasattr(self.telemetry, "prefill_complete")
            and self.telemetry.prefill_complete
        )

    async def _monitor_prefill_progress_async(self):
        """
        Async generator that yields prefill progress updates from telemetry.
        Used to report progress during the prefill phase before chunks arrive.
        """
        if not self._supports_prefill_progress():
            return

        last_yielded_progress = -1.0  # Track what we've actually yielded

        # Monitor until prefill is complete (progress reaches 1.0)
        while not self._is_prefill_complete() or last_yielded_progress < 1.0:
            # Check if we should report progress
            current_progress = self._get_current_progress()

            # Check if prefill is complete and we haven't reported 1.0 yet
            if (
                self._is_prefill_complete()
                and current_progress >= 1.0
                and last_yielded_progress < 1.0
            ):
                progress_chunk = create_progress_tool_call_chunk(1.0)
                yield f"data: {progress_chunk.model_dump_json()}\n\n"
                break

            # Call should_report_progress() only once to avoid side effects
            should_report = self.telemetry.should_report_progress()
            if should_report and current_progress != last_yielded_progress:
                progress_chunk = create_progress_tool_call_chunk(current_progress)
                yield f"data: {progress_chunk.model_dump_json()}\n\n"
                last_yielded_progress = current_progress

                # If we just yielded 1.0 (complete), we can stop monitoring
                if current_progress >= 1.0:
                    break

            # Small async sleep to avoid busy waiting
            await asyncio.sleep(0.01)

    async def _stream_with_progress(self, openai_client_params: dict):
        """
        Stream chat completion with optional prefill progress monitoring.

        Args:
            openai_client_params: Parameters for OpenAI API call

        Yields:
            SSE formatted strings containing either progress updates or completion chunks
        """
        # Reset prefill progress for new request
        if hasattr(self.telemetry, "reset_prefill_progress"):
            self.telemetry.reset_prefill_progress()

        # Create async OpenAI client for streaming
        async_client = AsyncOpenAI(
            base_url=self.address(),
            api_key="lemonade",
        )

        # Use asyncio.Queue to merge streams
        queue = asyncio.Queue()

        async def fetch_stream():
            """Fetch the actual completion stream from llama.cpp"""
            try:
                stream = await async_client.chat.completions.create(
                    **openai_client_params
                )
                async for chunk in stream:
                    await queue.put(("chunk", chunk))
            except OpenAIError as e:
                await queue.put(("error", e))
            finally:
                await queue.put(("done", None))

        async def monitor_progress():
            """Monitor prefill progress from telemetry"""
            async for progress_update in self._monitor_prefill_progress_async():
                await queue.put(("progress", progress_update))

        # Start fetch task
        fetch_task = asyncio.create_task(fetch_stream())
        
        # Only start progress monitoring if enabled
        monitor_task = None
        if self.prefill_progress:
            monitor_task = asyncio.create_task(monitor_progress())

        # Process items from queue
        first_chunk_received = False
        while True:
            try:
                # Use wait_for to allow checking both sources
                msg_type, data = await asyncio.wait_for(queue.get(), timeout=0.1)

                if msg_type == "done":
                    break
                elif msg_type == "error":
                    yield f'data: {{"error": "{str(data)}"}}\n\n'
                    break
                elif msg_type == "progress" and not first_chunk_received:
                    # Only yield progress before first real chunk
                    yield data
                elif msg_type == "chunk":
                    if not first_chunk_received:
                        first_chunk_received = True
                        if monitor_task:  # Only cancel if monitoring was enabled
                            monitor_task.cancel()  # Stop monitoring once real chunks arrive
                    yield f"data: {data.model_dump_json()}\n\n"
            except asyncio.TimeoutError:
                # Check if tasks are still running
                if fetch_task.done() and (not monitor_task or monitor_task.done()):
                    break
                continue

        yield "data: [DONE]\n\n"

    def chat_completion(self, chat_completion_request: ChatCompletionRequest):
        client = OpenAI(
            base_url=self.address(),
            api_key="lemonade",
        )

        # Convert Pydantic model to dict and remove unset/null values
        request_dict = chat_completion_request.model_dump(
            exclude_unset=True, exclude_none=True
        )

        # Separate standard OpenAI parameters from custom llama.cpp parameters
        openai_client_params = self._separate_openai_params(request_dict, "chat")

        # Check if streaming is requested
        if chat_completion_request.stream:

            async def event_stream():
                try:
                    # Stream with progress monitoring
                    async for data in self._stream_with_progress(openai_client_params):
                        yield data

                    # Show telemetry after completion
                    self.telemetry.show_telemetry()

                except Exception as e:  # pylint: disable=broad-exception-caught
                    yield f'data: {{"error": "{str(e)}"}}\n\n'

            return StreamingResponse(
                event_stream(),
                media_type="text/event-stream",
                headers={
                    "Cache-Control": "no-cache",
                    "Connection": "keep-alive",
                },
            )
        else:
            # Non-streaming response
            try:
                # Disable streaming for non-streaming requests
                # pylint: disable=missing-kwoa
                response = client.chat.completions.create(**openai_client_params)

                # Show telemetry after completion
                self.telemetry.show_telemetry()

                return response

            except Exception as e:  # pylint: disable=broad-exception-caught
                logging.error("Error during chat completion: %s", str(e))
                raise HTTPException(
                    status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                    detail=f"Chat completion error: {str(e)}",
                )

    def completion(self, completion_request: CompletionRequest):
        """
        Handle text completions using the wrapped server.

        Args:
            completion_request: The completion request containing prompt and parameters
            telemetry: Telemetry object containing the server port

        Returns:
            Completion response from the wrapped server
        """

        client = OpenAI(
            base_url=self.address(),
            api_key="lemonade",
        )

        # Convert Pydantic model to dict and remove unset/null values
        request_dict = completion_request.model_dump(
            exclude_unset=True, exclude_none=True
        )

        # Separate standard OpenAI parameters from custom llama.cpp parameters
        openai_client_params = self._separate_openai_params(request_dict, "completion")

        # Check if streaming is requested
        if completion_request.stream:

            def event_stream():
                try:
                    # Enable streaming
                    # pylint: disable=missing-kwoa
                    for chunk in client.completions.create(**openai_client_params):
                        yield f"data: {chunk.model_dump_json()}\n\n"
                    yield "data: [DONE]\n\n"

                    # Show telemetry after completion
                    self.telemetry.show_telemetry()

                except Exception as e:  # pylint: disable=broad-exception-caught
                    yield f'data: {{"error": "{str(e)}"}}\n\n'

            return StreamingResponse(
                event_stream(),
                media_type="text/event-stream",
                headers={
                    "Cache-Control": "no-cache",
                    "Connection": "keep-alive",
                },
            )
        else:
            # Non-streaming response
            try:
                # Disable streaming for non-streaming requests
                # pylint: disable=missing-kwoa
                response = client.completions.create(**openai_client_params)

                # Show telemetry after completion
                self.telemetry.show_telemetry()

                return response

            except Exception as e:  # pylint: disable=broad-exception-caught
                logging.error("Error during completion: %s", str(e))
                raise HTTPException(
                    status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                    detail=f"Completion error: {str(e)}",
                )

    def embeddings(self, embeddings_request: EmbeddingsRequest):
        """
        Generate embeddings using the wrapped server.

        Args:
            embeddings_request: The embeddings request containing input text/tokens
            telemetry: Telemetry object containing the server port

        Returns:
            Embeddings response from the wrapped server
        """
        client = OpenAI(
            base_url=self.address(),
            api_key="lemonade",
        )

        # Convert Pydantic model to dict and remove unset/null values
        request_dict = embeddings_request.model_dump(
            exclude_unset=True, exclude_none=True
        )

        try:
            # Call the embeddings endpoint
            response = client.embeddings.create(**request_dict)
            return response

        except Exception as e:  # pylint: disable=broad-exception-caught
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=f"Embeddings error: {str(e)}",
            )

    def reranking(self, reranking_request: RerankingRequest):
        """
        Rerank documents based on their relevance to a query using the wrapped server.

        Args:
            reranking_request: The reranking request containing query and documents
            telemetry: Telemetry object containing the server port

        Returns:
            Reranking response from the wrapped server containing ranked documents and scores
        """

        try:
            # Convert Pydantic model to dict and exclude unset/null values
            request_dict = reranking_request.model_dump(
                exclude_unset=True, exclude_none=True
            )

            # Call the reranking endpoint directly since it's not supported by the OpenAI API
            response = requests.post(
                f"{self.address()}/rerank",
                json=request_dict,
            )
            response.raise_for_status()
            return response.json()

        except Exception as e:
            logging.error("Error during reranking: %s", str(e))
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=f"Reranking error: {str(e)}",
            ) from e
