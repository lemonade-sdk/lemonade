"""
WrappedServer capability catalog.

Defines which features each WrappedServer (inference backend) supports,
organized by modality (matching test files).

Tests use the skip_if_unsupported decorator to skip tests for unsupported features.
"""

from functools import wraps
import unittest

# Global state for current test configuration
_current_wrapped_server = None
_current_backend = None
_current_modality = None


def set_current_config(wrapped_server: str, backend: str = None, modality: str = None):
    """Set the current wrapped server, backend, and modality for capability checks."""
    global _current_wrapped_server, _current_backend, _current_modality
    _current_wrapped_server = wrapped_server
    _current_backend = backend
    _current_modality = modality


def get_current_config():
    """Get the current wrapped server, backend, and modality."""
    return _current_wrapped_server, _current_backend, _current_modality


# =============================================================================
# Modality-first capability catalog
# =============================================================================
# Top-level keys are modalities (matching test files).
# Each modality contains per-backend capabilities and test models.

CAPABILITIES = {
    "llm": {
        "llamacpp": {
            "backends": ["vulkan", "rocm", "metal", "cpu"],
            "supports": {
                "chat_completions": True,
                "chat_completions_streaming": True,
                "chat_completions_async": True,
                "completions": True,
                "completions_streaming": True,
                "completions_async": True,
                "responses_api": True,
                "responses_api_streaming": True,
                "embeddings": True,
                "embeddings_batch": True,
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
                "responses_api": False,
                "responses_api_streaming": False,
                "embeddings": False,
                "embeddings_batch": False,
                "reranking": False,
                "tool_calls": True,
                "tool_calls_streaming": True,
                "multi_model": True,
                "stop_parameter": True,
                "echo_parameter": False,
                "generation_parameters": False,
            },
            "test_models": {
                "llm_cpu": "Qwen2.5-0.5B-Instruct-CPU",
                "llm_hybrid": "Qwen-2.5-1.5B-Instruct-Hybrid",
                "llm_npu": "Qwen2.5-3B-Instruct-NPU",
            },
        },
        "flm": {
            "backends": ["npu"],
            "supports": {
                "chat_completions": True,
                "chat_completions_streaming": True,
                "chat_completions_async": False,
                "completions": True,
                "completions_streaming": True,
                "completions_async": False,
                "responses_api": False,
                "responses_api_streaming": False,
                "embeddings": True,
                "embeddings_batch": False,
                "reranking": False,
                "tool_calls": False,
                "tool_calls_streaming": False,
                "multi_model": True,
                "stop_parameter": False,
                "echo_parameter": False,
                "generation_parameters": False,
            },
            "test_models": {
                "llm": "Llama-3.2-1B-FLM",
                "embedding": "embed-gemma-300m-FLM",
            },
        },
    },
    "whisper": {
        "whispercpp": {
            "backends": ["cpu", "npu", "vulkan"],
            "supports": {
                "transcription": True,
                "transcription_with_language": True,
                "rai_cache": True,
                "realtime_websocket": True,
            },
            "test_models": {
                "audio": "Whisper-Tiny",
            },
        },
        "flm": {
            "backends": ["npu"],
            "supports": {
                "transcription": True,
                "transcription_with_language": True,
                "rai_cache": False,
                "realtime_websocket": True,
            },
            "test_models": {
                "audio": "whisper-v3-turbo-FLM",
            },
        },
    },
    "stable_diffusion": {
        "sd-cpp": {
            "backends": ["cpu", "vulkan"],
            "supports": {
                "image_generation": True,
                "image_generation_b64": True,
            },
            "test_models": {
                "image": "SD-Turbo",
            },
        },
    },
}


# =============================================================================
# Backward-compatible flat alias
# =============================================================================
# Builds WRAPPED_SERVER_CAPABILITIES from CAPABILITIES by collecting all
# backend names across modalities. First occurrence wins for duplicates
# (e.g., "flm" appears in both "llm" and "whisper" — the "llm" one is used).


def _build_flat_capabilities():
    flat = {}
    for _modality, backends in CAPABILITIES.items():
        for backend_name, backend_caps in backends.items():
            if backend_name not in flat:
                flat[backend_name] = backend_caps
    return flat


WRAPPED_SERVER_CAPABILITIES = _build_flat_capabilities()


def get_all_wrapped_server_names():
    """Get all known wrapped server names across all modalities."""
    names = set()
    for backends in CAPABILITIES.values():
        names.update(backends.keys())
    return names


def get_wrapped_servers_for_modality(modality: str):
    """Get the set of valid wrapped server names for a given modality."""
    if modality not in CAPABILITIES:
        raise ValueError(f"Unknown modality: {modality}")
    return set(CAPABILITIES[modality].keys())


def get_capabilities(wrapped_server: str = None, modality: str = None):
    """
    Get the capability dict for the given wrapped server.
    If wrapped_server is None, uses the current global config.
    If modality is provided, looks up CAPABILITIES[modality][wrapped_server].
    Otherwise falls back to the flat WRAPPED_SERVER_CAPABILITIES.
    """
    if wrapped_server is None:
        wrapped_server = _current_wrapped_server
    if modality is None:
        modality = _current_modality

    if wrapped_server is None:
        raise ValueError("No wrapped server specified and no global config set")

    # Modality-specific lookup (unambiguous for backends like "flm")
    if modality and modality in CAPABILITIES:
        modality_backends = CAPABILITIES[modality]
        if wrapped_server in modality_backends:
            return modality_backends[wrapped_server]
        raise ValueError(
            f"Unknown wrapped server '{wrapped_server}' for modality '{modality}'"
        )

    # Flat fallback
    if wrapped_server not in WRAPPED_SERVER_CAPABILITIES:
        raise ValueError(f"Unknown wrapped server: {wrapped_server}")

    return WRAPPED_SERVER_CAPABILITIES[wrapped_server]


def supports(feature: str, wrapped_server: str = None, modality: str = None) -> bool:
    """
    Check if the current (or specified) wrapped server supports a feature.
    """
    caps = get_capabilities(wrapped_server, modality)
    return caps.get("supports", {}).get(feature, False)


def get_test_model(
    model_type: str,
    wrapped_server: str = None,
    backend: str = None,
    modality: str = None,
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
    if modality is None:
        modality = _current_modality

    caps = get_capabilities(wrapped_server, modality)
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
