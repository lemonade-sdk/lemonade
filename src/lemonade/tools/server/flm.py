import os
import logging
import subprocess
import time
import threading

import requests

from lemonade_server.pydantic_models import (
    PullConfig,
    ChatCompletionRequest,
)

from lemonade.tools.server.wrapped_server import WrappedServerTelemetry, WrappedServer
from lemonade.tools.flm.utils import install_flm, download_flm_model


class FlmTelemetry(WrappedServerTelemetry):
    """
    Manages telemetry data collection and display for FLM server.
    """

    def parse_telemetry_line(self, line: str):
        """
        Parse telemetry data from FLM server output lines.

        Note: as of FLM 0.9.10, no telemetry data is provided by the server CLI.
                This function is required to be implemented, so we leave it empty
                as a placeholder for now.
        """

        return


class FlmServer(WrappedServer):
    """
    Routes OpenAI API requests to an FLM server instance and returns the result
    back to Lemonade Server.
    """

    def __init__(self):
        self.flm_model_name = None
        super().__init__(server_name="flm-server", telemetry=FlmTelemetry())

    def address(self):
        return f"http://localhost:{self.port}/v1"

    def install_server(self):
        """
        Check if FLM is installed and at minimum version.
        If not, download and run the GUI installer, then wait for completion.
        """
        install_flm()

    def download_model(
        self, config_checkpoint, config_mmproj=None, do_not_upgrade=False
    ) -> dict:
        download_flm_model(config_checkpoint, config_mmproj, do_not_upgrade)

    def _launch_server_subprocess(
        self,
        model_config: PullConfig,
        snapshot_files: dict,
        ctx_size: int,
        supports_embeddings: bool = False,
        supports_reranking: bool = False,
    ):

        self._choose_port()

        # Keep track of the FLM model name so that we can use it later
        self.flm_model_name = model_config.checkpoint

        command = [
            "flm",
            "serve",
            f"{self.flm_model_name}",
            "--ctx-len",
            str(ctx_size),
            "--port",
            str(self.port),
        ]

        # Set up environment with library path for Linux
        env = os.environ.copy()

        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            env=env,
        )

        # Start background thread to log subprocess output
        threading.Thread(
            target=self._log_subprocess_output,
            args=("FLM SERVER",),
            daemon=True,
        ).start()

    def _wait_for_load(self):
        """
        FLM doesn't seem to have a health API, so we'll use the "list local models"
        API to check if the server is up.
        """
        status_code = None
        while not self.process.poll() and status_code != 200:
            health_url = f"http://localhost:{self.port}/api/tags"
            try:
                health_response = requests.get(health_url)
            except requests.exceptions.ConnectionError:
                logging.debug(
                    "Not able to connect to %s yet, will retry", self.server_name
                )
            else:
                status_code = health_response.status_code
                logging.debug(
                    "Testing %s readiness (will retry until ready), result: %s",
                    self.server_name,
                    health_response.json(),
                )
            time.sleep(1)

    def chat_completion(self, chat_completion_request: ChatCompletionRequest):
        # FLM requires the correct model name to be in the request
        # (whereas llama-server ignores the model name field in the request)
        chat_completion_request.model = self.flm_model_name

        return super().chat_completion(chat_completion_request)
