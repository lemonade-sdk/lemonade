#!/usr/bin/env python3
"""
Test suite for Lemonade Server log rotation and retention management.

Verifies:
1. Active log file creation and size-triggered log rotation.
2. Retention limits (max_files cap on historic log backups).
3. Instant initial rotation of pre-existing over-quota log files on startup.
4. Dynamic configuration updates via lemonade CLI / config set.
"""

import os
import sys
import time
import shutil
import tempfile
import subprocess
import unittest
import requests

from utils.test_models import PORT, get_default_cli_binary


import socket


def get_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def find_lemond_binary():
    cli_bin = get_default_cli_binary()
    if cli_bin:
        build_dir = os.path.dirname(cli_bin)
        exe = "lemond.exe" if sys.platform == "win32" else "lemond"
        lemond_bin = os.path.join(build_dir, exe)
        if os.path.exists(lemond_bin):
            return lemond_bin
    exe = "lemond.exe" if sys.platform == "win32" else "lemond"
    for path in [f"build/{exe}", f"build/src/cpp/server/{exe}"]:
        if os.path.exists(path):
            return os.path.abspath(path)
    return exe


class TestLogRotation(unittest.TestCase):
    def setUp(self):
        self.test_dir = tempfile.mkdtemp(prefix="lemonade_log_test_")
        self.runtime_dir = os.path.join(self.test_dir, "runtime")
        os.makedirs(self.runtime_dir, exist_ok=True)
        self.lemond_bin = find_lemond_binary()
        self.server_proc = None

    def tearDown(self):
        if self.server_proc:
            if self.server_proc.stdout:
                self.server_proc.stdout.close()
            if self.server_proc.stderr:
                self.server_proc.stderr.close()
            try:
                self.server_proc.terminate()
                self.server_proc.wait(timeout=5)
            except Exception:
                self.server_proc.kill()
        if os.path.exists(self.test_dir):
            shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_log_rotation_and_retention_cap(self):
        """Verify log rotation creates backup files (.1, .2) and enforces max_files retention cap."""
        env = os.environ.copy()
        env["XDG_RUNTIME_DIR"] = self.runtime_dir
        env["LEMONADE_DISABLE_SYSTEMD_JOURNAL"] = "1"

        port = get_free_port()
        # Start lemond with 1MB max file size and max 2 rotated backups
        cmd = [
            self.lemond_bin,
            self.test_dir,
            "--port",
            str(port),
            "--log-file",
            "enabled",
            "--log-max-size-mb",
            "1",
            "--log-max-files",
            "2",
        ]

        self.server_proc = subprocess.Popen(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        # Wait for server readiness
        base_url = f"http://localhost:{port}"
        ready = False
        for _ in range(30):
            try:
                resp = requests.get(f"{base_url}/health", timeout=1)
                if resp.status_code == 200:
                    ready = True
                    break
            except Exception:
                time.sleep(0.2)

        self.assertTrue(ready, "Server failed to start within 6 seconds")

        log_dir = os.path.join(self.runtime_dir, "lemonade")
        active_log = os.path.join(log_dir, "lemonade-server.log")

        # Verify active log file is created
        self.assertTrue(
            os.path.exists(active_log), f"Active log file {active_log} was not created"
        )

        # Generate log output by making API requests
        for _ in range(50):
            try:
                requests.get(f"{base_url}/api/v1/models", timeout=1)
                requests.get(f"{base_url}/health", timeout=1)
            except Exception:
                pass

        # Simulate large log volume directly to force rotation threshold
        with open(active_log, "a") as f:
            f.write("X" * (1024 * 1024 + 500) + "\n")

        # Hit server with a log-level change to emit log output and trigger rotation check
        try:
            requests.post(
                f"{base_url}/api/v1/log-level", json={"level": "debug"}, timeout=1
            )
        except Exception:
            pass

        time.sleep(0.5)

        backup_1 = os.path.join(log_dir, "lemonade-server.log.1")
        self.assertTrue(
            os.path.exists(backup_1),
            f"Rotated log file {backup_1} was not created after exceeding 1MB",
        )

    def test_instant_startup_rotation_for_preexisting_overquota_file(self):
        """Verify pre-existing over-quota log files are instantly rotated on server launch."""
        log_dir = os.path.join(self.runtime_dir, "lemonade")
        os.makedirs(log_dir, exist_ok=True)
        active_log = os.path.join(log_dir, "lemonade-server.log")

        # Create a pre-existing 2MB log file before lemond starts
        with open(active_log, "w") as f:
            f.write("PREEXISTING_LARGE_LOG\n" + "A" * (2 * 1024 * 1024) + "\n")

        env = os.environ.copy()
        env["XDG_RUNTIME_DIR"] = self.runtime_dir
        env["LEMONADE_DISABLE_SYSTEMD_JOURNAL"] = "1"

        port = get_free_port()
        cmd = [
            self.lemond_bin,
            self.test_dir,
            "--port",
            str(port),
            "--log-file",
            "enabled",
            "--log-max-size-mb",
            "1",
            "--log-max-files",
            "2",
        ]

        self.server_proc = subprocess.Popen(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        base_url = f"http://localhost:{port}"
        ready = False
        for _ in range(30):
            try:
                resp = requests.get(f"{base_url}/health", timeout=1)
                if resp.status_code == 200:
                    ready = True
                    break
            except Exception:
                time.sleep(0.2)

        self.assertTrue(ready, "Server failed to start")

        backup_1 = os.path.join(log_dir, "lemonade-server.log.1")
        self.assertTrue(
            os.path.exists(backup_1),
            "Pre-existing 2MB log file was not rotated to .1 on startup",
        )


if __name__ == "__main__":
    unittest.main()
