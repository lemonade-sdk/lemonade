import threading
import logging
from lemonade.tools.server.serve import Server


class ServerRunner(threading.Thread):
    """
    Thread class for running the Lemonade Server with a loaded model.
    """

    def __init__(
        self, model, tokenizer, checkpoint, recipe, host="localhost", port=8000
    ):
        threading.Thread.__init__(self)
        self.model = model
        self.tokenizer = tokenizer
        self.checkpoint = checkpoint
        self.recipe = recipe
        self.host = host
        self.port = port
        self.server = None
        self.ready_event = threading.Event()
        self.shutdown_event = threading.Event()
        self.uvicorn_server = None

    def run(self):
        try:
            # Create the server instance
            self.server = Server(port=self.port, log_level="warning")

            # Configure the server with model/tokenizer
            self.server.model = self.model
            self.server.tokenizer = self.tokenizer
            self.server.llm_loaded = type(
                "obj",
                (object,),
                {
                    "checkpoint": self.checkpoint,
                    "recipe": self.recipe,
                    "max_prompt_length": None,
                    "reasoning": False,
                    "model_name": "custom",
                },
            )

            # Set up the server for threaded execution
            self.uvicorn_server = self.server.run_in_thread(host=self.host)

            # Set the ready event
            self.ready_event.set()

            # Run the server until shutdown is requested
            logging.info(f"Starting server on http://{self.host}:{self.port}")
            self.uvicorn_server.run()

        except Exception as e:
            logging.error(f"Error starting server: {e}")
            self.ready_event.set()
            raise

    def shutdown(self):
        """Shutdown the server"""
        if hasattr(self, "uvicorn_server") and self.uvicorn_server:
            logging.info("Shutting down server...")
            self.uvicorn_server.should_exit = True
            self.shutdown_event.set()

        # Clean up resources properly to avoid memory leaks
        if hasattr(self, "server") and self.server:
            logging.info("Cleaning up model and tokenizer resources...")

            if hasattr(self.server, "model"):
                self.server.model = None

            if hasattr(self.server, "tokenizer"):
                self.server.tokenizer = None

            if hasattr(self.server, "llm_loaded"):
                self.server.llm_loaded = None

        # Clean up local references
        if hasattr(self, "model"):
            del self.model
        if hasattr(self, "tokenizer"):
            del self.tokenizer
