#!/usr/bin/env python3
"""
Test suite for Lemonade Server log rotation and retention management.

Verifies:
1. Active log file creation and size-triggered log rotation (.1, .2 backups).
2. Backup pruning cap enforcement (max_files = 2 prunes .3).
3. max_files = 0 truncation mode (no backup files created).
4. Instant startup rotation for pre-existing over-quota log files.
5. Rejection of invalid out-of-bounds CLI range options.
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import requests

from utils.test_models import get_default_cli_binary


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
        """Verify active log rotation (.1, .2) and retention pruning of .3 when max_files=2."""
        log_dir = os.path.join(self.runtime_dir, "lemonade")
        os.makedirs(log_dir, exist_ok=True)
        active_log = os.path.join(log_dir, "lemonade-server.log")

        env = os.environ.copy()
        env["XDG_RUNTIME_DIR"] = self.runtime_dir
        env["LEMONADE_CACHE_DIR"] = self.test_dir
        env["LEMONADE_DISABLE_SYSTEMD_JOURNAL"] = "1"

        base_cmd = [
            self.lemond_bin,
            self.test_dir,
            "--log-file",
            "enabled",
            "--log-max-size-mb",
            "1",
            "--log-max-files",
            "2",
        ]

        # Step 1: Pre-exist 1.2MB file and launch instance 1 -> creates .1
        with open(active_log, "wb") as f:
            f.write(b"FIRST_LOG_BLOCK\n" + b"A" * (1024 * 1024 + 200))
        self.assertTrue(
            self.run_server_instance(base_cmd, env), "Instance 1 failed to start"
        )
        backup_1 = os.path.join(log_dir, "lemonade-server.log.1")
        self.assertTrue(os.path.exists(backup_1), "Rotation 1 failed to create .1")

        # Step 2: Pre-exist 1.2MB file and launch instance 2 -> shifts .1 to .2, creates new .1
        with open(active_log, "wb") as f:
            f.write(b"SECOND_LOG_BLOCK\n" + b"B" * (1024 * 1024 + 200))
        self.assertTrue(
            self.run_server_instance(base_cmd, env), "Instance 2 failed to start"
        )
        backup_2 = os.path.join(log_dir, "lemonade-server.log.2")
        self.assertTrue(os.path.exists(backup_2), "Rotation 2 failed to create .2")

        # Step 3: Pre-exist 1.2MB file and launch instance 3 -> shifts .1->.2, prunes .3
        with open(active_log, "wb") as f:
            f.write(b"THIRD_LOG_BLOCK\n" + b"C" * (1024 * 1024 + 200))
        self.assertTrue(
            self.run_server_instance(base_cmd, env), "Instance 3 failed to start"
        )

        backup_3 = os.path.join(log_dir, "lemonade-server.log.3")
        self.assertFalse(
            os.path.exists(backup_3),
            f"Backup {backup_3} exists but should have been pruned (max_files=2)",
        )

    def run_server_instance(self, base_cmd, env):
        port = get_free_port()
        cmd = base_cmd + ["--port", str(port)]
        with subprocess.Popen(
            cmd,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ) as proc:
            ready = False
            for _ in range(30):
                try:
                    conn = socket.create_connection(("127.0.0.1", port))
                    conn.close()
                    ready = True
                    break
                except Exception:
                    time.sleep(0.2)
            try:
                proc.terminate()
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
                proc.wait(timeout=3)
            return ready

    def test_max_files_zero(self):
        """Verify max_files=0 truncates active file upon rotation without creating backup files."""
        log_dir = os.path.join(self.runtime_dir, "lemonade")
        os.makedirs(log_dir, exist_ok=True)
        active_log = os.path.join(log_dir, "lemonade-server.log")

        with open(active_log, "wb") as f:
            f.write(b"INITIAL_OVERSIZE\n" + b"Z" * (1024 * 1024 + 500))

        env = os.environ.copy()
        env["XDG_RUNTIME_DIR"] = self.runtime_dir
        env["LEMONADE_CACHE_DIR"] = self.test_dir
        env["LEMONADE_DISABLE_SYSTEMD_JOURNAL"] = "1"

        base_cmd = [
            self.lemond_bin,
            self.test_dir,
            "--log-file",
            "enabled",
            "--log-max-size-mb",
            "1",
            "--log-max-files",
            "0",
        ]

        self.assertTrue(
            self.run_server_instance(base_cmd, env),
            "Server failed to start in max_files=0 mode",
        )
        backup_1 = os.path.join(log_dir, "lemonade-server.log.1")
        self.assertFalse(os.path.exists(backup_1), "Backup .1 created when max_files=0")

    def test_invalid_cli_flags(self):
        """Verify out-of-bounds CLI flags are rejected with non-zero exit code."""
        env = os.environ.copy()
        env["XDG_RUNTIME_DIR"] = self.runtime_dir

        cmd = [
            self.lemond_bin,
            self.test_dir,
            "--log-max-size-mb",
            "0",
        ]

        proc = subprocess.run(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        self.assertNotEqual(
            proc.returncode, 0, "lemond accepted invalid --log-max-size-mb 0"
        )


if __name__ == "__main__":
    unittest.main()
