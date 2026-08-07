"""
Focused CLI API-key smoke tests.

The API-key CI jobs already verify the server-side HTTP enforcement directly.
This script verifies the CLI client side of the contract without running the
entire persistent CLI suite, which also covers model pulls, registry cleanup,
launch flows, and other unrelated behavior.
"""

import argparse
import os
import shutil
import subprocess
import sys
import traceback

import requests

from utils.test_models import PORT, TIMEOUT_DEFAULT, get_default_cli_binary


def resolve_cli_binary(cli_binary):
    """Resolve the CLI path the same way the broader CLI suite does."""
    if os.path.isabs(cli_binary):
        return cli_binary
    if os.path.sep in cli_binary or (os.path.altsep and os.path.altsep in cli_binary):
        return os.path.abspath(cli_binary)
    return shutil.which(cli_binary) or cli_binary


def run_cli(cli_binary, args, *, env=None, timeout=TIMEOUT_DEFAULT):
    """Run a Lemonade CLI command and echo output for CI diagnostics."""
    cmd = [resolve_cli_binary(cli_binary)] + args
    print(f"Running: {' '.join(cmd)}", flush=True)
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    if result.stdout:
        print(f"stdout:\n{result.stdout}", flush=True)
    if result.stderr:
        print(f"stderr:\n{result.stderr}", flush=True)
    print(f"exit code: {result.returncode}", flush=True)
    return result


def auth_failure_text(output):
    """Return True if CLI output clearly reflects an auth failure."""
    lower = output.lower()
    return any(
        marker in lower
        for marker in (
            "401",
            "unauthorized",
            "unauthorised",
            "api key",
            "authentication",
        )
    )


def assert_cli_succeeds(cli_binary, args, *, env=None):
    result = run_cli(cli_binary, args, env=env)
    if result.returncode != 0:
        raise AssertionError(
            f"Expected command to succeed, got exit code {result.returncode}: "
            f"stdout={result.stdout!r} stderr={result.stderr!r}"
        )
    return result


def assert_cli_auth_fails(cli_binary, args, *, env):
    result = run_cli(cli_binary, args, env=env)
    if result.returncode == 0:
        raise AssertionError(
            "Expected command to fail without the correct API key, but it succeeded: "
            f"stdout={result.stdout!r} stderr={result.stderr!r}"
        )
    output = result.stdout + result.stderr
    if not auth_failure_text(output):
        raise AssertionError(
            "Command failed, but output did not look like an auth failure: "
            f"stdout={result.stdout!r} stderr={result.stderr!r}"
        )
    return result


def assert_health_status(api_key, expected_status, label):
    headers = {}
    if api_key is not None:
        headers["Authorization"] = f"Bearer {api_key}"

    response = requests.get(
        f"http://127.0.0.1:{PORT}/api/v1/health",
        headers=headers,
        timeout=TIMEOUT_DEFAULT,
    )
    print(f"{label}: HTTP {response.status_code}", flush=True)
    if response.status_code != expected_status:
        raise AssertionError(
            f"{label}: expected HTTP {expected_status}, got {response.status_code}; "
            f"body={response.text[:500]!r}"
        )


def main():
    parser = argparse.ArgumentParser(description="Test CLI API-key propagation")
    parser.add_argument(
        "--cli-binary",
        default=get_default_cli_binary(),
        help="Path to lemonade CLI binary",
    )
    args = parser.parse_args()

    api_key = os.environ.get("LEMONADE_API_KEY")
    if not api_key:
        raise AssertionError("LEMONADE_API_KEY must be set for this test")

    cli_binary = args.cli_binary
    base_cli_args = ["--host", "127.0.0.1", "--port", str(PORT)]

    # This intentionally duplicates the workflow curl check so this focused
    # smoke test also fails clearly if server-side auth enforcement regresses.
    print("Verifying server-side API-key enforcement first...", flush=True)
    assert_health_status(None, 401, "no API key")
    assert_health_status("wrong-key", 401, "wrong API key")
    assert_health_status(api_key, 200, "correct API key")

    print("Verifying CLI succeeds when LEMONADE_API_KEY is inherited...", flush=True)
    assert_cli_succeeds(cli_binary, base_cli_args + ["status"])
    assert_cli_succeeds(cli_binary, base_cli_args + ["list"])

    print("Verifying CLI fails without the API key...", flush=True)
    no_key_env = os.environ.copy()
    no_key_env.pop("LEMONADE_API_KEY", None)
    no_key_env.pop("LEMONADE_ADMIN_API_KEY", None)
    assert_cli_auth_fails(cli_binary, base_cli_args + ["status"], env=no_key_env)

    print("Verifying CLI fails with the wrong API key...", flush=True)
    wrong_key_env = os.environ.copy()
    wrong_key_env.pop("LEMONADE_ADMIN_API_KEY", None)
    wrong_key_env["LEMONADE_API_KEY"] = "wrong-key"
    assert_cli_auth_fails(cli_binary, base_cli_args + ["status"], env=wrong_key_env)

    print("CLI API-key smoke tests passed.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr, flush=True)
        sys.exit(1)
    except Exception:
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)
