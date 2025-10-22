"""Wrapper for llama.cpp server subprocess."""

import os
import subprocess
import time
import logging
import requests
from typing import Optional


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
        # Check if llama-server is available
        llama_server_path = self._find_llama_server()

        if not llama_server_path:
            raise RuntimeError(
                "llama-server not found. Please install llama.cpp and ensure "
                "llama-server is in your PATH or set LLAMA_SERVER_PATH environment variable."
            )

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

    def _find_llama_server(self) -> Optional[str]:
        """Find llama-server executable."""
        # Check environment variable
        if "LLAMA_SERVER_PATH" in os.environ:
            path = os.environ["LLAMA_SERVER_PATH"]
            if os.path.exists(path):
                return path

        # Check common locations
        common_paths = [
            "llama-server",
            "llama-server.exe",
            "./llama-server",
            "./llama-server.exe",
        ]

        for path in common_paths:
            try:
                # Try to find in PATH
                result = subprocess.run(
                    ["which" if os.name != "nt" else "where", path],
                    capture_output=True,
                    text=True,
                )
                if result.returncode == 0:
                    return result.stdout.strip().split("\n")[0]
            except:
                pass

        return None

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

    def forward_request(self, endpoint: str, json_data: dict) -> requests.Response:
        """Forward request to llama-server."""
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
