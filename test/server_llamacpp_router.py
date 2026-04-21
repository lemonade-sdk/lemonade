"""
Tests for the Llama.cpp Router Mode.

Each test needs a fresh server with different CLI args / config, so this
file manages its own server lifecycle independently of ServerTestBase
(mirrors ``test_llamacpp_system_backend.py``).

The tests run end-to-end against a fake ``llama-server`` binary that we
drop onto ``PATH`` and select via the ``system`` llamacpp backend. The
fake implements the minimal surface area Lemonade cares about:

- ``GET  /health``                 -> 200 {"status": "ok"}
- ``GET  /v1/models``              -> 200 {"data": [{"id": ...}]}
- ``POST /v1/chat/completions``    -> 200 echo-style response
- ``POST /v1/completions``         -> 200 echo-style response
- ``POST /v1/embeddings``          -> 200 2-dim embedding stub

The fake's roster is derived from the ``--models-preset`` .ini file or
the ``--models-dir`` directory it receives, so we can assert that
Lemonade is wiring the router-mode CLI args through to the child.

Usage:
    python test/server_llamacpp_router.py
    python test/server_llamacpp_router.py --server-binary /path/to/lemonade-server
"""

import json
import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest

import requests

from utils.server_base import (
    get_server_binary,
    parse_args,
    wait_for_server,
    PORT,
)
from utils.test_models import (
    TIMEOUT_DEFAULT,
    TIMEOUT_MODEL_OPERATION,
)

args = parse_args()


# ---------------------------------------------------------------------------
# Mock llama-server binary that speaks just enough OpenAI surface area for
# Lemonade's router-mode wrapper to drive it.
# ---------------------------------------------------------------------------
MOCK_LLAMA_SERVER_PYTHON = r"""#!/usr/bin/env python3
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def get_arg(flag, default):
    if flag in sys.argv:
        idx = sys.argv.index(flag)
        if idx + 1 < len(sys.argv):
            return sys.argv[idx + 1]
    return default


def load_roster():
    models = []
    preset = get_arg("--models-preset", "")
    models_dir = get_arg("--models-dir", "")
    if preset and os.path.exists(preset):
        with open(preset, "r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line.startswith("[") and line.endswith("]"):
                    section = line[1:-1].strip()
                    if section and section.lower() != "defaults":
                        models.append(section)
    if not models and models_dir and os.path.isdir(models_dir):
        for name in sorted(os.listdir(models_dir)):
            if name.lower().endswith(".gguf"):
                models.append(name[:-5])
    return models


ROSTER = load_roster()
PORT = int(get_arg("--port", "13305"))
CAPTURE_PATH = os.environ.get("MOCK_LLAMA_REQUEST_PATH", "")


class ReusableHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True


class Handler(BaseHTTPRequestHandler):
    def _send_json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._send_json({"status": "ok"})
            return
        if self.path == "/v1/models":
            self._send_json({
                "object": "list",
                "data": [{"id": m, "object": "model"} for m in ROSTER],
            })
            return
        self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode("utf-8") if length else ""
        if CAPTURE_PATH:
            with open(CAPTURE_PATH, "w", encoding="utf-8") as handle:
                handle.write(raw)
        try:
            body = json.loads(raw) if raw else {}
        except Exception:
            body = {}
        model = body.get("model", "unknown")

        if self.path == "/v1/chat/completions":
            if body.get("stream"):
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                chunks = [
                    {"id": "c", "object": "chat.completion.chunk",
                     "choices": [{"index": 0, "delta": {"role": "assistant"},
                                  "finish_reason": None}]},
                    {"id": "c", "object": "chat.completion.chunk",
                     "choices": [{"index": 0, "delta": {"content": "hello " + model},
                                  "finish_reason": None}]},
                    {"id": "c", "object": "chat.completion.chunk",
                     "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
                ]
                for chunk in chunks:
                    self.wfile.write(
                        ("data: " + json.dumps(chunk) + "\n\n").encode("utf-8"))
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
                return
            self._send_json({
                "id": "chatcmpl-router",
                "object": "chat.completion",
                "model": model,
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant",
                                "content": "hello " + model},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 1, "completion_tokens": 2,
                          "total_tokens": 3},
            })
            return

        if self.path == "/v1/completions":
            self._send_json({
                "id": "cmpl-router",
                "object": "text_completion",
                "model": model,
                "choices": [{"index": 0, "text": "ok", "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1,
                          "total_tokens": 2},
            })
            return

        if self.path == "/v1/embeddings":
            self._send_json({
                "object": "list",
                "data": [{"object": "embedding", "embedding": [0.1, 0.2],
                          "index": 0}],
                "model": model,
                "usage": {"prompt_tokens": 1, "total_tokens": 1},
            })
            return

        self.send_error(404)

    def log_message(self, fmt, *argv):
        return


ReusableHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
"""


# ---------------------------------------------------------------------------
# Server lifecycle helpers (same pattern as test_llamacpp_system_backend.py)
# ---------------------------------------------------------------------------
def _is_server_running(port=PORT):
    try:
        conn = socket.create_connection(("localhost", port), timeout=2)
        conn.close()
        return True
    except (socket.error, socket.timeout):
        return False


def _wait_for_server_stop(port=PORT, timeout=30):
    start = time.time()
    while time.time() - start < timeout:
        if not _is_server_running(port):
            return True
        time.sleep(1)
    return False


def _stop_server():
    server_binary = get_server_binary()
    try:
        subprocess.run(
            [server_binary, "stop"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        _wait_for_server_stop()
    except Exception as exc:
        print(f"Warning: failed to stop server: {exc}")


def _start_server(extra_args=None, env_overrides=None, cache_dir=None, timeout=60):
    """Start lemond serve with the given extra args and env overrides.

    When ``cache_dir`` is supplied it is appended as the trailing
    positional argument so lemond creates a fresh ``config.json`` in that
    directory. This is important for router-mode tests because
    ``LEMONADE_*`` environment variables are only migrated into
    ``config.json`` when the file does not yet exist.
    """
    server_binary = get_server_binary()
    cmd = [server_binary, "serve", "--log-level", "debug"]
    if os.name == "nt" or os.getenv("LEMONADE_CI_MODE"):
        cmd.append("--no-tray")
    if extra_args:
        cmd.extend(extra_args)
    if cache_dir:
        cmd.append(cache_dir)

    env = os.environ.copy()
    if env_overrides:
        env.update(env_overrides)

    stdout = subprocess.DEVNULL if sys.platform == "win32" else subprocess.PIPE
    stderr = subprocess.DEVNULL if sys.platform == "win32" else subprocess.PIPE
    subprocess.Popen(
        cmd,
        stdout=stdout,
        stderr=stderr,
        text=(sys.platform != "win32"),
        env=env,
    )
    wait_for_server(timeout=timeout)
    print("Server started successfully")


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
@unittest.skipUnless(
    sys.platform.startswith("linux"),
    "Router-mode tests rely on the system llamacpp backend (Linux only).",
)
class LlamaCppRouterTests(unittest.TestCase):
    """
    End-to-end router-mode tests using a fake `llama-server` on PATH.

    Each test starts a fresh `lemond` so router-mode startup flags (which
    are read once at process boot) can be exercised cleanly.
    """

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.temp_bin_dir = tempfile.mkdtemp(prefix="lemonade-router-bin-")
        cls.temp_cfg_dir = tempfile.mkdtemp(prefix="lemonade-router-cfg-")
        cls.mock_path = os.path.join(cls.temp_bin_dir, "llama-server")
        if os.name == "nt":
            cls.mock_path += ".exe"
        with open(cls.mock_path, "w", encoding="utf-8") as handle:
            handle.write(MOCK_LLAMA_SERVER_PYTHON)
        os.chmod(cls.mock_path, os.stat(cls.mock_path).st_mode | stat.S_IEXEC)

        cls.original_path = os.environ.get("PATH", "")
        cls.original_llamacpp = os.environ.get("LEMONADE_LLAMACPP", "")
        cls.original_router_mode = os.environ.get("LEMONADE_ROUTER_MODE", "")
        cls.original_router_preset = os.environ.get("LEMONADE_ROUTER_MODELS_PRESET", "")
        cls.original_router_dir = os.environ.get("LEMONADE_ROUTER_MODELS_DIR", "")

    def _fresh_cache_dir(self):
        """Return a fresh per-test cache dir so env-var overlays apply.

        ``LEMONADE_*`` env vars are only migrated into config.json when the
        file does not exist, so we need a clean directory per lemond
        launch.
        """
        cache_dir = tempfile.mkdtemp(
            prefix="lemonade-router-cache-", dir=self.temp_cfg_dir
        )
        return cache_dir

    @classmethod
    def tearDownClass(cls):
        _stop_server()
        shutil.rmtree(cls.temp_bin_dir, ignore_errors=True)
        shutil.rmtree(cls.temp_cfg_dir, ignore_errors=True)
        os.environ["PATH"] = cls.original_path
        for key, value in (
            ("LEMONADE_LLAMACPP", cls.original_llamacpp),
            ("LEMONADE_ROUTER_MODE", cls.original_router_mode),
            ("LEMONADE_ROUTER_MODELS_PRESET", cls.original_router_preset),
            ("LEMONADE_ROUTER_MODELS_DIR", cls.original_router_dir),
        ):
            if value:
                os.environ[key] = value
            else:
                os.environ.pop(key, None)
        super().tearDownClass()

    def setUp(self):
        print(f"\n=== Starting test: {self._testMethodName} ===")
        _stop_server()
        os.environ["PATH"] = self.temp_bin_dir + os.pathsep + self.original_path
        os.environ["LEMONADE_LLAMACPP"] = "system"
        for var in (
            "LEMONADE_ROUTER_MODE",
            "LEMONADE_ROUTER_MODELS_PRESET",
            "LEMONADE_ROUTER_MODELS_DIR",
            "MOCK_LLAMA_REQUEST_PATH",
        ):
            os.environ.pop(var, None)

    def tearDown(self):
        _stop_server()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------
    def _write_preset(self, models):
        """Write a minimal llama-server .ini preset with the given model IDs."""
        preset_path = os.path.join(self.temp_cfg_dir, "models.ini")
        lines = []
        for model in models:
            lines.append(f"[{model}]")
            lines.append(f"model = /tmp/nonexistent/{model}.gguf\n")
        with open(preset_path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines))
        return preset_path

    def _write_models_dir(self, models):
        """Create a directory of empty .gguf stub files and return its path."""
        models_dir = os.path.join(self.temp_cfg_dir, "gguf-dir")
        os.makedirs(models_dir, exist_ok=True)
        for model in models:
            stub = os.path.join(models_dir, f"{model}.gguf")
            with open(stub, "wb") as handle:
                handle.write(b"")
        return models_dir

    def _start_router(self, preset_path=None, models_dir=None, env_overrides=None):
        extra_args = ["--router-mode"]
        if preset_path:
            extra_args += ["--models-preset", preset_path]
        if models_dir:
            extra_args += ["--models-dir", models_dir]
        env = {"LEMONADE_LLAMACPP": "system"}
        if env_overrides:
            env.update(env_overrides)
        cache_dir = self._fresh_cache_dir()
        _start_server(extra_args=extra_args, env_overrides=env, cache_dir=cache_dir)

    # ------------------------------------------------------------------
    # Tests
    # ------------------------------------------------------------------
    def test_001_health_reports_router_mode(self):
        """/health exposes router_mode=true when the router was enabled."""
        preset = self._write_preset(["router-test-alpha", "router-test-beta"])
        self._start_router(preset_path=preset)

        response = requests.get(
            f"http://localhost:{PORT}/api/v1/health",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertTrue(
            body.get("router_mode"),
            f"/health.router_mode should be true in router mode; got {body!r}",
        )

    def test_002_health_all_models_loaded_lists_router_roster(self):
        """/api/v1/health.all_models_loaded includes the router roster.

        Router-owned models are not in the Lemonade registry so they
        won't show up under /api/v1/models (which only enumerates
        downloaded models from the registry), but they MUST show up in
        the /health payload so clients can see what the router is
        hosting.
        """
        roster = ["router-test-alpha", "router-test-beta"]
        preset = self._write_preset(roster)
        self._start_router(preset_path=preset)

        response = requests.get(
            f"http://localhost:{PORT}/api/v1/health",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        body = response.json()
        loaded = body.get("all_models_loaded", [])
        loaded_names = {
            entry.get("model_name") for entry in loaded if isinstance(entry, dict)
        }
        for model in roster:
            self.assertIn(
                model,
                loaded_names,
                f"Expected router roster to appear in "
                f"all_models_loaded; got {loaded!r}",
            )

    def test_003_chat_completion_forwards_to_router(self):
        """Chat completion for a roster model is proxied to the child server."""
        roster = ["router-test-alpha"]
        preset = self._write_preset(roster)
        self._start_router(preset_path=preset)

        capture_path = os.path.join(self.temp_cfg_dir, "captured_chat_request.json")
        os.environ["MOCK_LLAMA_REQUEST_PATH"] = capture_path
        self.addCleanup(os.environ.pop, "MOCK_LLAMA_REQUEST_PATH", None)
        # Restart so the mock picks up the capture path.
        _stop_server()
        self._start_router(
            preset_path=preset,
            env_overrides={"MOCK_LLAMA_REQUEST_PATH": capture_path},
        )

        response = requests.post(
            f"http://localhost:{PORT}/api/v1/chat/completions",
            json={
                "model": "router-test-alpha",
                "messages": [{"role": "user", "content": "ping"}],
                "max_tokens": 4,
                "stream": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200, response.text)
        body = response.json()
        self.assertIn("choices", body)
        self.assertGreater(len(body["choices"]), 0)
        content = body["choices"][0]["message"]["content"]
        self.assertIn("router-test-alpha", content)

        # Confirm the child actually received the forwarded request.
        self.assertTrue(
            os.path.exists(capture_path),
            f"Mock llama-server did not capture a request at {capture_path}",
        )
        with open(capture_path, "r", encoding="utf-8") as handle:
            forwarded = json.load(handle)
        self.assertEqual(forwarded.get("model"), "router-test-alpha")

    def test_004_load_non_roster_llamacpp_model_is_rejected(self):
        """POST /api/v1/load for a registered llamacpp model not in the
        roster returns an error that mentions router mode."""
        preset = self._write_preset(["router-test-alpha"])
        self._start_router(preset_path=preset)

        # Qwen3-0.6B-GGUF is a registered llamacpp model in every Lemonade
        # shipment but is intentionally NOT in our preset, so the router
        # must refuse to load it.
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/load",
            json={"model_name": "Qwen3-0.6B-GGUF"},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertNotEqual(
            response.status_code,
            200,
            f"Expected /load to fail for non-roster model; body={response.text!r}",
        )
        self.assertIn("router", response.text.lower())

    def test_005_unload_router_owned_model_is_rejected(self):
        """POST /api/v1/unload targeting a router-owned model returns an
        error (routers are non-evictable)."""
        roster = ["router-test-alpha"]
        preset = self._write_preset(roster)
        self._start_router(preset_path=preset)

        response = requests.post(
            f"http://localhost:{PORT}/api/v1/unload",
            json={"model_name": "router-test-alpha"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertNotEqual(
            response.status_code,
            200,
            f"Expected /unload to refuse a router-owned model; body={response.text!r}",
        )

    def test_006_models_dir_is_plumbed_through(self):
        """--models-dir is accepted and its .gguf basenames are visible
        as router-owned models via /api/v1/health."""
        models_dir = self._write_models_dir(["router-dir-alpha", "router-dir-beta"])
        self._start_router(models_dir=models_dir)

        response = requests.get(
            f"http://localhost:{PORT}/api/v1/health",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        loaded = response.json().get("all_models_loaded", [])
        loaded_names = {
            entry.get("model_name") for entry in loaded if isinstance(entry, dict)
        }
        self.assertIn("router-dir-alpha", loaded_names)
        self.assertIn("router-dir-beta", loaded_names)

    def test_007_router_mode_env_var_enables_router(self):
        """LEMONADE_ROUTER_MODE / _MODELS_PRESET overlay config.json when
        router-mode CLI flags are omitted."""
        preset = self._write_preset(["router-env-alpha"])
        _start_server(
            extra_args=[],
            env_overrides={
                "LEMONADE_LLAMACPP": "system",
                "LEMONADE_ROUTER_MODE": "true",
                "LEMONADE_ROUTER_MODELS_PRESET": preset,
            },
            cache_dir=self._fresh_cache_dir(),
        )

        response = requests.get(
            f"http://localhost:{PORT}/api/v1/health",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        payload = response.json()
        self.assertTrue(
            payload.get("router_mode"),
            "router_mode should be enabled via LEMONADE_ROUTER_MODE=true",
        )

        loaded_names = {
            entry.get("model_name")
            for entry in payload.get("all_models_loaded", [])
            if isinstance(entry, dict)
        }
        self.assertIn("router-env-alpha", loaded_names)

    def test_008_load_overrides_for_router_model_are_rejected(self):
        """Router-hosted llamacpp models reject /load overrides and save_options.

        Runtime tuning in router mode must be done in the router source
        (--models-preset/--models-dir), not via Lemonade /load payload keys.
        """
        model_name = "Qwen3-0.6B-GGUF"
        preset = self._write_preset([model_name])
        self._start_router(preset_path=preset)

        test_payloads = [
            {
                "model_name": model_name,
                "ctx_size": 8192,
            },
            {
                "model_name": model_name,
                "save_options": True,
            },
        ]

        for payload in test_payloads:
            response = requests.post(
                f"http://localhost:{PORT}/api/v1/load",
                json=payload,
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(
                response.status_code,
                400,
                f"Expected /load override rejection; payload={payload!r}, "
                f"status={response.status_code}, body={response.text!r}",
            )
            body = response.json()
            self.assertEqual(
                body.get("error", {}).get("code"),
                "router_mode_options_unsupported",
                f"Unexpected error payload for {payload!r}: {body!r}",
            )


def _run_tests():
    print(f"\n{'=' * 70}")
    print("LLAMACPP ROUTER-MODE TESTS")
    print(f"{'=' * 70}\n")
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(LlamaCppRouterTests)
    runner = unittest.TextTestRunner(verbosity=2, buffer=False, failfast=True)
    result = runner.run(suite)
    sys.exit(0 if (result and result.wasSuccessful()) else 1)


if __name__ == "__main__":
    _run_tests()
