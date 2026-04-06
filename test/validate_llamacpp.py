#!/usr/bin/env python3
"""
Validate a llama.cpp backend release against all "hot" llamacpp models.

Usage:
    python test/validate_llamacpp.py --backend vulkan
    python test/validate_llamacpp.py --backend rocm

This script expects `lemond` to already be running on the target port.

This script:
1. Queries `/api/v1/models?show_all=true` and selects models with recipe
   `llamacpp` and label `hot`
2. Installs the requested llamacpp backend via `POST /api/v1/install`
3. For each hot model, loads it with the requested backend, sends a
   `chat/completions` request, queries `/api/v1/stats`, and unloads it
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
TIMEOUT_INFERENCE = 1800  # 30 minutes; large models may need 60+ GB download
CHAT_PROMPT = [
    {"role": "user", "content": "What is 2+2? Reply in one sentence."},
]


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
    response = requests.request(method, url, timeout=timeout, **kwargs)
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


def get_hot_llamacpp_models(base_url):
    """Return the catalog entries for hot llamacpp models from the API."""
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
        if model.get("recipe") == "llamacpp" and "hot" in labels:
            hot_models.append(model)

    hot_models.sort(key=lambda model: model["id"])
    return hot_models


def install_backend(base_url, backend):
    """Install or update the requested llama.cpp backend through the API."""
    print(f"Installing llamacpp backend via /install: {backend}", flush=True)
    response, body = request_json(
        "POST",
        f"{base_url}/install",
        timeout=TIMEOUT_INFERENCE,
        json={"recipe": "llamacpp", "backend": backend, "stream": False},
    )
    if response.status_code != 200:
        raise RuntimeError(
            f"Backend install failed: HTTP {response.status_code} - {body}"
        )
    print(f"Install response: {body}", flush=True)


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
            json={"model_name": model_name, "llamacpp_backend": backend},
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
            actual_backend = recipe_options.get("llamacpp_backend")
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
        if stats_resp.status_code == 200:
            stats = stats_body
            print(f"  Stats: {json.dumps(stats)}", flush=True)
        else:
            print(
                f"  Warning: stats returned HTTP {stats_resp.status_code}: {stats_body}",
                flush=True,
            )

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
        description="Validate a llama.cpp backend against hot Lemonade models"
    )
    parser.add_argument(
        "--backend",
        required=True,
        choices=["vulkan", "rocm", "cpu", "metal"],
        help="Backend to test (vulkan, rocm, cpu, metal)",
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
    args = parser.parse_args()

    base_url = f"http://localhost:{args.port}/api/v1"
    output_path = args.output or f"llamacpp_validation_{args.backend}.json"

    require_running_server(base_url, args.port)

    print("Unloading all models for clean state...", flush=True)
    try:
        unload_all_models(port=args.port)
    except requests.RequestException as exc:
        print(f"Warning: failed to unload pre-existing models: {exc}", flush=True)

    hot_models = get_hot_llamacpp_models(base_url)
    print(f"Found {len(hot_models)} hot llamacpp models:", flush=True)
    for model in hot_models:
        print(f"  - {model['id']} ({model.get('size', '?')} GB)", flush=True)

    if not hot_models:
        print("ERROR: No hot llamacpp models found!", file=sys.stderr, flush=True)
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
            success, response_text, stats = test_model(
                base_url, model_name, args.backend
            )
            result = {
                "model": model_name,
                "pass": success,
                "response": response_text,
                "input_tokens": stats.get("input_tokens", "N/A"),
                "output_tokens": stats.get("output_tokens", "N/A"),
                "time_to_first_token": stats.get("time_to_first_token", "N/A"),
                "tokens_per_second": stats.get("tokens_per_second", "N/A"),
            }
            results.append(result)
            status = "PASS" if success else "FAIL"
            print(f"  Result: {status}", flush=True)
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
        print(f"  [{status}] {result['model']}", flush=True)
    print(f"{'=' * 60}", flush=True)

    if not all_passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
