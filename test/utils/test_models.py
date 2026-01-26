"""
Standard test model definitions and constants.

This module provides model constants used across test files.
Prefer using get_test_model() from capabilities.py for dynamic model selection
based on the current wrapped server and backend.
"""

import os
import platform


def get_default_server_binary():
    """
    Get the default server binary path from the CMake build directory.
    
    This is the single source of truth for the default server binary path.
    All test files should import this function rather than computing the path themselves.
    
    Returns:
        Path to lemonade-server binary in the build directory.
    """
    # Get the workspace root (test/utils/test_models.py -> workspace root)
    this_file = os.path.abspath(__file__)
    utils_dir = os.path.dirname(this_file)
    test_dir = os.path.dirname(utils_dir)
    workspace_root = os.path.dirname(test_dir)

    if platform.system() == "Windows":
        return os.path.join(
            workspace_root, "src", "cpp", "build", "Release", "lemonade-server.exe"
        )
    else:
        return os.path.join(workspace_root, "src", "cpp", "build", "lemonade-server")


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

# Secondary model for multi-model testing (small, fast to load)
MULTI_MODEL_SECONDARY = "Tiny-Test-Model-GGUF"

# Tertiary model for LRU eviction testing
MULTI_MODEL_TERTIARY = "Qwen3-0.6B-GGUF"

# Whisper test configuration
WHISPER_MODEL = "Whisper-Tiny"
TEST_AUDIO_URL = (
    "https://raw.githubusercontent.com/lemonade-sdk/assets/main/audio/test_speech.wav"
)

# Stable Diffusion test configuration
SD_MODEL = "SD-Turbo"

# Models that should be pre-downloaded for offline testing
MODELS_FOR_OFFLINE_CACHE = [
    "Qwen3-0.6B-GGUF",
    "Qwen2.5-0.5B-Instruct-CPU",
    "Llama-3.2-1B-Instruct-CPU",
]
