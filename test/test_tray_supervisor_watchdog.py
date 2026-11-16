"""
Regression tests for the tray's OS-native server supervisor watchdog.

The tray spawns `lemond` with `--watchdog-fd <FD>` pointing at the read end of
a pipe, keeping the write end open only in the tray process. When the tray
exits for any reason (even SIGKILL) the OS closes the write end, so lemond's
blocking read() returns EOF and it shuts itself down. These tests simulate that
contract directly: spawn lemond with only the read end inherited, then close
the write end (as the OS would on parent death) and assert lemond exits.

Runs standalone (no inference backend needed). POSIX-only.
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

from utils.test_models import get_default_lemond_binary

IS_WINDOWS = os.name == "nt"

_LEMOND_BINARY = None


def parse_args():
    global _LEMOND_BINARY
    parser = argparse.ArgumentParser(description="Tray supervisor watchdog tests")
    parser.add_argument("--lemond-binary", type=str, default=None)
    args, remaining = parser.parse_known_args()
    _LEMOND_BINARY = args.lemond_binary
    return remaining


def _free_port():
    with socket.socket() as s:
        s.bind(("", 0))
        return s.getsockname()[1]


def _wait_reachable(port, proc, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def _resolve_lemond():
    if _LEMOND_BINARY:
        return _LEMOND_BINARY
    default_build = get_default_lemond_binary()
    lemond = (default_build if os.path.exists(default_build) else None) or shutil.which(
        "lemond"
    )
    if not lemond or not os.path.exists(lemond):
        raise FileNotFoundError(
            f"lemond binary not found at {lemond or default_build}; "
            "pass --lemond-binary"
        )
    return lemond


@unittest.skipIf(IS_WINDOWS, "pipe watchdog is not applicable on Windows")
class TraySupervisorWatchdogTest(unittest.TestCase):
    def _spawn_with_watchdog(self, lemond, cache, port, read_fd, env=None):
        return subprocess.Popen(
            [
                lemond,
                cache,
                "--port",
                str(port),
                "--host",
                "127.0.0.1",
                "--no-broadcast",
                f"--watchdog-fd={read_fd}",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=env,
            pass_fds=(read_fd,),
        )

    def _wait_for_exit(self, proc, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.2)
        return proc.poll() is not None

    def test_watchdog_shutdown(self):
        lemond = _resolve_lemond()
        port = _free_port()
        read_fd, write_fd = os.pipe()
        proc = None
        try:
            with tempfile.TemporaryDirectory() as cache:
                proc = self._spawn_with_watchdog(lemond, cache, port, read_fd)

                # Parent no longer needs its own read-end copy.
                os.close(read_fd)
                read_fd = -1

                self.assertTrue(
                    _wait_reachable(port, proc),
                    f"Spawned lemond did not become reachable on {port}",
                )
                self.assertIsNone(
                    proc.poll(), "lemond exited while the watchdog pipe was open"
                )

                # Simulate the tray dying: the OS closes the write end of the pipe.
                os.close(write_fd)
                write_fd = -1
                self.assertTrue(
                    self._wait_for_exit(proc),
                    "lemond did not shut down after the watchdog write end closed",
                )
        finally:
            if read_fd >= 0:
                os.close(read_fd)
            if write_fd >= 0:
                os.close(write_fd)
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()

    def test_watchdog_writer_inheritance(self):
        """An inherited watchdog writer keeps lemond alive until it is released.

        The tray sets FD_CLOEXEC on its own writer so helpers it launches cannot
        inherit it. This pins the pipe semantics that guarantee relies on: while
        any process holds the writer, lemond must stay up; once the last writer
        closes, it must shut down.
        """
        lemond = _resolve_lemond()
        port = _free_port()
        read_fd, write_fd = os.pipe()
        proc = None
        child_proc = None
        try:
            with tempfile.TemporaryDirectory() as cache:
                proc = self._spawn_with_watchdog(lemond, cache, port, read_fd)
                os.close(read_fd)
                read_fd = -1

                self.assertTrue(
                    _wait_reachable(port, proc),
                    f"Spawned lemond did not become reachable on {port}",
                )

                # The helper inherits the writer, so closing the parent's own copy
                # does not produce EOF.
                child_proc = subprocess.Popen(["sleep", "30"], pass_fds=(write_fd,))
                os.close(write_fd)
                write_fd = -1
                self.assertFalse(
                    self._wait_for_exit(proc, timeout=3),
                    "lemond exited while a helper still held the watchdog writer",
                )

                # Releasing the last writer must shut lemond down.
                child_proc.kill()
                child_proc.wait()
                child_proc = None
                self.assertTrue(
                    self._wait_for_exit(proc),
                    "lemond did not exit once the last watchdog writer closed",
                )
        finally:
            if read_fd >= 0:
                os.close(read_fd)
            if write_fd >= 0:
                os.close(write_fd)
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()
            if child_proc is not None and child_proc.poll() is None:
                child_proc.kill()
                child_proc.wait()

    def test_path_based_lemond_spawn(self):
        """lemond resolves and runs when invoked by bare name from PATH."""
        lemond = _resolve_lemond()
        port = _free_port()
        read_fd, write_fd = os.pipe()
        proc = None
        try:
            with tempfile.TemporaryDirectory() as cache:
                lemond_dir = os.path.dirname(os.path.abspath(lemond))
                env = os.environ.copy()
                env["PATH"] = f"{lemond_dir}:{env.get('PATH', '')}"

                proc = self._spawn_with_watchdog(
                    "lemond", cache, port, read_fd, env=env
                )
                os.close(read_fd)
                read_fd = -1

                self.assertTrue(
                    _wait_reachable(port, proc),
                    f"PATH-spawned lemond did not become reachable on {port}",
                )

                os.close(write_fd)
                write_fd = -1
                self.assertTrue(
                    self._wait_for_exit(proc),
                    "PATH-spawned lemond did not exit on watchdog EOF",
                )
        finally:
            if read_fd >= 0:
                os.close(read_fd)
            if write_fd >= 0:
                os.close(write_fd)
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    remaining = parse_args()
    sys.argv = [sys.argv[0]] + remaining
    unittest.main(verbosity=2)
