"""
Environment variable tests for lemond.

Verifies that legacy LEMONADE_* environment variables are picked up by lemond
(migrated into config.json on first startup) and reflected in the runtime config.

Usage:
    # Build lemond first, then:
    python test/server_env_vars.py
    python test/server_env_vars.py --lemond-binary /path/to/lemond
"""

import argparse
import os
import platform
import subprocess
import sys
import tempfile
import time
import unittest

import requests

from utils.test_models import PORT, TIMEOUT_DEFAULT, get_default_lemond_binary
from utils.server_base import wait_for_server

BASE = f"http://localhost:{PORT}"
HEALTH = f"{BASE}/v1/health"
CONFIG = f"{BASE}/internal/config"

IS_MACOS = platform.system() == "Darwin"

_lemond_binary = get_default_lemond_binary()


def shutdown_existing_server():
    """Shut down any pre-existing server on the test port (e.g. deb-installed in CI)."""
    try:
        requests.post(f"{BASE}/internal/shutdown", timeout=5)
    except Exception:
        return  # nothing was listening
    # Wait for port to be fully released before starting a new server
    for _ in range(20):
        try:
            requests.get(f"{BASE}/live", timeout=0.5)
            time.sleep(0.25)
        except Exception:
            break


def start_server(env_overrides=None):
    """Start lemond with given env overrides in an isolated temp cache dir.

    Returns (subprocess.Popen, cache_dir).
    """
    shutdown_existing_server()
    env = os.environ.copy()
    # Isolate from any existing lemonade env vars
    for k in list(env.keys()):
        if k.startswith("LEMONADE_"):
            del env[k]
    cache_dir = tempfile.mkdtemp(prefix="lemon_test_")
    env["LEMONADE_CACHE_DIR"] = cache_dir
    env["LEMONADE_PORT"] = str(PORT)
    env["LEMONADE_HOST"] = "localhost"
    env["LEMONADE_NO_BROADCAST"] = "1"
    if env_overrides:
        env.update(env_overrides)

    proc = subprocess.Popen(
        [_lemond_binary],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return proc, cache_dir


def stop_server(proc):
    """Stop lemond via /internal/shutdown and wait for port release."""
    try:
        requests.post(
            f"{BASE}/internal/shutdown",
            timeout=5,
        )
    except Exception:
        pass
    proc.kill()
    proc.wait()
    # Wait for port to be fully released
    for _ in range(20):
        try:
            requests.get(f"{BASE}/live", timeout=0.5)
            time.sleep(0.25)
        except Exception:
            break


def get_config():
    return requests.get(CONFIG, timeout=TIMEOUT_DEFAULT).json()


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
        cls.proc, cls.cache_dir = start_server(cls.env)
        wait_for_server(port=PORT)
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
        health = requests.get(HEALTH, timeout=TIMEOUT_DEFAULT).json()
        # All type slots should reflect max_loaded_models=3
        for slot, val in health["max_models"].items():
            self.assertEqual(val, 3, f"max_models.{slot} should be 3")

    def test_ctx_size(self):
        self.assertEqual(self.snapshot["ctx_size"], 2048)

    @unittest.skipIf(IS_MACOS, "llamacpp backend selection not applicable on macOS")
    def test_llamacpp_backend(self):
        self.assertEqual(self.snapshot["llamacpp"]["backend"], "cpu")

    @unittest.skipIf(IS_MACOS, "llamacpp args not applicable on macOS")
    def test_llamacpp_args(self):
        self.assertEqual(self.snapshot["llamacpp"]["args"], "--flash-attn on")

    @unittest.skipIf(IS_MACOS, "whispercpp backend selection not applicable on macOS")
    def test_whispercpp_backend(self):
        self.assertEqual(self.snapshot["whispercpp"]["backend"], "cpu")

    @unittest.skipIf(IS_MACOS, "whispercpp args not applicable on macOS")
    def test_whispercpp_args(self):
        self.assertEqual(self.snapshot["whispercpp"]["args"], "--convert")

    @unittest.skipIf(IS_MACOS, "FLM is NPU-only, not available on macOS")
    def test_flm_args(self):
        self.assertEqual(self.snapshot["flm"]["args"], "--socket 20")


# ---------------------------------------------------------------------------
# Test: LEMONADE_API_KEY
# ---------------------------------------------------------------------------


class TestApiKeyEnvVar(unittest.TestCase):
    """Verify LEMONADE_API_KEY enables Bearer auth on API routes."""

    proc = None
    API_KEY = "test-secret-key-12345"

    @classmethod
    def setUpClass(cls):
        cls.proc, cls.cache_dir = start_server({"LEMONADE_API_KEY": cls.API_KEY})
        # /live is unauthenticated, use it to detect readiness
        wait_for_server(port=PORT)

    @classmethod
    def tearDownClass(cls):
        if cls.proc:
            stop_server(cls.proc)

    def test_no_key_rejected(self):
        r = requests.get(f"{BASE}/v1/health", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(r.status_code, 401)

    def test_wrong_key_rejected(self):
        r = requests.get(
            f"{BASE}/v1/health",
            headers={"Authorization": "Bearer wrong-key"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 401)

    def test_correct_key_accepted(self):
        r = requests.get(
            f"{BASE}/v1/health",
            headers={"Authorization": f"Bearer {self.API_KEY}"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 200)

    def test_internal_config_also_requires_key(self):
        r = requests.get(CONFIG, timeout=TIMEOUT_DEFAULT)
        self.assertEqual(r.status_code, 401)

    def test_internal_config_with_key(self):
        r = requests.get(
            CONFIG,
            headers={"Authorization": f"Bearer {self.API_KEY}"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 200)


# ---------------------------------------------------------------------------
# Test: LEMONADE_ADMIN_API_KEY
# ---------------------------------------------------------------------------


class TestAdminApiKeyEnvVar(unittest.TestCase):
    """Verify LEMONADE_ADMIN_API_KEY provides elevated access to internal endpoints."""

    proc = None
    ADMIN_API_KEY = "admin-secret-key-xyz"

    @classmethod
    def setUpClass(cls):
        cls.proc, cls.cache_dir = start_server(
            {"LEMONADE_ADMIN_API_KEY": cls.ADMIN_API_KEY}
        )
        wait_for_server(port=PORT)

    @classmethod
    def tearDownClass(cls):
        if cls.proc:
            stop_server(cls.proc)

    def test_no_key_rejected_on_internal(self):
        """Internal endpoints require admin key when LEMONADE_ADMIN_API_KEY is set."""
        r = requests.get(CONFIG, timeout=TIMEOUT_DEFAULT)
        self.assertEqual(r.status_code, 401)

    def test_wrong_key_rejected_on_internal(self):
        """Wrong admin key should be rejected on internal endpoints."""
        r = requests.get(
            CONFIG,
            headers={"Authorization": "Bearer wrong-admin-key"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 401)

    def test_correct_admin_key_accepted_on_internal(self):
        """Correct admin key should grant access to internal endpoints."""
        r = requests.get(
            CONFIG,
            headers={"Authorization": f"Bearer {self.ADMIN_API_KEY}"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 200)

    def test_admin_key_works_on_regular_api_endpoints(self):
        """Admin key should also work on regular API endpoints."""
        r = requests.get(
            f"{BASE}/v1/health",
            headers={"Authorization": f"Bearer {self.ADMIN_API_KEY}"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 200)

    def test_no_key_accepted_on_regular_endpoints(self):
        """When only LEMONADE_ADMIN_API_KEY is set, regular API endpoints should be accessible without auth."""
        r = requests.get(f"{BASE}/v1/health", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(r.status_code, 200)


# ---------------------------------------------------------------------------
# Test: LEMONADE_API_KEY and LEMONADE_ADMIN_API_KEY together
# ---------------------------------------------------------------------------


class TestBothApiKeysEnvVar(unittest.TestCase):
    """Verify behavior when both LEMONADE_API_KEY and LEMONADE_ADMIN_API_KEY are set."""

    proc = None
    REGULAR_API_KEY = "regular-key-abc"
    ADMIN_API_KEY = "admin-key-xyz"

    @classmethod
    def setUpClass(cls):
        cls.proc, cls.cache_dir = start_server(
            {
                "LEMONADE_API_KEY": cls.REGULAR_API_KEY,
                "LEMONADE_ADMIN_API_KEY": cls.ADMIN_API_KEY,
            }
        )
        wait_for_server(port=PORT)

    @classmethod
    def tearDownClass(cls):
        if cls.proc:
            stop_server(cls.proc)

    def test_regular_key_works_on_regular_endpoints(self):
        """Regular API key should work on regular API endpoints."""
        r = requests.get(
            f"{BASE}/v1/health",
            headers={"Authorization": f"Bearer {self.REGULAR_API_KEY}"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 200)

    def test_admin_key_works_on_regular_endpoints(self):
        """Admin API key should also work on regular API endpoints."""
        r = requests.get(
            f"{BASE}/v1/health",
            headers={"Authorization": f"Bearer {self.ADMIN_API_KEY}"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 200)

    def test_regular_key_rejected_on_internal(self):
        """Regular API key should NOT work on internal endpoints."""
        r = requests.get(
            CONFIG,
            headers={"Authorization": f"Bearer {self.REGULAR_API_KEY}"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 401)

    def test_admin_key_works_on_internal(self):
        """Admin API key should work on internal endpoints."""
        r = requests.get(
            CONFIG,
            headers={"Authorization": f"Bearer {self.ADMIN_API_KEY}"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 200)

    def test_wrong_key_rejected_everywhere(self):
        """Wrong key should be rejected on all endpoints."""
        # Regular endpoint
        r = requests.get(
            f"{BASE}/v1/health",
            headers={"Authorization": "Bearer wrong-key"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 401)
        # Internal endpoint
        r = requests.get(
            CONFIG,
            headers={"Authorization": "Bearer wrong-key"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(r.status_code, 401)


# ---------------------------------------------------------------------------
# Test: defaults when no env vars set
# ---------------------------------------------------------------------------


class TestDefaults(unittest.TestCase):
    """Verify that config defaults are correct when no env vars are set."""

    proc = None

    @classmethod
    def setUpClass(cls):
        cls.proc, cls.cache_dir = start_server()
        wait_for_server(port=PORT)
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
        self.assertEqual(self.snapshot["ctx_size"], 4096)

    def test_default_extra_models_dir(self):
        self.assertEqual(self.snapshot["extra_models_dir"], "")


# ---------------------------------------------------------------------------
# Test: wrong .gguf variant must not mark model as downloaded
# ---------------------------------------------------------------------------


class TestWrongGgufVariantNotDownloaded(unittest.TestCase):
    """Regression test: wrong .gguf sibling must not mark model as downloaded.

    Creates a fake HF cache containing a .gguf file that does NOT match the
    exact filename expected by the Tiny-Test-Model-GGUF registry entry
    (gemma-3-270m-it-UD-IQ2_M.gguf).  The server must report downloaded=false
    for that model and omit it from the default /models list.

    See: https://github.com/lemonade-sdk/lemonade/pull/1502
    """

    proc = None

    @classmethod
    def setUpClass(cls):
        # Build a fake HF cache with the wrong .gguf sibling.
        # Tiny-Test-Model-GGUF expects:
        #   repo  = unsloth/gemma-3-270m-it-GGUF
        #   file  = gemma-3-270m-it-UD-IQ2_M.gguf
        cls.fake_hf_cache = tempfile.mkdtemp(prefix="lemon_test_hf_")
        snapshot_dir = os.path.join(
            cls.fake_hf_cache,
            "models--unsloth--gemma-3-270m-it-GGUF",
            "snapshots",
            "abc123",
        )
        os.makedirs(snapshot_dir)
        # Plant a .gguf file with the WRONG name
        with open(os.path.join(snapshot_dir, "WRONG-variant.gguf"), "wb") as f:
            f.write(b"\x00" * 64)

        cls.proc, cls.cache_dir = start_server({"HF_HUB_CACHE": cls.fake_hf_cache})
        wait_for_server(port=PORT)

    @classmethod
    def tearDownClass(cls):
        if cls.proc:
            stop_server(cls.proc)

    def test_show_all_reports_not_downloaded(self):
        """Model with wrong .gguf variant should report downloaded=false."""
        resp = requests.get(f"{BASE}/v1/models?show_all=true", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(resp.status_code, 200)
        models = {m["id"]: m for m in resp.json()["data"]}
        self.assertIn("Tiny-Test-Model-GGUF", models)
        self.assertFalse(
            models["Tiny-Test-Model-GGUF"]["downloaded"],
            "Model with wrong .gguf sibling must report downloaded=false",
        )

    def test_default_models_list_excludes_model(self):
        """Model with wrong .gguf variant should not appear in default /models."""
        resp = requests.get(f"{BASE}/v1/models", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(resp.status_code, 200)
        model_ids = [m["id"] for m in resp.json()["data"]]
        self.assertNotIn(
            "Tiny-Test-Model-GGUF",
            model_ids,
            "Model with wrong .gguf should not appear in downloaded list",
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument(
        "--lemond-binary",
        type=str,
        default=get_default_lemond_binary(),
    )
    args, remaining = parser.parse_known_args()
    _lemond_binary = args.lemond_binary

    # Pass remaining args to unittest
    unittest.main(argv=[sys.argv[0]] + remaining, verbosity=2)
