"""Assert which ggml backend sd-server actually initialized.

sd-server picks its ggml device at load time and silently falls back to CPU
when the requested GPU backend fails to come up, so a validation run that only
checks for a decoded PNG passes while exercising the wrong backend (#3401).

The evidence lives in sd-server's stdout: lemond inherits its own stdout/stderr
handles to the child, so ggml's device lines land in whatever files the caller
redirected lemond into. This module is deliberately dependency-free so it can
be unit tested without a server or the HTTP client stack.
"""

from __future__ import annotations

import re
from pathlib import Path

# "ggml_extend_backend.cpp:395 - Initializing backend: Vulkan0"
INIT_BACKEND_RE = re.compile(r"Initializing backend:\s*(\S+)")

# ggml names each device after the backend that registered it. HIP builds
# report "ROCm<n>" because GGML_CUDA_NAME is "ROCm" there.
GGML_DEVICE_PATTERNS = {
    "cpu": re.compile(r"CPU\d*", re.IGNORECASE),
    "vulkan": re.compile(r"Vulkan\d*", re.IGNORECASE),
    "rocm": re.compile(r"(?:ROCm|HIP)\d*", re.IGNORECASE),
    "cuda": re.compile(r"CUDA\d*", re.IGNORECASE),
    "metal": re.compile(r"Metal\d*", re.IGNORECASE),
}

# Echoed on failure so the job log names the reason the backend did not come
# up, e.g. "ggml_cuda_init: failed to initialize ROCm: no ROCm-capable device
# is detected".
INIT_ERROR_RE = re.compile(
    r"^.*(?:failed to initialize|device is detected|no usable devices|"
    r"unable to load backend).*$",
    re.IGNORECASE | re.MULTILINE,
)

MAX_REPORTED_REASONS = 5


def read_server_logs(log_paths: list[Path]) -> str:
    """Concatenate the captured lemond/sd-server logs.

    An unreadable file raises rather than degrading to an empty string: the
    assertion must never be skipped because the evidence could not be read.
    """
    chunks: list[str] = []
    for path in log_paths:
        try:
            chunks.append(Path(path).read_text(encoding="utf-8", errors="replace"))
        except OSError as exc:
            raise RuntimeError(f"could not read server log {path}: {exc}") from exc
    return "\n".join(chunks)


def find_initialized_devices(log_text: str) -> list[str]:
    """Every ggml device sd-server reported initializing, in order."""
    return INIT_BACKEND_RE.findall(log_text)


def find_init_failure_reasons(log_text: str) -> list[str]:
    """Deduplicated backend-initialization error lines, in order."""
    reasons = (line.strip() for line in INIT_ERROR_RE.findall(log_text))
    return list(dict.fromkeys(reason for reason in reasons if reason))


def assert_active_backend(backend: str, log_text: str) -> list[str]:
    """Fail unless every sd-server instance initialized ``backend``.

    Returns the ggml devices observed. Raises RuntimeError when a device does
    not belong to the requested backend, or when the log carries no evidence at
    all -- passing an unverified leg is the failure this exists to prevent.
    """
    expected = GGML_DEVICE_PATTERNS.get(backend)
    if expected is None:
        raise RuntimeError(f"no ggml device pattern known for backend '{backend}'")

    devices = find_initialized_devices(log_text)
    if not devices:
        raise RuntimeError(
            "server log contains no 'Initializing backend:' line, so the active "
            f"ggml backend could not be verified against the requested "
            f"'{backend}'. sd-server only logs it when run verbosely, which "
            "lemond does under debug logging."
        )

    mismatched = [device for device in devices if not expected.fullmatch(device)]
    if not mismatched:
        return devices

    message = (
        f"requested backend '{backend}' but sd-server initialized "
        f"{sorted(set(mismatched))} (all devices seen: {devices})"
    )
    reasons = find_init_failure_reasons(log_text)[:MAX_REPORTED_REASONS]
    if reasons:
        message += ". Backend init reported: " + "; ".join(reasons)
    raise RuntimeError(message)
