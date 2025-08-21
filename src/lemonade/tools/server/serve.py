import sys
import asyncio
import statistics
import time
from threading import Thread, Event
import logging
import platform
import tempfile
import traceback
from typing import Optional, Union
import json
from pathlib import Path

from fastapi import FastAPI, HTTPException, status, Request
from fastapi.responses import StreamingResponse
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
import uvicorn
from uvicorn.config import Config
from uvicorn.server import Server as UvicornServer
from tabulate import tabulate

from openai.types.completion import Completion, CompletionChoice
from openai.types.chat import ChatCompletion, ChatCompletionChunk
from openai.types.chat import ChatCompletionMessage
from openai.types.chat.chat_completion_message_tool_call import (
    ChatCompletionMessageToolCall,
    Function,
)
from openai.types.completion_usage import CompletionUsage
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
from lemonade.tools.server.wrapped_server import WrappedServer
from lemonade.tools.server.llamacpp import LlamaServer
from lemonade.tools.server.tool_calls import extract_tool_calls, get_tool_call_pattern
from lemonade.tools.server.webapp import get_webapp_html
from lemonade.tools.server.utils.port import lifespan

from lemonade_server.model_manager import ModelManager
from lemonade_server.pydantic_models import (
    DEFAULT_PORT,
    DEFAULT_HOST,
    DEFAULT_LOG_LEVEL,
    DEFAULT_LLAMACPP_BACKEND,
    DEFAULT_CTX_SIZE,
    LoadConfig,
    CompletionRequest,
    ChatCompletionRequest,
    EmbeddingsRequest,
    RerankingRequest,
    ResponsesRequest,
    PullConfig,
    DeleteConfig,
    LogLevelConfig,
)
from lemonade_server.settings import save_setting

# Set to a high number to allow for interesting experiences in real apps
# Tests should use the max_new_tokens argument to set a lower value
DEFAULT_MAX_NEW_TOKENS = 1500

# Only import tray on Windows
if platform.system() == "Windows":
    # pylint: disable=ungrouped-imports
    from lemonade.tools.server.tray import LemonadeTray, OutputDuplicator


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


class StopOnEvent:
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


class Server:
    """
    Open a web server that apps can use to communicate with the LLM.

    The server exposes these endpoints:
    - /api/v1/pull: install an LLM by its Lemonade Server Model Name.
    - /api/v1/delete: delete an LLM by its Lemonade Server Model Name.
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

    def __init__(
        self,
        port: int = DEFAULT_PORT,
        host: str = DEFAULT_HOST,
        log_level: str = DEFAULT_LOG_LEVEL,
        ctx_size: int = DEFAULT_CTX_SIZE,
        tray: bool = False,
        log_file: str = None,
        llamacpp_backend: str = DEFAULT_LLAMACPP_BACKEND,
    ):
        super().__init__()

        # Save args as members
        self.port = port
        self.host = host
        self.log_level = log_level
        self.ctx_size = ctx_size
        self.tray = tray
        self.log_file = log_file
        self.llamacpp_backend = llamacpp_backend

        # Initialize FastAPI app
        self.app = FastAPI(lifespan=lifespan)

        # Lifespan will load some tasks in the background, and then set the
        # app.initialized flag to True when this is done
        self.app.initialized = False

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

        # Set up Web App
        self.app.get("/")(self.webapp)

        # Mount a static assets dir for HTML responses, such
        # as the Web App
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

        # Subprocess handle for wrapped instance of llama_server.exe, etc.
        self.wrapped_server: WrappedServer = None

    def setup_routes(self, api_prefixes: list[str]):
        for prefix in api_prefixes:
            # Custom routes
            self.app.post(f"{prefix}/pull")(self.pull)
            self.app.post(f"{prefix}/delete")(self.delete)
            self.app.post(f"{prefix}/load")(self.load_llm)
            self.app.post(f"{prefix}/unload")(self.unload_llm)
            self.app.get(f"{prefix}/health")(self.health)
            self.app.get(f"{prefix}/halt")(self.halt_generation)
            self.app.get(f"{prefix}/stats")(self.send_stats)
            self.app.get(f"{prefix}/system-info")(self.get_system_info)
            self.app.post(f"{prefix}/completions")(self.completions)
            self.app.post(f"{prefix}/responses")(self.responses)
            self.app.post(f"{prefix}/log-level")(self.set_log_level)

            # OpenAI-compatible routes
            self.app.post(f"{prefix}/chat/completions")(self.chat_completions)
            self.app.post(f"{prefix}/embeddings")(self.embeddings)
            self.app.get(f"{prefix}/models")(self.models)

            # JinaAI routes (jina.ai/reranker/)
            self.app.post(f"{prefix}/reranking")(self.reranking)
            self.app.post(f"{prefix}/rerank")(self.reranking)

    async def set_log_level(self, config: LogLevelConfig):
        """
        Set the logging level of the server.
        """
        try:
            log_level_upper = config.level.upper()
            numeric_level = getattr(logging, log_level_upper, None)
            if not isinstance(numeric_level, int):
                raise ValueError(f"Invalid log level: {config.level}")

            # Get the root logger
            logger = logging.getLogger()
            logger.setLevel(numeric_level)

            # Update all handlers
            for handler in logger.handlers:
                handler.setLevel(numeric_level)

            logging.getLogger("uvicorn.error").setLevel(numeric_level)
            self.debug_logging_enabled = numeric_level <= logging.DEBUG

            # Save the setting
            save_setting("log_level", config.level)

            logging.info(f"Log level changed to: {config.level}")
            return {"status": "success", "message": f"Log level set to {config.level}"}
        except Exception as e:
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=f"Failed to set log level: {str(e)}",
            )

    def _log_request_parameters(self, request, endpoint_name: str):
        """
        Log request parameters excluding content fields like messages, prompt, or input.

        Args:
            request: Any request object (CompletionRequest, ChatCompletionRequest, etc.)
            endpoint_name: Name of the endpoint for logging context
        """
        if not logging.getLogger().isEnabledFor(logging.DEBUG):
            return

        # Fields to exclude from logging (content fields)
        excluded_fields = {"messages", "prompt", "input"}

        # Get all attributes from the request object
        request_params = {}
        if hasattr(request, "__dict__"):
            # For pydantic models, get the dict representation
            if hasattr(request, "model_dump"):
                all_params = request.model_dump()
            elif hasattr(request, "dict"):
                all_params = request.dict()
            else:
                all_params = request.__dict__

            # Filter out excluded fields and add special handling for certain fields
            for key, value in all_params.items():
                if key not in excluded_fields:
                    # Special handling for tools field - show count instead of full content
                    if key == "tools" and value is not None:
                        request_params[key] = (
                            f"{len(value)} tools" if isinstance(value, list) else value
                        )
                    # Special handling for input type in responses
                    elif key == "input" and hasattr(request, "input"):
                        request_params["input_type"] = type(value).__name__
                    else:
                        request_params[key] = value

        logging.debug(f"{endpoint_name} request parameters: {request_params}")

    def _setup_server_common(
        self,
        tray: bool = False,
        threaded_mode: bool = False,
    ):
        """
        Common setup logic shared between run() and run_in_thread().

        Args:
            tray: Whether to run the server in tray mode
            threaded_mode: Whether this is being set up for threaded execution
        """

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
            logging_level = getattr(logging, self.log_level.upper())

            # Set up file handler for logging to lemonade.log
            uvicorn_formatter = uvicorn.logging.DefaultFormatter(
                fmt="%(levelprefix)s %(message)s",
                use_colors=True,
            )
            if not self.log_file:
                self.log_file = tempfile.NamedTemporaryFile(
                    prefix="lemonade_", suffix=".log", delete=False
                ).name
            file_handler = logging.FileHandler(
                self.log_file, mode="a", encoding="utf-8"
            )
            file_handler.setLevel(logging_level)
            file_handler.setFormatter(uvicorn_formatter)

            # Set up console handler
            console_handler = logging.StreamHandler()
            console_handler.setLevel(logging_level)
            console_handler.setFormatter(uvicorn_formatter)

            # Configure root logger with both handlers
            logging.basicConfig(
                level=logging_level,
                handlers=[file_handler, console_handler],
                force=True,
            )

        # Update debug logging state after setting log level
        self.debug_logging_enabled = logging.getLogger().isEnabledFor(logging.DEBUG)
        if tray:
            # Save original stdout/stderr
            sys.stdout = OutputDuplicator(self.log_file, sys.stdout)
            sys.stderr = OutputDuplicator(self.log_file, sys.stderr)

            # Open lemonade server in tray mode
            # lambda function used for deferred instantiation and thread safety
            LemonadeTray(
                self.log_file, self.port, lambda: self, log_level=self.log_level
            ).run()
            sys.exit(0)

        if self.debug_logging_enabled:
            # Print the elapsed time for each request
            self.setup_middleware_timer()

        # Let the app know what port it's running on, so
        # that the lifespan can access it
        self.app.port = self.port
        # FastAPI already has a `host` function and we cannot use `_host` as
        # PyLint will believe its private
        self.app.host_ = self.host

    def run(self):
        # Common setup
        self._setup_server_common(
            threaded_mode=False,
            tray=self.tray,
        )

        uvicorn.run(self.app, host=self.host, port=self.port, log_level=self.log_level)

    def run_in_thread(self, host: str = "localhost"):
        """
        Set up the server for running in a thread.
        Returns a uvicorn server instance that can be controlled externally.
        """
        # Common setup
        self._setup_server_common(
            threaded_mode=True,
            tray=False,
        )

        class CustomServer(UvicornServer):
            """Custom Uvicorn server that can be properly shutdown from another thread"""

            def install_signal_handlers(self):
                pass

        # Configure the server
        config = Config(
            app=self.app,
            host=host,
            port=self.port,
            log_level=self.log_level,
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

    def webapp(self):
        """
        Serve the Web App to the user's browser.
        """

        return get_webapp_html(port=self.app.port)

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
            # This scenario is only supported for run-as-thread
            lc = LoadConfig(model_name="custom", checkpoint=request.model)
        else:
            # The model should be a reference to a built-in model
            lc = LoadConfig(model_name=request.model)

        return lc

    async def completions(self, completion_request: CompletionRequest):
        """
        Stream completion responses using HTTP chunked transfer encoding.
        """

        lc = self.initialize_load_config(completion_request)

        # Log request parameters (excluding message content for brevity)
        self._log_request_parameters(completion_request, "Completions")

        # Load the model if it's different from the currently loaded one
        await self.load_llm(lc)

        if self.llm_loaded.recipe == "llamacpp":
            return self.wrapped_server.completion(completion_request)

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
            "repeat_penalty": completion_request.repeat_penalty,
            "top_k": completion_request.top_k,
            "top_p": completion_request.top_p,
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

            usage = CompletionUsage(
                prompt_tokens=self.input_tokens,
                completion_tokens=self.output_tokens,
                total_tokens=self.input_tokens + self.output_tokens,
            )

            return Completion(
                id="0",
                choices=[choice],
                usage=usage,
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

        # Log request parameters (excluding message history for brevity)
        self._log_request_parameters(chat_completion_request, "Chat completions")

        # Load the model if it's different from the currently loaded one
        await self.load_llm(lc)

        if self.llm_loaded.recipe == "llamacpp":
            return self.wrapped_server.chat_completion(chat_completion_request)

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
            "repeat_penalty": chat_completion_request.repeat_penalty,
            "top_k": chat_completion_request.top_k,
            "top_p": chat_completion_request.top_p,
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

            usage = CompletionUsage(
                prompt_tokens=self.input_tokens,
                completion_tokens=self.output_tokens,
                total_tokens=self.input_tokens + self.output_tokens,
            )

            return ChatCompletion(
                id="0",
                choices=[choice],
                usage=usage,
                model=self.llm_loaded.checkpoint,
                object="chat.completion",
                created=int(time.time()),
            )

    async def embeddings(self, embeddings_request: EmbeddingsRequest):
        """
        Generate embeddings for the provided input.
        """
        # Initialize load config from embeddings request
        lc = LoadConfig(model_name=embeddings_request.model)

        # Load the model if it's different from the currently loaded one
        await self.load_llm(lc)

        if self.llm_loaded.recipe == "llamacpp":
            try:
                return self.wrapped_server.embeddings(embeddings_request)
            except Exception as e:  # pylint: disable=broad-exception-caught
                # Check if model has embeddings label
                model_info = ModelManager().supported_models.get(
                    self.llm_loaded.model_name, {}
                )
                if "embeddings" not in model_info.get("labels", []):
                    raise HTTPException(
                        status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                        detail="You tried to generate embeddings for a model that is "
                        "not labeled as an embeddings model. Please use another model "
                        "or re-register the current model with the 'embeddings' label.",
                    ) from e
                else:
                    raise e
        else:
            raise HTTPException(
                status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                detail=f"Embeddings not supported for recipe: {self.llm_loaded.recipe}",
            )

    async def reranking(self, reranking_request: RerankingRequest):
        """
        Rerank documents based on their relevance to a query.
        """
        # Initialize load config from reranking request
        lc = LoadConfig(model_name=reranking_request.model)

        # Load the model if it's different from the currently loaded one
        await self.load_llm(lc)

        if self.llm_loaded.recipe == "llamacpp":
            try:
                return self.wrapped_server.reranking(reranking_request)
            except Exception as e:  # pylint: disable=broad-exception-caught
                # Check if model has reranking label
                model_info = ModelManager().supported_models.get(
                    self.llm_loaded.model_name, {}
                )
                if "reranking" not in model_info.get("labels", []):
                    raise HTTPException(
                        status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                        detail="You tried to use reranking for a model that is "
                        "not labeled as a reranking model. Please use another model "
                        "or re-register the current model with the 'reranking' label.",
                    ) from e
                else:
                    raise e
        else:
            raise HTTPException(
                status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                detail=f"Reranking not supported for recipe: {self.llm_loaded.recipe}",
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

        # Log request parameters (excluding message history for brevity)
        self._log_request_parameters(responses_request, "Responses")

        # Load the model if it's different from the currently loaded one
        await self.load_llm(lc)

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
            "repeat_penalty": responses_request.repeat_penalty,
            "top_k": responses_request.top_k,
            "top_p": responses_request.top_p,
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
        repeat_penalty: float | None = None,
        top_k: int | None = None,
        top_p: float | None = None,
    ):
        """
        Core streaming completion logic, separated from response handling.
        Returns an async generator that yields tokens.
        """

        while not self.app.initialized:
            # Wait for the app's background tasks to finish before
            # allowing generation to proceed
            logging.debug("Waiting for server to fully initialize")
            asyncio.sleep(0.5)
        # These should already be imported as part of the app initialization process,
        # they are just here to make 100% certain and to make the linter happy
        from transformers import TextIteratorStreamer, StoppingCriteriaList

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
            from lemonade.tools.oga.utils import OrtGenaiStreamer

            streamer = OrtGenaiStreamer(tokenizer)
            self.input_tokens = len(input_ids)
        else:
            streamer = TextIteratorStreamer(
                tokenizer,
                skip_prompt=True,
            )
            self.input_tokens = len(input_ids[0])

        # For non-llamacpp recipes, truncate inputs to ctx_size if needed
        if self.llm_loaded.recipe != "llamacpp" and self.input_tokens > self.ctx_size:
            # Truncate input ids
            truncate_amount = self.input_tokens - self.ctx_size
            input_ids = input_ids[: self.ctx_size]

            # Update token count
            self.input_tokens = len(input_ids)

            # Show warning message
            truncation_message = (
                f"Input exceeded {self.ctx_size} tokens. "
                f"Truncated {truncate_amount} tokens from the beginning."
            )
            logging.warning(truncation_message)

        # Log the input tokens early to avoid this not showing due to potential crashes
        logging.debug(f"Input Tokens: {self.input_tokens}")
        logging.trace(f"Input Message: {message}")

        if self.llm_loaded.recipe.startswith("hf"):
            stopping_criteria = StoppingCriteriaList([StopOnEvent(self.stop_event)])
        else:
            # HF expects StoppingCriteriaList, which requires torch
            # If we aren't using HF, we can just use a list of StopOnEvent to
            # avoid the torch dep
            stopping_criteria = [StopOnEvent(self.stop_event)]

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
            "repeat_penalty": repeat_penalty,
            "top_k": top_k,
            "top_p": top_p,
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
            return self.wrapped_server.telemetry.get_telemetry_data()

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

    async def get_system_info(self, request: Request):
        """
        Return system and device enumeration information.
        Supports optional 'verbose' query parameter.
        """
        from lemonade.common.system_info import (
            get_system_info_dict,
            get_device_info_dict,
            get_system_info as get_system_info_obj,
        )

        # Get verbose parameter from query string (default to False)
        verbose = request.query_params.get("verbose", "false").lower() in ["true", "1"]

        info = get_system_info_dict()
        info["devices"] = get_device_info_dict()

        # Filter out verbose-only information if not in verbose mode
        if not verbose:
            essential_keys = ["OS Version", "Processor", "Physical Memory", "devices"]
            info = {k: v for k, v in info.items() if k in essential_keys}
        else:
            # In verbose mode, add Python packages at the end
            system_info_obj = get_system_info_obj()
            info["Python Packages"] = system_info_obj.get_python_packages()

        return info

    def model_load_failure(self, model_reference: str, message: Optional[str] = None):
        """
        Clean up after a model load failure, then log it and raise
        an HTTPException with details.
        """
        self.llm_loaded = None
        self.tokenizer = None
        self.model = None

        default_message = "see stack trace and error message below"
        if message:
            detail = message
        else:
            detail = default_message

        logging.exception(f"Tried to load LLM {model_reference} and failed: {detail}")

        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=detail,
        )

    async def pull(self, config: PullConfig):
        """
        Install a supported LLM by its Lemonade Model Name.
        """

        # Install the model
        ModelManager().download_models(
            [config.model_name],
            checkpoint=config.checkpoint,
            recipe=config.recipe,
            reasoning=config.reasoning,
            mmproj=config.mmproj,
            # The pull endpoint will download an upgraded model if available, even
            # if we already have a local copy of the model
            do_not_upgrade=False,
        )

        # Refresh the list of downloaded models, to ensure it
        # includes the model we just installed
        self.local_models = ModelManager().downloaded_models_enabled

    async def delete(self, config: DeleteConfig):
        """
        Delete a supported LLM by its Lemonade Model Name.
        """
        try:
            # If the model to be deleted is currently loaded, unload it first
            if self.llm_loaded and self.llm_loaded.model_name == config.model_name:
                await self.unload_llm(require_lock=True)

            # Delete the model
            ModelManager().delete_model(config.model_name)

            # Refresh the list of downloaded models
            self.local_models = ModelManager().downloaded_models_enabled

            return {
                "status": "success",
                "message": f"Deleted model: {config.model_name}",
            }
        except ValueError as e:
            raise HTTPException(
                status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                detail=str(e),
            )
        except Exception as e:
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=f"Failed to delete model {config.model_name}: {str(e)}",
            )

    async def load_llm(self, config: LoadConfig):
        """
        Load a registered LLM into system memory. Install the model first, if needed.
            config: the information required to load the model
        """
        try:
            await self._load_lock.acquire()

            # Acquire all generate locks
            for _ in range(self.max_concurrent_generations):
                await self._generate_semaphore.acquire()

            # Make sure the model is already registered
            supported_models = ModelManager().supported_models

            # The `custom` name allows run-as-thread servers to bypass loading
            if config.model_name == "custom":
                config_to_use = config
            else:
                if config.model_name not in supported_models.keys():
                    self.model_load_failure(
                        config.model_name,
                        message=(
                            f"Load request for model_name={config.model_name} "
                            "not registered with Lemonade Server. You can register and "
                            "install new models with a `pull` request."
                        ),
                    )

                # Get additional properties from the model registry
                config_to_use = LoadConfig(**supported_models[config.model_name])

            # Caching mechanism: if the checkpoint is already loaded there is nothing else to do
            if (
                self.llm_loaded
                and config_to_use.checkpoint == self.llm_loaded.checkpoint
            ):
                if (
                    self.llm_loaded.recipe == "llamacpp"
                    and self.wrapped_server.process.poll()
                ):
                    # wrapped server process has gone away for some reason, so we should
                    # proceed with loading to get it back
                    pass
                else:
                    return {
                        "status": "success",
                        "message": f"Model already loaded: {config.model_name}",
                    }

            # Unload the current model if needed
            if self.llm_loaded:
                await self.unload_llm(require_lock=False)

            logging.info(f"Loading llm: {config.model_name}")
            try:
                if config_to_use.recipe == "llamacpp":
                    self.wrapped_server = LlamaServer(self.llamacpp_backend)
                    self.wrapped_server.load(
                        model_config=config_to_use,
                        ctx_size=self.ctx_size,
                        do_not_upgrade=True,
                    )

                else:
                    self.model, self.tokenizer = lemonade_api.from_pretrained(
                        checkpoint=config_to_use.checkpoint, recipe=config_to_use.recipe
                    )
                self.llm_loaded = config_to_use

                return {
                    "status": "success",
                    "message": f"Loaded model: {config.model_name}",
                }
            except HTTPException:
                raise
            except Exception:  # pylint: disable=broad-exception-caught
                self.model_load_failure(config.model_name)

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
                self.wrapped_server.process.terminate()

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
