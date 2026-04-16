"""
Tests for server log level control.
Verifies that log level can be changed via /log-level and /internal/set,
and that the changes are reflected in the server configuration.
"""

import unittest
import requests
import json
import time

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
)
from utils.test_models import (
    PORT,
    TIMEOUT_DEFAULT,
)


class LogLevelTests(ServerTestBase):
    """Tests for log level control."""

    def test_log_level_via_dedicated_endpoint(self):
        """Test changing log level via the dedicated /log-level endpoint."""
        # 1. Get initial level
        response = requests.get(f"{self.internal_config_url}", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)
        initial_level = response.json().get("log_level")
        print(f"Initial log level: {initial_level}")

        # 2. Change to debug
        target_level = "debug" if initial_level != "debug" else "info"
        print(f"Changing log level to {target_level} via /log-level...")
        response = requests.post(
            f"{self.base_url}/log-level",
            json={"level": target_level},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertEqual(data.get("status"), "success")
        self.assertEqual(data.get("level"), target_level)

        # 3. Verify in config
        response = requests.get(f"{self.internal_config_url}", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json().get("log_level"), target_level)
        print(f"[OK] Log level verified as {target_level} in config")

        # 4. Restore initial level
        print(f"Restoring log level to {initial_level}...")
        requests.post(
            f"{self.base_url}/log-level",
            json={"level": initial_level},
            timeout=TIMEOUT_DEFAULT,
        )

    def test_log_level_via_config_set(self):
        """Test changing log level via the generic /internal/set endpoint (used by CLI)."""
        # 1. Get initial level
        response = requests.get(f"{self.internal_config_url}", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)
        initial_level = response.json().get("log_level")

        # 2. Change to warning via /internal/set
        target_level = "warning" if initial_level != "warning" else "error"
        print(f"Changing log level to {target_level} via /internal/set...")
        response = requests.post(
            f"{self.base_url}/internal/set",
            json={"log_level": target_level},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

        # 3. Verify in config
        response = requests.get(f"{self.internal_config_url}", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json().get("log_level"), target_level)
        print(f"[OK] Log level verified as {target_level} in config via /internal/set")

        # 4. Restore initial level
        requests.post(
            f"{self.base_url}/internal/set",
            json={"log_level": initial_level},
            timeout=TIMEOUT_DEFAULT,
        )

    def test_log_level_critical(self):
        """Test that the 'critical' log level is accepted and reflected."""
        print("Testing log level 'critical'...")
        response = requests.post(
            f"{self.base_url}/log-level",
            json={"level": "critical"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json().get("level"), "critical")

        # Verify in config
        response = requests.get(f"{self.internal_config_url}", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.json().get("log_level"), "critical")
        print("[OK] Log level 'critical' verified")

    def test_log_level_notice(self):
        """Test that the 'notice' log level is accepted and reflected."""
        print("Testing log level 'notice'...")
        response = requests.post(
            f"{self.base_url}/log-level",
            json={"level": "notice"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json().get("level"), "notice")
        print("[OK] Log level 'notice' verified")

    def test_log_level_fatal(self):
        """Test that the 'fatal' log level is accepted and reflected."""
        print("Testing log level 'fatal'...")
        response = requests.post(
            f"{self.base_url}/log-level",
            json={"level": "fatal"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json().get("level"), "fatal")
        print("[OK] Log level 'fatal' verified")

    def test_log_level_none(self):
        """Test that the 'none' log level is accepted and reflected."""
        print("Testing log level 'none'...")
        response = requests.post(
            f"{self.base_url}/log-level",
            json={"level": "none"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json().get("level"), "none")

        # Verify in config
        response = requests.get(f"{self.internal_config_url}", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(response.json().get("log_level"), "none")
        print("[OK] Log level 'none' verified")

    def test_invalid_log_level(self):
        """Test that invalid log levels are rejected."""
        response = requests.post(
            f"{self.base_url}/log-level",
            json={"level": "invalid_level_name"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400)
        print("[OK] Invalid log level rejected with 400")


if __name__ == "__main__":
    run_server_tests(LogLevelTests, "LOG LEVEL TESTS")
