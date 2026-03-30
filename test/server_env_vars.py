"""
Environment variable tests for lemond.

Verifies that every environment variable documented in docs/server/configuration.md
is picked up by lemond and reflected in the runtime config or server behavior.

Usage:
    # Build lemond first, then:
    python test/server_env_vars.py
    python test/server_env_vars.py --server-binary /path/to/lemond
"""

import argparse
import json
import os
import platform
import signal
import subprocess
import sys
import tempfile
import time
import unittest

import requests

PORT = 12120
BASE = f"http://localhost:{PORT}"
HEALTH = f"{BASE}/v1/health"
CONFIG = f"{BASE}/internal/config"

IS_MACOS = platform.system() == "Darwin"


def get_default_binary():
    test_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(test_dir)
    return os.path.join(root, "build", "lemond")


_binary = get_default_binary()


def wait_for_server(url, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = requests.get(url, timeout=2)
            if r.status_code == 200:
                return True
        except requests.ConnectionError:
            pass
        time.sleep(0.3)
    return False


def start_server(env_overrides=None, extra_args=None):
    """Start lemond with given env overrides. Returns subprocess.Popen."""
    env = os.environ.copy()
    # Isolate from any existing lemonade env vars
    for k in list(env.keys()):
        if k.startswith("LEMONADE_"):
            del env[k]
    home_dir = tempfile.mkdtemp(prefix="lemon_test_")
    env["LEMONADE_CACHE_DIR"] = home_dir
    env["LEMONADE_PORT"] = str(PORT)
    env["LEMONADE_HOST"] = "localhost"
    env["LEMONADE_NO_BROADCAST"] = "1"
    if env_overrides:
        env.update(env_overrides)

    cmd = [_binary]
    if extra_args:
        cmd.extend(extra_args)

    proc = subprocess.Popen(
        cmd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return proc, home_dir


def stop_server(proc):
    if proc.poll() is None:
        if platform.system() == "Windows":
            proc.terminate()
        else:
            proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def get_config():
    return requests.get(CONFIG, timeout=5).json()


# ---------------------------------------------------------------------------
# Test: server config env vars reflected in /internal/config
# ---------------------------------------------------------------------------


class TestConfigEnvVars(unittest.TestCase):
    """Start lemond once with all config-based env vars set, verify snapshot."""

    proc = None

    @classmethod
    def setUpClass(cls):
        cls.env = {
            "LEMONADE_PORT": str(PORT),
            "LEMONADE_HOST": "localhost",
            "LEMONADE_LOG_LEVEL": "debug",
            "LEMONADE_EXTRA_MODELS_DIR": "/tmp/lemon_extra_models_test",
            "LEMONADE_GLOBAL_TIMEOUT": "999",
            "LEMONADE_MAX_LOADED_MODELS": "3",
            # Recipe-option env vars
            "LEMONADE_CTX_SIZE": "2048",
        }
        if not IS_MACOS:
            cls.env.update(
                {
                    "LEMONADE_LLAMACPP": "cpu",
                    "LEMONADE_LLAMACPP_ARGS": "--flash-attn on",
                    "LEMONADE_WHISPERCPP": "cpu",
                    "LEMONADE_WHISPERCPP_ARGS": "--convert",
                    "LEMONADE_FLM_ARGS": "--socket 20",
                }
            )
        cls.proc, cls.home_dir = start_server(cls.env)
        if not wait_for_server(HEALTH):
            out = cls.proc.stdout.read().decode() if cls.proc.stdout else ""
            err = cls.proc.stderr.read().decode() if cls.proc.stderr else ""
            stop_server(cls.proc)
            raise RuntimeError(
                f"Server failed to start.\nstdout:\n{out}\nstderr:\n{err}"
            )
        cls.snapshot = get_config()

    @classmethod
    def tearDownClass(cls):
        if cls.proc:
            stop_server(cls.proc)

    def test_port(self):
        self.assertEqual(self.snapshot["port"], PORT)

    def test_host(self):
        self.assertEqual(self.snapshot["host"], "localhost")

    def test_log_level(self):
        self.assertEqual(self.snapshot["log_level"], "debug")

    def test_extra_models_dir(self):
        self.assertEqual(
            self.snapshot["extra_models_dir"], "/tmp/lemon_extra_models_test"
        )

    def test_global_timeout(self):
        self.assertEqual(self.snapshot["global_timeout"], 999)

    def test_max_loaded_models(self):
        self.assertEqual(self.snapshot["max_loaded_models"], 3)

    def test_max_loaded_models_in_health(self):
        health = requests.get(HEALTH, timeout=5).json()
        # All type slots should reflect max_loaded_models=3
        for slot, val in health["max_models"].items():
            self.assertEqual(val, 3, f"max_models.{slot} should be 3")

    def test_ctx_size(self):
        self.assertEqual(self.snapshot["ctx_size"], 2048)

    @unittest.skipIf(IS_MACOS, "llamacpp backend selection not applicable on macOS")
    def test_llamacpp_backend(self):
        self.assertEqual(self.snapshot["llamacpp_backend"], "cpu")

    @unittest.skipIf(IS_MACOS, "llamacpp args not applicable on macOS")
    def test_llamacpp_args(self):
        self.assertEqual(self.snapshot["llamacpp_args"], "--flash-attn on")

    @unittest.skipIf(IS_MACOS, "whispercpp backend selection not applicable on macOS")
    def test_whispercpp_backend(self):
        self.assertEqual(self.snapshot["whispercpp_backend"], "cpu")

    @unittest.skipIf(IS_MACOS, "whispercpp args not applicable on macOS")
    def test_whispercpp_args(self):
        self.assertEqual(self.snapshot["whispercpp_args"], "--convert")

    @unittest.skipIf(IS_MACOS, "FLM is NPU-only, not available on macOS")
    def test_flm_args(self):
        self.assertEqual(self.snapshot["flm_args"], "--socket 20")


# ---------------------------------------------------------------------------
# Test: LEMONADE_API_KEY
# ---------------------------------------------------------------------------


class TestApiKeyEnvVar(unittest.TestCase):
    """Verify LEMONADE_API_KEY enables Bearer auth on API routes."""

    proc = None
    API_KEY = "test-secret-key-12345"

    @classmethod
    def setUpClass(cls):
        cls.proc, cls.home_dir = start_server({"LEMONADE_API_KEY": cls.API_KEY})
        if not wait_for_server(f"{BASE}/live"):
            out = cls.proc.stdout.read().decode() if cls.proc.stdout else ""
            err = cls.proc.stderr.read().decode() if cls.proc.stderr else ""
            stop_server(cls.proc)
            raise RuntimeError(
                f"Server failed to start.\nstdout:\n{out}\nstderr:\n{err}"
            )

    @classmethod
    def tearDownClass(cls):
        if cls.proc:
            stop_server(cls.proc)

    def test_no_key_rejected(self):
        r = requests.get(f"{BASE}/v1/health", timeout=5)
        self.assertEqual(r.status_code, 401)

    def test_wrong_key_rejected(self):
        r = requests.get(
            f"{BASE}/v1/health",
            headers={"Authorization": "Bearer wrong-key"},
            timeout=5,
        )
        self.assertEqual(r.status_code, 401)

    def test_correct_key_accepted(self):
        r = requests.get(
            f"{BASE}/v1/health",
            headers={"Authorization": f"Bearer {self.API_KEY}"},
            timeout=5,
        )
        self.assertEqual(r.status_code, 200)

    def test_internal_config_also_requires_key(self):
        r = requests.get(CONFIG, timeout=5)
        self.assertEqual(r.status_code, 401)

    def test_internal_config_with_key(self):
        r = requests.get(
            CONFIG,
            headers={"Authorization": f"Bearer {self.API_KEY}"},
            timeout=5,
        )
        self.assertEqual(r.status_code, 200)


# ---------------------------------------------------------------------------
# Test: defaults when no env vars set
# ---------------------------------------------------------------------------


class TestDefaults(unittest.TestCase):
    """Verify that config defaults are correct when no env vars are set."""

    proc = None

    @classmethod
    def setUpClass(cls):
        cls.proc, cls.home_dir = start_server()
        if not wait_for_server(HEALTH):
            out = cls.proc.stdout.read().decode() if cls.proc.stdout else ""
            err = cls.proc.stderr.read().decode() if cls.proc.stderr else ""
            stop_server(cls.proc)
            raise RuntimeError(
                f"Server failed to start.\nstdout:\n{out}\nstderr:\n{err}"
            )
        cls.snapshot = get_config()

    @classmethod
    def tearDownClass(cls):
        if cls.proc:
            stop_server(cls.proc)

    def test_default_log_level(self):
        self.assertEqual(self.snapshot["log_level"], "info")

    def test_default_global_timeout(self):
        self.assertEqual(self.snapshot["global_timeout"], 300)

    def test_default_max_loaded_models(self):
        self.assertEqual(self.snapshot["max_loaded_models"], 1)

    def test_default_ctx_size(self):
        # ctx_size is a recipe option; absent from snapshot means default (4096)
        self.assertNotIn("ctx_size", self.snapshot)

    def test_default_extra_models_dir(self):
        self.assertEqual(self.snapshot["extra_models_dir"], "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--server-binary", type=str, default=get_default_binary())
    args, remaining = parser.parse_known_args()
    _binary = args.server_binary

    # Pass remaining args to unittest
    unittest.main(argv=[sys.argv[0]] + remaining, verbosity=2)
