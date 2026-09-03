"""
Regression test for the tray's OS-native server supervisor watchdog.

The tray spawns `lemond` with LEMONADE_WATCHDOG_FD pointing at the read end of
a pipe, keeping the write end open only in the tray process. When the tray
exits for any reason (even SIGKILL) the OS closes the write end, so lemond's
blocking read() returns EOF and it shuts itself down. This simulates that
contract directly: spawn lemond with only the read end inherited, then close
the write end (as the OS would on parent death) and assert lemond exits.

Runs standalone (no inference backend needed). POSIX-only.
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from utils.test_models import get_default_lemond_binary


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


def test_watchdog_shutdown():
    lemond = _resolve_lemond()
    port = _free_port()
    read_fd, write_fd = os.pipe()
    proc = None
    try:
        with tempfile.TemporaryDirectory() as cache:
            proc = subprocess.Popen(
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
                pass_fds=(read_fd,),
            )

            # Parent no longer needs its own read-end copy.
            os.close(read_fd)
            read_fd = -1

            if not _wait_reachable(port, proc):
                sys.exit(f"[FAIL] Spawned lemond did not become reachable on {port}")

            if proc.poll() is not None:
                sys.exit("[FAIL] lemond exited while the watchdog pipe was still open")

            # Simulate the tray dying: the OS closes the write end of the pipe.
            os.close(write_fd)
            write_fd = -1

            deadline = time.time() + 15
            while time.time() < deadline and proc.poll() is None:
                time.sleep(0.2)

            if proc.poll() is None:
                proc.kill()
                sys.exit(
                    "[FAIL] lemond did not shut down after the watchdog write end closed"
                )

            print("[PASS] Tray supervisor watchdog terminated lemond on parent death.")
    finally:
        if read_fd >= 0:
            os.close(read_fd)
        if write_fd >= 0:
            os.close(write_fd)
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()


def test_watchdog_cloexec_prevents_writer_leak():
    """Verify that child processes spawned by the parent (e.g. desktop app / browser)
    do not keep the watchdog write-end alive if close-on-exec (FD_CLOEXEC) is set."""
    lemond = _resolve_lemond()
    port = _free_port()
    read_fd, write_fd = os.pipe()
    proc = None
    child_proc = None
    try:
        with tempfile.TemporaryDirectory() as cache:
            proc = subprocess.Popen(
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
                pass_fds=(read_fd,),
            )
            os.close(read_fd)
            read_fd = -1

            if not _wait_reachable(port, proc):
                sys.exit(f"[FAIL] Spawned lemond did not become reachable on {port}")

            # Spawn a simulated external helper (e.g. sleep / browser) with close_fds=True (CLOEXEC behavior)
            child_proc = subprocess.Popen(
                ["sleep", "30"],
                close_fds=True,
            )

            # Parent closes write_fd (simulating tray exit while external helper is still running)
            os.close(write_fd)
            write_fd = -1

            # lemond MUST exit because child_proc did not inherit write_fd
            deadline = time.time() + 15
            while time.time() < deadline and proc.poll() is None:
                time.sleep(0.2)

            if proc.poll() is None:
                proc.kill()
                sys.exit(
                    "[FAIL] lemond stayed alive because a spawned process inherited the watchdog writer"
                )

            print("[PASS] Watchdog writer is not leaked to spawned helper processes.")
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


def _resolve_lemond():
    default_build = get_default_lemond_binary()
    lemond = (
        os.environ.get("LEMONADE_LEMOND_BINARY")
        or (default_build if os.path.exists(default_build) else None)
        or shutil.which("lemond")
    )
    if not lemond or not os.path.exists(lemond):
        print(
            f"lemond binary not found at {lemond or default_build}",
            file=sys.stderr,
        )
        sys.exit(1)
    return lemond


def test_path_based_lemond_spawn():
    """Verify that lemond can be found and executed when resolved purely via PATH."""
    lemond = _resolve_lemond()
    port = _free_port()
    read_fd, write_fd = os.pipe()
    proc = None
    try:
        with tempfile.TemporaryDirectory() as cache:
            lemond_dir = os.path.dirname(os.path.abspath(lemond))
            env = os.environ.copy()
            env["PATH"] = f"{lemond_dir}:{env.get('PATH', '')}"
            env["LEMONADE_WATCHDOG_FD"] = str(read_fd)

            # Invoke with bare "lemond" on PATH (testing posix_spawnp behavior)
            proc = subprocess.Popen(
                [
                    "lemond",
                    cache,
                    "--port",
                    str(port),
                    "--host",
                    "127.0.0.1",
                    "--no-broadcast",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=env,
                pass_fds=(read_fd,),
            )
            os.close(read_fd)
            read_fd = -1

            if not _wait_reachable(port, proc):
                sys.exit(
                    f"[FAIL] PATH-spawned lemond did not become reachable on {port}"
                )

            os.close(write_fd)
            write_fd = -1

            deadline = time.time() + 15
            while time.time() < deadline and proc.poll() is None:
                time.sleep(0.2)

            if proc.poll() is None:
                proc.kill()
                sys.exit("[FAIL] PATH-spawned lemond did not exit on watchdog EOF")

            print("[PASS] PATH-based lemond discovery and execution succeeded.")
    finally:
        if read_fd >= 0:
            os.close(read_fd)
        if write_fd >= 0:
            os.close(write_fd)
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()


def run_test():
    if os.name == "nt":
        print("[SKIP] POSIX pipe watchdog not applicable on Windows", file=sys.stderr)
        sys.exit(0)

    test_watchdog_shutdown()
    test_watchdog_cloexec_prevents_writer_leak()
    test_path_based_lemond_spawn()


if __name__ == "__main__":
    run_test()
