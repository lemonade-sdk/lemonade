"""
Tool for loading a model into Lemonade Server via the /api/v1/load endpoint.
"""

import argparse
import platform
from typing import Optional

import requests

import lemonade.common.status as status
import lemonade.common.printing as printing
from lemonade.state import State
from lemonade.tools import FirstTool
from lemonade.tools.adapter import ModelAdapter, PassthroughTokenizer
from lemonade.cache import Keys

DEFAULT_SERVER_URL = "http://localhost:8000"
DEFAULT_TIMEOUT = 300  # 5 minutes for model loading

# Process names for wrapped servers that lemonade-server may spawn
WRAPPED_SERVER_PROCESS_NAMES = [
    "llama-server",
    "llama-server.exe",
    "flm",
    "flm.exe",
    "ryzenai-server",
    "ryzenai-server.exe",
    "lemonade-server",
    "lemonade-server.exe",
]


def get_wrapped_server_peak_memory() -> Optional[int]:
    """
    Get the combined peak working set memory for wrapped server processes
    (llama-server, flm, ryzenai-server, etc.) spawned by lemonade-server.

    Returns:
        Peak working set in bytes, or None if unavailable
    """
    if platform.system() != "Windows":
        # peak_wset is Windows-only
        return None

    try:
        import psutil
    except ImportError:
        return None

    total_peak = 0
    for proc in psutil.process_iter(["pid", "name"]):
        try:
            proc_name = proc.info["name"].lower()
            for server_name in WRAPPED_SERVER_PROCESS_NAMES:
                if server_name.lower() == proc_name:
                    mem_info = proc.memory_info()
                    if hasattr(mem_info, "peak_wset") and mem_info.peak_wset:
                        total_peak += mem_info.peak_wset
                    break
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue

    return total_peak if total_peak > 0 else None


class ServerTokenizerAdapter(PassthroughTokenizer):
    """
    Tokenizer adapter for server-based models.
    Since the server handles tokenization internally, this just passes through text.
    """


class ServerAdapter(ModelAdapter):
    """
    Model adapter that interfaces with Lemonade Server for inference.
    """

    def __init__(
        self,
        server_url: str,
        model_name: str,
        timeout: int = 60,
    ):
        super().__init__()
        self.server_url = server_url.rstrip("/")
        self.model_name = model_name
        self.timeout = timeout
        self.type = "server"

    def generate(
        self,
        input_ids,
        max_new_tokens: int = 512,
        temperature: float = 1.0,
        top_p: float = 1.0,
        top_k: int = None,
        repeat_penalty: float = None,
        save_max_memory_used: bool = False,
        **kwargs,  # pylint: disable=unused-argument
    ):
        """
        Generate text using the Lemonade Server /chat/completions endpoint.

        Args:
            input_ids: The input text prompt (passed through from tokenizer)
            max_new_tokens: Maximum number of tokens to generate
            temperature: Sampling temperature
            top_p: Top-p sampling parameter
            top_k: Top-k sampling parameter
            repeat_penalty: Repetition penalty
            save_max_memory_used: If True, capture wrapped server memory usage
            **kwargs: Additional arguments (ignored)

        Returns:
            Generated text response
        """
        prompt = input_ids  # PassthroughTokenizer passes text directly

        # Build request payload using chat/completions format
        payload = {
            "model": self.model_name,
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": max_new_tokens,
            "stream": False,
            "cache_prompt": False,  # Disable prompt caching for accurate benchmarking
        }

        if temperature is not None:
            payload["temperature"] = temperature
        if top_p is not None:
            payload["top_p"] = top_p
        if top_k is not None:
            payload["top_k"] = top_k
        if repeat_penalty is not None:
            payload["repeat_penalty"] = repeat_penalty

        # Make the chat completion request
        response = requests.post(
            f"{self.server_url}/api/v1/chat/completions",
            json=payload,
            timeout=self.timeout,
        )
        response.raise_for_status()
        completion_data = response.json()

        # Extract the generated text from chat completion response
        if "choices" in completion_data and len(completion_data["choices"]) > 0:
            message = completion_data["choices"][0].get("message", {})
            generated_text = message.get("content", "")
            # For reasoning models, response may be in reasoning_content
            if not generated_text:
                generated_text = message.get("reasoning_content", "")
        else:
            generated_text = ""

        # Get stats from the server
        try:
            stats_response = requests.get(
                f"{self.server_url}/api/v1/stats",
                timeout=10,
            )
            stats_response.raise_for_status()
            stats_data = stats_response.json()

            self.time_to_first_token = stats_data.get("time_to_first_token")
            self.tokens_per_second = stats_data.get("tokens_per_second")
            self.prompt_tokens = stats_data.get("input_tokens") or stats_data.get(
                "prompt_tokens"
            )
            self.response_tokens = stats_data.get("output_tokens")
        except Exception as e:  # pylint: disable=broad-exception-caught
            printing.log_warning(f"Failed to get stats from server: {e}")
            self.time_to_first_token = None
            self.tokens_per_second = None
            self.prompt_tokens = None
            self.response_tokens = None

        # Get peak memory from wrapped server processes (like oga/huggingface bench do)
        if save_max_memory_used:
            self.peak_wset = get_wrapped_server_peak_memory()
        else:
            self.peak_wset = None

        return generated_text


class Load(FirstTool):
    """
    Tool that loads a model into Lemonade Server.

    This tool connects to a running Lemonade Server instance and loads the specified
    model using the /api/v1/load endpoint. The model can then be used for inference
    by subsequent tools like `bench`.

    Input: Lemonade Server model name (e.g., "Qwen3-0.6B-GGUF")

    Output:
        state.model: ServerAdapter instance for interacting with the loaded model
        state.tokenizer: ServerTokenizerAdapter instance
        state.checkpoint: Name of the model loaded
        state.server_url: URL of the Lemonade Server

    Example usage:
        lemonade-eval -i Qwen3-0.6B-GGUF load --server-url http://localhost:8000
    """

    unique_name = "load"

    def __init__(self):
        super().__init__(monitor_message="Loading model on Lemonade Server")

        self.status_stats = [
            Keys.CHECKPOINT,
            Keys.BACKEND,
        ]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Load a model into Lemonade Server",
            add_help=add_help,
        )

        parser.add_argument(
            "--server-url",
            type=str,
            default=DEFAULT_SERVER_URL,
            help=f"URL of the Lemonade Server (default: {DEFAULT_SERVER_URL})",
        )

        parser.add_argument(
            "--ctx-size",
            type=int,
            default=None,
            help="Context size for the model. Overrides the server's default.",
        )

        parser.add_argument(
            "--llamacpp-backend",
            type=str,
            choices=["vulkan", "rocm", "metal", "cpu"],
            default=None,
            help="LlamaCpp backend to use (only applies to llamacpp models).",
        )

        parser.add_argument(
            "--llamacpp-args",
            type=str,
            default=None,
            help="Custom arguments to pass to llama-server. "
            "The following are NOT allowed: -m, --port, --ctx-size, -ngl.",
        )

        parser.add_argument(
            "--save-options",
            action="store_true",
            help="Save ctx_size, llamacpp_backend, and llamacpp_args to recipe_options.json "
            "for this model.",
        )

        parser.add_argument(
            "--timeout",
            type=int,
            default=DEFAULT_TIMEOUT,
            help=f"Timeout in seconds for model loading (default: {DEFAULT_TIMEOUT})",
        )

        return parser

    def run(
        self,
        state: State,
        input: str,
        server_url: str = DEFAULT_SERVER_URL,
        ctx_size: Optional[int] = None,
        llamacpp_backend: Optional[str] = None,
        llamacpp_args: Optional[str] = None,
        save_options: bool = False,
        timeout: int = DEFAULT_TIMEOUT,
    ) -> State:
        """
        Load a model into Lemonade Server.

        Args:
            state: Lemonade state object
            input: Model name to load (e.g., "Qwen3-0.6B-GGUF")
            server_url: URL of the Lemonade Server
            ctx_size: Context size for the model
            llamacpp_backend: LlamaCpp backend (vulkan, rocm, metal, cpu)
            llamacpp_args: Custom llama-server arguments
            save_options: Whether to save options to recipe_options.json
            timeout: Timeout for model loading in seconds

        Returns:
            Updated state with model and tokenizer set
        """
        model_name = input
        server_url = server_url.rstrip("/")

        # Save checkpoint info
        state.checkpoint = model_name
        state.save_stat(Keys.CHECKPOINT, model_name)

        # Store server URL in state for other tools
        state.server_url = server_url

        # Check server health first
        printing.log_info(f"Connecting to Lemonade Server at {server_url}...")
        try:
            health_response = requests.get(
                f"{server_url}/api/v1/health",
                timeout=10,
            )
            health_response.raise_for_status()
            printing.log_info("Server is healthy")
        except requests.exceptions.ConnectionError:
            raise ConnectionError(
                f"Cannot connect to Lemonade Server at {server_url}. "
                "Make sure the server is running with 'lemonade-server serve'."
            )
        except requests.exceptions.RequestException as e:
            raise ConnectionError(f"Error connecting to Lemonade Server: {e}")

        # Build the load request payload
        load_payload = {"model_name": model_name}

        if ctx_size is not None:
            load_payload["ctx_size"] = ctx_size
        if llamacpp_backend is not None:
            load_payload["llamacpp_backend"] = llamacpp_backend
        if llamacpp_args is not None:
            load_payload["llamacpp_args"] = llamacpp_args
        if save_options:
            load_payload["save_options"] = True

        # Load the model
        printing.log_info(f"Loading model '{model_name}' on server...")
        try:
            load_response = requests.post(
                f"{server_url}/api/v1/load",
                json=load_payload,
                timeout=timeout,
            )
            load_response.raise_for_status()
            load_result = load_response.json()

            if load_result.get("status") == "error":
                raise RuntimeError(
                    f"Failed to load model: {load_result.get('message', 'Unknown error')}"
                )

            printing.log_info(f"Model loaded: {load_result.get('message', 'Success')}")

        except requests.exceptions.Timeout:
            raise TimeoutError(
                f"Model loading timed out after {timeout} seconds. "
                "Try increasing --timeout or check server logs."
            )
        except requests.exceptions.RequestException as e:
            raise RuntimeError(f"Failed to load model on server: {e}")

        # Look up the actual backend being used
        backend_str = self._get_backend_string(server_url, model_name)
        state.save_stat(Keys.BACKEND, backend_str)

        # Create adapters for the loaded model
        state.model = ServerAdapter(
            server_url=server_url,
            model_name=model_name,
            timeout=timeout,
        )
        state.tokenizer = ServerTokenizerAdapter()

        # Add to status
        status.add_to_state(state=state, name=model_name, model=model_name)

        return state

    def _get_backend_string(self, server_url: str, model_name: str) -> str:
        """
        Query the server to determine the actual backend being used.

        Returns a backend string like:
        - "llamacpp vulkan" / "llamacpp rocm" / "llamacpp cpu" / "llamacpp metal"
        - "hybrid"
        - "npu"
        - "flm"
        - "cpu"
        """
        try:
            # Get model info from /api/v1/models/{model_id}
            model_response = requests.get(
                f"{server_url}/api/v1/models/{model_name}",
                timeout=10,
            )
            model_response.raise_for_status()
            model_info = model_response.json()

            recipe = model_info.get("recipe", "").lower()
            recipe_options = model_info.get("recipe_options", {})

            # Get device info from /api/v1/health
            health_response = requests.get(
                f"{server_url}/api/v1/health",
                timeout=10,
            )
            health_response.raise_for_status()
            health_info = health_response.json()

            # Find the loaded model's device
            device = ""
            for loaded_model in health_info.get("all_models_loaded", []):
                if loaded_model.get("model_name") == model_name:
                    device = loaded_model.get("device", "").lower()
                    break

            # Determine backend string based on recipe and device
            if recipe == "llamacpp":
                # For llamacpp, include the specific backend (vulkan, rocm, cpu, metal)
                llamacpp_backend = recipe_options.get("llamacpp_backend", "")
                if llamacpp_backend:
                    return f"llamacpp {llamacpp_backend}"
                elif "gpu" in device:
                    return "llamacpp gpu"
                else:
                    return "llamacpp cpu"

            elif recipe == "flm":
                return "flm"

            elif "hybrid" in recipe or ("gpu" in device and "npu" in device):
                return "hybrid"

            elif "npu" in recipe or device == "npu":
                return "npu"

            elif "cpu" in recipe or device == "cpu":
                return "cpu"

            else:
                # Fallback: return recipe or "Lemonade Server"
                return recipe if recipe else "Lemonade Server"

        except Exception as e:  # pylint: disable=broad-exception-caught
            printing.log_warning(f"Could not determine backend: {e}")
            return "Lemonade Server"


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
