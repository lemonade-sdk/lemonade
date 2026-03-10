#!/usr/bin/env python3
"""
Validate a new llama.cpp release by testing all "hot" llamacpp models.

Usage:
    python test/validate_llamacpp.py --backend vulkan
    python test/validate_llamacpp.py --backend rocm

This script:
1. Reads server_models.json to find models with recipe "llamacpp" and label "hot"
2. Installs the llamacpp backend via `lemonade-server recipes --install llamacpp:<backend>`
3. For each hot model, sends a chat/completions request and queries /stats
4. Outputs a JSON results file for CI consumption
"""

import argparse
import json
import os
import subprocess
import sys
import time

import requests

DEFAULT_PORT = 8000
TIMEOUT_HEALTH = 60
TIMEOUT_INFERENCE = 600
SERVER_STARTUP_TIMEOUT = 120
CHAT_PROMPT = [
    {"role": "user", "content": "What is 2+2? Reply in one sentence."},
]


def find_server_binary():
    """Find lemonade-server binary, checking common locations."""
    candidates = [
        os.environ.get("LEMONADE_SERVER_BINARY", ""),
        "lemonade-server",
        "lemonade-server.exe",
        os.path.join("src", "cpp", "build", "Release", "lemonade-server.exe"),
        os.path.join("build", "Release", "lemonade-server.exe"),
    ]
    for c in candidates:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    # Fallback: assume it's on PATH
    return "lemonade-server"


def find_models_json():
    """Find server_models.json in the source tree or installed resources."""
    candidates = [
        os.path.join("src", "cpp", "resources", "server_models.json"),
        os.path.join("resources", "server_models.json"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    raise FileNotFoundError("Cannot find server_models.json")


def get_hot_llamacpp_models(models_json_path):
    """Return list of (model_name, model_config) for hot llamacpp models."""
    with open(models_json_path, "r", encoding="utf-8") as f:
        models = json.load(f)

    hot_models = []
    for name, config in models.items():
        if config.get("recipe") == "llamacpp" and "hot" in config.get("labels", []):
            hot_models.append((name, config))
    return hot_models


def wait_for_server(base_url, timeout=SERVER_STARTUP_TIMEOUT):
    """Wait for the server health endpoint to respond."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            resp = requests.get(f"{base_url}/v1/health", timeout=5)
            if resp.status_code == 200:
                return True
        except requests.ConnectionError:
            pass
        time.sleep(2)
    return False


def install_backend(server_binary, backend):
    """Run lemonade-server recipes --install llamacpp:<backend>."""
    cmd = [server_binary, "recipes", "--install", f"llamacpp:{backend}"]
    print(f"Running: {' '.join(cmd)}", flush=True)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    print(result.stdout, flush=True)
    if result.returncode != 0:
        print(f"STDERR: {result.stderr}", file=sys.stderr, flush=True)
        raise RuntimeError(f"Backend install failed with exit code {result.returncode}")


def start_server(server_binary, port):
    """Start lemonade-server serve in the background."""
    cmd = [server_binary, "serve", "--port", str(port), "--log-level", "debug"]
    print(f"Starting server: {' '.join(cmd)}", flush=True)
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return proc


def stop_server(server_binary):
    """Stop the lemonade server."""
    try:
        subprocess.run(
            [server_binary, "stop"],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except Exception:
        pass


def test_model(base_url, model_name, max_tokens=50):
    """Send a chat/completions request and return (success, response_text, stats)."""
    # Load the model first
    print(f"  Loading model: {model_name}", flush=True)
    try:
        load_resp = requests.post(
            f"{base_url}/v1/load",
            json={"model": model_name},
            timeout=TIMEOUT_INFERENCE,
        )
        if load_resp.status_code != 200:
            return (
                False,
                f"Load failed: HTTP {load_resp.status_code} - {load_resp.text}",
                {},
            )
    except Exception as e:
        return False, f"Load failed: {e}", {}

    # Send chat/completions request
    print(f"  Sending chat/completions request...", flush=True)
    try:
        chat_resp = requests.post(
            f"{base_url}/v1/chat/completions",
            json={
                "model": model_name,
                "messages": CHAT_PROMPT,
                "max_completion_tokens": max_tokens,
            },
            timeout=TIMEOUT_INFERENCE,
        )
    except Exception as e:
        return False, f"Chat request failed: {e}", {}

    if chat_resp.status_code != 200:
        return False, f"HTTP {chat_resp.status_code}: {chat_resp.text}", {}

    try:
        chat_data = chat_resp.json()
        content = chat_data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, json.JSONDecodeError) as e:
        return False, f"Bad response format: {e} - {chat_resp.text}", {}

    if not content or len(content) == 0:
        return False, "Empty response content", {}

    # Get stats
    stats = {}
    try:
        stats_resp = requests.get(f"{base_url}/v1/stats", timeout=30)
        if stats_resp.status_code == 200:
            stats = stats_resp.json()
            print(f"  Stats: {json.dumps(stats)}", flush=True)
    except Exception as e:
        print(f"  Warning: Failed to get stats: {e}", flush=True)

    # Unload the model to free memory for the next one
    try:
        requests.post(
            f"{base_url}/v1/unload",
            json={"model": model_name},
            timeout=60,
        )
    except Exception:
        pass

    return True, content, stats


def main():
    parser = argparse.ArgumentParser(description="Validate llama.cpp release")
    parser.add_argument(
        "--backend",
        required=True,
        choices=["vulkan", "rocm", "cpu", "metal"],
        help="Backend to test (vulkan, rocm, cpu, metal)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"Server port (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--server-binary",
        default=None,
        help="Path to lemonade-server binary",
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
        "--skip-server-start",
        action="store_true",
        help="Skip starting the server (assume already running)",
    )
    args = parser.parse_args()

    server_binary = args.server_binary or find_server_binary()
    base_url = f"http://localhost:{args.port}"
    output_path = args.output or f"llamacpp_validation_{args.backend}.json"

    # Find hot models
    models_json = find_models_json()
    hot_models = get_hot_llamacpp_models(models_json)
    print(f"Found {len(hot_models)} hot llamacpp models:", flush=True)
    for name, config in hot_models:
        print(f"  - {name} ({config.get('size', '?')} GB)", flush=True)

    if not hot_models:
        print("ERROR: No hot llamacpp models found!", file=sys.stderr)
        sys.exit(1)

    # Install backend
    if not args.skip_install:
        install_backend(server_binary, args.backend)

    # Start server
    server_proc = None
    if not args.skip_server_start:
        server_proc = start_server(server_binary, args.port)
        if not wait_for_server(base_url):
            print("ERROR: Server failed to start!", file=sys.stderr)
            if server_proc:
                server_proc.kill()
            sys.exit(1)
        print("Server is ready.", flush=True)

    # Test each model
    results = []
    all_passed = True
    try:
        for model_name, model_config in hot_models:
            print(f"\nTesting: {model_name}", flush=True)
            success, response_text, stats = test_model(base_url, model_name)
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
        # Stop server
        if server_proc:
            stop_server(server_binary)
            server_proc.terminate()
            try:
                server_proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                server_proc.kill()

    # Write results
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults written to {output_path}", flush=True)

    # Summary
    print(f"\n{'='*60}", flush=True)
    passed = sum(1 for r in results if r["pass"])
    print(f"Results: {passed}/{len(results)} models passed", flush=True)
    for r in results:
        status = "PASS" if r["pass"] else "FAIL"
        print(f"  [{status}] {r['model']}", flush=True)
    print(f"{'='*60}", flush=True)

    if not all_passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
