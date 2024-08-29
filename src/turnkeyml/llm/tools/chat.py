import argparse
from threading import Thread
import asyncio
from fastapi import FastAPI, WebSocket
from fastapi.responses import HTMLResponse
from pydantic import BaseModel
from transformers import TextIteratorStreamer
import uvicorn
from turnkeyml.state import State
from turnkeyml.tools import Tool
from turnkeyml.llm.tools.adapter import ModelAdapter, TokenizerAdapter


class LLMPrompt(Tool):
    """
    Send a prompt to an LLM instance and print the response to the screen.

    Required input state:
        - state.model: LLM instance that supports the generate() method.
        - state.tokenizer: LLM tokenizer instance that supports the __call__() (ie, encode)
            and decode() methods.

    Output state produced:
        - "response": text response from the LLM.
    """

    unique_name = "llm-prompt"

    def __init__(self):
        super().__init__(monitor_message="Prompting LLM")

        self.status_stats = ["response"]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Prompt an LLM and print the result",
            add_help=add_help,
        )

        parser.add_argument("--prompt", "-p", help="Prompt input to the LLM")

        parser.add_argument(
            "--max-new-tokens",
            "-m",
            default=512,
            type=int,
            help="Maximum number of new tokens in the response",
        )

        return parser

    def run(
        self,
        state: State,
        prompt: str = "Hello",
        max_new_tokens: int = 512,
    ) -> State:

        model: ModelAdapter = state.model
        tokenizer: TokenizerAdapter = state.tokenizer

        input_ids = tokenizer(prompt, return_tensors="pt").input_ids
        response = model.generate(input_ids, max_new_tokens=max_new_tokens)
        response_text = tokenizer.decode(response[0], skip_special_tokens=True).strip()

        state.response = response_text
        state.save_stat("response", response_text)

        return state


class Serve(Tool):
    """
    Open a web server that apps can use to communicate with the LLM.

    There are two ways interact with the server:
    - Send an http request to "http://localhost:8000/generate" and
        receive back a response with the complete prompt.
    - Open a WebSocket with "ws://localhost:8000" and receive a
        streaming response to the prompt.

    The WebSocket functionality is demonstrated by the webpage served at
    http://localhost:8000, which you can visit with a web browser after
    opening the server.

    Required input state:
        - state.model: model instance serve. Must be compatible with the
            huggingface TextIteratorStreamer.
        - state.tokenizer: tokenizer instance used to generate inputs for the
            model. Must be compatible with the huggingface TextIteratorStreamer.

    Output state produced: None
    """

    unique_name = "serve"

    def __init__(self):
        # Disable the build logger since the server is interactive
        super().__init__(
            monitor_message="Launching LLM Server",
            enable_logger=False,
        )

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Open an HTTP server for the model",
            add_help=add_help,
        )

        parser.add_argument(
            "--max-new-tokens",
            required=False,
            type=int,
            default=300,
            help="Number of new tokens the LLM should make (default: 300)",
        )

        return parser

    def run(
        self,
        state: State,
        max_new_tokens: int = 300,
    ) -> State:

        # Disable the build monitor since the server is persistent and interactive
        if self.progress:
            self.progress.terminate()
            print("\n")

        app = FastAPI()

        # Load the model and tokenizer
        model = state.model
        tokenizer = state.tokenizer

        class Message(BaseModel):
            text: str

        html = """
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
                <p id="allMessages"></p> <!-- Use a <p> element to display all messages -->
                <script>
                    const messageQueue = []; // Store incoming messages
                    const allMessagesContainer = document.getElementById('allMessages'); // Get the container element
                    var ws = new WebSocket("ws://localhost:8000/ws");
                    ws.onmessage = function(event) {
                        const message = event.data;
                        messageQueue.push(message); // Add the received message to the queue
                        displayAllMessages(); // Display all messages
                    };
                    function displayAllMessages() {
                        if (messageQueue.length > 0) {
                            const allMessages = messageQueue.join(' '); // Concatenate all messages
                            allMessagesContainer.textContent = allMessages; // Set the content of the container
                        }
                    }
                    function sendMessage(event) {
                        var input = document.getElementById("messageText")
                        ws.send(input.value)
                        input.value = ''
                        event.preventDefault()
                    }
                </script>
            </body>
        </html>
        """

        @app.get("/")
        async def get():
            return HTMLResponse(html)

        @app.post("/generate")
        async def generate_response(message: Message):
            input_ids = tokenizer(message.text, return_tensors="pt").input_ids
            response = model.generate(
                input_ids,
                max_new_tokens=max_new_tokens,
                do_sample=True,
                top_k=50,
                top_p=0.95,
                temperature=0.7,
                pad_token_id=tokenizer.eos_token_id,
            )
            generated_text = tokenizer.decode(response[0], skip_special_tokens=True)

            # Remove the input prompt from the generated text
            generated_text = generated_text.replace(message.text, "").strip()

            return {"response": generated_text}

        @app.websocket("/ws")
        async def stream_response(websocket: WebSocket):
            await websocket.accept()
            while True:

                message = await websocket.receive_text()

                if message == "done":
                    break
                input_ids = tokenizer(message, return_tensors="pt").input_ids

                # Set up the generation parameters
                if isinstance(model, ModelAdapter) and model.type == "ort-genai":
                    # Onnxruntime-genai models
                    import turnkeyml.llm.tools.ort_genai.oga as oga

                    streamer = oga.OrtGenaiStreamer(tokenizer)

                else:
                    # Huggingface-like models
                    streamer = TextIteratorStreamer(
                        tokenizer,
                        skip_prompt=True,
                    )
                generation_kwargs = {
                    "input_ids": input_ids,
                    "streamer": streamer,
                    "max_new_tokens": max_new_tokens,
                    "do_sample": True,
                    "top_k": 50,
                    "top_p": 0.95,
                    "temperature": 0.7,
                    "pad_token_id": tokenizer.eos_token_id,
                }

                thread = Thread(target=model.generate, kwargs=generation_kwargs)
                thread.start()

                # Generate the response using streaming
                for new_text in streamer:
                    print(new_text, end="", flush=True)

                    # Send the generated text to the client
                    await asyncio.sleep(0.1)  # Add a small delay (adjust as needed)
                    await websocket.send_text(new_text)

                print("\n")
                thread.join()

            await websocket.close()

        uvicorn.run(app, host="localhost", port=8000)

        return state
