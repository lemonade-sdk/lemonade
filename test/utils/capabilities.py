"""
WrappedServer capability catalog.

Defines which features each WrappedServer (inference backend) supports.
Tests use the skip_if_unsupported decorator to skip tests for unsupported features.
"""

from functools import wraps
import unittest

# Global state for current test configuration
_current_wrapped_server = None
_current_backend = None


def set_current_config(wrapped_server: str, backend: str = None):
    """Set the current wrapped server and backend for capability checks."""
    global _current_wrapped_server, _current_backend
    _current_wrapped_server = wrapped_server
    _current_backend = backend


def get_current_config():
    """Get the current wrapped server and backend."""
    return _current_wrapped_server, _current_backend


# Capability catalog for each WrappedServer
WRAPPED_SERVER_CAPABILITIES = {
    "llamacpp": {
        "backends": ["vulkan", "rocm", "metal", "cpu"],
        "supports": {
            "chat_completions": True,
            "chat_completions_streaming": True,
            "chat_completions_async": True,
            "completions": True,
            "completions_streaming": True,
            "completions_async": True,
            "responses_api": False,  # Not supported
            "responses_api_streaming": False,
            "embeddings": True,
            "reranking": True,
            "tool_calls": False,
            "tool_calls_streaming": False,
            "multi_model": True,
            "stop_parameter": True,
            "echo_parameter": False,
            "generation_parameters": False,
        },
        "test_models": {
            "llm": "LFM2-1.2B-GGUF",
            "embedding": "nomic-embed-text-v2-moe-GGUF",
            "reranking": "jina-reranker-v1-tiny-en-GGUF",
        },
    },
    "ryzenai": {
        "backends": ["cpu", "hybrid", "npu"],
        "supports": {
            "chat_completions": True,
            "chat_completions_streaming": True,
            "chat_completions_async": True,
            "completions": True,
            "completions_streaming": True,
            "completions_async": True,
            "responses_api": True,  # Supported
            "responses_api_streaming": True,
            "embeddings": False,  # Not yet
            "reranking": False,  # Not yet
            "tool_calls": True,
            "tool_calls_streaming": True,
            "multi_model": True,
            "stop_parameter": True,
            "echo_parameter": False,
            "generation_parameters": True,
        },
        "test_models": {
            "llm_cpu": "Qwen2.5-0.5B-Instruct-CPU",
            "llm_hybrid": "Qwen-2.5-1.5B-Instruct-Hybrid",
            "llm_npu": "Qwen-2.5-3B-Instruct-NPU",
        },
    },
    "flm": {
        "backends": ["npu"],
        "supports": {
            "chat_completions": True,
            "chat_completions_streaming": True,
            "chat_completions_async": False,  # Not tested
            "completions": True,  # Supported
            "completions_streaming": True,  # Supported
            "completions_async": False,  # Not tested
            "responses_api": False,  # Not supported
            "responses_api_streaming": False,
            "embeddings": False,
            "reranking": False,
            "tool_calls": False,
            "tool_calls_streaming": False,
            "multi_model": False,
            "stop_parameter": False,  # Not tested
            "echo_parameter": False,  # Not tested
            "generation_parameters": False,  # Not tested
        },
        "test_models": {
            "llm": "Llama-3.2-1B-FLM",
        },
    },
    "whisper": {
        "backends": ["cpu"],
        "supports": {
            "transcription": True,
            "transcription_with_language": True,
        },
        "test_models": {
            "audio": "Whisper-Tiny",
        },
    },
    "stable_diffusion": {
        "backends": ["cpu", "vulkan"],
        "supports": {
            "image_generation": True,
            "image_generation_b64": True,
        },
        "test_models": {
            "image": "SD-Turbo",
        },
    },
}


def get_capabilities(wrapped_server: str = None):
    """
    Get the capability dict for the given wrapped server.
    If wrapped_server is None, uses the current global config.
    """
    if wrapped_server is None:
        wrapped_server = _current_wrapped_server

    if wrapped_server is None:
        raise ValueError("No wrapped server specified and no global config set")

    if wrapped_server not in WRAPPED_SERVER_CAPABILITIES:
        raise ValueError(f"Unknown wrapped server: {wrapped_server}")

    return WRAPPED_SERVER_CAPABILITIES[wrapped_server]


def supports(feature: str, wrapped_server: str = None) -> bool:
    """
    Check if the current (or specified) wrapped server supports a feature.
    """
    caps = get_capabilities(wrapped_server)
    return caps.get("supports", {}).get(feature, False)


def get_test_model(
    model_type: str, wrapped_server: str = None, backend: str = None
) -> str:
    """
    Get the appropriate test model for the given type and configuration.

    For ryzenai, model_type can be 'llm' and it will be resolved based on backend
    (e.g., 'llm' with backend='cpu' -> 'llm_cpu')
    """
    if wrapped_server is None:
        wrapped_server = _current_wrapped_server
    if backend is None:
        backend = _current_backend

    caps = get_capabilities(wrapped_server)
    test_models = caps.get("test_models", {})

    # Try backend-specific model first (e.g., llm_cpu, llm_hybrid)
    if backend:
        backend_specific_key = f"{model_type}_{backend}"
        if backend_specific_key in test_models:
            return test_models[backend_specific_key]

    # Fall back to generic model type
    if model_type in test_models:
        return test_models[model_type]

    raise ValueError(
        f"No test model found for type '{model_type}' with wrapped_server='{wrapped_server}', backend='{backend}'"
    )


def skip_if_unsupported(feature: str):
    """
    Decorator to skip a test if the current wrapped server doesn't support a feature.

    Usage:
        @skip_if_unsupported("responses_api")
        def test_responses_api(self):
            ...
    """

    def decorator(test_func):
        @wraps(test_func)
        def wrapper(self, *args, **kwargs):
            if not supports(feature):
                wrapped_server = _current_wrapped_server or "unknown"
                self.skipTest(f"Skipping: {feature} not supported by {wrapped_server}")
            return test_func(self, *args, **kwargs)

        return wrapper

    return decorator


def requires_backend(backend: str):
    """
    Decorator to skip a test if not running on the specified backend.

    Usage:
        @requires_backend("rocm")
        def test_rocm_specific_feature(self):
            ...
    """

    def decorator(test_func):
        @wraps(test_func)
        def wrapper(self, *args, **kwargs):
            if _current_backend != backend:
                self.skipTest(f"Skipping: test requires {backend} backend")
            return test_func(self, *args, **kwargs)

        return wrapper

    return decorator
