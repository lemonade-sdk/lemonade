#!/usr/bin/env python3
"""Deterministic end-to-end tests for MCP remote model discovery.

A local HTTP fixture stands in for Hugging Face via HF_ENDPOINT, and this test
starts its own lemond instance on an ephemeral port. No external registry or
model download is required.
"""

import argparse
import json
import os
import socket
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import requests

SEARCH_RESULTS = [
    {
        "id": "acme/Qwen-Coder-GGUF",
        "downloads": 1234,
        "likes": 42,
        "tags": ["gguf", "text-generation"],
        "pipeline_tag": "text-generation",
    },
    {
        "id": "acme/Qwen-Coder-Alt-GGUF",
        "downloads": 321,
        "likes": 7,
        "tags": ["gguf"],
        "pipeline_tag": "text-generation",
    },
]

REPOSITORIES = {
    "acme/Qwen-Coder-GGUF": {
        "sha": "0123456789abcdef",
        "siblings": [
            {
                "rfilename": "Qwen-Coder-Q4_K_M-00001-of-00002.gguf",
                "size": 100,
            },
            {
                "rfilename": "Qwen-Coder-Q4_K_M-00002-of-00002.gguf",
                "size": 200,
            },
            {"rfilename": "Qwen-Coder-Q8_0.gguf", "size": 400},
            {"rfilename": "README.md", "size": 20},
        ],
    },
    "acme/No-GGUF": {
        "sha": "fedcba9876543210",
        "siblings": [{"rfilename": "README.md", "size": 20}],
    },
    "acme/Malformed": {"sha": "badbadbad"},
}


class RegistryFixtureHandler(BaseHTTPRequestHandler):
    server_version = "LemonadeRegistryFixture/1"

    def log_message(self, _format, *_args):
        return

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/models":
            params = parse_qs(parsed.query)
            query = params.get("search", [""])[0]
            if query == "rate limit":
                self._send_json(429, {"error": "mock rate limit"})
                return
            limit = int(params.get("limit", ["12"])[0])
            self.server.last_search_limit = limit
            self._send_json(200, SEARCH_RESULTS[:limit])
            return

        prefix = "/api/models/"
        if parsed.path.startswith(prefix):
            repo_id = parsed.path[len(prefix) :]
            payload = REPOSITORIES.get(repo_id)
            if payload is None:
                self._send_json(404, {"error": "repository not found"})
            else:
                self._send_json(200, payload)
            return

        self._send_json(404, {"error": "fixture path not found"})


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
    registry = ThreadingHTTPServer(("127.0.0.1", 0), RegistryFixtureHandler)
    registry.last_search_limit = None
    registry_thread = threading.Thread(target=registry.serve_forever, daemon=True)
    registry_thread.start()

    try:
        with tempfile.TemporaryDirectory(prefix="lemonade-mcp-remote-") as temp_dir:
            port = free_port()
            base_url = f"http://127.0.0.1:{port}"
            mcp_url = f"{base_url}/mcp"
            env = os.environ.copy()
            env["HF_ENDPOINT"] = f"http://127.0.0.1:{registry.server_port}"
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

                    listed = requests.post(
                        mcp_url,
                        json={"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                        timeout=10,
                    ).json()
                    names = {tool["name"] for tool in listed["result"]["tools"]}
                    assert "lemonade_search_models" in names
                    assert "lemonade_get_pull_variants" in names

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

                    offline = requests.post(
                        f"{base_url}/internal/set",
                        json={"offline": True},
                        timeout=10,
                    )
                    assert offline.status_code == 200, offline.text
                    offline_search = tool_call(
                        mcp_url,
                        "lemonade_search_models",
                        {"query": "qwen coder"},
                    )
                    assert_tool_error(offline_search, "offline")
                    assert (
                        offline_search["structuredContent"]["code"] == "lemond_offline"
                    )
                    offline_variants = tool_call(
                        mcp_url,
                        "lemonade_get_pull_variants",
                        {"checkpoint": "acme/Qwen-Coder-GGUF"},
                    )
                    assert_tool_error(offline_variants, "offline")
                    assert (
                        offline_variants["structuredContent"]["code"]
                        == "lemond_offline"
                    )
                    online = requests.post(
                        f"{base_url}/internal/set",
                        json={"offline": False},
                        timeout=10,
                    )
                    assert online.status_code == 200, online.text

                    search = tool_call(
                        mcp_url,
                        "lemonade_search_models",
                        {"query": "qwen coder", "source": "huggingface", "limit": 1},
                    )
                    assert search.get("isError") is False, search
                    assert registry.last_search_limit == 1
                    structured = search.get("structuredContent")
                    assert isinstance(structured, dict), search
                    assert structured["query"] == "qwen coder"
                    assert structured["source"] == "huggingface"
                    assert len(structured["candidates"]) == 1
                    candidate = structured["candidates"][0]
                    assert candidate["checkpoint"] == "acme/Qwen-Coder-GGUF"
                    assert candidate["downloads"] == 1234
                    assert candidate["likes"] == 42
                    assert candidate["tags"] == ["gguf", "text-generation"]
                    assert len(search["content"]) == 1
                    assert (
                        search["content"][0]["text"] == "Found 1 remote repositories."
                    )

                    assert_tool_error(
                        tool_call(mcp_url, "lemonade_get_pull_variants", {}),
                        "checkpoint",
                    )

                    variants = tool_call(
                        mcp_url,
                        "lemonade_get_pull_variants",
                        {
                            "checkpoint": "acme/Qwen-Coder-GGUF",
                            "source": "huggingface",
                        },
                    )
                    assert variants.get("isError") is False, variants
                    structured = variants.get("structuredContent")
                    assert structured["checkpoint"] == "acme/Qwen-Coder-GGUF"
                    assert structured["source"] == "huggingface"
                    assert structured["repo_kind"] == "gguf"
                    assert len(structured["variants"]) == 2
                    by_name = {item["name"]: item for item in structured["variants"]}
                    q4 = by_name["Q4_K_M"]
                    assert q4["primary_file"] == "Qwen-Coder-Q4_K_M-00001-of-00002.gguf"
                    assert q4["files"] == [
                        "Qwen-Coder-Q4_K_M-00001-of-00002.gguf",
                        "Qwen-Coder-Q4_K_M-00002-of-00002.gguf",
                    ]
                    assert q4["sharded"] is True
                    assert q4["size_bytes"] == 300
                    q8 = by_name["Q8_0"]
                    assert q8["primary_file"] == "Qwen-Coder-Q8_0.gguf"
                    assert q8["size_bytes"] == 400
                    assert "selected_variant" not in structured
                    assert variants["content"][0]["text"] == "Found 2 GGUF variants."

                    rate_limited = tool_call(
                        mcp_url,
                        "lemonade_search_models",
                        {"query": "rate limit", "source": "huggingface"},
                    )
                    assert_tool_error(rate_limited, "registry search failed")
                    assert (
                        rate_limited["structuredContent"]["upstream_status_code"] == 429
                    )

                    assert_tool_error(
                        tool_call(
                            mcp_url,
                            "lemonade_get_pull_variants",
                            {"checkpoint": "acme/Does-Not-Exist"},
                        ),
                        "not found",
                    )
                    assert_tool_error(
                        tool_call(
                            mcp_url,
                            "lemonade_get_pull_variants",
                            {"checkpoint": "acme/No-GGUF"},
                        ),
                        "no supported model files",
                    )
                    assert_tool_error(
                        tool_call(
                            mcp_url,
                            "lemonade_get_pull_variants",
                            {"checkpoint": "acme/Malformed"},
                        ),
                        "siblings",
                    )
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

    finally:
        registry.shutdown()
        registry.server_close()
        registry_thread.join(timeout=2)


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
