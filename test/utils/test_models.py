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


def get_hf_cache_dir():
    """Resolve the HF cache directory for on-disk assertions.

    Mirrors path_utils.cpp resolve_hf_cache_dir() — the env-var / platform
    default chain:
      1. HF_HUB_CACHE env var (direct path)
      2. HF_HOME env var + /hub
      3. Platform default (~/.cache/huggingface/hub)

    NOTE: This does NOT cover the server's models_dir override
    (path_utils.cpp get_hf_cache_dir() / config.json "models_dir").
    If the server under test has models_dir set to something other than
    "auto", on-disk assertions using this path will inspect the wrong
    location. There is no API to query the server's effective models_dir
    and no env var mapping for it — it is only settable via config.json.
    """
    hf_hub_cache = os.environ.get("HF_HUB_CACHE", "")
    if hf_hub_cache:
        return hf_hub_cache
    hf_home = os.environ.get("HF_HOME", "")
    if hf_home:
        return os.path.join(hf_home, "hub")
    if platform.system() == "Windows":
        userprofile = os.environ.get("USERPROFILE", "C:\\")
        return os.path.join(userprofile, ".cache", "huggingface", "hub")
    home = os.environ.get("HOME", "/tmp")
    return os.path.join(home, ".cache", "huggingface", "hub")


def get_default_hf_cache_dir():
    """Return the platform-default HF cache directory, ignoring HF_* overrides."""
    if platform.system() == "Windows":
        userprofile = os.environ.get("USERPROFILE", "C:\\")
        return os.path.join(userprofile, ".cache", "huggingface", "hub")
    home = os.environ.get("HOME", "/tmp")
    return os.path.join(home, ".cache", "huggingface", "hub")


def get_hf_cache_dir_candidates():
    """Return likely HF cache roots for on-disk assertions.

    Order matters:
      1. Env-derived HF cache root from get_hf_cache_dir()
      2. Platform default HF cache root, ignoring HF_HUB_CACHE / HF_HOME

    This still does not cover config.json "models_dir" overrides.
    """
    candidates = []
    seen = set()

    for path in [get_hf_cache_dir(), get_default_hf_cache_dir()]:
        normalized = os.path.normcase(os.path.abspath(path))
        if normalized in seen:
            continue
        seen.add(normalized)
        candidates.append(path)

    return candidates


# Default port for lemonade server
PORT = 13305

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
ENDPOINT_TEST_MODEL_CTX_SIZE = 2048

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
VISION_MODEL = "Qwen3.5-0.8B-GGUF"

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

# Models for shared-repo dependency testing (same repo, different quants)
SHARED_REPO_MODEL_A_NAME = "user.SharedRepo-TestA"
SHARED_REPO_MODEL_A_CHECKPOINT = (
    "unsloth/SmolLM2-135M-Instruct-GGUF:SmolLM2-135M-Instruct-Q2_K.gguf"
)
SHARED_REPO_MODEL_B_NAME = "user.SharedRepo-TestB"
SHARED_REPO_MODEL_B_CHECKPOINT = (
    "unsloth/SmolLM2-135M-Instruct-GGUF:SmolLM2-135M-Instruct-Q4_K_M.gguf"
)

# Models for multi-repo dependency testing (different repos, shared text_encoder)
# Scenario: Model A has main(repo1) + text_encoder(repo2-shared)
#           Model B has main(repo3) + text_encoder(repo2-shared)
# Deleting A must keep repo2 (still needed by B). Deleting B then cleans up repo2+repo3.
MULTI_REPO_MODEL_A_NAME = "user.MultiRepo-TestA"
MULTI_REPO_MODEL_A_MAIN = (
    "unsloth/SmolLM2-135M-Instruct-GGUF:SmolLM2-135M-Instruct-Q2_K.gguf"
)
MULTI_REPO_MODEL_B_NAME = "user.MultiRepo-TestB"
MULTI_REPO_MODEL_B_MAIN = "Comfy-Org/z_image:split_files/vae/ae.safetensors"
MULTI_REPO_SHARED_CHECKPOINT = (
    "mradermacher/SmolLM2-135M-Instruct-GGUF:SmolLM2-135M-Instruct.Q2_K.gguf"
)
# Cache directory names for on-disk verification (repo_id with / replaced by --)
MULTI_REPO_MODEL_A_CACHE_DIR = "models--unsloth--SmolLM2-135M-Instruct-GGUF"
MULTI_REPO_MODEL_B_CACHE_DIR = "models--Comfy-Org--z_image"
MULTI_REPO_SHARED_CACHE_DIR = "models--mradermacher--SmolLM2-135M-Instruct-GGUF"

# Models that should be pre-downloaded for offline testing
MODELS_FOR_OFFLINE_CACHE = [
    "Qwen3-0.6B-GGUF",
    "Qwen2.5-0.5B-Instruct-CPU",
    "Llama-3.2-1B-Instruct-CPU",
]
