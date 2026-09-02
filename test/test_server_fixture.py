import os
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

# Ensure test/ directory is in sys.path when run directly or via unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

try:
    from utils.server_fixture import (
        allocate_free_port,
        lemond_server,
        make_clean_env,
        wait_for_http_health,
    )
    from utils.test_models import get_default_lemond_binary
except ImportError:
    from test.utils.server_fixture import (
        allocate_free_port,
        lemond_server,
        make_clean_env,
        wait_for_http_health,
    )
    from test.utils.test_models import get_default_lemond_binary


class ServerFixtureTests(unittest.TestCase):
    def _require_lemond_binary(self):
        bin_path = get_default_lemond_binary()
        if not bin_path or not os.path.exists(bin_path):
            if not shutil.which("lemond"):
                raise unittest.SkipTest("lemond binary not found")

    def test_allocate_free_port(self):
        port = allocate_free_port()
        self.assertIsInstance(port, int)
        self.assertGreater(port, 0)

    @patch.dict(
        os.environ,
        {
            "LEMONADE_API_KEY": "secret_test_key",
            "LEMONADE_ADMIN_API_KEY": "admin_test_key",
            "LEMONADE_ALLOWED_ORIGINS": "http://example.com",
        },
        clear=False,
    )
    def test_make_clean_env(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            env = make_clean_env(tmpdir)
            self.assertIn("LEMONADE_CACHE_DIR", env)
            self.assertTrue(env["LEMONADE_CACHE_DIR"].startswith(tmpdir))
            self.assertIn("LEMONADE_CONFIG_DIR", env)
            self.assertTrue(env["LEMONADE_CONFIG_DIR"].startswith(tmpdir))
            self.assertIn("LEMONADE_MODELS_DIR", env)
            self.assertTrue(env["LEMONADE_MODELS_DIR"].startswith(tmpdir))
            self.assertIn("HF_HUB_CACHE", env)
            self.assertTrue(env["HF_HUB_CACHE"].startswith(tmpdir))

            self.assertNotIn("LEMONADE_API_KEY", env)
            self.assertNotIn("LEMONADE_ADMIN_API_KEY", env)
            self.assertNotIn("LEMONADE_ALLOWED_ORIGINS", env)

    def test_lemond_server_starts_and_stops(self):
        self._require_lemond_binary()
        with tempfile.TemporaryDirectory() as tmpdir:
            env = make_clean_env(tmpdir)
            port = allocate_free_port()

            with lemond_server(port=port, env=env, cache_dir=tmpdir) as proc:
                self.assertIsNone(proc.poll())
                self.assertTrue(wait_for_http_health(port))

            # Context manager exit should have terminated the process
            self.assertIsNotNone(proc.poll())

    def test_lemond_server_wait_health(self):
        self._require_lemond_binary()
        with tempfile.TemporaryDirectory() as tmpdir:
            env = make_clean_env(tmpdir)
            port = allocate_free_port()

            with lemond_server(
                port=port, env=env, cache_dir=tmpdir, wait_health=True
            ) as proc:
                self.assertIsNone(proc.poll())
                self.assertTrue(wait_for_http_health(port))

            self.assertIsNotNone(proc.poll())

    def test_lemond_server_wait_health_early_exit_diagnostic(self):
        self._require_lemond_binary()
        with tempfile.TemporaryDirectory() as tmpdir:
            env = make_clean_env(tmpdir)
            port = allocate_free_port()

            with self.assertRaises(RuntimeError) as ctx:
                with lemond_server(
                    port=port,
                    env=env,
                    cache_dir=tmpdir,
                    args=["--completely-invalid-flag-xyz"],
                    wait_health=True,
                    health_timeout=2.0,
                ):
                    pass
            self.assertIn("process exited early with code", str(ctx.exception))

    def test_lemond_server_inherits_cache_and_config_from_env(self):
        self._require_lemond_binary()
        with tempfile.TemporaryDirectory() as tmpdir:
            env = make_clean_env(tmpdir)
            port = allocate_free_port()

            with lemond_server(port=port, env=env, wait_health=True) as proc:
                self.assertIsNone(proc.poll())
                self.assertTrue(wait_for_http_health(port))

            self.assertIsNotNone(proc.poll())

    def test_lemond_server_config_dir_requires_cache_dir(self):
        with self.assertRaises(ValueError):
            with lemond_server(config_dir="/tmp/some_config"):
                pass

    def test_lemond_server_explicit_binary_path_missing_raises(self):
        with self.assertRaises(RuntimeError) as ctx:
            with lemond_server(binary_path="/nonexistent/lemond_bin_12345"):
                pass
        self.assertIn("explicit binary_path", str(ctx.exception))

    def test_lemond_server_skip_if_unavailable(self):
        with self.assertRaises(unittest.SkipTest):
            with lemond_server(
                binary_path="/nonexistent/lemond_bin_12345",
                skip_if_unavailable=True,
            ):
                pass


if __name__ == "__main__":
    unittest.main()
