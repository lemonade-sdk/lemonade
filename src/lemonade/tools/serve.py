import argparse
import asyncio
import statistics
import time
from threading import Thread, Event

from fastapi import FastAPI
from fastapi.responses import HTMLResponse, StreamingResponse
from pydantic import BaseModel
import uvicorn
import torch  # pylint: disable=unused-import
from transformers import TextIteratorStreamer, StoppingCriteria, StoppingCriteriaList

from turnkeyml.state import State
from turnkeyml.tools.management_tools import ManagementTool
from lemonade.tools.adapter import ModelAdapter
from lemonade.tools.chat import DEFAULT_GENERATE_PARAMS
from lemonade.tools.huggingface_load import HuggingfaceLoad


# Custom huggingface-style stopping criteria to allow
# us to halt streaming in-progress generations
class StopOnEvent(StoppingCriteria):
    def __init__(self, stop_event: Event):
        super().__init__()
        self.stop_event = stop_event

    def __call__(self, input_ids, scores, **kwargs):
        return self.stop_event.is_set()


class CompletionsServerConfig(BaseModel):
    cache_dir: str
    checkpoint: str
    max_new_tokens: int = 500
    device: str = "hybrid"
    dtype: str = "int4"


class CompletionRequest(BaseModel):
    text: str
    model: str = None


END_OF_STREAM = "</s>"


class ServerPreview(ManagementTool):
    """
    Open a web server that apps can use to communicate with the LLM.

    There are two ways to perform generations with the server:
    - Send an http request to "http://localhost:8000/generate" and
        receive back a response with the complete prompt.
    - Open a WebSocket with "ws://localhost:8000" and receive a
        streaming response to the prompt.

    The server also exposes these helpful endpoints:
    - /api/v0/load: load a model checkpoint
    - /api/v0/unload: unload a model checkpoint
    - /api/v0/health: check whether a model is loaded and ready to serve.
    - /api/v0/stats: performance statistics for the generation.
    - /api/v0/halt: stop an in-progress generation from make more tokens.
    - /api/v0/completions: stream completion responses using HTTP chunked transfer encoding.

    The WebSocket functionality is demonstrated by the webpage served at
    http://localhost:8000, which you can visit with a web browser after
    opening the server.

    Optional inputs:
    - --cache-dir: directory to store model artifacts (default: ~/.cache/lemonade)
    - --checkpoint: name of the checkpoint to load
    - --max-new-tokens: number of new tokens the LLM should make (default: 500)
    - --port: port number to run the server on (default: 8000)
    """

    unique_name = "server-preview"

    def __init__(self):
        super().__init__()

        # Initialize FastAPI app
        self.app = FastAPI()

        # Set up routes
        self.app.post("/api/v0/load")(self.load_llm)
        self.app.post("/api/v0/unload")(self.unload_llm)
        self.app.get("/api/v0/health")(self.health)
        self.app.get("/api/v0/halt")(self.halt_generation)
        self.app.get("/api/v0/stats")(self.send_stats)
        self.app.post("/api/v0/completions")(self.completions)
        self.app.get("/")(self.get)

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

        # Placeholders for state and configs
        self.state = None
        self.max_new_tokens = None

        self.html = """
        <!DOCTYPE html>
        <html>
            <head>
                <title>Chat</title>
            </head>
            <body>
                <h1>Lemonade Chat</h1>
                <form action="" onsubmit="sendMessage(event)">
                    <input type="text" id="messageText" autocomplete="off"/>
                    <button type="submit">Send</button>
                </form>
                <button onclick="showStats()">Show Stats</button>
                <button onclick="halt()">Halt</button>
                <button onclick="health()">Health</button>
                <div id="chatContainer"></div>
                <p id="statsMessage"></p>
                <script>
                    const chatContainer = document.getElementById('chatContainer');
                    const statsMessageContainer = document.getElementById('statsMessage');

                    async function sendMessage(event) {
                        event.preventDefault();
                        var input = document.getElementById("messageText");
                        const message = input.value;
                        input.value = '';
                        
                        // Add message
                        const messageDiv = document.createElement('div');
                        messageDiv.textContent = message;
                        chatContainer.appendChild(messageDiv);

                        // Create response container
                        const responseDiv = document.createElement('div');
                        chatContainer.appendChild(responseDiv);
                        
                        try {
                            const response = await fetch('/api/v0/completions', {
                                method: 'POST',
                                headers: {
                                    'Content-Type': 'application/json',
                                },
                                body: JSON.stringify({ text: message })
                            });

                            const reader = response.body.getReader();
                            const decoder = new TextDecoder();
                            
                            while (true) {
                                const {value, done} = await reader.read();
                                if (done) break;
                                
                                const text = decoder.decode(value);
                                responseDiv.textContent += text;
                                // Scroll to bottom as new content arrives
                                window.scrollTo(0, document.body.scrollHeight);
                            }
                        } catch (error) {
                            console.error('Error:', error);
                            responseDiv.textContent += '\\nError: ' + error.message;
                        }
                    }

                    function showStats() {
                        fetch('/api/v0/stats')
                        .then(response => response.json())
                        .then(data => {
                            statsMessageContainer.textContent = JSON.stringify(data);
                        })
                        .catch(error => {
                            console.error('Error:', error);
                        });
                    }

                    function halt() {
                        fetch('/api/v0/halt')
                        .then(response => response.json())
                        .then(data => {
                            statsMessageContainer.textContent = JSON.stringify(data);
                        })
                        .catch(error => {
                            console.error('Error:', error);
                        });
                    }

                    function health() {
                        fetch('/api/v0/health')
                        .then(response => response.json())
                        .then(data => {
                            statsMessageContainer.textContent = JSON.stringify(data);
                        })
                        .catch(error => {
                            console.error('Error:', error);
                        });
                    }
                </script>
            </body>
        </html>
        """

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Industry Standard Model Server (Preview)",
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
            config = CompletionsServerConfig(
                cache_dir=cache_dir,
                checkpoint=checkpoint,
                max_new_tokens=max_new_tokens,
            )
            asyncio.run(self.load_llm(config))

        uvicorn.run(self.app, host="localhost", port=port)

    async def get(self):
        return HTMLResponse(self.html)

    async def completions(self, completion_request: CompletionRequest):
        """
        Stream completion responses using HTTP chunked transfer encoding.
        """

        if completion_request.model:
            # Call load_llm with the model name
            await self.load_llm(
                CompletionsServerConfig(checkpoint=completion_request.model)
            )

        async def generate():
            async for token in self._generate_tokens(completion_request.text):
                yield token.encode("utf-8")

        return StreamingResponse(
            generate(),
            media_type="text/plain",
        )

    async def _generate_tokens(self, message: str):
        """
        Core streaming completion logic, separated from response handling.
        Returns an async generator that yields tokens.
        """
        if self.state is None:
            raise Exception("Model not loaded")

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
                # Add a small delay between tokens to make the streaming more visible
                await asyncio.sleep(0.00001)

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

                yield new_text

                # Allow the user to finish the response early
                if self.stop_event.is_set():
                    print("Stopping generation early.")
                    break

            if new_text != END_OF_STREAM:
                yield END_OF_STREAM

            self.tokens_per_second = 1 / statistics.mean(self.decode_token_times)
            print("\n")
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

    async def load_llm(self, config: CompletionsServerConfig):
        self.max_new_tokens = config.max_new_tokens
        self.llm_loaded = True
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
                    dtype=(
                        torch.bfloat16 if config.dtype == "bfloat16" else torch.float32
                    ),
                )
            else:
                from lemonade.tools.ort_genai.oga import OgaLoad

                oga_loader = OgaLoad()
                self.state = oga_loader.run(
                    state,
                    input=config.checkpoint,
                    device=config.device,
                    dtype=config.dtype,
                    force=True,
                )

            self.max_new_tokens = config.max_new_tokens
            self.llm_loaded = True

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
