#!/usr/bin/env python3
"""
Validate a vLLM backend release against all "hot" vLLM models.

Usage:
    python test/validate_vllm.py --backend rocm

This script expects `lemond` to already be running on the target port.

This script:
1. Queries `/api/v1/models?show_all=true` and selects models with recipe
   `vllm` and label `hot`
2. Installs the requested vLLM backend via `POST /api/v1/install`
3. For each hot model, loads it with the requested backend, sends a
   `chat/completions` request, queries `/api/v1/stats`, and unloads it
4. Checks lemond's captured logs for proof that vLLM ran on the GPU
5. Outputs a JSON results file for CI consumption
"""

import argparse
import glob
import json
import os
import re
import shutil
import sys
import tempfile

import requests

from utils.gpu_offload import GPU_LOG_FILENAMES, read_new_log_text
from utils.server_base import _auth_headers, unload_all_models, wait_for_server
from utils.test_models import PORT, TIMEOUT_DEFAULT

TIMEOUT_HEALTH = 60
TIMEOUT_INFERENCE = 1800  # 30 minutes; first run downloads + compiles Triton kernels
# Longest gap between install events: listing and extracting the ~25 GB bundle
# sends nothing for several minutes.
TIMEOUT_INSTALL_IDLE = 900
CHAT_PROMPT = [
    {"role": "user", "content": "What is 2+2? Reply in one sentence."},
]

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
ENGINE_RE = re.compile(
    r"Initializing a V1 LLM engine \(v([^)\s]+)\) with config: .*?\bdevice_config=cuda\b"
)
# vLLM logs at INFO level, without a device name, so these together are the proof:
# a CUDA/HIP engine, the ROCm platform choosing attention, and memory on the GPU.
GPU_EVIDENCE = {
    "ROCm engine": ENGINE_RE,
    "ROCm attention backend": re.compile(
        r"\brocm\.py:\d+\]\s.*?(?:out of potential backends|\(selected via --attention-backend\))"
    ),
    "weights on GPU": re.compile(r"\bModel loading took ([\d.]+) GiB memory"),
    "KV cache on GPU": re.compile(r"\bGPU(?: \+ host)? KV cache size: ([\d,]+) tokens"),
}
CPU_RE = re.compile(r"\bdevice_config=cpu\b|\bCPU(?: \+ host)? KV cache size:")
GPU_FAILURE_HINTS = [
    re.compile(r"Failed to infer device type"),
    re.compile(r"Failed to import from (?:amdsmi|vllm\._C|vllm\._rocm_C)"),
    re.compile(r"AMDSMI_STATUS_\w+"),
    re.compile(r"needs the system C toolchain"),
]


def gpu_evidence(log_text, expected_version=None):
    """Return (status, details) for one model's slice of lemond's log."""
    text = ANSI_RE.sub("", log_text)
    missing = []
    evidence = {}
    for name, pattern in GPU_EVIDENCE.items():
        match = pattern.search(text)
        if not match:
            missing.append(name)
            continue
        if match.groups():
            evidence[name] = match.group(1)

    weights = evidence.get("weights on GPU")
    if weights is not None and float(weights) <= 0:
        missing.append("weights on GPU (0 GiB)")
    kv_tokens = evidence.get("KV cache on GPU")
    if kv_tokens is not None and int(kv_tokens.replace(",", "")) <= 0:
        missing.append("KV cache on GPU (0 tokens)")

    engine_version = evidence.get("ROCm engine")
    if (
        expected_version
        and engine_version
        and not expected_version.startswith(engine_version)
    ):
        missing.append(
            f"pinned vLLM {expected_version} (engine reported {engine_version})"
        )

    details = {"evidence": evidence}
    if CPU_RE.search(text):
        missing.append("not on the CPU platform")
    if missing:
        details["missing"] = missing
        details["hints"] = [
            m.group(0) for p in GPU_FAILURE_HINTS for m in [p.search(text)] if m
        ]
    return ("FAIL" if missing else "PASS"), details


def expected_vllm_version(pin):
    """'vllm-portable-rocm-device-all-0.32.1.dev0_gabc.d20261124.rocm103-py314-x64'
    -> '0.32.1.dev0+gabc.d20261124.rocm103' (the URL form uses '_' for '+')."""
    match = re.fullmatch(r"vllm-portable-rocm-[a-z-]+?-(\d[^-]*)-py\d+-x64", pin or "")
    return match.group(1).replace("_", "+", 1) if match else None


def collect_server_logs(output_dir):
    """Collect Lemonade log files into the output directory."""
    os.makedirs(output_dir, exist_ok=True)
    temp_dir = tempfile.gettempdir()
    patterns = ["lemonade*.log", "lemond*.log", "lemonade-server*.log"]
    copied = []
    for pattern in patterns:
        for log_file in glob.glob(os.path.join(temp_dir, pattern)):
            dest = os.path.join(output_dir, os.path.basename(log_file))
            try:
                shutil.copy2(log_file, dest)
                size = os.path.getsize(dest)
                copied.append(f"{os.path.basename(log_file)} ({size} bytes)")
            except Exception as exc:
                print(f"  Warning: Failed to copy {log_file}: {exc}", flush=True)
    if copied:
        print(f"Collected server logs: {', '.join(copied)}", flush=True)
    else:
        print("No server log files found to collect.", flush=True)
    return copied


def request_json(method, url, timeout, **kwargs):
    """Perform an HTTP request and parse the JSON response when present."""
    auth_headers = _auth_headers()
    headers = {**(kwargs.get("headers") or {}), **auth_headers}
    request_kwargs = {**kwargs, "headers": headers}
    response = requests.request(method, url, timeout=timeout, **request_kwargs)
    body = {}
    if response.content:
        try:
            body = response.json()
        except ValueError:
            body = {"raw_text": response.text}
    return response, body


def require_running_server(base_url, port):
    """Wait for a running server and confirm the health endpoint responds."""
    wait_for_server(port=port, timeout=TIMEOUT_HEALTH)
    response, body = request_json(
        "GET",
        f"{base_url}/health",
        timeout=TIMEOUT_DEFAULT,
    )
    if response.status_code != 200:
        raise RuntimeError(
            f"Server health check failed: HTTP {response.status_code} - {body}"
        )
    print(
        f"Server is reachable on port {port} (status={body.get('status', 'unknown')})",
        flush=True,
    )


def get_hot_vllm_models(base_url):
    """Return the catalog entries for hot vllm models from the API."""
    response, body = request_json(
        "GET",
        f"{base_url}/models?show_all=true",
        timeout=TIMEOUT_DEFAULT,
    )
    if response.status_code != 200:
        raise RuntimeError(
            f"Failed to query model catalog: HTTP {response.status_code} - {body}"
        )

    hot_models = []
    for model in body.get("data", []):
        labels = model.get("labels", [])
        if model.get("recipe") == "vllm" and "hot" in labels:
            hot_models.append(model)

    hot_models.sort(key=lambda model: model["id"])
    return hot_models


def install_backend(base_url, backend):
    """Install or update the requested vLLM backend through the streaming API.

    The bundle is ~10 GB, so a fixed timeout on a blocking /install would fail on a
    slow link; the timeout here applies between progress events instead.
    """
    print(f"Installing vllm backend via /install: {backend}", flush=True)
    with requests.post(
        f"{base_url}/install",
        json={"recipe": "vllm", "backend": backend, "stream": True},
        headers=_auth_headers(),
        stream=True,
        timeout=(30, TIMEOUT_INSTALL_IDLE),
    ) as response:
        if response.status_code != 200:
            raise RuntimeError(
                f"Backend install failed: HTTP {response.status_code} - {response.text[:1000]}"
            )
        # Preflight answers (setup guide, unsupported) arrive as plain JSON.
        if "text/event-stream" not in response.headers.get("Content-Type", ""):
            body = response.json()
            if body.get("action", "").endswith(".html"):
                raise RuntimeError(
                    f"Backend install blocked by preflight: {body.get('action')}"
                )
            print(f"Install response: {body}", flush=True)
            return

        event = ""
        last_decile = -1
        for raw_line in response.iter_lines(decode_unicode=True):
            line = (raw_line or "").strip()
            if line.startswith("event:"):
                event = line[len("event:") :].strip()
                continue
            if not line.startswith("data:"):
                continue
            data = line[len("data:") :].strip()
            if event == "error":
                raise RuntimeError(f"Backend install failed: {data[:1000]}")
            if event == "complete":
                print(f"Install complete: {data[:300]}", flush=True)
                return
            try:
                percent = int(json.loads(data).get("percent", 0))
            except (ValueError, TypeError, AttributeError):
                continue
            if percent // 10 > last_decile:
                last_decile = percent // 10
                print(f"  install progress: {percent}%", flush=True)
    raise RuntimeError("Backend install stream ended without a 'complete' event")


def unload_model(base_url, model_name=None):
    """Unload a specific model or all models."""
    payload = {}
    if model_name:
        payload["model_name"] = model_name
    response, body = request_json(
        "POST",
        f"{base_url}/unload",
        timeout=TIMEOUT_DEFAULT,
        json=payload,
    )
    if response.status_code not in (200, 404):
        print(
            f"  Warning: unload returned HTTP {response.status_code}: {body}",
            flush=True,
        )


def test_model(base_url, model_name, backend, max_tokens=50):
    """Send a chat/completions request and return (success, response_text, stats)."""
    print(f"  Loading model: {model_name} (backend={backend})", flush=True)
    try:
        load_resp, load_body = request_json(
            "POST",
            f"{base_url}/load",
            timeout=TIMEOUT_INFERENCE,
            json={"model_name": model_name, "vllm_backend": backend},
        )
        if load_resp.status_code != 200:
            return (
                False,
                f"Load failed: HTTP {load_resp.status_code} - {load_body}",
                {},
            )

        health_resp, health_body = request_json(
            "GET",
            f"{base_url}/health",
            timeout=TIMEOUT_DEFAULT,
        )
        if health_resp.status_code == 200:
            loaded = {
                model["model_name"]: model
                for model in health_body.get("all_models_loaded", [])
            }
            loaded_model = loaded.get(model_name, {})
            recipe_options = loaded_model.get("recipe_options", {})
            actual_backend = recipe_options.get("vllm_backend")
            if actual_backend and actual_backend != backend:
                return (
                    False,
                    f"Model loaded with backend '{actual_backend}' instead of '{backend}'",
                    {},
                )

        print("  Sending chat/completions request...", flush=True)
        chat_resp, chat_body = request_json(
            "POST",
            f"{base_url}/chat/completions",
            timeout=TIMEOUT_INFERENCE,
            json={
                "model": model_name,
                "messages": CHAT_PROMPT,
                "max_completion_tokens": max_tokens,
            },
        )
        if chat_resp.status_code != 200:
            return False, f"HTTP {chat_resp.status_code}: {chat_body}", {}

        message = chat_body["choices"][0]["message"]
        content = message.get("content") or ""
        reasoning = message.get("reasoning_content") or ""
        combined = content + reasoning
        if not combined:
            return False, "Empty response (no content or reasoning_content)", {}

        stats = {}
        stats_resp, stats_body = request_json(
            "GET",
            f"{base_url}/stats",
            timeout=TIMEOUT_DEFAULT,
        )
        if stats_resp.status_code != 200:
            return (
                False,
                f"/stats returned HTTP {stats_resp.status_code}: {stats_body}",
                {},
            )
        stats = stats_body
        print(f"  Stats: {json.dumps(stats)}", flush=True)
        output_tokens = stats.get("output_tokens")
        if not isinstance(output_tokens, int) or output_tokens <= 0:
            return False, f"/stats reported no output tokens: {stats}", stats

        response_text = content if content else f"[reasoning] {reasoning}"
        return True, response_text, stats
    except (KeyError, IndexError, TypeError, ValueError) as exc:
        return False, f"Bad response format: {exc}", {}
    except requests.RequestException as exc:
        return False, f"Request failed: {exc}", {}
    finally:
        try:
            unload_model(base_url, model_name)
        except requests.RequestException as exc:
            print(f"  Warning: failed to unload {model_name}: {exc}", flush=True)


def main():
    parser = argparse.ArgumentParser(
        description="Validate a vLLM backend against hot Lemonade models"
    )
    parser.add_argument(
        "--backend",
        required=True,
        choices=["rocm"],
        help="Backend to test (currently only rocm is supported)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=PORT,
        help=f"Server port (default: {PORT})",
    )
    parser.add_argument(
        "--output",
        default=None,
        help="Path to write JSON results file",
    )
    parser.add_argument(
        "--skip-install",
        action="store_true",
        help="Skip backend installation step",
    )
    parser.add_argument(
        "--logs-dir",
        default=None,
        help="Directory to collect server log files into (for CI artifact upload)",
    )
    parser.add_argument(
        "--lite",
        action="store_true",
        help="Lite mode: only test the smallest hot model",
    )
    parser.add_argument(
        "--expected-pin",
        default=None,
        help="vllm.rocm pin from backend_versions.json; the engine must report its version",
    )
    args = parser.parse_args()

    expected_version = expected_vllm_version(args.expected_pin)
    if args.expected_pin and not expected_version:
        print(
            f"ERROR: cannot read a vLLM version from pin '{args.expected_pin}'",
            file=sys.stderr,
            flush=True,
        )
        sys.exit(1)
    gpu_log_paths = (
        [os.path.join(args.logs_dir, name) for name in GPU_LOG_FILENAMES]
        if args.logs_dir
        else []
    )
    if not any(os.path.isfile(path) for path in gpu_log_paths):
        print(
            "ERROR: GPU use cannot be verified without lemond's logs.\n"
            "Start lemond in debug mode (set env LEMONADE_CI_MODE=1, or log_level=debug) "
            "with its stdout/stderr saved into a directory, e.g.:\n"
            f"  lemond <cache_dir> --port {args.port} "
            "1><dir>/lemond.stdout.log 2><dir>/lemond.stderr.log\n"
            "Then re-run this script with: --logs-dir <dir>",
            file=sys.stderr,
            flush=True,
        )
        sys.exit(1)
    log_offsets = {}

    base_url = f"http://localhost:{args.port}/api/v1"
    output_path = args.output or f"vllm_validation_{args.backend}.json"

    require_running_server(base_url, args.port)

    print("Unloading all models for clean state...", flush=True)
    try:
        unload_all_models(port=args.port)
    except requests.RequestException as exc:
        print(f"Warning: failed to unload pre-existing models: {exc}", flush=True)

    hot_models = get_hot_vllm_models(base_url)
    print(f"Found {len(hot_models)} hot vllm models:", flush=True)
    for model in hot_models:
        print(f"  - {model['id']} ({model.get('size', '?')} GB)", flush=True)

    if not hot_models:
        print("ERROR: No hot vllm models found!", file=sys.stderr, flush=True)
        sys.exit(1)

    if args.lite and len(hot_models) > 1:
        smallest = min(hot_models, key=lambda m: m.get("size", float("inf")))
        print(
            f"Lite mode: testing only smallest model: "
            f"{smallest['id']} ({smallest.get('size', '?')} GB)",
            flush=True,
        )
        hot_models = [smallest]

    if not args.skip_install:
        install_backend(base_url, args.backend)

    results = []
    all_passed = True
    try:
        for model in hot_models:
            model_name = model["id"]
            print(f"\nTesting: {model_name}", flush=True)
            # Only lines written during this model's test count as its GPU evidence.
            for path in gpu_log_paths:
                log_offsets[path] = os.path.getsize(path) if os.path.isfile(path) else 0
            success, response_text, stats = test_model(
                base_url, model_name, args.backend
            )
            # test_model unloads before returning, so this model's vLLM lines are flushed.
            log_chunk = ""
            for path in gpu_log_paths:
                chunk, log_offsets[path] = read_new_log_text(path, log_offsets[path])
                log_chunk += chunk
            gpu_status, gpu_details = gpu_evidence(log_chunk, expected_version)
            result = {
                "model": model_name,
                "backend": args.backend,
                "pass": success,
                "gpu": gpu_status,
                "gpu_evidence": gpu_details,
                "response": response_text,
                "input_tokens": stats.get("input_tokens", "N/A"),
                "output_tokens": stats.get("output_tokens", "N/A"),
                "time_to_first_token": stats.get("time_to_first_token", "N/A"),
                "tokens_per_second": stats.get("tokens_per_second", "N/A"),
            }
            results.append(result)
            status = "PASS" if success else "FAIL"
            print(f"  Inference: {status}  GPU: {gpu_status}", flush=True)
            print(f"  GPU evidence: {json.dumps(gpu_details)}", flush=True)
            if gpu_status == "FAIL":
                print(
                    f"  [WARN] {model_name} on {args.backend}: vLLM did not log GPU "
                    f"placement (missing: {', '.join(gpu_details['missing'])})"
                    + (
                        f"; hints: {', '.join(gpu_details['hints'])}"
                        if gpu_details["hints"]
                        else ""
                    ),
                    file=sys.stderr,
                    flush=True,
                )
            if not success:
                all_passed = False
                print(f"  Error: {response_text}", flush=True)
    finally:
        try:
            unload_all_models(port=args.port)
        except requests.RequestException as exc:
            print(f"Warning: failed to unload models during cleanup: {exc}", flush=True)
        if args.logs_dir:
            collect_server_logs(args.logs_dir)

    with open(output_path, "w", encoding="utf-8") as output_file:
        json.dump(results, output_file, indent=2)
    print(f"\nResults written to {output_path}", flush=True)

    print(f"\n{'=' * 60}", flush=True)
    passed = sum(1 for result in results if result["pass"])
    print(f"Results: {passed}/{len(results)} models passed", flush=True)
    for result in results:
        status = "PASS" if result["pass"] else "FAIL"
        print(f"  [{status}] {result['model']} (GPU: {result['gpu']})", flush=True)
    print(f"{'=' * 60}", flush=True)

    if not all_passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
