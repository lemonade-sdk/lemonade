import argparse
import asyncio
import statistics
import time
from threading import Thread, Event

from fastapi import FastAPI
from fastapi.responses import StreamingResponse
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import uvicorn
import torch  # pylint: disable=unused-import
from transformers import TextIteratorStreamer, StoppingCriteria, StoppingCriteriaList

from openai.types.chat import ChatCompletion, ChatCompletionChunk
from openai.types.chat.chat_completion_message import ChatCompletionMessage
from openai.types.chat.chat_completion import Choice
from openai.types.chat.chat_completion_chunk import ChoiceDelta
from openai.types.model import Model

from turnkeyml.state import State
from turnkeyml.tools.management_tools import ManagementTool
from lemonade.tools.adapter import ModelAdapter
from lemonade.tools.prompt import DEFAULT_GENERATE_PARAMS
from lemonade.tools.huggingface_load import HuggingfaceLoad
from lemonade.cache import DEFAULT_CACHE_DIR


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


class LoadConfig(BaseModel):
    """
    Configuration for loading a language model.

    Specifies the model checkpoint, cache directory, generation parameters,
    and hardware configuration for model loading.
    """

    checkpoint: str
    cache_dir: str = DEFAULT_CACHE_DIR
    max_new_tokens: int = 500
    device: str = "cpu"


class CompletionRequest(BaseModel):
    """
    Request model for text completion API endpoint.

    Contains the input text to be completed and a model identifier.
    """

    text: str
    model: str


class ChatCompletionRequest(BaseModel):
    """
    Request model for chat completion API endpoint.

    Contains a list of chat messages, a model identifier,
    and a streaming flag to control response delivery.
    """

    messages: list[dict]
    model: str
    stream: bool = False


class Server(ManagementTool):
    """
    Open a web server that apps can use to communicate with the LLM.

    The server exposes these endpoints:
    - /api/v0/load: load a model checkpoint
    - /api/v0/unload: unload a model checkpoint
    - /api/v0/health: check whether a model is loaded and ready to serve.
    - /api/v0/stats: performance statistics for the generation.
    - /api/v0/halt: stop an in-progress generation from make more tokens.
    - /api/v0/completions: completion responses using HTTP chunked transfer encoding.
    - /api/v0/chat/completions: chat completion responses using HTTP chunked transfer encoding.
    - /api/v0/models: list all available models.

    Optional inputs:
    - --cache-dir: directory to store model artifacts (default: ~/.cache/lemonade)
    - --checkpoint: name of the checkpoint to load
    - --max-new-tokens: number of new tokens the LLM should make (default: 500)
    - --port: port number to run the server on (default: 8000)
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
        self.app.post("/api/v0/load")(self.load_llm)
        self.app.post("/api/v0/unload")(self.unload_llm)
        self.app.get("/api/v0/health")(self.health)
        self.app.get("/api/v0/halt")(self.halt_generation)
        self.app.get("/api/v0/stats")(self.send_stats)
        self.app.post("/api/v0/completions")(self.completions)

        # Set up OpenAI-compatible routes
        self.app.post("/api/v0/chat/completions")(self.chat_completions)
        self.app.get("/api/v0/models")(self.models)

        # Performance stats that are set during /ws and can be
        # fetched in /stats
        self.time_to_first_token = None
        self.tokens_per_second = None
        self.input_tokens = None
        self.output_tokens = None
        self.decode_token_times = None

        # Flag that tells the LLM to stop generating text and end the response
        self.stop_event = Event()

        # Helpers
        self.llm_loaded = False
        self.tokenizer = None

        # Placeholders for state and configs
        self.state = None
        self.max_new_tokens = None

        # Curated list of "Instruct" and "Chat" models.
        self.builtin_models = {
            "Qwen2.5-0.5B-Instruct-CPU": {
                "checkpoint": "Qwen/Qwen2.5-0.5B-Instruct",
                "device": "cpu",
            },
            "Llama-3.2-1B-Instruct-Hybrid": {
                "checkpoint": "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
                "device": "hybrid",
            },
            "Llama-3.2-3B-Instruct-Hybrid": {
                "checkpoint": "amd/Llama-3.2-3B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
                "device": "hybrid",
            },
            "Phi-3.5-Mini-Instruct-Hybrid": {
                "checkpoint": "amd/Phi-3.5-mini-instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
                "device": "hybrid",
            },
            "Phi-3-Mini-Instruct-Hybrid": {
                "checkpoint": "amd/Phi-3-mini-4k-instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
                "device": "hybrid",
            },
            "Llama-2-7B-Chat-Hybrid": {
                "checkpoint": "amd/Llama-2-7b-chat-hf-awq-g128-int4-asym-fp16-onnx-hybrid",
                "device": "hybrid",
            },
            "Qwen-1.5-7B-Chat-Hybrid": {
                "checkpoint": "amd/Qwen1.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid",
                "device": "hybrid",
            },
            "Mistral-7B-Instruct-Hybrid": {
                "checkpoint": "amd/Mistral-7B-Instruct-v0.3-awq-g128-int4-asym-fp16-onnx-hybrid",
                "device": "hybrid",
            },
        }

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Industry Standard Model Server",
            add_help=add_help,
        )

        parser.add_argument(
            "--checkpoint",
            required=False,
            type=str,
            help="Name of the model checkpoint to load (optional)",
        )
        parser.add_argument(
            "--max-new-tokens",
            required=False,
            type=int,
            default=300,
            help="Number of new tokens the LLM should make (default: 300)",
        )
        parser.add_argument(
            "--port",
            required=False,
            type=int,
            default=8000,
            help="Port number to run the server on (default: 8000)",
        )

        return parser

    def run(
        self,
        cache_dir: str,
        checkpoint: str,
        max_new_tokens: int = 500,
        port: int = 8000,
    ):
        # Only load the model when starting the server if checkpoint was provided
        if checkpoint:
            config = LoadConfig(
                cache_dir=cache_dir,
                checkpoint=checkpoint,
                max_new_tokens=max_new_tokens,
            )
            asyncio.run(self.load_llm(config))

        uvicorn.run(self.app, host="localhost", port=port)

    async def completions(self, completion_request: CompletionRequest):
        """
        Stream completion responses using HTTP chunked transfer encoding.
        """

        if completion_request.model:
            # Call load_llm with the model name
            await self.load_llm(LoadConfig(checkpoint=completion_request.model))

        async def generate():
            async for token in self._generate_tokens(completion_request.text):
                yield token.encode("utf-8")

        return StreamingResponse(
            generate(),
            media_type="text/plain",
        )

    async def chat_completions(self, chat_completion_request: ChatCompletionRequest):
        """
        Stream chat completion responses using HTTP chunked transfer encoding.
        """

        # Get model config
        if chat_completion_request.model in self.builtin_models:
            model_config = self.builtin_models[chat_completion_request.model]
            lc = LoadConfig(**model_config)
        else:
            # If the model is not built-in, we assume it corresponds to a checkpoint
            lc = LoadConfig(checkpoint=chat_completion_request.model)

        if lc.checkpoint != self.llm_loaded:
            # Unload the current model if needed
            if self.llm_loaded:
                await self.unload_llm()

            # Load the new model
            await self.load_llm(lc)

        # Convert chat messages to text using the model's chat template
        if hasattr(self.tokenizer, "apply_chat_template"):
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
            formatted_messages = []
            for msg in chat_completion_request.messages:
                role = msg.get("role", "user")
                content = msg.get("content", "")
                role_marker = "<|assistant|>" if role == "assistant" else "<|user|>"
                formatted_messages.append(f"{role_marker}\n{content} <|end|>")
            text = "\n".join(formatted_messages) + "\n<|assistant|>"

        if chat_completion_request.stream:

            # Stream the response
            async def generate():
                async for token in self._generate_tokens(text):
                    # Create a ChatCompletionChunk
                    chunk = ChatCompletionChunk.model_construct(
                        id="0",
                        object="chat.completion.chunk",
                        created=int(time.time()),
                        model=self.llm_loaded,
                        choices=[
                            Choice.model_construct(
                                index=0,
                                delta=ChoiceDelta(
                                    content=token,
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
            full_response = ""
            async for token in self._generate_tokens(text):
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
                model=self.llm_loaded,
                object="chat.completion",
                created=int(time.time()),
            )

    async def _generate_tokens(self, message: str):
        """
        Core streaming completion logic, separated from response handling.
        Returns an async generator that yields tokens.
        """

        model = self.state.model  # pylint: disable=no-member
        tokenizer = self.state.tokenizer  # pylint: disable=no-member

        # Reset the early-exit flag before we start each generation
        self.stop_event.clear()

        input_ids = tokenizer(message, return_tensors="pt").input_ids

        # Set up the generation parameters
        if isinstance(model, ModelAdapter) and model.type == "ort-genai":
            from lemonade.tools.ort_genai.oga import OrtGenaiStreamer

            streamer = OrtGenaiStreamer(tokenizer)
            self.input_tokens = len(input_ids)
        else:
            # Huggingface-like models
            streamer = TextIteratorStreamer(
                tokenizer,
                skip_prompt=True,
            )
            self.input_tokens = len(input_ids[0])

        stopping_criteria = StoppingCriteriaList([StopOnEvent(self.stop_event)])

        generation_kwargs = {
            "input_ids": input_ids,
            "streamer": streamer,
            "max_new_tokens": self.max_new_tokens,
            "min_new_tokens": 1,
            "pad_token_id": tokenizer.eos_token_id,
            "stopping_criteria": stopping_criteria,
            **DEFAULT_GENERATE_PARAMS,
        }

        # Initialize performance variables
        generation_start_time = time.perf_counter()
        first_token = True
        self.decode_token_times = []
        self.output_tokens = 0

        # Begin generation
        thread = Thread(target=model.generate, kwargs=generation_kwargs)
        thread.start()

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

                yield new_text

                # Allow the user to finish the response early
                if self.stop_event.is_set():
                    print("Stopping generation early.")
                    break

            self.tokens_per_second = 1 / statistics.mean(self.decode_token_times)

        finally:
            thread.join()

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
            "model_loaded": (
                self.state.checkpoint  # pylint: disable=no-member
                if self.state
                else None
            ),
        }

    async def load_llm(self, config: LoadConfig):
        self.max_new_tokens = config.max_new_tokens
        print("Loading llm:", config.checkpoint)
        try:
            state = State(
                cache_dir=config.cache_dir,
                build_name="main",
            )

            if config.device == "cpu":
                huggingface_loader = HuggingfaceLoad()
                self.state = huggingface_loader.run(
                    state,
                    input=config.checkpoint,
                    device=config.device,
                    dtype=torch.bfloat16,
                )
            else:
                from lemonade.tools.ort_genai.oga import OgaLoad

                oga_loader = OgaLoad()
                self.state = oga_loader.run(
                    state,
                    input=config.checkpoint,
                    device=config.device,
                    dtype="int4",
                    force=True,
                )
            self.max_new_tokens = config.max_new_tokens
            self.llm_loaded = config.checkpoint
            self.tokenizer = self.state.tokenizer.tokenizer  # pylint: disable=no-member

            return {
                "status": "success",
                "message": f"Loaded model: {config.checkpoint}",
            }
        except Exception as e:  # pylint: disable=broad-exception-caught
            self.llm_loaded = False
            self.state = None
            return {
                "status": "error",
                "message": f"Failed to load llm: {str(e)}",
            }

    async def unload_llm(self):
        try:
            self.llm_loaded = False
            self.state = None
            return {"status": "success", "message": "Unloaded model"}
        except Exception as e:  # pylint: disable=broad-exception-caught
            return {
                "status": "error",
                "message": f"Failed to unload model: {str(e)}",
            }

    async def models(self):
        """
        Return a list of available models in OpenAI-compatible format.
        """
        models_list = []
        for model in self.builtin_models:
            m = Model(
                id=model,
                owned_by="lemonade",
                object="model",
                created=int(time.time()),
            )
            models_list.append(m)

        return {"object": "list", "data": models_list}
