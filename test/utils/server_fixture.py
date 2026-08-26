import contextlib
import json
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
    """Create a minimal, isolated environment dict backed by a temporary directory.

    Isolates Lemonade caches/models and Hugging Face home directories to prevent
    host environment leakage.
    """
    env = os.environ.copy()
    for key in (
        "LEMONADE_API_KEY",
        "LEMONADE_ADMIN_API_KEY",
        "LEMONADE_ALLOWED_ORIGINS",
    ):
        env.pop(key, None)
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
    timeout: float = 10.0,
    headers: dict[str, str] | None = None,
    proc: subprocess.Popen | None = None,
    host: str = "127.0.0.1",
) -> bool:
    """Poll the /api/v1/health endpoint until HTTP 200 is returned or timeout expires.

    Returns True if healthy within timeout, False otherwise.
    If `proc` is provided, short-circuits immediately if the process terminates early.
    """
    url = f"http://{host}:{port}/api/v1/health"
    deadline = time.monotonic() + timeout

    with contextlib.ExitStack() as stack:
        if httpx is not None:
            client = stack.enter_context(
                httpx.Client(
                    headers=headers,
                    timeout=1.0,
                    limits=httpx.Limits(max_keepalive_connections=0),
                )
            )

            def _probe():
                return client.get(url).status_code == 200

            err_types = (httpx.TransportError, httpx.TimeoutException)
        elif requests is not None:
            session = stack.enter_context(requests.Session())
            if headers:
                session.headers.update(headers)

            def _probe():
                return session.get(url, timeout=1.0).status_code == 200

            err_types = (
                requests.exceptions.RequestException,
                requests.exceptions.Timeout,
            )
        else:

            def _probe():
                req = urllib.request.Request(url, headers=headers or {})
                with urllib.request.urlopen(req, timeout=1.0) as resp:
                    return resp.getcode() == 200

            err_types = (urllib.error.URLError, TimeoutError, OSError)

        while time.monotonic() < deadline:
            if proc is not None and proc.poll() is not None:
                return False
            try:
                if _probe():
                    return True
            except err_types:
                pass
            time.sleep(0.05)

    return False


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
    config: dict | None = None,
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

    if config is not None:
        target_dir = effective_config_dir or effective_cache_dir
        if target_dir is None:
            raise ValueError(
                "config requires cache_dir, config_dir, or environment variables to be set"
            )
        os.makedirs(target_dir, exist_ok=True)
        payload = (
            {"config_version": 2, **config}
            if "config_version" not in config
            else dict(config)
        )
        with open(os.path.join(target_dir, "config.json"), "w", encoding="utf-8") as f:
            json.dump(payload, f)

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
