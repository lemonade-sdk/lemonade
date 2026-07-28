#!/usr/bin/env python3
"""Validate Lemonade stable-diffusion.cpp image generation.

Expected server state: ``lemond`` is already running. This script switches
Lemonade to the requested sd-cpp backend, then generates deterministic PNGs for
all requested model/size combinations. It records review evidence: model,
backend, prompt, seed, size, generated PNG path, byte size, request wall time,
and whether an untimed warm-up was used before collecting timings.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import struct
import sys
import time
from pathlib import Path
from typing import Any

import requests

DEFAULT_PORT = int(os.environ.get("LEMONADE_PORT", "13305"))
DEFAULT_TIMEOUT = int(os.environ.get("LEMONADE_VALIDATE_SD_TIMEOUT", "3600"))
DEFAULT_STEPS = int(os.environ.get("LEMONADE_VALIDATE_SD_STEPS", "4"))
DEFAULT_PROMPT = os.environ.get(
    "LEMONADE_VALIDATE_SD_PROMPT",
    "A small glass of lemonade on a clean table, product photo, high detail",
)
DEFAULT_MODELS = ("SD-Turbo-GGUF", "Flux-2-Klein-4B")
DEFAULT_SIZES = ("512x256", "1024x1024")


def parse_size(value: str) -> tuple[int, int]:
    try:
        width_s, height_s = value.lower().split("x", 1)
        width = int(width_s)
        height = int(height_s)
    except Exception as exc:  # noqa: BLE001 - argparse shows the clean message
        raise argparse.ArgumentTypeError(
            f"Invalid size '{value}'. Expected WIDTHxHEIGHT, e.g. 512x256."
        ) from exc
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("Image dimensions must be positive.")
    return width, height


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--backend",
        required=True,
        choices=["cpu", "vulkan", "rocm", "cuda"],
    )
    parser.add_argument("--channel", choices=["stable"], default=None)
    parser.add_argument(
        "--model",
        action="append",
        default=None,
        help=(
            "Model to validate. May be repeated. "
            "Default: SD-Turbo-GGUF and Flux-2-Klein-4B."
        ),
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    parser.add_argument("--steps", type=int, default=DEFAULT_STEPS)
    parser.add_argument("--prompt", default=DEFAULT_PROMPT)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument(
        "--size",
        action="append",
        type=parse_size,
        default=None,
        help="Image size to validate. May be repeated. Default: 512x256 and 1024x1024.",
    )
    parser.add_argument(
        "--lite",
        action="store_true",
        help="Only run the first model at the first size; intended for PR smoke checks.",
    )
    parser.add_argument(
        "--warmup",
        action="store_true",
        help=(
            "Before recording timed images for each model, run one untimed generation "
            "at the first requested size. This keeps per-image timings from being "
            "dominated by backend/model install and load time."
        ),
    )
    parser.add_argument("--output", default="sdcpp_validation.json")
    parser.add_argument("--images-dir", default="sdcpp-validation-images")
    return parser.parse_args()


def wait_for_server(base_url: str, timeout_s: int = 120) -> None:
    deadline = time.monotonic() + timeout_s
    last_error = ""
    while time.monotonic() < deadline:
        try:
            response = requests.get(f"{base_url}/live", timeout=5)
            if response.status_code == 200:
                return
            last_error = f"HTTP {response.status_code}: {response.text[:200]}"
        except requests.RequestException as exc:
            last_error = str(exc)
        time.sleep(2)
    raise RuntimeError(f"lemond did not become ready within {timeout_s}s: {last_error}")


def post_json(url: str, payload: dict[str, Any], timeout: int) -> requests.Response:
    return requests.post(url, json=payload, timeout=timeout)


def configure_backend(base_url: str, backend: str, channel: str | None) -> None:
    config: dict[str, Any] = {"sdcpp": {"backend": backend}}
    if backend == "rocm":
        config["rocm_channel"] = channel or "stable"

    response = post_json(f"{base_url}/internal/set", config, timeout=60)
    response.raise_for_status()

    # Keep matrix legs isolated even when a previous run left a model loaded.
    unload = post_json(f"{base_url}/api/v1/unload", {}, timeout=60)
    if unload.status_code not in {200, 404}:
        unload.raise_for_status()


def png_dimensions(data: bytes) -> tuple[int, int]:
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("decoded image is not a PNG")
    if data[12:16] != b"IHDR":
        raise ValueError("PNG IHDR chunk not found at expected offset")
    return struct.unpack(">II", data[16:24])


def generate_image(
    api_base_url: str,
    model: str,
    prompt: str,
    size: tuple[int, int],
    steps: int,
    seed: int,
    timeout: int,
) -> tuple[bytes, dict[str, Any], float]:
    width, height = size
    payload = {
        "model": model,
        "prompt": prompt,
        "size": f"{width}x{height}",
        "steps": steps,
        "seed": seed,
        "n": 1,
        "response_format": "b64_json",
    }
    started = time.monotonic()
    response = post_json(f"{api_base_url}/images/generations", payload, timeout=timeout)
    request_elapsed_s = time.monotonic() - started
    if response.status_code != 200:
        raise RuntimeError(
            f"image generation failed for {model} {width}x{height}: "
            f"HTTP {response.status_code}: {response.text[:1000]}"
        )
    result = response.json()
    image_b64 = result["data"][0]["b64_json"]
    return base64.b64decode(image_b64), result, request_elapsed_s


def safe_slug(value: str) -> str:
    allowed = []
    for char in value.lower():
        if char.isalnum():
            allowed.append(char)
        elif char in {"-", "_", "."}:
            allowed.append(char)
        else:
            allowed.append("-")
    return "".join(allowed).strip("-")


def safe_label(backend: str, channel: str | None) -> str:
    if backend == "rocm":
        return f"rocm-{channel or 'stable'}"
    return backend


def record_failure(
    args: argparse.Namespace,
    label: str,
    model: str,
    size: tuple[int, int],
    started: float,
    exc: Exception,
) -> dict[str, Any]:
    width, height = size
    return {
        "backend": args.backend,
        "channel": args.channel,
        "label": label,
        "model": model,
        "prompt": args.prompt,
        "size": f"{width}x{height}",
        "steps": args.steps,
        "seed": args.seed,
        "pass": False,
        "warmup": args.warmup,
        "timing_scope": (
            "request_wall_s_after_warmup" if args.warmup else "request_wall_s_cold"
        ),
        "error": str(exc),
        "request_elapsed_s": round(time.monotonic() - started, 3),
        # Backward-compatible alias for older PR body renderers.
        "elapsed_s": round(time.monotonic() - started, 3),
    }


def warmup_model(
    api_base_url: str,
    model: str,
    prompt: str,
    size: tuple[int, int],
    steps: int,
    seed: int,
    timeout: int,
    label: str,
) -> float:
    width, height = size
    print(f"[INFO] Warming {model} {width}x{height} on {label}")
    _image, _response_json, request_elapsed_s = generate_image(
        api_base_url=api_base_url,
        model=model,
        prompt=prompt,
        size=size,
        steps=steps,
        seed=seed,
        timeout=timeout,
    )
    print(f"[INFO] Warmed {model} on {label} in {request_elapsed_s:.3f}s")
    return request_elapsed_s


def main() -> int:
    args = parse_args()
    sizes = args.size or [parse_size(value) for value in DEFAULT_SIZES]
    models = args.model or list(DEFAULT_MODELS)
    if args.lite:
        sizes = sizes[:1]
        models = models[:1]

    base_url = f"http://{args.host}:{args.port}"
    api_base_url = f"{base_url}/api/v1"
    label = safe_label(args.backend, args.channel)
    images_dir = Path(args.images_dir)
    images_dir.mkdir(parents=True, exist_ok=True)

    results: list[dict[str, Any]] = []
    overall_pass = True
    warmup_elapsed_by_model: dict[str, float] = {}

    print(f"[INFO] Waiting for Lemonade server at {base_url}")
    wait_for_server(base_url)
    print(
        f"[INFO] Configuring sd-cpp backend={args.backend} channel={args.channel or ''}"
    )
    configure_backend(base_url, args.backend, args.channel)

    for model in models:
        if args.warmup:
            try:
                warmup_elapsed_by_model[model] = warmup_model(
                    api_base_url=api_base_url,
                    model=model,
                    prompt=args.prompt,
                    size=sizes[0],
                    steps=args.steps,
                    seed=args.seed,
                    timeout=args.timeout,
                    label=label,
                )
            except Exception as exc:  # noqa: BLE001 - keep JSON on all failures
                overall_pass = False
                print(f"[FAIL] warmup {model} on {label}: {exc}", file=sys.stderr)
                for size in sizes:
                    results.append(
                        record_failure(args, label, model, size, time.monotonic(), exc)
                    )
                continue

        for width, height in sizes:
            started = time.monotonic()
            record: dict[str, Any] = {
                "backend": args.backend,
                "channel": args.channel,
                "label": label,
                "model": model,
                "prompt": args.prompt,
                "size": f"{width}x{height}",
                "steps": args.steps,
                "seed": args.seed,
                "pass": False,
                "warmup": args.warmup,
                "warmup_elapsed_s": (
                    round(warmup_elapsed_by_model.get(model, 0.0), 3)
                    if args.warmup
                    else 0.0
                ),
                "timing_scope": (
                    "request_wall_s_after_warmup"
                    if args.warmup
                    else "request_wall_s_cold"
                ),
            }
            try:
                print(f"[INFO] Generating {model} {width}x{height} on {label}")
                image, response_json, request_elapsed_s = generate_image(
                    api_base_url=api_base_url,
                    model=model,
                    prompt=args.prompt,
                    size=(width, height),
                    steps=args.steps,
                    seed=args.seed,
                    timeout=args.timeout,
                )
                actual_width, actual_height = png_dimensions(image)
                if (actual_width, actual_height) != (width, height):
                    raise AssertionError(
                        f"PNG dimensions are {actual_width}x{actual_height}, expected {width}x{height}"
                    )
                image_path = (
                    images_dir
                    / f"sdcpp-{label}-{safe_slug(model)}-{width}x{height}.png"
                )
                image_path.write_bytes(image)
                record.update(
                    {
                        "pass": True,
                        "bytes": len(image),
                        "width": actual_width,
                        "height": actual_height,
                        "image_path": str(image_path),
                        "request_elapsed_s": round(request_elapsed_s, 3),
                        # Backward-compatible alias for older PR body renderers.
                        "elapsed_s": round(request_elapsed_s, 3),
                        "created": response_json.get("created"),
                    }
                )
                print(
                    f"[OK] {model} {width}x{height}: wrote {image_path} "
                    f"({len(image)} bytes, request {record['request_elapsed_s']}s)"
                )
            except Exception as exc:  # noqa: BLE001 - keep JSON on all failures
                overall_pass = False
                record.update(
                    {
                        "error": str(exc),
                        "request_elapsed_s": round(time.monotonic() - started, 3),
                        "elapsed_s": round(time.monotonic() - started, 3),
                    }
                )
                print(f"[FAIL] {model} {width}x{height}: {exc}", file=sys.stderr)
            results.append(record)

    output_path = Path(args.output)
    output_path.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(f"[INFO] Wrote validation summary to {output_path}")
    return 0 if overall_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
