import os
import logging
import subprocess
import time

import requests

from lemonade_server.pydantic_models import (
    PullConfig,
)

from lemonade.tools.server.wrapped_server import WrappedServerTelemetry, WrappedServer
from lemonade.tools.flm.utils import install_flm


class FlmTelemetry(WrappedServerTelemetry):
    """
    Manages telemetry data collection and display for FLM server.
    """

    def parse_telemetry_line(self, line: str):
        """
        Parse telemetry data from FLM server output lines.
        """

        # TODO: parse perf data

        return


class FlmServer(WrappedServer):
    """
    Routes OpenAI API requests to an FLM server instance and returns the result
    back to Lemonade Server.
    """

    def __init__(self):
        super().__init__(server_name="flm-server", telemetry=FlmTelemetry())

    def _choose_port(self):
        """
        `flm serve` doesn't support port selection as of v0.9.4
        """
        self.port = 11434

    def address(self):
        """
        `flm serve` doesn't support host name selection as of v0.9.4
        """

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
        command = ["flm", "pull", f"{config_checkpoint}"]

        subprocess.run(command, check=True)

    def _launch_server_subprocess(
        self,
        model_config: PullConfig,
        snapshot_files: dict,
        ctx_size: int,
        supports_embeddings: bool = False,
        supports_reranking: bool = False,
    ):

        # This call is a placeholder for now; eventually we'll pass the
        # port into the command below when its supported
        self._choose_port()

        command = ["flm", "serve", f"{model_config.checkpoint}"]

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
