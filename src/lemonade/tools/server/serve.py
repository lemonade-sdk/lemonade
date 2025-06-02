import argparse
import asyncio
import statistics
import time
from threading import Thread, Event
import logging
import traceback
from typing import Optional, Union
import json
import subprocess
from pathlib import Path

from fastapi import FastAPI, HTTPException, status, Request
from fastapi.responses import StreamingResponse
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
import uvicorn
from uvicorn.config import Config
from uvicorn.server import Server as UvicornServer
from transformers import TextIteratorStreamer, StoppingCriteria, StoppingCriteriaList
from tabulate import tabulate

from openai.types.completion import Completion, CompletionChoice
from openai.types.chat import ChatCompletion, ChatCompletionChunk
from openai.types.chat import ChatCompletionMessage
from openai.types.chat.chat_completion_message_tool_call import (
    ChatCompletionMessageToolCall,
    Function,
)
from openai.types.chat.chat_completion import Choice
from openai.types.chat.chat_completion_chunk import (
    ChoiceDelta,
    ChoiceDeltaToolCall,
    ChoiceDeltaToolCallFunction,
)
from openai.types.completion_choice import Logprobs
from openai.types.model import Model
from openai.types.responses import (
    Response,
    ResponseOutputMessage,
    ResponseOutputText,
    ResponseCreatedEvent,
    ResponseTextDeltaEvent,
    ResponseCompletedEvent,
)

import lemonade.api as lemonade_api
from lemonade_server.model_manager import ModelManager
from lemonade.tools.management_tools import ManagementTool
import lemonade.tools.server.llamacpp as llamacpp
from lemonade.tools.server.pydantic_models import (
    DEFAULT_MAX_NEW_TOKENS,
    LoadConfig,
    CompletionRequest,
    ChatCompletionRequest,
    ResponsesRequest,
    PullConfig,
)
from lemonade.tools.server.tool_calls import extract_tool_calls, get_tool_call_pattern
from lemonade.tools.server.instructions import get_instructions_html
from lemonade.tools.server.port_utils import lifespan

DEFAULT_PORT = 8000
DEFAULT_LOG_LEVEL = "info"


class ServerModel(Model):
    """
    An extension of OpenAI's Model class that adds
    checkpoint and recipe attributes.
    """

    checkpoint: str
    recipe: str


class GeneratorThread(Thread):
    """
    Thread class designed for use with streaming generation within
    an LLM server. It needs access to the streamer in order to order
    to help the completions APIs escape the "for text in streamer" loop.
    It also provides exception handling that works nicely with HTTP
    servers by providing the stack trace and making the exception
    information available to the main thread.
    """

    def __init__(self, streamer, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.exception = None
        self.streamer = streamer

    def run(self):
        try:
            if self._target:
                self._target(*self._args, **self._kwargs)
        except Exception as e:  # pylint: disable=broad-except
            self.exception = e
            logging.error(f"Exception raised in generate thread: {e}")
            traceback.print_exc()
            self.streamer.done()


class StopOnEvent(StoppingCriteria):
    """
    Custom stopping criteria that halts text generation when a specified event is set.

    This allows for external control of generation, such as stopping a generation
    before it reaches the maximum token limit.
    """

    def __init__(self, stop_event: Event):
        super().__init__()
        self.stop_event = stop_event

    def __call__(self, input_ids, scores, **kwargs):
        return self.stop_event.is_set()


class Server(ManagementTool):
    """
    Open a web server that apps can use to communicate with the LLM.

    The server exposes these endpoints:
    - /api/v1/pull: install an LLM by its Lemonade Server Model Name.
    - /api/v1/load: load a model checkpoint.
    - /api/v1/unload: unload a model checkpoint.
    - /api/v1/health: check whether a model is loaded and ready to serve.
    - /api/v1/stats: performance statistics for the generation.
    - /api/v1/halt: stop an in-progress generation from make more tokens.
    - /api/v1/completions: completion responses using HTTP chunked transfer encoding.
    - /api/v1/chat/completions: chat completion responses using HTTP chunked transfer encoding.
    - /api/v1/responses: responses API using HTTP chunked transfer encoding.
    - /api/v1/models: list all available models.
    """

    unique_name = "serve"

    def __init__(self):
        super().__init__()

        # Initialize FastAPI app
        self.app = FastAPI(lifespan=lifespan)

        # Add CORS middleware
        self.app.add_middleware(
            CORSMiddleware,
            allow_origins=["*"],  # Allows all origins
            allow_credentials=True,
            allow_methods=["*"],  # Allows all methods
            allow_headers=["*"],  # Allows all headers
        )

        # Set up custom routes
        self.setup_routes(["/api/v0", "/api/v1"])

        # Set up instructions
        self.app.get("/")(self.instructions)

        # Mount a static assets dir for HTML responses, such
        # as the instructions
        static_dir = Path(__file__).parent / "static"
        self.app.mount(
            "/static", StaticFiles(directory=static_dir), name="static_assets"
        )

        # Performance stats that are set during /ws and can be
        # fetched in /stats
        self.time_to_first_token = None
        self.tokens_per_second = None
        self.input_tokens = None
        self.output_tokens = None
        self.decode_token_times = None

        # Input truncation settings
        self.truncate_inputs = False

        # Store debug logging state
        self.debug_logging_enabled = logging.getLogger().isEnabledFor(logging.DEBUG)

        # Flag that tells the LLM to stop generating text and end the response
        self.stop_event = Event()

        self.llm_loaded: LoadConfig = None
        self.tokenizer = None

        # Placeholders for model and configs
        self.model = None

        # Initialize semaphore for tracking active generations
        self.max_concurrent_generations = 1
        self._generate_semaphore = asyncio.Semaphore(self.max_concurrent_generations)

        # Dictionary of installed LLM, by model name : information about those models
        # Does not include non-installed models
        self.local_models = ModelManager().downloaded_models_enabled

        # Add lock for load/unload operations
        self._load_lock = asyncio.Lock()

        # Subprocess handle for llama_server.exe
        self.llama_server_process: subprocess.Popen = None

        # Telemetry instance for llama server
        self.llama_telemetry = llamacpp.LlamaTelemetry()

    def setup_routes(self, api_prefixes: list[str]):
        for prefix in api_prefixes:
            # Custom routes
            self.app.post(f"{prefix}/pull")(self.pull)
            self.app.post(f"{prefix}/load")(self.load_llm)
            self.app.post(f"{prefix}/unload")(self.unload_llm)
            self.app.get(f"{prefix}/health")(self.health)
            self.app.get(f"{prefix}/halt")(self.halt_generation)
            self.app.get(f"{prefix}/stats")(self.send_stats)
            self.app.post(f"{prefix}/completions")(self.completions)
            self.app.post(f"{prefix}/responses")(self.responses)

            # OpenAI-compatible routes
            self.app.post(f"{prefix}/chat/completions")(self.chat_completions)
            self.app.get(f"{prefix}/models")(self.models)

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Launch an industry-standard LLM server",
            add_help=add_help,
        )

        parser.add_argument(
            "--port",
            required=False,
            type=int,
            default=DEFAULT_PORT,
            help=f"Port number to run the server on (default: {DEFAULT_PORT})",
        )
        parser.add_argument(
            "--log-level",
            required=False,
            type=str,
            default=DEFAULT_LOG_LEVEL,
            choices=["critical", "error", "warning", "info", "debug", "trace"],
            help=f"Logging level (default: {DEFAULT_LOG_LEVEL})",
        )

        return parser

    def _setup_server_common(
        self,
        port: int,
        truncate_inputs: bool = False,
        log_level: str = DEFAULT_LOG_LEVEL,
        threaded_mode: bool = False,
    ):
        """
        Common setup logic shared between run() and run_in_thread().

        Args:
            port: Port number for the server
            truncate_inputs: Whether to truncate inputs if they exceed max length
            log_level: Logging level to configure
            threaded_mode: Whether this is being set up for threaded execution
        """
        # Store truncation settings
        self.truncate_inputs = truncate_inputs

        # Define TRACE level
        logging.TRACE = 9  # Lower than DEBUG which is 10
        logging.addLevelName(logging.TRACE, "TRACE")

        # Add a convenience function at the module level
        def trace(message, *args, **kwargs):
            logging.log(logging.TRACE, message, *args, **kwargs)

        logging.trace = trace

        # Configure logging based on mode
        if threaded_mode:
            # Configure logging for warning level (to reduce noise in threaded execution)
            logging.getLogger("uvicorn.error").setLevel(logging.WARNING)
        else:
            # Configure logging to match uvicorn's format
            logging_level = getattr(logging, log_level.upper())
            logging.basicConfig(
                level=logging_level,
                format="%(levelprefix)s %(message)s",
                datefmt="%Y-%m-%d %H:%M:%S",
            )

            # Add uvicorn's log formatter
            logging.root.handlers[0].formatter = uvicorn.logging.DefaultFormatter(
                fmt="%(levelprefix)s %(message)s",
                use_colors=True,
            )

            # Ensure the log level is properly set
            logging.getLogger().setLevel(logging_level)

        # Update debug logging state after setting log level
        self.debug_logging_enabled = logging.getLogger().isEnabledFor(logging.DEBUG)

        if self.debug_logging_enabled:
            # Print the elapsed time for each request
            self.setup_middleware_timer()

        # Let the app know what port it's running on, so
        # that the lifespan can access it
        self.app.port = port

    def run(
        self,
        # ManagementTool has a required cache_dir arg, but
        # we always use the default cache directory
        _=None,
        port: int = DEFAULT_PORT,
        log_level: str = DEFAULT_LOG_LEVEL,
        truncate_inputs: bool = False,
    ):
        # Common setup
        self._setup_server_common(
            port=port,
            truncate_inputs=truncate_inputs,
            log_level=log_level,
            threaded_mode=False,
        )

        uvicorn.run(self.app, host="localhost", port=port, log_level=log_level)

    def run_in_thread(
        self,
        port: int = DEFAULT_PORT,
        host: str = "localhost",
        log_level: str = "warning",
        truncate_inputs: bool = False,
    ):
        """
        Set up the server for running in a thread.
        Returns a uvicorn server instance that can be controlled externally.
        """
        # Common setup
        self._setup_server_common(
            port=port,
            truncate_inputs=truncate_inputs,
            log_level=log_level,
            threaded_mode=True,
        )

        class CustomServer(UvicornServer):
            """Custom Uvicorn server that can be properly shutdown from another thread"""

            def install_signal_handlers(self):
                pass

        # Configure the server
        config = Config(
            app=self.app,
            host=host,
            port=port,
            log_level=log_level,
            log_config=None,
        )

        # Create and return the uvicorn server
        return CustomServer(config=config)

    async def _show_telemetry(self):
        """
        Show telemetry data in debug mode.
        """
        # Exit early if debug logging is disabled or no telemetry data is available
        if not self.debug_logging_enabled or self.tokens_per_second is None:
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

    def instructions(self):
        """
        Show instructions on how to use the server.
        """

        return get_instructions_html(port=self.app.port)

    def initialize_load_config(
        self, request: Union[ChatCompletionRequest, CompletionRequest]
    ) -> LoadConfig:
        """
        Turn the Request object into a partially-complete LoadConfig.

        The load_llm() method is responsible for filling in the rest of
        LoadConfig's parameters.
        """

        # Get model config
        if "/" in request.model:
            # We know the model is a Hugging Face checkpoint if it contains a /
            lc = LoadConfig(checkpoint=request.model)
        else:
            # The model should be a reference to a built-in model
            lc = LoadConfig(model_name=request.model)

        return lc

    async def completions(self, completion_request: CompletionRequest):
        """
        Stream completion responses using HTTP chunked transfer encoding.
        """

        lc = self.initialize_load_config(completion_request)

        # Load the model if it's different from the currently loaded one
        await self.load_llm(lc, internal_call=True)

        # Check if the model supports reasoning
        reasoning_first_token = self.llm_loaded.reasoning

        # If the model supports reasoning, we:
        # 1. add a <think> tag to the model's context
        # 2. ensure that the first token is a <think> token
        text = completion_request.prompt
        if reasoning_first_token:
            text += "<think>"

        # Prepare generation arguments
        generation_args = {
            "message": text,
            "stop": completion_request.stop,
            "temperature": completion_request.temperature,
            "max_new_tokens": completion_request.max_tokens,
        }

        if completion_request.stream:

            if completion_request.logprobs:
                logging.warning("logprobs is not supported for streaming completion")
            if completion_request.echo:
                logging.warning(
                    "`Echo` parameter is not supported for streaming completions"
                )

            # Stream the response
            async def generate():
                # Declare it's the same variable from outside scope
                # This is necessary because the variable is modified
                # in the inner function
                nonlocal reasoning_first_token

                async for token in self._generate_tokens(**generation_args):
                    choice = CompletionChoice(
                        text=("<think>" + token if reasoning_first_token else token),
                        index=0,
                        finish_reason="stop",
                        logprobs=None,
                    )

                    completion = Completion(
                        id="0",
                        choices=[choice],
                        model=self.llm_loaded.checkpoint,
                        object="text_completion",
                        created=int(time.time()),
                    )

                    # Format as SSE
                    reasoning_first_token = False
                    yield f"data: {completion.model_dump_json()}\n\n".encode("utf-8")

                # Send the [DONE] marker
                yield b"data: [DONE]\n\n"

            return StreamingResponse(
                generate(),
                media_type="text/event-stream",
                headers={
                    "Cache-Control": "no-cache",
                    "Connection": "keep-alive",
                },
            )

        # If streaming is not requested, collect all generated tokens into a single response
        else:
            full_response = text if completion_request.echo else ""
            async for token in self._generate_tokens(**generation_args):
                full_response += token

            # If logprobs are requested, create a logprobs object
            logprobs = None
            if completion_request.logprobs:

                # Compute the logprobs
                text_offset, token_logprobs, tokens, top_logprobs = (
                    self.model.compute_logprobs(
                        text=full_response,
                        tokenizer=self.tokenizer,
                        logprobs=completion_request.logprobs,
                    )
                )
                logprobs = Logprobs.model_construct(
                    text_offset=text_offset,
                    token_logprobs=token_logprobs,
                    tokens=tokens,
                    top_logprobs=top_logprobs,
                )

            choice = CompletionChoice(
                text=full_response,
                index=0,
                finish_reason="stop",
                logprobs=logprobs,
            )

            return Completion(
                id="0",
                choices=[choice],
                model=self.llm_loaded.checkpoint,
                object="text_completion",
                created=int(time.time()),
            )

    async def chat_completions(self, chat_completion_request: ChatCompletionRequest):
        """
        Stream chat completion responses using HTTP chunked transfer encoding.
        """

        if chat_completion_request.logprobs:
            logging.warning("logprobs is not supported on chat completion")

        lc = self.initialize_load_config(chat_completion_request)

        # Load the model if it's different from the currently loaded one
        await self.load_llm(lc, internal_call=True)

        if self.llm_loaded.recipe == "llamacpp":
            return llamacpp.chat_completion(
                chat_completion_request, self.llama_telemetry
            )

        # Convert chat messages to text using the model's chat template
        text = self.apply_chat_template(
            chat_completion_request.messages,
            tools=chat_completion_request.tools,
        )

        # If the model supports reasoning, we:
        # 1. add a <think> tag to the model's context
        # 2. ensure that the first token is a <think> token
        reasoning_first_token = self.llm_loaded.reasoning

        if reasoning_first_token:
            text += "<think>"
        # Set the max_new_tokens parameter
        if (
            chat_completion_request.max_completion_tokens
            and chat_completion_request.max_tokens
        ):
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail=(
                    "Both max_tokens and max_completion_tokens were provided. "
                    "Please use only one of these parameters.",
                ),
            )
        max_new_tokens = (
            chat_completion_request.max_completion_tokens
            if chat_completion_request.max_completion_tokens
            else chat_completion_request.max_tokens
        )

        # Prepare generation arguments
        generation_args = {
            "message": text,
            "stop": chat_completion_request.stop,
            "temperature": chat_completion_request.temperature,
            "max_new_tokens": max_new_tokens,
        }

        if chat_completion_request.tools:
            # Get the tool call pattern
            tool_call_pattern = get_tool_call_pattern(
                self.tokenizer.auto_tokenizer.added_tokens_decoder
            )

        if chat_completion_request.stream:

            # Stream the response
            async def generate():
                # Declare it's the same variable from outside scope
                # This is necessary because the variable is modified
                # in the inner function
                nonlocal reasoning_first_token

                # Keep track of the full response for tool call extraction
                full_response = ""

                async for token in self._generate_tokens(**generation_args):
                    # Continuously look for tool calls embedded into the generated text
                    openai_tool_calls = None
                    if chat_completion_request.tools:

                        # Append the token to the full response
                        full_response += token

                        tool_calls, _ = extract_tool_calls(
                            full_response,
                            tool_call_pattern,
                        )

                        # If there are tool calls, reset the full response for the next tool call
                        if tool_calls:
                            openai_tool_calls = []
                            full_response = ""
                        for tool_call in tool_calls:
                            openai_tool_calls.append(
                                ChoiceDeltaToolCall(
                                    index=0,
                                    id="-",
                                    function=ChoiceDeltaToolCallFunction(
                                        arguments=json.dumps(tool_call["arguments"]),
                                        name=tool_call["name"],
                                    ),
                                    type="function",
                                )
                            )

                    # Create a ChatCompletionChunk
                    chunk = ChatCompletionChunk.model_construct(
                        id="0",
                        object="chat.completion.chunk",
                        created=int(time.time()),
                        model=self.llm_loaded.checkpoint,
                        choices=[
                            Choice.model_construct(
                                index=0,
                                delta=ChoiceDelta(
                                    content=(
                                        "<think>" + token
                                        if reasoning_first_token
                                        else token
                                    ),
                                    function_call=None,
                                    role="assistant",
                                    tool_calls=openai_tool_calls,
                                    refusal=None,
                                ),
                                finish_reason=None,
                                logprobs=None,
                            )
                        ],
                    )

                    # Format as SSE
                    reasoning_first_token = False
                    yield f"data: {chunk.model_dump_json()}\n\n".encode("utf-8")

                # Send the [DONE] marker
                yield b"data: [DONE]\n\n"

            return StreamingResponse(
                generate(),
                media_type="text/event-stream",
                headers={
                    "Cache-Control": "no-cache",
                    "Connection": "keep-alive",
                },
            )

        # If streaming is not requested, collect all generated tokens into a single response
        else:
            full_response = "<think>" if reasoning_first_token else ""
            async for token in self._generate_tokens(**generation_args):
                full_response += token

            # Extract tool calls from the response
            openai_tool_calls = None
            if chat_completion_request.tools:
                tool_calls, full_response = extract_tool_calls(
                    full_response, tool_call_pattern
                )
                if tool_calls:
                    openai_tool_calls = []
                for tool_call in tool_calls:
                    openai_tool_calls.append(
                        ChatCompletionMessageToolCall(
                            id="-",
                            function=Function(
                                arguments=json.dumps(tool_call["arguments"]),
                                name=tool_call["name"],
                            ),
                            type="function",
                        )
                    )

            ccm = ChatCompletionMessage(
                content=full_response,
                role="assistant",
                refusal=None,
                audio=None,
                function_call=None,
                tool_calls=openai_tool_calls,
            )

            choice = Choice(
                finish_reason="stop",
                index=0,
                message=ccm,
                logprobs=None,
            )

            return ChatCompletion(
                id="0",
                choices=[choice],
                model=self.llm_loaded.checkpoint,
                object="chat.completion",
                created=int(time.time()),
            )

    def apply_chat_template(
        self, messages: list[dict], tools: list[dict] | None = None
    ):
        """
        Apply the model's chat template to the messages.
        """
        if self.tokenizer.chat_template:

            return self.tokenizer.apply_chat_template(
                messages,
                tokenize=False,
                add_generation_prompt=True,
                tools=tools,
            )

        # Fallback to a standardized template if the model doesn't provide one
        logging.warning("No chat template found. Using default template.")
        formatted_messages = []
        for msg in messages:
            role = msg.get("role", "user")
            content = msg.get("content", "")
            role_marker = "<|assistant|>" if role == "assistant" else "<|user|>"
            formatted_messages.append(f"{role_marker}\n{content} <|end|>")
        return "\n".join(formatted_messages) + "\n<|assistant|>"

    async def responses(self, responses_request: ResponsesRequest):
        """
        Stream responses using HTTP chunked transfer encoding.
        """

        lc = self.initialize_load_config(responses_request)

        # Load the model if it's different from the currently loaded one
        await self.load_llm(lc, internal_call=True)

        # Convert chat messages to text using the model's chat template
        if isinstance(responses_request.input, str):
            text = responses_request.input
        else:
            text = self.apply_chat_template(responses_request.input)

        # If the model supports reasoning, we:
        # 1. add a <think> tag to the model's context
        # 2. ensure that the first token is a <think> token
        reasoning_first_token = self.llm_loaded.reasoning

        if reasoning_first_token:
            text += "<think>"

        # Prepare generation arguments
        generation_args = {
            "message": text,
            "temperature": responses_request.temperature,
            "max_new_tokens": responses_request.max_output_tokens,
        }

        if responses_request.stream:

            # Stream the response
            async def generate():
                # Declare it's the same variable from outside scope
                # This is necessary because the variable is modified
                # in the inner function
                nonlocal reasoning_first_token

                # Send initial creation event
                response = Response(
                    id="0",
                    model=self.llm_loaded.checkpoint,
                    created_at=int(time.time()),
                    object="response",
                    output=[],
                    parallel_tool_calls=True,
                    tool_choice="auto",
                    tools=[],
                )
                created_event = ResponseCreatedEvent(
                    response=response,
                    type="response.created",
                    sequence_number=0,
                )
                yield f"data: {created_event.model_dump_json()}\n\n".encode("utf-8")

                full_response = "<think>" if reasoning_first_token else ""

                async for token in self._generate_tokens(**generation_args):

                    # Create an event
                    delta_event = ResponseTextDeltaEvent(
                        content_index=0,
                        delta=("<think>" + token if reasoning_first_token else token),
                        item_id="0 ",
                        output_index=0,
                        type="response.output_text.delta",
                        sequence_number=0,
                    )
                    full_response += token

                    # Format as SSE
                    reasoning_first_token = False
                    yield f"data: {delta_event.model_dump_json()}\n\n".encode("utf-8")

                # Send the completed event
                response_output_message = ResponseOutputMessage(
                    id="0",
                    content=[
                        ResponseOutputText(
                            annotations=[],
                            text=full_response,
                            type="output_text",
                        )
                    ],
                    role="assistant",
                    status="completed",
                    type="message",
                )
                response = Response(
                    id="0",
                    model=self.llm_loaded.checkpoint,
                    created_at=int(time.time()),
                    object="response",
                    output=[response_output_message],
                    parallel_tool_calls=True,
                    tool_choice="auto",
                    tools=[],
                )
                completed_event = ResponseCompletedEvent(
                    response=response,
                    type="response.completed",
                    sequence_number=0,
                )
                yield f"data: {completed_event.model_dump_json()}\n\n".encode("utf-8")

                # Send the [DONE] marker
                yield b"data: [DONE]\n\n"

            return StreamingResponse(
                generate(),
                media_type="text/event-stream",
                headers={
                    "Cache-Control": "no-cache",
                    "Connection": "keep-alive",
                },
            )

        # If streaming is not requested, collect all generated tokens into a single response
        else:
            full_response = "<think>" if reasoning_first_token else ""
            async for token in self._generate_tokens(**generation_args):
                full_response += token

            # Send the completed event
            response_output_message = ResponseOutputMessage(
                id="0",
                content=[
                    ResponseOutputText(
                        annotations=[],
                        text=full_response,
                        type="output_text",
                    )
                ],
                role="assistant",
                status="completed",
                type="message",
            )
            return Response(
                id="0",
                model=self.llm_loaded.checkpoint,
                created_at=int(time.time()),
                object="response",
                output=[response_output_message],
                parallel_tool_calls=True,
                tool_choice="auto",
                tools=[],
            )

    async def _generate_tokens(
        self,
        message: str,
        stop: list[str] | str | None = None,
        max_new_tokens: int | None = None,
        temperature: float | None = None,
    ):
        """
        Core streaming completion logic, separated from response handling.
        Returns an async generator that yields tokens.
        """
        model = self.model
        tokenizer = self.tokenizer

        # Reset the early-exit flag before we start each generation
        self.stop_event.clear()

        input_ids = tokenizer(message, return_tensors="pt").input_ids

        # Process stop sequences
        stop_sequences = []
        if stop is not None:
            if isinstance(stop, str):
                stop_sequences = [stop]
            else:
                stop_sequences = stop[:4]  # Limit to 4 sequences as per spec

        # Set up the generation parameters
        if "oga-" in self.llm_loaded.recipe:
            from lemonade.tools.ort_genai.oga import OrtGenaiStreamer

            streamer = OrtGenaiStreamer(tokenizer)
            self.input_tokens = len(input_ids)
        else:
            streamer = TextIteratorStreamer(
                tokenizer,
                skip_prompt=True,
            )
            self.input_tokens = len(input_ids[0])

        if (
            self.llm_loaded.max_prompt_length
            and self.input_tokens > self.llm_loaded.max_prompt_length
        ):
            if self.truncate_inputs:
                # Truncate input ids
                truncate_amount = self.input_tokens - self.llm_loaded.max_prompt_length
                input_ids = input_ids[: self.llm_loaded.max_prompt_length]

                # Update token count
                self.input_tokens = len(input_ids)

                # Show warning message
                truncation_message = (
                    f"Input exceeded {self.llm_loaded.max_prompt_length} tokens. "
                    f"Truncated {truncate_amount} tokens."
                )
                logging.warning(truncation_message)
            else:
                raise RuntimeError(
                    f"Prompt tokens ({self.input_tokens}) cannot be greater "
                    f"than the model's max prompt length ({self.llm_loaded.max_prompt_length})"
                )

        # Log the input tokens early to avoid this not showing due to potential crashes
        logging.debug(f"Input Tokens: {self.input_tokens}")
        logging.trace(f"Input Message: {message}")

        stopping_criteria = StoppingCriteriaList([StopOnEvent(self.stop_event)])

        generation_kwargs = {
            "input_ids": input_ids,
            "streamer": streamer,
            "max_new_tokens": (
                max_new_tokens if max_new_tokens else DEFAULT_MAX_NEW_TOKENS
            ),
            "min_new_tokens": 1,
            "pad_token_id": tokenizer.eos_token_id,
            "stopping_criteria": stopping_criteria,
            "temperature": temperature,
        }

        # Initialize performance variables
        generation_start_time = time.perf_counter()
        first_token = True
        self.decode_token_times = []
        self.output_tokens = 0

        # Begin generation
        thread = GeneratorThread(
            streamer, target=model.generate, kwargs=generation_kwargs
        )
        thread.start()

        # Acquire the generation semaphore
        await self._generate_semaphore.acquire()
        active_generations = (
            self.max_concurrent_generations
            - self._generate_semaphore._value  # pylint: disable=protected-access
        )

        logging.debug(f"Active generations: {active_generations}")

        try:
            # Generate the response using streaming
            new_text = ""
            for new_text in streamer:
                # Yield control back to the event loop
                # This gives the FastAPI server a chance to send the chunks to the client
                await asyncio.sleep(0)

                # Capture performance stats about this token
                self.output_tokens = self.output_tokens + 1
                if first_token:
                    self.time_to_first_token = (
                        time.perf_counter() - generation_start_time
                    )
                    first_token = False
                else:
                    self.decode_token_times.append(
                        time.perf_counter() - next_token_start_time
                    )
                next_token_start_time = time.perf_counter()

                # Remove the EOS token from the response if needed
                if hasattr(self.tokenizer, "eos_token"):
                    new_text = new_text.replace(self.tokenizer.eos_token, "")

                # Check for stop sequences
                if stop_sequences:
                    for stop_seq in stop_sequences:
                        if stop_seq in new_text:
                            # Make sure we yield the text up to before the stop sequence
                            new_text = new_text[: new_text.find(stop_seq)]
                            self.stop_event.set()

                yield new_text

                # Allow the user to finish the response early
                if self.stop_event.is_set():
                    logging.info("Stopping generation early.")
                    break

            if len(self.decode_token_times) > 0:
                self.tokens_per_second = 1 / statistics.mean(self.decode_token_times)
            else:
                self.tokens_per_second = 0

        finally:
            thread.join()

            # Release the semaphore when generation is complete (or if an error occurs)
            self._generate_semaphore.release()
            active_generations = (
                self.max_concurrent_generations
                - self._generate_semaphore._value  # pylint: disable=protected-access
            )

            # Check if an exception occurred in the generation thread
            # If it did, raise it as an HTTPException so that the client
            # knows they wont be getting a completion
            if thread.exception:
                raise HTTPException(
                    status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                    detail=f"Completion failure: {thread.exception}",
                )

            # Display telemetry if in debug mode
            await self._show_telemetry()

    async def send_stats(self):
        """
        Send performance statistics to the client.
        """
        # If using llama server, get telemetry from the telemetry instance
        if self.llm_loaded and self.llm_loaded.recipe == "llamacpp":
            return self.llama_telemetry.get_telemetry_data()

        # For built-in server, use the existing telemetry
        return {
            "time_to_first_token": self.time_to_first_token,
            "tokens_per_second": self.tokens_per_second,
            "input_tokens": self.input_tokens,
            "output_tokens": self.output_tokens,
            "decode_token_times": self.decode_token_times,
        }

    async def halt_generation(self):
        """
        Allow the client to halt an in-progress generation.
        """

        self.stop_event.set()

        return {
            "terminated": True,
        }

    async def health(self):
        """
        Report server health information to the client.
        """
        self.stop_event.set()

        return {
            "status": "ok",
            "checkpoint_loaded": (
                self.llm_loaded.checkpoint if self.llm_loaded else None
            ),
            "model_loaded": (
                self.llm_loaded.model_name
                if (self.llm_loaded and self.llm_loaded.model_name)
                else None
            ),
        }

    def model_load_failure(self, model_reference: str, message: Optional[str] = None):
        """
        Clean up after a model load failure, then log it and raise
        an HTTPException with details.
        """
        self.llm_loaded = None
        self.tokenizer = None
        self.model = None

        default_message = f"model {model_reference} not found"
        if message:
            detail = message
        else:
            detail = default_message

        logging.exception(f"Tried to load LLM {model_reference} and failed: {detail}")

        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=detail,
        )

    def recipe_missing_error(self, model_reference: str):
        self.model_load_failure(
            model_reference,
            message=(
                f"Attempted to load model by checkpoint name {model_reference}, "
                "however the required 'recipe' parameter was not provided"
            ),
        )

    async def pull(self, config: PullConfig):
        """
        Install a supported LLM by its Lemonade Model Name.
        """

        # Install the model
        ModelManager().download_models([config.model_name])

        # Refresh the list of downloaded models, to ensure it
        # includes the model we just installed
        self.local_models = ModelManager().downloaded_models_enabled

    async def load_llm(self, config: LoadConfig, internal_call=False):
        """
        Load an LLM into system memory.
            config: the information required to load the model
            internal_call: indicates whether the call to this function came from
                an endpoint (False) or a method of this class (True)

        There are 3 ways this method can be called:
          1. An external application asks to load a model by name, using the load endpoint
              a. This only differs from #2 in that an external application may
                  provide more parameters than in #2, so we need to validate
                  that those parameters are ok.
              b. Load the model

          2. An external application asks to load a model by name,
                  using the completions or chat_completions endpoints
              a. Look up the name in the built-in model dictionary to create
                  a fully-populated LoadConfig.
              b. Load the model

          3. An external application asks to load a model by checkpoint and recipe,
                  using the load endpoint
              a. Populate the checkpoint and recipe into a LoadConfig
              b. Load the model

          4. Completions or ChatCompletions asks to "load" a model by checkpoint
              a. This is only available when #3 has already been executed
              b. Verify that the checkpoint is already loaded,
                  and raise an exception if it hasn't (don't load anything new)
        """
        try:
            await self._load_lock.acquire()

            # Acquire all generate locks
            for _ in range(self.max_concurrent_generations):
                await self._generate_semaphore.acquire()

            # We will populate a LoadConfig that has all of the required fields
            config_to_use: LoadConfig

            # First, validate that the arguments are valid
            if config.model_name:
                # Get the dictionary of supported model from disk
                supported_models = ModelManager().supported_models

                # Refer to the model by name, since we know the name
                model_reference = config.model_name

                if config.checkpoint or config.recipe:
                    # Option #1, verify that there are no parameter mismatches
                    built_in_config = supported_models[config.model_name]
                    if config.checkpoint != built_in_config["checkpoint"]:
                        self.model_load_failure(
                            model_reference,
                            message=(
                                f"Load request for model_name={config.model_name} "
                                "included a mismatched "
                                f"checkpoint={config.checkpoint} parameter. Remove the checkpoint "
                                f"parameter, or change it to {built_in_config['checkpoint']}."
                            ),
                        )
                    if config.recipe != built_in_config["recipe"]:
                        self.model_load_failure(
                            model_reference,
                            message=(
                                f"Load request for model_name={config.model_name} "
                                "included a mismatched "
                                f"recipe={config.recipe} parameter. Remove the checkpoint "
                                f"parameter, or change it to {built_in_config['recipe']}."
                            ),
                        )

                    # Use the config as-is
                    config_to_use = config
                else:
                    # Option #2, look up the config from the supported models dictionary
                    config_to_use = LoadConfig(**supported_models[config.model_name])

            elif config.checkpoint:
                # Refer to the model by checkpoint
                model_reference = config.checkpoint

                if config.recipe and not internal_call:
                    # Option 3, use the config as-is, but add a custom model name
                    config_to_use = config
                    config_to_use.model_name = "Custom"
                elif internal_call:
                    # Option 4, make sure the right checkpoint is loaded and then return
                    if (
                        self.llm_loaded
                        and config.checkpoint == self.llm_loaded.checkpoint
                    ):
                        return {
                            "status": "success",
                            "message": f"Model already loaded: {model_reference}",
                        }
                    else:
                        self.model_load_failure(
                            model_reference,
                            message=(
                                "Attempted run completions by using model=<checkpoint name>, "
                                "however, "
                                "this feature only works if the model has already been loaded "
                                "using the load endpoint."
                            ),
                        )
                else:
                    self.recipe_missing_error(model_reference)
            else:
                self.model_load_failure(
                    None,
                    message="Load requests must contain either a model_name or a "
                    "checkpoint parameter",
                )

            # Caching mechanism: if the checkpoint is already loaded there is nothing else to do
            if (
                self.llm_loaded
                and config_to_use.checkpoint == self.llm_loaded.checkpoint
            ):
                return {
                    "status": "success",
                    "message": f"Model already loaded: {model_reference}",
                }

            # Unload the current model if needed
            if self.llm_loaded:
                await self.unload_llm(require_lock=False)

            logging.info(f"Loading llm: {model_reference}")
            try:
                if config_to_use.recipe == "llamacpp":
                    self.llama_server_process = llamacpp.server_load(
                        checkpoint=config_to_use.checkpoint,
                        model_reference=model_reference,
                        telemetry=self.llama_telemetry,
                    )

                else:
                    self.model, self.tokenizer = lemonade_api.from_pretrained(
                        checkpoint=config_to_use.checkpoint, recipe=config_to_use.recipe
                    )
                self.llm_loaded = config_to_use

                return {
                    "status": "success",
                    "message": f"Loaded model: {model_reference}",
                }
            except HTTPException:
                raise
            except Exception:  # pylint: disable=broad-exception-caught
                self.model_load_failure(model_reference)

        finally:
            self._load_lock.release()

            # Release all generate locks
            for _ in range(self.max_concurrent_generations):
                self._generate_semaphore.release()

            # Refresh the list of downloaded models, to ensure it
            # includes the model we just loaded
            if config.model_name not in self.local_models:
                self.local_models = ModelManager().downloaded_models_enabled

    async def unload_llm(self, require_lock: bool = True):
        try:
            if require_lock:
                await self._load_lock.acquire()

                # Acquire all generate locks
                for _ in range(self.max_concurrent_generations):
                    await self._generate_semaphore.acquire()

            if self.llm_loaded.recipe == "llamacpp":
                self.llama_server_process.terminate()

            self.llm_loaded = None
            self.tokenizer = None
            self.model = None
            return {"status": "success", "message": "Unloaded model"}
        except Exception as e:  # pylint: disable=broad-exception-caught
            return {
                "status": "error",
                "message": f"Failed to unload model: {str(e)}",
            }
        finally:
            if require_lock:
                self._load_lock.release()

                # Release all generate locks
                for _ in range(self.max_concurrent_generations):
                    self._generate_semaphore.release()

    async def models(self):
        """
        Return a list of available models in OpenAI-compatible format.
        """
        models_list = []
        for model in self.local_models:
            m = ServerModel(
                id=model,
                owned_by="lemonade",
                object="model",
                created=int(time.time()),
                checkpoint=self.local_models[model]["checkpoint"],
                recipe=self.local_models[model]["recipe"],
            )
            models_list.append(m)

        return {"object": "list", "data": models_list}

    def setup_middleware_timer(self):
        logging.info("Middleware set up")

        @self.app.middleware("http")
        async def log_request_time(request: Request, call_next):
            """
            Log the request processing time for any request.
            For streaming responses, wraps the body iterator to measure total time.
            Only applies the wrapper in debug mode.
            """
            start_time = time.perf_counter()
            response = await call_next(request)

            if (
                self.debug_logging_enabled
                and hasattr(response, "body_iterator")
                and response.body_iterator is not None
            ):
                original_iterator = response.body_iterator

                async def wrapped_iterator():
                    async for chunk in original_iterator:
                        yield chunk
                    request_time = time.perf_counter() - start_time
                    logging.debug(
                        f"Total request time (streamed): {request_time:.4f} seconds"
                    )

                response.body_iterator = wrapped_iterator()
            else:
                request_time = time.perf_counter() - start_time
                if self.debug_logging_enabled:
                    logging.debug(f"Total request time: {request_time:.4f} seconds")
            return response


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
