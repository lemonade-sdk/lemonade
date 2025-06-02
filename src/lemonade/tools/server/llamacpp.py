import sys
import os
import logging
import time
import subprocess
import zipfile
import re
import threading

import requests
from tabulate import tabulate
from fastapi import HTTPException, status
from fastapi.responses import StreamingResponse

from openai import OpenAI

from lemonade_server.model_manager import ModelManager
from lemonade.tools.server.pydantic_models import ChatCompletionRequest
from lemonade.tools.server.port_utils import find_free_port

LLAMA_VERSION = "b5543"

LLAMA_SERVER_EXE_DIR = os.path.join(
    os.path.dirname(sys.executable),
    "llama_server",
)

LLAMA_SERVER_EXE_PATH = os.path.join(
    LLAMA_SERVER_EXE_DIR,
    "llama-server.exe",
)


class LlamaTelemetry:
    """
    Manages telemetry data collection and display for llama server.
    """

    def __init__(self):
        self.input_tokens = None
        self.output_tokens = None
        self.time_to_first_token = None
        self.tokens_per_second = None
        self.prompt_eval_time = None
        self.eval_time = None
        self.port = None

    def choose_port(self):
        """
        Users probably don't care what port we start llama-server on, so let's
        search for an empty port
        """

        self.port = find_free_port()

        if self.port is None:
            msg = "Failed to find an empty port to start llama-server on"
            logging.error(msg)
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=msg,
            )

    def parse_telemetry_line(self, line: str):
        """
        Parse telemetry data from llama server output lines.
        """

        # Parse prompt evaluation line
        prompt_match = re.search(
            # pylint: disable=C0301
            r"prompt eval time\s*=\s*([\d.]+)\s*ms\s*/\s*(\d+)\s*tokens.*?([\d.]+)\s*tokens per second",
            line,
        )
        if prompt_match:
            prompt_time_ms = float(prompt_match.group(1))
            input_tokens = int(prompt_match.group(2))

            self.prompt_eval_time = prompt_time_ms / 1000.0
            self.input_tokens = input_tokens
            self.time_to_first_token = prompt_time_ms / 1000.0
            return

        # Parse generation evaluation line
        eval_match = re.search(
            r"eval time\s*=\s*([\d.]+)\s*ms\s*/\s*(\d+)\s*tokens.*?([\d.]+)\s*tokens per second",
            line,
        )
        if eval_match:
            eval_time_ms = float(eval_match.group(1))
            output_tokens = int(eval_match.group(2))
            tokens_per_second = float(eval_match.group(3))

            self.eval_time = eval_time_ms / 1000.0
            self.output_tokens = output_tokens
            self.tokens_per_second = tokens_per_second
            return

    def get_telemetry_data(self):
        return {
            "input_tokens": self.input_tokens,
            "output_tokens": self.output_tokens,
            "time_to_first_token": self.time_to_first_token,
            "tokens_per_second": self.tokens_per_second,
            "decode_token_times": None,
        }

    def show_telemetry(self):
        # Check if debug logging is enabled
        if not logging.getLogger().isEnabledFor(logging.DEBUG):
            return

        # Prepare telemetry data (transposed format)
        telemetry = [
            ["Input tokens", self.input_tokens],
            ["Output tokens", self.output_tokens],
            ["TTFT (s)", f"{self.time_to_first_token:.2f}"],
            ["TPS", f"{self.tokens_per_second:.2f}"],
        ]

        table = tabulate(
            telemetry, headers=["Metric", "Value"], tablefmt="fancy_grid"
        ).split("\n")

        # Show telemetry in debug while complying with uvicorn's log indentation
        logging.debug("\n          ".join(table))


def _log_subprocess_output(
    process: subprocess.Popen, prefix: str, telemetry: LlamaTelemetry
):
    """
    Read subprocess output line by line, log to debug, and parse telemetry
    """

    if process.stdout:
        for line in iter(process.stdout.readline, ""):
            if line:
                line_stripped = line.strip()
                logging.debug("%s: %s", prefix, line_stripped)

                telemetry.parse_telemetry_line(line_stripped)

            if process.poll() is not None:
                break


def _wait_for_load(
    llama_server_process: subprocess.Popen, port: int, fail_message: str
):
    status_code = None
    while not llama_server_process.poll() and status_code != 200:
        health_url = f"http://localhost:{port}/health"
        try:
            health_response = requests.get(health_url)
        except requests.exceptions.ConnectionError:
            logging.warning(fail_message)
        else:
            status_code = health_response.status_code
            logging.debug(
                "Testing llama-server readiness (will retry until ready), "
                f"result: {health_response.json()}"
            )
        time.sleep(1)


def _launch_llama_subprocess(
    model_path: str, use_gpu: bool, telemetry: LlamaTelemetry
) -> subprocess.Popen:
    """
    Launch llama server subprocess with GPU or CPU configuration
    """

    # Find a port, and save it in the telemetry object for future reference
    # by other functions
    telemetry.choose_port()

    base_command = [
        LLAMA_SERVER_EXE_PATH,
        "-m",
        model_path,
        "--port",
        str(telemetry.port),
        "--jinja",
    ]

    # Configure GPU layers: 99 for GPU, 0 for CPU-only
    ngl_value = "99" if use_gpu else "0"
    command = base_command + ["-ngl", ngl_value]

    # Start subprocess with output capture
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
    )

    # Start background thread to log subprocess output
    device_type = "GPU" if use_gpu else "CPU"
    threading.Thread(
        target=_log_subprocess_output,
        args=(process, f"LLAMA SERVER {device_type}", telemetry),
        daemon=True,
    ).start()

    return process


def server_load(checkpoint: str, model_reference: str, telemetry: LlamaTelemetry):
    # Download llama.cpp server if it isn't already available
    if not os.path.exists(LLAMA_SERVER_EXE_DIR):
        # Download llama.cpp server zip
        # pylint: disable=C0301
        llama_zip_url = f"https://github.com/ggml-org/llama.cpp/releases/download/{LLAMA_VERSION}/llama-{LLAMA_VERSION}-bin-win-vulkan-x64.zip"
        llama_zip_path = os.path.join(
            os.path.dirname(sys.executable), "llama-server.zip"
        )
        logging.info(f"Downloading llama.cpp server from {llama_zip_url}")

        with requests.get(llama_zip_url, stream=True) as r:
            r.raise_for_status()
            with open(llama_zip_path, "wb") as f:
                for chunk in r.iter_content(chunk_size=8192):
                    f.write(chunk)

        # Extract zip
        logging.info(f"Extracting {llama_zip_path} to {LLAMA_SERVER_EXE_DIR}")
        with zipfile.ZipFile(llama_zip_path, "r") as zip_ref:
            zip_ref.extractall(LLAMA_SERVER_EXE_DIR)

        # Save version.txt
        version_txt_path = os.path.join(LLAMA_SERVER_EXE_DIR, "version.txt")
        with open(version_txt_path, "w", encoding="utf-8") as vf:
            vf.write(LLAMA_VERSION)

        # Delete zip file
        os.remove(llama_zip_path)
        logging.info("Cleaned up zip file")

    # Download the gguf to the hugging face cache
    snapshot_path = ModelManager().download_gguf(checkpoint)
    model_path = os.path.join(snapshot_path, os.listdir(snapshot_path)[0])
    logging.debug(f"GGUF file path: {model_path}")

    # Start the llama-serve.exe process
    logging.debug(f"Using llama_server for GGUF model: {LLAMA_SERVER_EXE_PATH}")

    # Attempt loading on GPU first
    llama_server_process = _launch_llama_subprocess(
        model_path, use_gpu=True, telemetry=telemetry
    )

    # Check the /health endpoint until GPU server is ready
    _wait_for_load(
        llama_server_process,
        telemetry.port,
        f"Loading {model_reference} on GPU didn't work, re-attempting on CPU",
    )

    # If loading on GPU failed, try loading on CPU
    if llama_server_process.poll():
        llama_server_process = _launch_llama_subprocess(
            model_path, use_gpu=False, telemetry=telemetry
        )

        # Check the /health endpoint until CPU server is ready
        _wait_for_load(
            llama_server_process,
            telemetry.port,
            f"Loading {model_reference} on CPU didn't work",
        )

    if llama_server_process.poll():
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=f"Failed to load {model_reference} with llama.cpp",
        )

    return llama_server_process


def chat_completion(
    chat_completion_request: ChatCompletionRequest, telemetry: LlamaTelemetry
):
    base_url = f"http://127.0.0.1:{telemetry.port}/v1"
    client = OpenAI(
        base_url=base_url,
        api_key="lemonade",
    )

    # Convert Pydantic model to dict and remove unset/null values
    request_dict = chat_completion_request.model_dump(
        exclude_unset=True, exclude_none=True
    )

    def event_stream():
        try:
            # Enable streaming
            request_dict["stream"] = True
            for chunk in client.chat.completions.create(**request_dict):
                yield f"data: {chunk.model_dump_json()}\n\n"
            yield "data: [DONE]\n\n"

            # Show telemetry after completion
            telemetry.show_telemetry()

        except Exception as e:  # pylint: disable=broad-exception-caught
            yield f'data: {{"error": "{str(e)}"}}\n\n'

    return StreamingResponse(
        event_stream(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
        },
    )
