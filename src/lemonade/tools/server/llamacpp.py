import os
import logging
import subprocess
import re
import threading
import platform
import time

from dotenv import load_dotenv
from fastapi import HTTPException, status

from lemonade_server.pydantic_models import (
    PullConfig,
)
from lemonade.tools.llamacpp.utils import (
    get_llama_server_exe_path,
    install_llamacpp,
    download_gguf,
)
from lemonade.tools.server.wrapped_server import WrappedServerTelemetry, WrappedServer


class LlamaTelemetry(WrappedServerTelemetry):
    """
    Manages telemetry data collection and display for llama server.
    """

    def __init__(self):
        super().__init__()
        # Prefill progress tracking
        self.prefill_progress = 0.0  # Current progress (0.0 to 1.0)
        self.last_reported_progress = 0.0  # Track last reported to avoid spam
        self.prefill_total_tokens = 0  # Total tokens to process
        self.prefill_processed_tokens = 0  # Tokens processed so far
        self.last_progress_time = 0.0  # Track last update time for throttling
        self.prefill_complete = False  # Flag to indicate prefill completion

    def reset_prefill_progress(self):
        """Reset prefill progress tracking for a new request."""
        self.prefill_progress = 0.0
        self.last_reported_progress = 0.0
        self.prefill_total_tokens = 0
        self.prefill_processed_tokens = 0
        self.last_progress_time = 0.0
        self.prefill_complete = False

    def should_report_progress(self) -> bool:
        """Check if progress should be reported based on throttling rules."""
        current_time = time.time()
        time_since_last = current_time - self.last_progress_time
        progress_change = abs(self.prefill_progress - self.last_reported_progress)

        # Report if: significant progress change (>10%) OR enough time passed (>100ms)
        if progress_change >= 0.1 or time_since_last >= 0.1:
            self.last_progress_time = current_time
            self.last_reported_progress = self.prefill_progress
            return True
        return False

    def parse_telemetry_line(self, line: str):
        """
        Parse telemetry data from llama server output lines.
        """

        # Parse prefill progress from llama.cpp server output
        # Look for "prompt processing progress" with progress value
        progress_match = re.search(
            r"prompt processing progress.*progress\s*=\s*([\d.]+)", line
        )
        if progress_match:
            new_progress = float(progress_match.group(1))
            self.prefill_progress = min(new_progress, 1.0)
            return

        # Also check for "prompt done" to mark completion
        if "prompt done" in line:
            self.prefill_complete = True
            self.prefill_progress = 1.0
            return

        if "vk::PhysicalDevice::createDevice: ErrorExtensionNotPresent" in line:
            msg = (
                "Your AMD GPU driver version is not compatible with this software.\n"
                "Please update and try again: "
                "https://www.amd.com/en/support/download/drivers.html"
            )
            logging.error(msg)
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=msg,
            )
        elif "error" in line.lower():
            logging.error(line)

        # Parse Vulkan device detection
        vulkan_match = re.search(r"ggml_vulkan: Found (\d+) Vulkan devices?:", line)
        if vulkan_match:
            device_count = int(vulkan_match.group(1))
            if device_count > 0:
                logging.info(
                    f"GPU acceleration active: {device_count} device(s) "
                    "detected by llama-server"
                )
            return

        # Parse prompt evaluation line (marks end of prefill)
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

            # Mark prefill as complete
            self.prefill_complete = True
            self.prefill_progress = 1.0
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


class LlamaServer(WrappedServer):
    def __init__(self, backend: str):
        self.telemetry = LlamaTelemetry()
        self.backend = backend
        super().__init__(server_name="llama-server", telemetry=self.telemetry)

    def install_server(self, backend=None):
        """
        Install the wrapped server
        """
        install_llamacpp(self.backend)

    def download_model(
        self, config_checkpoint, config_mmproj=None, do_not_upgrade=False
    ) -> dict:
        """
        Download a model for the wrapper server
        """
        return download_gguf(
            config_checkpoint=config_checkpoint,
            config_mmproj=config_mmproj,
            do_not_upgrade=do_not_upgrade,
        )

    def _launch_device_backend_subprocess(
        self,
        snapshot_files: dict,
        use_gpu: bool,
        ctx_size: int,
        supports_embeddings: bool = False,
        supports_reranking: bool = False,
    ) -> subprocess.Popen:
        """
        Launch llama server subprocess with appropriate configuration.

        Args:
            snapshot_files: Dictionary of model files to load
            use_gpu: Whether to use GPU acceleration
            telemetry: Telemetry object for tracking performance metrics
            backend: Backend to use (e.g., 'vulkan', 'rocm')
            supports_embeddings: Whether the model supports embeddings
            supports_reranking: Whether the model supports reranking

        Returns:
            Subprocess handle for the llama server
        """

        # Get the current executable path (handles both Windows and Ubuntu structures)
        exe_path = get_llama_server_exe_path(self.backend)

        # Build the base command
        base_command = [
            exe_path,
            "-m",
            snapshot_files["variant"],
            "--ctx-size",
            str(ctx_size),
        ]

        # Lock random seed for deterministic behavior in CI
        if os.environ.get("LEMONADE_CI_MODE"):
            base_command.extend(["--seed", "42"])
            logging.info(f"Seed applied to base command: {base_command}")

        if "mmproj" in snapshot_files:
            base_command.extend(["--mmproj", snapshot_files["mmproj"]])
            if not use_gpu:
                base_command.extend(["--no-mmproj-offload"])

        # Find a port, and save it in the telemetry object for future reference
        # by other functions
        self.choose_port()

        # Add port and jinja to enable tool use
        base_command.extend(["--port", str(self.port), "--jinja"])

        # Disable jinja for gpt-oss-120b on Vulkan
        if (
            self.backend == "vulkan"
            and "gpt-oss-120b" in snapshot_files["variant"].lower()
        ):
            base_command.remove("--jinja")
            logging.warning(
                "Jinja is disabled for gpt-oss-120b on Vulkan due to a llama.cpp bug "
                "(see https://github.com/ggml-org/llama.cpp/issues/15274). "
                "The model cannot use tools. If needed, use the ROCm backend instead."
            )

        # Use legacy reasoning formatting, since not all apps support the new
        # reasoning_content field
        base_command.extend(["--reasoning-format", "none"])

        # Add embeddings support if the model supports it
        if supports_embeddings:
            base_command.append("--embeddings")

        # Add reranking support if the model supports it
        if supports_reranking:
            base_command.append("--reranking")

        # Configure GPU layers: 99 for GPU, 0 for CPU-only
        ngl_value = "99" if use_gpu else "0"
        command = base_command + ["-ngl", ngl_value]

        # Set up environment with library path for Linux
        env = os.environ.copy()

        # Load environment variables from .env file in the executable directory
        exe_dir = os.path.dirname(exe_path)
        env_file_path = os.path.join(exe_dir, ".env")
        if os.path.exists(env_file_path):
            load_dotenv(env_file_path, override=False)
            env.update(os.environ)
            logging.debug(f"Loaded environment variables from {env_file_path}")

        if platform.system().lower() == "linux":
            lib_dir = os.path.dirname(exe_path)  # Same directory as the executable
            current_ld_path = env.get("LD_LIBRARY_PATH", "")
            if current_ld_path:
                env["LD_LIBRARY_PATH"] = f"{lib_dir}:{current_ld_path}"
            else:
                env["LD_LIBRARY_PATH"] = lib_dir
            logging.debug(f"Set LD_LIBRARY_PATH to {env['LD_LIBRARY_PATH']}")

        # Start subprocess with output capture
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
        device_type = "GPU" if use_gpu else "CPU"
        threading.Thread(
            target=self._log_subprocess_output,
            args=(f"LLAMA SERVER {device_type}",),
            daemon=True,
        ).start()

    def _launch_server_subprocess(
        self,
        model_config: PullConfig,
        snapshot_files: dict,
        ctx_size: int,
        supports_embeddings: bool = False,
        supports_reranking: bool = False,
    ):

        # Attempt loading on GPU first
        self._launch_device_backend_subprocess(
            snapshot_files,
            use_gpu=True,
            ctx_size=ctx_size,
            supports_embeddings=supports_embeddings,
            supports_reranking=supports_reranking,
        )

        # Check the /health endpoint until GPU server is ready
        self._wait_for_load()

        # If loading on GPU failed, try loading on CPU
        if self.process.poll():
            logging.warning(
                f"Loading {model_config.model_name} on GPU didn't work, re-attempting on CPU"
            )

            if os.environ.get("LEMONADE_LLAMACPP_NO_FALLBACK"):
                # Used for testing, when the test should fail if GPU didn't work
                raise Exception("llamacpp GPU loading failed")

            self._launch_device_backend_subprocess(
                snapshot_files,
                use_gpu=False,
                ctx_size=ctx_size,
                supports_embeddings=supports_embeddings,
                supports_reranking=supports_reranking,
            )
