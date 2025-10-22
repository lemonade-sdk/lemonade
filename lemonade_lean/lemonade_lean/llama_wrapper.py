"""Wrapper for llama.cpp server subprocess."""

import os
import subprocess
import time
import logging
import requests
from typing import Optional
from pathlib import Path

from lemonade_lean.download import ensure_llama_server
from lemonade_lean.hf_cache import get_model_snapshot_path


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

        # Resolve model path - could be a file path or HuggingFace repo ID
        resolved_path = self._resolve_model_path(self.model_path)

        if not os.path.exists(resolved_path):
            raise FileNotFoundError(f"Model file not found: {resolved_path}")

        # Build command
        cmd = [
            llama_server_path,
            "-m",
            resolved_path,
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

    def _resolve_model_path(self, model_path: str) -> str:
        """
        Resolve model path. Can be:
        1. A direct file path (e.g., /path/to/model.gguf)
        2. A HuggingFace repo ID (e.g., unsloth/Qwen3-0.6B-GGUF)
        3. A HuggingFace repo ID with file (e.g., unsloth/Qwen3-0.6B-GGUF:Qwen3-0.6B-Q4_0.gguf)

        Returns the resolved file path.
        """
        # If it's already a file that exists, return it
        if os.path.exists(model_path):
            return model_path

        # Check if it looks like a HuggingFace repo ID (contains '/')
        if "/" in model_path:
            # Parse repo_id and optional filename
            if ":" in model_path:
                repo_id, filename = model_path.split(":", 1)
            else:
                repo_id = model_path
                filename = None

            # Try to get the snapshot path
            snapshot_path = get_model_snapshot_path(repo_id)

            if snapshot_path is None:
                raise FileNotFoundError(
                    f"HuggingFace model '{repo_id}' not found in cache. "
                    f"Please download it first or provide a direct file path."
                )

            # If filename was specified, use it
            if filename:
                full_path = snapshot_path / filename
                if not full_path.exists():
                    raise FileNotFoundError(
                        f"Model file '{filename}' not found in {repo_id}. "
                        f"Available files: {list(snapshot_path.glob('*.gguf'))}"
                    )
                logging.info(f"Resolved {model_path} to {full_path}")
                return str(full_path)

            # Otherwise, find GGUF files in the snapshot
            gguf_files = list(snapshot_path.glob("**/*.gguf"))

            if not gguf_files:
                raise FileNotFoundError(
                    f"No GGUF files found in HuggingFace model '{repo_id}'. "
                    f"Snapshot path: {snapshot_path}"
                )

            if len(gguf_files) > 1:
                # List available files
                file_list = "\n  ".join([f.name for f in gguf_files])
                raise ValueError(
                    f"Multiple GGUF files found in '{repo_id}':\n  {file_list}\n"
                    f"Please specify which file to use: {repo_id}:filename.gguf"
                )

            # Use the single GGUF file found
            resolved = str(gguf_files[0])
            logging.info(f"Resolved {model_path} to {resolved}")
            return resolved

        # Not a repo ID and doesn't exist - return as-is and let the error happen
        return model_path

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
