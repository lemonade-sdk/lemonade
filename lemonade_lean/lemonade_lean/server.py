"""Minimal FastAPI server for llama.cpp."""

import time
import logging
from typing import Optional
from fastapi import FastAPI, HTTPException
from fastapi.responses import StreamingResponse
from fastapi.middleware.cors import CORSMiddleware

from lemonade_lean.models import (
    ChatCompletionRequest,
    CompletionRequest,
    ChatCompletionResponse,
    CompletionResponse,
    HealthResponse,
    ModelInfo,
)
from lemonade_lean.llama_wrapper import LlamaServerWrapper


class LemonadeServer:
    """Minimal Lemonade server."""

    def __init__(
        self,
        model_path: str,
        port: int = 8000,
        host: str = "localhost",
        ctx_size: int = 4096,
    ):
        self.model_path = model_path
        self.port = port
        self.host = host
        self.ctx_size = ctx_size
        self.llama_wrapper: Optional[LlamaServerWrapper] = None

        # Create FastAPI app
        self.app = FastAPI(title="Lemonade Lean Server")

        # Add CORS
        self.app.add_middleware(
            CORSMiddleware,
            allow_origins=["*"],
            allow_credentials=True,
            allow_methods=["*"],
            allow_headers=["*"],
        )

        # Setup routes
        self._setup_routes()

    def _setup_routes(self):
        """Setup API routes."""

        @self.app.on_event("startup")
        async def startup():
            """Start llama-server on startup."""
            logging.info(f"Loading model: {self.model_path}")
            self.llama_wrapper = LlamaServerWrapper(
                self.model_path, port=self.port, ctx_size=self.ctx_size
            )
            self.llama_wrapper.start()
            logging.info("Model loaded successfully")

        @self.app.on_event("shutdown")
        async def shutdown():
            """Stop llama-server on shutdown."""
            if self.llama_wrapper:
                self.llama_wrapper.stop()

        @self.app.get("/v1/health")
        @self.app.get("/health")
        async def health():
            """Health check."""
            return HealthResponse(
                status="ok",
                model_loaded=self.model_path if self.llama_wrapper else None,
            )

        @self.app.get("/v1/models")
        @self.app.get("/models")
        async def list_models():
            """List models."""
            return {
                "object": "list",
                "data": [ModelInfo(id="llama-model", created=int(time.time()))],
            }

        @self.app.post("/v1/chat/completions")
        @self.app.post("/chat/completions")
        async def chat_completions(request: ChatCompletionRequest):
            """Chat completions endpoint."""
            if not self.llama_wrapper:
                raise HTTPException(status_code=503, detail="Model not loaded")

            # Convert to llama.cpp format
            llama_request = {
                "messages": [msg.dict() for msg in request.messages],
                "temperature": request.temperature,
                "max_tokens": request.max_tokens,
                "stream": request.stream,
            }

            if request.stop:
                llama_request["stop"] = request.stop

            try:
                response = self.llama_wrapper.forward_request(
                    "/v1/chat/completions", llama_request
                )

                if request.stream:
                    # Return streaming response
                    return StreamingResponse(
                        response.iter_content(chunk_size=None),
                        media_type="text/event-stream",
                    )
                else:
                    return response.json()

            except Exception as e:
                logging.error(f"Chat completion error: {e}")
                raise HTTPException(status_code=500, detail=str(e))

        @self.app.post("/v1/completions")
        @self.app.post("/completions")
        async def completions(request: CompletionRequest):
            """Text completions endpoint."""
            if not self.llama_wrapper:
                raise HTTPException(status_code=503, detail="Model not loaded")

            # Convert to llama.cpp format
            llama_request = {
                "prompt": request.prompt,
                "temperature": request.temperature,
                "max_tokens": request.max_tokens,
                "stream": request.stream,
            }

            if request.stop:
                llama_request["stop"] = request.stop

            try:
                response = self.llama_wrapper.forward_request(
                    "/v1/completions", llama_request
                )

                if request.stream:
                    return StreamingResponse(
                        response.iter_content(chunk_size=None),
                        media_type="text/event-stream",
                    )
                else:
                    return response.json()

            except Exception as e:
                logging.error(f"Completion error: {e}")
                raise HTTPException(status_code=500, detail=str(e))

    def run(self):
        """Run the server."""
        import uvicorn

        uvicorn.run(self.app, host=self.host, port=self.port)
