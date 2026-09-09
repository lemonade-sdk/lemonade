#!/usr/bin/env python3
"""Deterministic MCP contract tests for remote model discovery.

This test starts an isolated lemond instance and exercises the MCP discovery
descriptors, validation, and offline handling without making remote registry
requests. While registry transport and response normalization are covered separately
by the C++ model-registry tests.
"""

import argparse
import os
import socket
import subprocess
import tempfile
import time
from pathlib import Path

import requests


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_ready(base_url, process, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"lemond exited early with code {process.returncode}")
        try:
            response = requests.get(f"{base_url}/api/v1/health", timeout=1)
            if response.status_code == 200:
                return
        except requests.RequestException:
            pass
        time.sleep(0.1)
    raise RuntimeError("lemond did not become ready")


def tool_call(mcp_url, name, arguments):
    response = requests.post(
        mcp_url,
        json={
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        },
        timeout=10,
    )
    response.raise_for_status()
    body = response.json()
    assert "error" not in body, body
    assert "result" in body, body
    return body["result"]


def assert_tool_error(result, contains=None):
    assert result.get("isError") is True, result
    content = result.get("content") or []
    assert content and content[0].get("type") == "text", result
    if contains:
        assert contains.lower() in content[0].get("text", "").lower(), result


def run_tests(lemond):
    with tempfile.TemporaryDirectory(prefix="lemonade-mcp-remote-") as temp_dir:
        port = free_port()
        dead_registry_port = free_port()
        base_url = f"http://127.0.0.1:{port}"
        mcp_url = f"{base_url}/mcp"
        env = os.environ.copy()
        # Keep any startup-time registry checks on loopback while preserving the
        # production HTTPS-only transport policy. No registry server is started.
        dead_registry = f"https://127.0.0.1:{dead_registry_port}"
        env["HF_ENDPOINT"] = dead_registry
        env["MODELSCOPE_ENDPOINT"] = dead_registry
        env.pop("LEMONADE_API_KEY", None)
        env.pop("LEMONADE_ADMIN_API_KEY", None)
        log_path = Path(temp_dir) / "lemond.log"
        cache_dir = Path(temp_dir) / "cache"
        cache_dir.mkdir()

        with log_path.open("w", encoding="utf-8") as log_file:
            process = subprocess.Popen(
                [
                    str(lemond),
                    str(cache_dir),
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(port),
                ],
                env=env,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                text=True,
            )

            try:
                wait_ready(base_url, process)

                # Put the isolated server in offline mode before exercising the
                # discovery tools. Validation happens before the offline guard,
                # so both paths remain covered without a registry transport mock.
                offline = requests.post(
                    f"{base_url}/internal/set",
                    json={"offline": True},
                    timeout=10,
                )
                assert offline.status_code == 200, offline.text

                listed = requests.post(
                    mcp_url,
                    json={"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                    timeout=10,
                ).json()
                tools = {tool["name"]: tool for tool in listed["result"]["tools"]}
                assert "lemonade_search_models" in tools
                assert "lemonade_get_pull_variants" in tools

                search_schema = tools["lemonade_search_models"]["inputSchema"]
                assert search_schema["type"] == "object"
                assert search_schema["required"] == ["query"]
                search_props = search_schema["properties"]
                assert search_props["query"]["minLength"] == 3
                assert search_props["source"]["enum"] == [
                    "huggingface",
                    "modelscope",
                ]
                assert search_props["source"]["default"] == "huggingface"
                assert search_props["limit"]["minimum"] == 1
                assert search_props["limit"]["maximum"] == 50
                assert search_props["limit"]["default"] == 12

                variants_schema = tools["lemonade_get_pull_variants"]["inputSchema"]
                assert variants_schema["type"] == "object"
                assert variants_schema["required"] == ["checkpoint"]
                variants_props = variants_schema["properties"]
                assert variants_props["checkpoint"]["minLength"] == 1
                assert variants_props["source"]["enum"] == [
                    "huggingface",
                    "modelscope",
                ]
                assert variants_props["source"]["default"] == "huggingface"

                assert_tool_error(
                    tool_call(mcp_url, "lemonade_search_models", {}), "query"
                )
                assert_tool_error(
                    tool_call(mcp_url, "lemonade_search_models", {"query": "  q "}),
                    "at least 3",
                )
                assert_tool_error(
                    tool_call(
                        mcp_url,
                        "lemonade_search_models",
                        {"query": "qwen coder", "source": "other"},
                    ),
                    "source",
                )
                assert_tool_error(
                    tool_call(
                        mcp_url,
                        "lemonade_search_models",
                        {"query": "qwen coder", "limit": 0},
                    ),
                    "limit",
                )
                assert_tool_error(
                    tool_call(mcp_url, "lemonade_get_pull_variants", {}),
                    "checkpoint",
                )
                assert_tool_error(
                    tool_call(
                        mcp_url,
                        "lemonade_get_pull_variants",
                        {
                            "checkpoint": "acme/Qwen-Coder-GGUF",
                            "source": "other",
                        },
                    ),
                    "source",
                )

                offline_search = tool_call(
                    mcp_url,
                    "lemonade_search_models",
                    {"query": "qwen coder"},
                )
                assert_tool_error(offline_search, "offline")
                assert offline_search["structuredContent"]["code"] == "lemond_offline"

                offline_variants = tool_call(
                    mcp_url,
                    "lemonade_get_pull_variants",
                    {"checkpoint": "acme/Qwen-Coder-GGUF"},
                )
                assert_tool_error(offline_variants, "offline")
                assert offline_variants["structuredContent"]["code"] == "lemond_offline"
            except Exception:
                log_file.flush()
                print("\n--- lemond log ---", file=os.sys.stderr)
                print(log_path.read_text(encoding="utf-8"), file=os.sys.stderr)
                raise
            finally:
                try:
                    requests.post(f"{base_url}/internal/shutdown", timeout=2)
                except requests.RequestException:
                    pass
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lemond", required=True, type=Path)
    args = parser.parse_args()

    lemond = args.lemond.resolve()
    if not lemond.is_file():
        raise SystemExit(f"lemond binary not found: {lemond}")

    run_tests(lemond)
    print("MCP remote discovery tests passed.")


if __name__ == "__main__":
    main()
