#!/usr/bin/env python3
"""
Validate a vLLM backend release against test models.

Usage:
    python test/validate_vllm.py --backend rocm

This script expects `lemond` to already be running on the target port.

This script:
1. Installs the vLLM backend via `POST /api/v1/install`
2. Loads a small test model (OPT-125M-vllm by default)
3. Sends a chat/completions request and verifies a non-empty response
4. Outputs a JSON results file for CI consumption
"""

import argparse
import glob
import json
import os
import shutil
import sys
import tempfile

import requests

from utils.server_base import unload_all_models, wait_for_server
from utils.test_models import PORT, TIMEOUT_DEFAULT

TIMEOUT_HEALTH = 60
TIMEOUT_INFERENCE = 1800  # 30 minutes; first run downloads model + compiles kernels
CHAT_PROMPT = [
    {"role": "user", "content": "What is 2+2? Reply in one sentence."},
]
DEFAULT_MODEL = "OPT-125M-vllm"


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
                copied.append(dest)
            except OSError:
                pass
    return copied


def request_json(method, url, timeout=TIMEOUT_DEFAULT, **kwargs):
    """Send a request and return (response, body_dict)."""
    resp = requests.request(method, url, timeout=timeout, **kwargs)
    try:
        body = resp.json()
    except Exception:
        body = {"raw": resp.text}
    return resp, body


def install_backend(base_url, backend):
    """Install the vLLM backend through the API."""
    print(f"Installing vllm backend via /install: {backend}", flush=True)
    response, body = request_json(
        "POST",
        f"{base_url}/install",
        timeout=TIMEOUT_INFERENCE,
        json={"recipe": "vllm", "backend": backend, "stream": False},
    )
    if response.status_code != 200:
        raise RuntimeError(
            f"Backend install failed: HTTP {response.status_code} - {body}"
        )
    print(f"Install response: {body}", flush=True)


def test_model(base_url, model_name, backend, max_tokens=50):
    """Load a model, send chat request, return (success, text, stats)."""
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
        if stats_resp.status_code == 200:
            stats = stats_body
            print(f"  Stats: {json.dumps(stats)}", flush=True)

        return True, combined, stats
    except Exception as exc:
        return False, str(exc), {}
    finally:
        # Unload
        try:
            request_json(
                "POST",
                f"{base_url}/unload",
                timeout=TIMEOUT_DEFAULT,
                json={"model_name": model_name},
            )
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser(description="Validate vLLM backend")
    parser.add_argument(
        "--backend", required=True, choices=["rocm"],
        help="vLLM backend to validate"
    )
    parser.add_argument(
        "--model", default=DEFAULT_MODEL,
        help="Model to test (default: %(default)s)"
    )
    parser.add_argument(
        "--port", type=int, default=PORT,
        help="Lemonade server port"
    )
    parser.add_argument(
        "--output-dir", default="validate_vllm_output",
        help="Directory for results"
    )
    args = parser.parse_args()

    base_url = f"http://localhost:{args.port}/api/v1"

    # Wait for server
    print(f"Waiting for lemond on port {args.port}...", flush=True)
    wait_for_server(timeout=TIMEOUT_HEALTH, port=args.port)
    print("Server is ready.", flush=True)

    # Install backend
    install_backend(base_url, args.backend)

    # Test model
    print(f"\nTesting model: {args.model}", flush=True)
    success, text, stats = test_model(base_url, args.model, args.backend)

    result = {
        "model": args.model,
        "backend": args.backend,
        "success": success,
        "response": text[:500] if success else text,
        "stats": stats,
    }

    print(f"\n{'PASS' if success else 'FAIL'}: {args.model}", flush=True)
    if success:
        print(f"  Response: {text[:200]}", flush=True)
    else:
        print(f"  Error: {text}", flush=True)

    # Save results
    os.makedirs(args.output_dir, exist_ok=True)
    results_file = os.path.join(args.output_dir, "results.json")
    with open(results_file, "w") as f:
        json.dump(result, f, indent=2)
    print(f"\nResults saved to: {results_file}", flush=True)

    # Collect logs
    collect_server_logs(args.output_dir)

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
