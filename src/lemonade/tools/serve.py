import argparse
import asyncio
import statistics
import time
from threading import Thread, Event
import logging
import traceback
from typing import Optional, Union

from fastapi import FastAPI, HTTPException, status, Request
from fastapi.responses import StreamingResponse, HTMLResponse
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import uvicorn
from transformers import TextIteratorStreamer, StoppingCriteria, StoppingCriteriaList
from tabulate import tabulate

from openai.types.completion import Completion, CompletionChoice
from openai.types.chat import ChatCompletion, ChatCompletionChunk
from openai.types.chat.chat_completion_message import ChatCompletionMessage
from openai.types.chat.chat_completion import Choice
from openai.types.chat.chat_completion_chunk import ChoiceDelta
from openai.types.completion_choice import Logprobs

from openai.types.model import Model

from turnkeyml.tools.management_tools import ManagementTool
import lemonade.api as lemonade_api
from lemonade_server.model_manager import ModelManager

# Set to a high number to allow for interesting experiences in real apps
# Tests should use the max_new_tokens argument to set a lower value
DEFAULT_MAX_NEW_TOKENS = 1500

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


# Custom huggingface-style stopping criteria to allow
# us to halt streaming in-progress generations
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


class PullConfig(BaseModel):
    """
    Configurating for installing a supported LLM.
    """

    model_name: str


class LoadConfig(BaseModel):
    """
    Configuration for loading a language model.

    Specifies the model checkpoint, generation parameters,
    and hardware/framework configuration (recipe) for model loading.
    """

    model_name: Optional[str] = None
    checkpoint: Optional[str] = None
    max_new_tokens: int = DEFAULT_MAX_NEW_TOKENS
    recipe: Optional[str] = None
    # Indicates the maximum prompt length allowed for that specific
    # checkpoint + recipe combination
    max_prompt_length: Optional[int] = None
    # Indicates whether the model is a reasoning model, like DeepSeek
    reasoning: Optional[bool] = False


class CompletionRequest(BaseModel):
    """
    Request model for text completion API endpoint.

    Contains a prompt, a model identifier, and a streaming
    flag to control response delivery.
    """

    prompt: str
    model: str
    echo: bool = False
    stream: bool = False
    logprobs: int | None = False
    stop: list[str] | str | None = None
    temperature: float | None = None
    max_tokens: int | None = None


class ChatCompletionRequest(BaseModel):
    """
    Request model for chat completion API endpoint.

    Contains a list of chat messages, a model identifier,
    and a streaming flag to control response delivery.
    """

    messages: list[dict]
    model: str
    stream: bool = False
    logprobs: int | None = False
    stop: list[str] | str | None = None
    temperature: float | None = None
    max_tokens: int | None = None
    max_completion_tokens: int | None = None


class Server(ManagementTool):
    """
    Open a web server that apps can use to communicate with the LLM.

    The server exposes these endpoints:
    - /api/v0/pull: install an LLM by its Lemonade Server Model Name.
    - /api/v0/load: load a model checkpoint.
    - /api/v0/unload: unload a model checkpoint.
    - /api/v0/health: check whether a model is loaded and ready to serve.
    - /api/v0/stats: performance statistics for the generation.
    - /api/v0/halt: stop an in-progress generation from make more tokens.
    - /api/v0/completions: completion responses using HTTP chunked transfer encoding.
    - /api/v0/chat/completions: chat completion responses using HTTP chunked transfer encoding.
    - /api/v0/models: list all available models.
    """

    unique_name = "serve"

    def __init__(self):
        super().__init__()

        # Initialize FastAPI app
        self.app = FastAPI()

        # Add CORS middleware
        self.app.add_middleware(
            CORSMiddleware,
            allow_origins=["*"],  # Allows all origins
            allow_credentials=True,
            allow_methods=["*"],  # Allows all methods
            allow_headers=["*"],  # Allows all headers
        )

        # Set up custom routes
        self.app.post("/api/v0/pull")(self.pull)
        self.app.post("/api/v0/load")(self.load_llm)
        self.app.post("/api/v0/unload")(self.unload_llm)
        self.app.get("/api/v0/health")(self.health)
        self.app.get("/api/v0/halt")(self.halt_generation)
        self.app.get("/api/v0/stats")(self.send_stats)
        self.app.post("/api/v0/completions")(self.completions)

        # Set up OpenAI-compatible routes
        self.app.post("/api/v0/chat/completions")(self.chat_completions)
        self.app.post("/api/v0/completions")(self.completions)
        self.app.get("/api/v0/models")(self.models)

        # Set up instructions
        self.app.get("/")(self.instructions)

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

    def run(
        self,
        # ManagementTool has a required cache_dir arg, but
        # we always use the default cache directory
        _=None,
        port: int = DEFAULT_PORT,
        log_level: str = DEFAULT_LOG_LEVEL,
    ):

        # Define TRACE level
        logging.TRACE = 9  # Lower than DEBUG which is 10
        logging.addLevelName(logging.TRACE, "TRACE")

        # Add a convenience function at the module level
        def trace(message, *args, **kwargs):
            logging.log(logging.TRACE, message, *args, **kwargs)

        logging.trace = trace

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

        uvicorn.run(self.app, host="localhost", port=port, log_level=log_level)

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
        html_content = """
            <!DOCTYPE html>
            <html>
            <head>
                <title>Lemonade Server</title>
                <link rel="icon" href="data:,">
            </head>
            <body>
                <h1>🍋 Welcome to Lemonade Server!</h1>
                <p>
                    A standards-compliant server that provides REST APIs for LLM communication.
                    To get started, simply point your OpenAI-compatible application at the server's endpoint.
                </p>
                <div class="links">
                    <h3>Documentation:</h3>
                    <ul>
                        <li><a href="https://github.com/onnx/turnkeyml/tree/main/examples/lemonade/server">Examples & Usage</a></li>
                        <li><a href="https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_integration.md">Integration Guide</a></li>
                        <li><a href="https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_spec.md">Server Specification</a></li>
                    </ul>
                </div>
            </body>
            </html>
            """
        return HTMLResponse(content=html_content, status_code=200)

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

        lc = self.initialize_load_config(chat_completion_request)

        # Load the model if it's different from the currently loaded one
        await self.load_llm(lc, internal_call=True)

        # Convert chat messages to text using the model's chat template
        if self.tokenizer.chat_template:
            # Use the model's built-in chat template if available
            messages_dict = [
                {"role": msg.get("role", "user"), "content": msg.get("content", "")}
                for msg in chat_completion_request.messages
            ]
            text = self.tokenizer.apply_chat_template(
                messages_dict, tokenize=False, add_generation_prompt=True
            )
        else:
            # Fallback to a standardized template if the model doesn't provide one
            logging.warning("No chat template found. Using default template.")
            formatted_messages = []
            for msg in chat_completion_request.messages:
                role = msg.get("role", "user")
                content = msg.get("content", "")
                role_marker = "<|assistant|>" if role == "assistant" else "<|user|>"
                formatted_messages.append(f"{role_marker}\n{content} <|end|>")
            text = "\n".join(formatted_messages) + "\n<|assistant|>"

        # If the model supports reasoning, we:
        # 1. add a <think> tag to the model's context
        # 2. ensure that the first token is a <think> token
        reasoning_first_token = self.llm_loaded.reasoning

        if reasoning_first_token:
            text += "<think>"

        if chat_completion_request.logprobs:
            logging.warning("logprobs is not supported on chat completion")

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

        if chat_completion_request.stream:

            # Stream the response
            async def generate():
                # Declare it's the same variable from outside scope
                # This is necessary because the variable is modified
                # in the inner function
                nonlocal reasoning_first_token

                async for token in self._generate_tokens(**generation_args):

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
                                    tool_calls=None,
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

            ccm = ChatCompletionMessage(
                content=full_response,
                role="assistant",
                refusal=None,
                audio=None,
                function_call=None,
                tool_calls=None,
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
                    detail=f"Completion failure {thread.exception}",
                )

            # Display telemetry if in debug mode
            await self._show_telemetry()

    async def send_stats(self):
        """
        Send performance statistics to the client.
        """
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
                self.model, self.tokenizer = lemonade_api.from_pretrained(
                    checkpoint=config_to_use.checkpoint, recipe=config_to_use.recipe
                )
                self.llm_loaded = config_to_use

                return {
                    "status": "success",
                    "message": f"Loaded model: {model_reference}",
                }
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
            Log the request processing time for any request
            """

            start_time = time.perf_counter()
            response = await call_next(request)
            request_time = time.perf_counter() - start_time
            logging.debug(f"Total request time: {request_time:.4f} seconds")
            return response
