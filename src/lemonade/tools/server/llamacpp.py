import sys
import os
import logging
import time
import subprocess
import zipfile
import re
import threading
import platform
import shutil

import requests
from tabulate import tabulate
from fastapi import HTTPException, status
from fastapi.responses import StreamingResponse

from openai import OpenAI

from lemonade_server.pydantic_models import ChatCompletionRequest, PullConfig
from lemonade_server.model_manager import ModelManager
from lemonade.tools.server.utils.port import find_free_port

LLAMA_VERSION = "b5699"


def get_llama_server_paths():
    """
    Get platform-specific paths for llama server directory and executable
    """
    base_dir = os.path.join(os.path.dirname(sys.executable), "llama_server")

    if platform.system().lower() == "windows":
        return base_dir, os.path.join(base_dir, "llama-server.exe")
    else:  # Linux/Ubuntu
        # Check if executable exists in build/bin subdirectory (Current Ubuntu structure)
        build_bin_path = os.path.join(base_dir, "build", "bin", "llama-server")
        if os.path.exists(build_bin_path):
            return base_dir, build_bin_path
        else:
            # Fallback to root directory
            return base_dir, os.path.join(base_dir, "llama-server")


def get_binary_url_and_filename(version):
    """
    Get the appropriate binary URL and filename based on platform
    """
    system = platform.system().lower()

    if system == "windows":
        filename = f"llama-{version}-bin-win-vulkan-x64.zip"
    elif system == "linux":
        filename = f"llama-{version}-bin-ubuntu-vulkan-x64.zip"
    else:
        raise NotImplementedError(
            f"Platform {system} not supported for llamacpp. Supported: Windows, Ubuntu Linux"
        )

    url = (
        f"https://github.com/ggml-org/llama.cpp/releases/download/{version}/{filename}"
    )
    return url, filename


def validate_platform_support():
    """
    Validate platform support before attempting download
    """
    system = platform.system().lower()

    if system not in ["windows", "linux"]:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=(
                f"Platform {system} not supported for llamacpp. "
                "Supported: Windows, Ubuntu Linux"
            ),
        )

    if system == "linux":
        # Check if we're actually on Ubuntu/compatible distro and log a warning if not
        try:
            with open("/etc/os-release", "r", encoding="utf-8") as f:
                os_info = f.read().lower()
                if "ubuntu" not in os_info and "debian" not in os_info:
                    logging.warning(
                        "llamacpp binaries are built for Ubuntu. "
                        "Compatibility with other Linux distributions is not guaranteed."
                    )
        except (FileNotFoundError, PermissionError, OSError) as e:
            logging.warning(
                "Could not determine Linux distribution (%s). "
                "llamacpp binaries are built for Ubuntu.",
                str(e),
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

        # Parse Vulkan device detection
        vulkan_match = re.search(r"ggml_vulkan: Found (\d+) Vulkan devices?:", line)
        if vulkan_match:
            device_count = int(vulkan_match.group(1))
            if device_count > 0:
                logging.info(
                    f"GPU acceleration active: {device_count} Vulkan device(s) "
                    "detected by llama-server"
                )
            return

        # Parse prompt evaluation line
        prompt_match = re.search(
            r"prompt eval time\s*=\s*([\d.]+)\s*ms\s*/\s*(\d+)\s*tokens.*?"
            r"([\d.]+)\s*tokens per second",
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
            r"eval time\s*=\s*([\d.]+)\s*ms\s*/\s*(\d+)\s*tokens.*?"
            r"([\d.]+)\s*tokens per second",
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


def _wait_for_load(llama_server_process: subprocess.Popen, port: int):
    status_code = None
    while not llama_server_process.poll() and status_code != 200:
        health_url = f"http://localhost:{port}/health"
        try:
            health_response = requests.get(health_url)
        except requests.exceptions.ConnectionError:
            logging.debug("Not able to connect to llama-server yet, will retry")
        else:
            status_code = health_response.status_code
            logging.debug(
                "Testing llama-server readiness (will retry until ready), "
                f"result: {health_response.json()}"
            )
        time.sleep(1)


def _launch_llama_subprocess(
    snapshot_files: dict, use_gpu: bool, telemetry: LlamaTelemetry
) -> subprocess.Popen:
    """
    Launch llama server subprocess with GPU or CPU configuration
    """

    # Get the current executable path (handles both Windows and Ubuntu structures)
    _, exe_path = get_llama_server_paths()

    # Build the base command
    base_command = [exe_path, "-m", snapshot_files["variant"]]
    if "mmproj" in snapshot_files:
        base_command.extend(["--mmproj", snapshot_files["mmproj"]])
        if not use_gpu:
            base_command.extend(["--no-mmproj-offload"])

    # Find a port, and save it in the telemetry object for future reference
    # by other functions
    telemetry.choose_port()

    # Add port and jinja to enable tool use
    base_command.extend(["--port", str(telemetry.port), "--jinja"])

    # Use legacy reasoning formatting, since not all apps support the new
    # reasoning_content field
    base_command.extend(["--reasoning-format", "none"])

    # Configure GPU layers: 99 for GPU, 0 for CPU-only
    ngl_value = "99" if use_gpu else "0"
    command = base_command + ["-ngl", ngl_value]

    # Set up environment with library path for Linux
    env = os.environ.copy()
    if platform.system().lower() == "linux":
        lib_dir = os.path.dirname(exe_path)  # Same directory as the executable
        current_ld_path = env.get("LD_LIBRARY_PATH", "")
        if current_ld_path:
            env["LD_LIBRARY_PATH"] = f"{lib_dir}:{current_ld_path}"
        else:
            env["LD_LIBRARY_PATH"] = lib_dir
        logging.debug(f"Set LD_LIBRARY_PATH to {env['LD_LIBRARY_PATH']}")

    # Start subprocess with output capture
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
    )

    # Start background thread to log subprocess output
    device_type = "GPU" if use_gpu else "CPU"
    threading.Thread(
        target=_log_subprocess_output,
        args=(process, f"LLAMA SERVER {device_type}", telemetry),
        daemon=True,
    ).start()

    return process


def server_load(model_config: PullConfig, telemetry: LlamaTelemetry):

    # Validate platform support before proceeding
    validate_platform_support()

    # Get platform-specific paths at runtime
    llama_server_exe_dir, llama_server_exe_path = get_llama_server_paths()

    # Check whether the llamacpp install needs an upgrade
    version_txt_path = os.path.join(llama_server_exe_dir, "version.txt")
    if os.path.exists(version_txt_path):
        with open(version_txt_path, "r", encoding="utf-8") as f:
            llamacpp_installed_version = f.read()

        if llamacpp_installed_version != LLAMA_VERSION:
            # Remove the existing install, which will trigger a new install
            # in the next code block
            shutil.rmtree(llama_server_exe_dir)

    # Download llama.cpp server if it isn't already available
    if not os.path.exists(llama_server_exe_dir):
        # Download llama.cpp server zip
        llama_zip_url, filename = get_binary_url_and_filename(LLAMA_VERSION)
        llama_zip_path = os.path.join(os.path.dirname(sys.executable), filename)
        logging.info(f"Downloading llama.cpp server from {llama_zip_url}")

        with requests.get(llama_zip_url, stream=True) as r:
            r.raise_for_status()
            with open(llama_zip_path, "wb") as f:
                for chunk in r.iter_content(chunk_size=8192):
                    f.write(chunk)

        # Extract zip
        logging.info(f"Extracting {llama_zip_path} to {llama_server_exe_dir}")
        with zipfile.ZipFile(llama_zip_path, "r") as zip_ref:
            zip_ref.extractall(llama_server_exe_dir)

        # Make executable on Linux - need to update paths after extraction
        if platform.system().lower() == "linux":
            # Re-get the paths since extraction might have changed the directory structure
            _, updated_exe_path = get_llama_server_paths()
            if os.path.exists(updated_exe_path):
                os.chmod(updated_exe_path, 0o755)
                logging.info(f"Set executable permissions for {updated_exe_path}")
            else:
                logging.warning(
                    f"Could not find llama-server executable at {updated_exe_path}"
                )

        # Save version.txt
        with open(version_txt_path, "w", encoding="utf-8") as vf:
            vf.write(LLAMA_VERSION)

        # Delete zip file
        os.remove(llama_zip_path)
        logging.info("Cleaned up zip file")

    # Download the gguf to the hugging face cache
    snapshot_files = ModelManager().download_gguf(model_config)
    logging.debug(f"GGUF file paths: {snapshot_files}")

    # Start the llama-serve.exe process
    logging.debug(f"Using llama_server for GGUF model: {llama_server_exe_path}")

    # Attempt loading on GPU first
    llama_server_process = _launch_llama_subprocess(
        snapshot_files, use_gpu=True, telemetry=telemetry
    )

    # Check the /health endpoint until GPU server is ready
    _wait_for_load(
        llama_server_process,
        telemetry.port,
    )

    # If loading on GPU failed, try loading on CPU
    if llama_server_process.poll():
        logging.warning(
            f"Loading {model_config.model_name} on GPU didn't work, re-attempting on CPU"
        )

        llama_server_process = _launch_llama_subprocess(
            snapshot_files, use_gpu=False, telemetry=telemetry
        )

        # Check the /health endpoint until CPU server is ready
        _wait_for_load(
            llama_server_process,
            telemetry.port,
        )

    if llama_server_process.poll():
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=f"Failed to load {model_config.model_name} with llama.cpp",
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

    # Check if streaming is requested
    if chat_completion_request.stream:

        def event_stream():
            try:
                # Enable streaming
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
    else:
        # Non-streaming response
        try:
            # Disable streaming for non-streaming requests
            response = client.chat.completions.create(**request_dict)

            # Show telemetry after completion
            telemetry.show_telemetry()

            return response

        except Exception as e:  # pylint: disable=broad-exception-caught
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=f"Chat completion error: {str(e)}",
            )
