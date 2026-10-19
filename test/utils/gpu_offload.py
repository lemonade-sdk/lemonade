"""Shared GPU-offload log assertions for GGML-backed validators.

Both the sd-cpp and llama.cpp validators wrap GGML engines whose subprocess logs
are captured by lemond when it runs in debug mode (``LEMONADE_CI_MODE=1``).

The validators tail those logs per model and assert the engine actually engaged a GPU
device instead of silently falling back to CPU.

The device/offload lines differ per engine, so callers pass their own compiled patterns;
the reading and matching mechanics are shared here.
"""

from __future__ import annotations

import os
import re
import sys
from typing import Iterable

GPU_LOG_FILENAMES = ("lemond.stdout.log", "lemond.stderr.log")


def read_new_log_text(log_path: str, offset: int) -> tuple[str, int]:
    """Read a live log file from a byte offset; return (new_text, new_offset).

    Missing file or a read error yields ("", offset) so the caller keeps polling
    without advancing.
    """
    if not log_path or not os.path.isfile(log_path):
        return "", offset
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as log_file:
            log_file.seek(offset)
            chunk = log_file.read()
            return chunk, log_file.tell()
    except OSError as exc:
        print(f"[WARN] could not read server log '{log_path}': {exc}", file=sys.stderr)
        return "", offset


def gpu_offload_confirmed(
    log_chunk: str, patterns: "Iterable[re.Pattern[str]]"
) -> bool:
    """True when any pattern matches the log chunk.

    A match means the engine reported the GPU device was engaged for this model.
    """
    return any(pattern.search(log_chunk) for pattern in patterns)
