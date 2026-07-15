"""Reset mutable Lemonade server state between bundled CI test suites.

The GitHub-hosted CLI/Endpoints jobs intentionally run several unittest modules
against one long-lived server to avoid repeated setup and model downloads. That
makes the job vulnerable to state leakage: a previous module can leave a model
loaded, change the bind host via /internal/set, or otherwise make the next module
fail for reasons unrelated to the code under test.

This helper keeps the shared server model but normalizes the small amount of
state that is known to leak between modules.
"""

import argparse
import os
import sys
import time
from typing import Iterable

import requests

from .test_models import PORT, TIMEOUT_DEFAULT

HOST_CANDIDATES = ("127.0.0.1", "localhost")


def _auth_headers() -> dict:
    api_key = os.environ.get("LEMONADE_API_KEY")
    if api_key:
        return {"Authorization": f"Bearer {api_key}"}
    return {}


def _urls(path: str, port: int) -> Iterable[str]:
    if not path.startswith("/"):
        path = "/" + path
    for host in HOST_CANDIDATES:
        yield f"http://{host}:{port}{path}"


def _request_with_host_fallback(method: str, path: str, *, port: int, **kwargs):
    last_exc = None
    last_response = None

    for url in _urls(path, port):
        try:
            response = requests.request(method, url, **kwargs)
        except requests.RequestException as exc:
            last_exc = exc
            continue

        last_response = response
        if response.status_code < 500:
            return response

    if last_response is not None:
        return last_response
    raise RuntimeError(f"Could not reach Lemonade server at {path}: {last_exc}")


def _wait_for_live(port: int, timeout: int) -> None:
    deadline = time.time() + timeout
    last_error = None

    while time.time() < deadline:
        try:
            response = _request_with_host_fallback(
                "GET",
                "/live",
                port=port,
                timeout=2,
            )
            if response.status_code == 200:
                return
            last_error = f"status={response.status_code}, body={response.text[:500]}"
        except Exception as exc:  # noqa: BLE001 - diagnostic helper
            last_error = str(exc)
        time.sleep(1)

    raise RuntimeError(f"Server did not become live within {timeout}s: {last_error}")


def _print_diagnostics(port: int) -> None:
    for path in ("/live", "/api/v1/health", "/api/ps"):
        try:
            response = _request_with_host_fallback(
                "GET",
                path,
                port=port,
                headers=_auth_headers(),
                timeout=5,
            )
            print(
                f"[reset] GET {path} -> {response.status_code}: "
                f"{response.text[:1200]}"
            )
        except Exception as exc:  # noqa: BLE001 - best-effort diagnostics
            print(f"[reset] GET {path} failed: {exc}")


def _running_models(port: int):
    response = _request_with_host_fallback(
        "GET",
        "/api/ps",
        port=port,
        headers=_auth_headers(),
        timeout=5,
    )
    response.raise_for_status()
    return response.json().get("models", [])


def _wait_for_no_running_models(port: int, timeout: int) -> None:
    deadline = time.time() + timeout
    last_models = []
    last_error = None

    while time.time() < deadline:
        try:
            models = _running_models(port)
            if not models:
                return
            last_models = models
            last_error = None
        except Exception as exc:  # noqa: BLE001 - diagnostic helper
            last_error = str(exc)
        time.sleep(1)

    if last_error:
        raise RuntimeError(f"Could not verify empty model state: {last_error}")
    raise RuntimeError(
        "Server still reports running models after unload: "
        f"{str(last_models)[:1200]}"
    )


def reset_server_state(
    *,
    port: int = PORT,
    restore_host: bool = True,
    unload: bool = True,
    timeout: int = TIMEOUT_DEFAULT,
) -> None:
    _wait_for_live(port, timeout=timeout)

    if restore_host:
        # Use the fallback host list because a previous test may have rebound the
        # server to 0.0.0.0, while localhost can resolve to IPv6 on some runners.
        response = _request_with_host_fallback(
            "POST",
            "/internal/set",
            port=port,
            json={"host": "localhost"},
            headers=_auth_headers(),
            timeout=10,
        )
        response.raise_for_status()
        _wait_for_live(port, timeout=timeout)

    if unload:
        response = _request_with_host_fallback(
            "POST",
            "/api/v1/unload",
            port=port,
            json={},
            headers=_auth_headers(),
            timeout=30,
        )
        # Some server versions return 404 when no model is loaded. That still
        # means the desired postcondition is true. A 200 response can still be
        # asynchronous while a backend is busy, so verify that /api/ps reaches
        # the actual postcondition before declaring reset complete.
        if response.status_code not in (200, 404):
            response.raise_for_status()
        _wait_for_no_running_models(port, timeout=timeout)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Reset Lemonade server state for CI")
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--timeout", type=int, default=TIMEOUT_DEFAULT)
    parser.add_argument("--label", default="")
    parser.add_argument("--no-restore-host", action="store_true")
    parser.add_argument("--no-unload", action="store_true")
    parser.add_argument(
        "--best-effort",
        action="store_true",
        help="Print diagnostics but do not fail if reset cannot complete",
    )
    args = parser.parse_args(argv)

    prefix = f" for {args.label}" if args.label else ""
    print(f"[reset] Resetting Lemonade server state{prefix}")

    try:
        reset_server_state(
            port=args.port,
            restore_host=not args.no_restore_host,
            unload=not args.no_unload,
            timeout=args.timeout,
        )
    except Exception as exc:  # noqa: BLE001 - CLI boundary with diagnostics
        print(f"[reset] Failed to reset Lemonade server state: {exc}", file=sys.stderr)
        _print_diagnostics(args.port)
        return 0 if args.best_effort else 1

    _print_diagnostics(args.port)
    print(f"[reset] Server state reset complete{prefix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
