"""Lemonade Lean - Minimal LLM server with llama.cpp Vulkan backend."""

__version__ = "0.1.0"

# Expose HF cache utilities
from lemonade_lean.hf_cache import (
    scan_hf_cache,
    find_gguf_models,
    get_hf_cache_dir,
    get_model_snapshot_path,
)

__all__ = [
    "scan_hf_cache",
    "find_gguf_models",
    "get_hf_cache_dir",
    "get_model_snapshot_path",
    "__version__",
]
