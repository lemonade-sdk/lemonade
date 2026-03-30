"""
Standard test model definitions and constants.

This module provides model constants used across test files.
Prefer using get_test_model() from capabilities.py for dynamic model selection
based on the current wrapped server and backend.
"""

import os
import platform


def _workspace_root():
    """Return the workspace root directory."""
    this_file = os.path.abspath(__file__)
    utils_dir = os.path.dirname(this_file)
    test_dir = os.path.dirname(utils_dir)
    return os.path.dirname(test_dir)


def _default_build_binary(name):
    """Return the default path for a build binary, handling multi-config generators."""
    root = _workspace_root()
    if platform.system() == "Windows":
        release_path = os.path.join(root, "build", "Release", f"{name}.exe")
        debug_path = os.path.join(root, "build", "Debug", f"{name}.exe")
        if os.path.exists(release_path):
            return release_path
        return debug_path
    else:
        return os.path.join(root, "build", name)


def get_default_server_binary():
    """
    Get the default lemonade-server binary path from the CMake build directory.

    This is the single source of truth for the default server binary path.
    All test files should import this function rather than computing the path themselves.

    Returns:
        Path to lemonade-server binary in the build directory.
    """
    return _default_build_binary("lemonade-server")


def get_default_lemond_binary():
    """
    Get the default lemond binary path from the CMake build directory.

    Used by tests that start lemond directly (env var tests, system-info mock tests).

    Returns:
        Path to lemond binary in the build directory.
    """
    return _default_build_binary("lemond")


# Default port for lemonade server
PORT = 8000

# =============================================================================
# TIMEOUT CONSTANTS (in seconds)
# =============================================================================

# For requests that could download a model (inference, load, pull)
# Model downloads can take several minutes on slow connections
TIMEOUT_MODEL_OPERATION = 500

# For requests that don't download a model (health, unload, stats, etc.)
TIMEOUT_DEFAULT = 60

# Standard test messages for chat completions
STANDARD_MESSAGES = [
    {"role": "system", "content": "You are a helpful assistant."},
    {"role": "user", "content": "Who won the world series in 2020?"},
    {"role": "assistant", "content": "The LA Dodgers won in 2020."},
    {"role": "user", "content": "What was the best play?"},
]

# Standard test messages for responses
RESPONSES_MESSAGES = [
    {"role": "system", "content": "You are a helpful assistant."},
    {"role": "user", "content": "Who won the world series in 2020?"},
    {
        "role": "assistant",
        "type": "message",
        "content": [{"text": "The LA Dodgers won in 2020.", "type": "output_text"}],
    },
    {"role": "user", "content": "What was the best play?"},
]

# Simple test messages for quick tests
SIMPLE_MESSAGES = [
    {"role": "user", "content": "Say hello in exactly 5 words."},
]

# Test prompt for completions endpoint
TEST_PROMPT = "Hello, how are you?"

# Sample tool schema for tool call testing (based on mcp-server-calculator)
SAMPLE_TOOL = {
    "type": "function",
    "function": {
        "name": "calculator_calculate",
        "parameters": {
            "properties": {"expression": {"title": "Expression", "type": "string"}},
            "required": ["expression"],
            "title": "calculateArguments",
            "type": "object",
        },
    },
}

# Models for endpoint testing (inference-agnostic, just need any valid small model)
ENDPOINT_TEST_MODEL = "Tiny-Test-Model-GGUF"

# Model for tool-calling tests (must have native tool-calling support in its chat template)
TOOL_CALLING_MODEL = "Qwen3-4B-Instruct-2507-GGUF"

# Secondary model for multi-model testing (small, fast to load)
MULTI_MODEL_SECONDARY = "Tiny-Test-Model-GGUF"

# Tertiary model for LRU eviction testing
MULTI_MODEL_TERTIARY = "Qwen3-0.6B-GGUF"

# Whisper test configuration
WHISPER_MODEL = "Whisper-Tiny"
TEST_AUDIO_URL = (
    "https://raw.githubusercontent.com/lemonade-sdk/assets/main/audio/test_speech.wav"
)

# Vision model test configuration
VISION_MODEL = "Gemma-3-4b-it-GGUF"

# Stable Diffusion test configuration
SD_MODEL = "SD-Turbo"

# ESRGAN upscale model test configuration
ESRGAN_MODEL = "RealESRGAN-x4plus"

# Text-to-Speech test configuration
TTS_MODEL = "kokoro-v1"

# User models. The combinations of files seen here do not work but we will only test download
USER_MODEL_NAME = "user.Dummy-Model"
USER_MODEL_MAIN_CHECKPOINT = (
    "unsloth/SmolLM2-135M-Instruct-GGUF:SmolLM2-135M-Instruct-Q2_K.gguf"
)
USER_MODEL_TE_CHECKPOINT = (
    "mradermacher/SmolLM2-135M-Instruct-GGUF:SmolLM2-135M-Instruct.Q2_K.gguf"
)
# Using a file not at repo top-level
USER_MODEL_VAE_CHECKPOINT = "Comfy-Org/z_image:split_files/vae/ae.safetensors"

# Models that should be pre-downloaded for offline testing
MODELS_FOR_OFFLINE_CACHE = [
    "Qwen3-0.6B-GGUF",
    "Qwen2.5-0.5B-Instruct-CPU",
    "Llama-3.2-1B-Instruct-CPU",
]
