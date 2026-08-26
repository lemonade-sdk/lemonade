import contextlib
import os
import shutil
import socket
import subprocess
import time
import unittest
import urllib.error
import urllib.request
from typing import Iterator

try:
    import httpx
except ImportError:
    httpx = None

try:
    import requests
except ImportError:
    requests = None

from .test_models import get_default_lemond_binary


def allocate_free_port() -> int:
    """Finds and returns a free ephemeral port on IPv4 loopback.

    Note: Has an inherent bind-close-rebind TOCTOU window acceptable for isolated tests.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def make_clean_env(tmpdir: str) -> dict[str, str]:
    """Returns a clean copy of the environment isolated to the tmpdir."""
    env = os.environ.copy()
    for auth_var in (
        "LEMONADE_API_KEY",
        "LEMONADE_ADMIN_API_KEY",
        "LEMONADE_ALLOWED_ORIGINS",
    ):
        env.pop(auth_var, None)
    for k in list(env.keys()):
        if k.startswith("LEMONADE_") and k.endswith("_API_KEY"):
            env.pop(k, None)
    env["LEMONADE_CACHE_DIR"] = os.path.join(tmpdir, "cache")
    env["LEMONADE_CONFIG_DIR"] = os.path.join(tmpdir, "config")
    env["LEMONADE_MODELS_DIR"] = os.path.join(tmpdir, "models")
    env["HF_HOME"] = os.path.join(tmpdir, "hf_home")
    env["HF_HUB_CACHE"] = os.path.join(tmpdir, "hf_cache")
    return env


def wait_for_http_health(
    port: int,
    host: str = "127.0.0.1",
    timeout: float = 10.0,
    headers: dict[str, str] | None = None,
    proc: subprocess.Popen | None = None,
) -> bool:
    """Polls the /api/v1/health endpoint until HTTP 200 OK or timeout."""
    url = f"http://{host}:{port}/api/v1/health"

    stack = contextlib.ExitStack()
    try:
        if httpx is not None:
            client = stack.enter_context(httpx.Client(timeout=1.0))
            probe = lambda: client.get(url, headers=headers).status_code == 200
            errors = (httpx.RequestError, httpx.HTTPError)
        elif requests is not None:
            probe = (
                lambda: requests.get(url, headers=headers, timeout=1.0).status_code
                == 200
            )
            errors = (requests.RequestException,)
        else:
            req = urllib.request.Request(url, headers=headers or {})

            def probe():
                with urllib.request.urlopen(req, timeout=1.0) as resp:
                    return resp.status == 200

            errors = (urllib.error.URLError, urllib.error.HTTPError, OSError)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if proc is not None and proc.poll() is not None:
                return False
            try:
                if probe():
                    return True
            except errors:
                pass
            time.sleep(0.1)
        return False
    finally:
        stack.close()


def _terminate_proc(p: subprocess.Popen, timeout: float = 10.0):
    if p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait(timeout=timeout)
    for pipe in (p.stdout, p.stderr, p.stdin):
        if pipe is not None and not getattr(pipe, "closed", True):
            try:
                pipe.close()
            except Exception:
                pass


@contextlib.contextmanager
def lemond_server(
    port: int | None = None,
    cache_dir: str | None = None,
    config_dir: str | None = None,
    args: list[str] | None = None,
    env: dict[str, str] | None = None,
    timeout: float = 10.0,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
    text: bool = True,
    binary_path: str | None = None,
    wait_health: bool = False,
    health_timeout: float = 10.0,
    health_headers: dict[str, str] | None = None,
    skip_if_unavailable: bool = False,
) -> Iterator[subprocess.Popen]:
    """Context manager for running a hermetic lemond subprocess.

    Ensures deterministic process teardown upon exit (SIGTERM -> SIGKILL on POSIX,
    TerminateProcess on Windows).
    """
    effective_cache_dir = cache_dir
    if effective_cache_dir is None and env is not None:
        effective_cache_dir = env.get("LEMONADE_CACHE_DIR")

    effective_config_dir = config_dir
    if effective_config_dir is None and env is not None:
        effective_config_dir = env.get("LEMONADE_CONFIG_DIR")

    if (config_dir is not None or effective_config_dir is not None) and (
        cache_dir is None and effective_cache_dir is None
    ):
        raise ValueError("config_dir requires cache_dir to be specified as well")

    if binary_path is not None:
        if not os.path.exists(binary_path):
            if skip_if_unavailable:
                raise unittest.SkipTest(
                    f"lemond binary not found at explicit binary_path: {binary_path}"
                )
            raise RuntimeError(
                f"lemond binary not found at explicit binary_path: {binary_path}"
            )
        lemond_bin = binary_path
    else:
        lemond_bin = get_default_lemond_binary()
        if not lemond_bin or not os.path.exists(lemond_bin):
            lemond_bin = shutil.which("lemond")
            if not lemond_bin:
                if skip_if_unavailable:
                    raise unittest.SkipTest(
                        f"lemond binary not found in build dir ({get_default_lemond_binary()}) or PATH"
                    )
                raise RuntimeError(
                    f"lemond binary not found in build dir ({get_default_lemond_binary()}) or PATH"
                )

    server_port = port if port is not None else allocate_free_port()
    cmd = [lemond_bin]
    if effective_cache_dir is not None:
        cmd.append(effective_cache_dir)
    if effective_config_dir is not None:
        cmd.append(effective_config_dir)
    cmd.extend(["--port", str(server_port)])
    if args:
        cmd.extend(args)

    proc = subprocess.Popen(
        cmd,
        stdout=stdout,
        stderr=stderr,
        env=env,
        text=text,
    )
    setattr(proc, "port", server_port)
    try:
        if wait_health:
            if not wait_for_http_health(
                server_port,
                timeout=health_timeout,
                headers=health_headers,
                proc=proc,
            ):
                exited = proc.poll()
                detail = (
                    f"process exited early with code {exited}"
                    if exited is not None
                    else f"process still running but no 200 within {health_timeout}s"
                )
                raise RuntimeError(
                    f"lemond server failed to become healthy on port {server_port} ({detail})"
                )
        yield proc
    finally:
        _terminate_proc(proc, timeout=timeout)
