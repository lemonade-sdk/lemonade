"""
Self-contained tests for Lemonade model-router endpoints.

Unlike most server tests, this file starts its own temporary lemond so it can
control the cache directory and routers.json without touching a user's running
server. It does not load or download inference models.

Usage:
    python test/router_endpoints.py --lemond-binary build/lemond
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
TIMEOUT = 10

LOCAL_MODEL = "Qwen3.5-35B-A3B-GGUF"
REMOTE_MODEL = "fireworks.kimi-k2p6"
HEURISTIC_ROUTER = "router.test.heuristic-qwen35-fireworks"
AGENTIC_ROUTER = "router.test.agentic-qwen35-fireworks"


def _free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _wait_for_health(base_url, proc, log_path):
    deadline = time.time() + 30
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                f"lemond exited with {proc.returncode}. Log:\n{log_path.read_text(errors='replace')}"
            )
        try:
            status, _headers, _body = _http_get(f"{base_url}/health", timeout=1)
            if status == 200:
                return
        except Exception:
            pass
        time.sleep(0.25)
    raise RuntimeError(f"Timed out waiting for lemond. Log:\n{log_path.read_text(errors='replace')}")


def _http_get(url, timeout=TIMEOUT):
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, dict(response.headers), response.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode("utf-8")


def _http_post_json(url, payload, timeout=TIMEOUT):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, dict(response.headers), response.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode("utf-8")


def _heuristic_config():
    return {
        "version": 1,
        "routers": [
            {
                "id": HEURISTIC_ROUTER,
                "type": "heuristic",
                "description": "Test heuristic router",
                "endpoints": ["chat.completions", "completions", "responses"],
                "default_model": LOCAL_MODEL,
                "recommended_max_loaded_models": 1,
                "candidates": [
                    {"model": LOCAL_MODEL, "description": "local"},
                    {"model": REMOTE_MODEL, "description": "remote"},
                ],
                "rules": [
                    {
                        "id": "coding",
                        "match": {"regex": "\\b(debug|stack trace|python|cmake|compile)\\b"},
                        "route_to": REMOTE_MODEL,
                    },
                    {
                        "id": "long",
                        "match": {"min_chars": 2000},
                        "route_to": REMOTE_MODEL,
                    },
                ],
            }
        ],
    }


def _agentic_config():
    return {
        "version": 1,
        "routers": [
            {
                "id": AGENTIC_ROUTER,
                "type": "agentic",
                "description": "Test agentic router",
                "endpoints": ["chat.completions", "completions", "responses"],
                "router_model": LOCAL_MODEL,
                "default_model": LOCAL_MODEL,
                "recommended_max_loaded_models": 1,
                "max_decision_tokens": 128,
                "temperature": 0,
                "on_failure": "default",
                "candidates": [
                    {"model": LOCAL_MODEL, "description": "local"},
                    {"model": REMOTE_MODEL, "description": "remote"},
                ],
                "system_prompt": "Return only JSON with model and reason.",
            }
        ],
    }


class RouterEndpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lemond_binary = Path(
            os.environ.get("LEMONADE_TEST_LEMOND_BINARY", str(REPO_ROOT / "build" / "lemond"))
        )
        if not cls.lemond_binary.exists():
            raise RuntimeError(f"lemond binary not found: {cls.lemond_binary}")

        cls.temp_dir = Path(tempfile.mkdtemp(prefix="lemonade-router-test-"))
        cls.cache_dir = cls.temp_dir / "cache"
        cls.runtime_dir = cls.temp_dir / "runtime"
        cls.cache_dir.mkdir()
        cls.runtime_dir.mkdir()
        os.chmod(cls.runtime_dir, 0o700)
        cls.routers_path = cls.cache_dir / "routers.json"
        cls.log_path = cls.cache_dir / "lemond.log"
        cls.port = _free_port()
        cls.base_url = f"http://127.0.0.1:{cls.port}/v1"

        cls.write_routers(_heuristic_config())

        env = os.environ.copy()
        env["XDG_RUNTIME_DIR"] = str(cls.runtime_dir)
        cls.log_file = cls.log_path.open("w")
        cls.proc = subprocess.Popen(
            [
                str(cls.lemond_binary),
                str(cls.cache_dir),
                "--host",
                "127.0.0.1",
                "--port",
                str(cls.port),
            ],
            cwd=str(REPO_ROOT),
            env=env,
            stdout=cls.log_file,
            stderr=subprocess.STDOUT,
        )
        _wait_for_health(cls.base_url, cls.proc, cls.log_path)

    @classmethod
    def tearDownClass(cls):
        proc = getattr(cls, "proc", None)
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
        log_file = getattr(cls, "log_file", None)
        if log_file:
            log_file.close()
        temp_dir = getattr(cls, "temp_dir", None)
        if temp_dir:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @classmethod
    def write_routers(cls, payload):
        cls.routers_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        # Ensure mtime changes enough for hot reload checks on coarse filesystems.
        time.sleep(0.05)

    def _models(self):
        status, _headers, body = _http_get(f"{self.base_url}/models")
        self.assertEqual(status, 200, body)
        return json.loads(body)["data"]

    def _router_by_id(self, router_id):
        for model in self._models():
            if model["id"] == router_id:
                return model
        self.fail(f"Router alias missing from /models: {router_id}")

    def test_001_router_alias_appears_in_models(self):
        model = self._router_by_id(HEURISTIC_ROUTER)
        self.assertEqual(model["recipe"], "router")
        self.assertEqual(model["router"]["type"], "heuristic")
        self.assertEqual(model["router"]["recommended_max_loaded_models"], 1)
        self.assertEqual(model["router"]["default_model"], LOCAL_MODEL)

    def test_002_router_model_by_id(self):
        status, _headers, body = _http_get(f"{self.base_url}/models/{HEURISTIC_ROUTER}")
        self.assertEqual(status, 200, body)
        model = json.loads(body)
        self.assertEqual(model["id"], HEURISTIC_ROUTER)
        self.assertEqual(model["recipe"], "router")

    def test_003_heuristic_evaluate_routes_by_rule_and_default(self):
        status, _headers, body = _http_post_json(
            f"{self.base_url}/router/evaluate",
            {
                "router": HEURISTIC_ROUTER,
                "endpoint": "chat.completions",
                "request": {
                    "messages": [
                        {"role": "user", "content": "Please debug this Python stack trace."}
                    ]
                },
            },
        )
        self.assertEqual(status, 200, body)
        coding = json.loads(body)
        self.assertEqual(coding["decision"]["selected_model"], REMOTE_MODEL)
        self.assertEqual(coding["decision"]["rule"], "coding")

        status, _headers, body = _http_post_json(
            f"{self.base_url}/router/evaluate",
            {
                "router": HEURISTIC_ROUTER,
                "endpoint": "chat.completions",
                "request": {"messages": [{"role": "user", "content": "Say hello."}]},
            },
        )
        self.assertEqual(status, 200, body)
        simple = json.loads(body)
        self.assertEqual(simple["decision"]["selected_model"], LOCAL_MODEL)
        self.assertEqual(simple["decision"]["reason"], "default_model")

    def test_004_concrete_model_is_not_a_router(self):
        status, _headers, body = _http_post_json(
            f"{self.base_url}/router/evaluate",
            {
                "endpoint": "chat.completions",
                "request": {
                    "model": LOCAL_MODEL,
                    "messages": [{"role": "user", "content": "hello"}],
                },
            },
        )
        self.assertEqual(status, 400, body)
        self.assertEqual(json.loads(body)["error"]["code"], "not_a_router")

    def test_005_invalid_config_does_not_crash_server(self):
        self.write_routers({"version": 1, "routers": [{"id": "bad", "type": "unknown"}]})
        status, _headers, body = _http_get(f"{self.base_url}/models")
        self.assertEqual(status, 200, body)
        ids = [m["id"] for m in json.loads(body)["data"]]
        self.assertNotIn("bad", ids)

        self.write_routers(_heuristic_config())
        self.assertEqual(self._router_by_id(HEURISTIC_ROUTER)["recipe"], "router")

    def test_006_agentic_router_metadata(self):
        self.write_routers(_agentic_config())
        model = self._router_by_id(AGENTIC_ROUTER)
        self.assertEqual(model["router"]["type"], "agentic")
        self.assertEqual(model["router"]["router_model"], LOCAL_MODEL)
        self.assertEqual(model["router"]["recommended_max_loaded_models"], 1)
        self.assertEqual(model["router"]["default_model"], LOCAL_MODEL)

    def test_007_routing_stats_and_metrics_exist(self):
        status, _headers, body = _http_get(f"{self.base_url}/stats")
        self.assertEqual(status, 200, body)
        stats = json.loads(body)
        self.assertIn("routing", stats)
        self.assertEqual(stats["routing"]["decisions_total"], 0)
        self.assertEqual(stats["routing"]["fallbacks_total"], 0)
        self.assertIn("by_router", stats["routing"])
        self.assertIn("by_selected_model", stats["routing"])
        self.assertIn("by_rule", stats["routing"])

        status, _headers, body = _http_get(f"http://127.0.0.1:{self.port}/metrics")
        self.assertEqual(status, 200, body)
        self.assertIn("lemonade_router_decisions_total", body)
        self.assertIn("lemonade_router_fallbacks_total", body)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--lemond-binary", default=str(REPO_ROOT / "build" / "lemond"))
    args, remaining = parser.parse_known_args()
    os.environ["LEMONADE_TEST_LEMOND_BINARY"] = args.lemond_binary
    unittest.main(argv=[sys.argv[0]] + remaining, verbosity=2)
