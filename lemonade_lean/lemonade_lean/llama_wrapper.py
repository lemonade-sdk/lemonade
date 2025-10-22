"""Wrapper for llama.cpp server subprocess."""

import os
import subprocess
import time
import logging
import requests
from typing import Optional

from lemonade_lean.download import ensure_llama_server


class LlamaServerWrapper:
    """Manages llama-server subprocess."""

    def __init__(self, model_path: str, port: int = 8080, ctx_size: int = 4096):
        self.model_path = model_path
        self.port = port
        self.ctx_size = ctx_size
        self.process: Optional[subprocess.Popen] = None
        self.llama_port = port + 1  # Use different port for llama-server

    def start(self):
        """Start llama-server subprocess."""
        # Ensure llama-server is available (download if needed)
        llama_server_path = ensure_llama_server()

        if not os.path.exists(self.model_path):
            raise FileNotFoundError(f"Model file not found: {self.model_path}")

        # Build command
        cmd = [
            llama_server_path,
            "-m",
            self.model_path,
            "--ctx-size",
            str(self.ctx_size),
            "--port",
            str(self.llama_port),
            "-ngl",
            "99",  # GPU layers
            "--jinja",  # Enable tool support
        ]

        logging.info(f"Starting llama-server: {' '.join(cmd)}")

        # Start subprocess
        self.process = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )

        # Wait for server to be ready
        self._wait_for_ready()

    def _wait_for_ready(self, timeout: int = 30):
        """Wait for llama-server to be ready."""
        start_time = time.time()

        while time.time() - start_time < timeout:
            try:
                response = requests.get(
                    f"http://localhost:{self.llama_port}/health", timeout=1
                )
                if response.status_code == 200:
                    logging.info("llama-server is ready")
                    return
            except:
                pass

            # Check if process died
            if self.process.poll() is not None:
                stderr = self.process.stderr.read() if self.process.stderr else ""
                raise RuntimeError(f"llama-server failed to start: {stderr}")

            time.sleep(0.5)

        raise TimeoutError("llama-server failed to start within timeout")

    def is_alive(self) -> bool:
        """Check if llama-server process is alive."""
        if not self.process:
            return False
        return self.process.poll() is None

    def forward_request(self, endpoint: str, json_data: dict) -> requests.Response:
        """Forward request to llama-server."""
        # Ensure process is still alive
        if not self.is_alive():
            raise RuntimeError("llama-server process is not running")

        url = f"http://localhost:{self.llama_port}{endpoint}"

        # Handle streaming
        if json_data.get("stream"):
            return requests.post(url, json=json_data, stream=True, timeout=300)
        else:
            return requests.post(url, json=json_data, timeout=300)

    def stop(self):
        """Stop llama-server subprocess."""
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

            logging.info("llama-server stopped")
