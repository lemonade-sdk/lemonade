"""
Tests for FLM status detection with mock hardware and mock FLM executable.

Validates the 5 FLM states: unsupported, installable, update_required,
action_required, installed — using mock hardware_info.json and LEMONADE_MOCK_FLM_PATH.

Tests each state against all relevant API actions:
  A. GET /api/v1/system-info      — state/message/action fields
  B. GET /api/v1/models?show_all  — FLM model visibility
  C. POST /api/v1/load            — FLM error hints
  D. POST /api/v1/pull            — FLM readiness guard
  E. POST /api/v1/install         — backend install behavior

Usage:
    python test/test_flm_status.py
    python test/test_flm_status.py --server-binary /path/to/lemonade-server
"""

import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import argparse
from contextlib import contextmanager

try:
    import requests
except ImportError as e:
    raise ImportError("You must `pip install requests` to run this test", e)

from utils.test_models import PORT, TIMEOUT_DEFAULT, get_default_server_binary
from utils.server_base import wait_for_server

IS_WINDOWS = sys.platform == "win32"
IS_LINUX = sys.platform.startswith("linux")
IS_X86_64 = platform.machine().lower() in ("x86_64", "amd64")

BASE_URL = f"http://localhost:{PORT}/api/v1"

# The required FLM version from backend_versions.json — read dynamically
REQUIRED_FLM_VERSION = None

# Model name that the "installed" mock's flm list returns
# flm list returns {"models":[{"name":"test-model:1b",...}]}
# model_manager turns "test-model:1b" into "test-model-1b-FLM"
MOCK_FLM_MODEL_NAME = "test-model-1b-FLM"

# A model name that will never exist in the registry
FAKE_FLM_MODEL = "nonexistent-model-FLM"


def get_server_version(server_binary: str) -> str:
    try:
        result = subprocess.run(
            [server_binary, "--version"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        output = result.stdout.strip()
        if "version" in output.lower():
            parts = output.split()
            for part in parts:
                if part[0].isdigit():
                    return part
        return output
    except Exception:
        return "9.0.0"


def stop_server(server_binary):
    try:
        subprocess.run(
            [server_binary, "stop"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        time.sleep(2)
    except Exception:
        pass


# Hardware config with XDNA2 NPU
NPU_HARDWARE = {
    "OS Version": (
        "Windows 11 Pro 10.0.22631"
        if IS_WINDOWS
        else "Linux-6.5.0-generic Ubuntu 22.04.3 LTS"
    ),
    "Processor": "AMD Ryzen AI 9 HX 370",
    "Physical Memory": "32 GB",
    "devices": {
        "cpu": {
            "name": "AMD Ryzen AI 9 HX 370",
            "cores": 12,
            "threads": 24,
            "available": True,
            "family": "x86_64",
        },
        "amd_igpu": {
            "name": "AMD Radeon 890M",
            "vram_gb": 8.0,
            "available": True,
            "family": "gfx1150",
        },
        "amd_dgpu": [],
        "nvidia_dgpu": [],
        "amd_npu": {
            "name": "AMD Ryzen AI 9 HX 370",
            "available": True,
            "power_mode": "default",
            "family": "XDNA2",
        },
    },
}

# Hardware without NPU
NO_NPU_HARDWARE = {
    "OS Version": (
        "Windows 11 Pro 10.0.22631"
        if IS_WINDOWS
        else "Linux-6.5.0-generic Ubuntu 22.04.3 LTS"
    ),
    "Processor": "Intel Core i9-13900K",
    "Physical Memory": "32 GB",
    "devices": {
        "cpu": {
            "name": "Intel Core i9-13900K",
            "cores": 24,
            "threads": 32,
            "available": True,
            "family": "x86_64",
        },
        "amd_igpu": {"name": "None", "available": False, "family": ""},
        "amd_dgpu": [],
        "nvidia_dgpu": [],
        "amd_npu": {"name": "None", "available": False, "family": ""},
    },
}


def create_mock_flm_script(
    directory,
    version="0.9.35",
    validate_ready=True,
    list_json='{"models":[]}',
    broken_version=False,
):
    """Create a mock FLM executable script that returns controlled output.

    Args:
        directory: Directory to create the script in.
        version: Version string to return from 'version --json'.
        validate_ready: Whether 'validate --json' should return ready=true.
        list_json: JSON string to return from 'list --json'.
        broken_version: If True, simulate old FLM that doesn't understand --json
                        (returns error text instead of JSON for all commands).
    """
    if IS_WINDOWS:
        script_path = os.path.join(directory, "mock_flm.cmd")
        if broken_version:
            content = """@echo off
echo Error parsing arguments: unrecognised option '--json'
exit /b 1
"""
        else:
            content = f"""@echo off
setlocal EnableDelayedExpansion
set "MOCK_VERSION={version}"
set "MOCK_VALIDATE_READY={'true' if validate_ready else 'false'}"

if "%1"=="version" (
    echo {{"version": "%MOCK_VERSION%"}}
    exit /b 0
)
if "%1"=="validate" (
    if "%MOCK_VALIDATE_READY%"=="true" (
        echo {{"ready": true, "amd_device_found": true, "all_fw_ok": true, "kernel_ok": true, "memlock_ok": true, "npu_driver_ok": true}}
        exit /b 0
    ) else (
        echo {{"ready": false, "amd_device_found": true, "all_fw_ok": true, "kernel_ok": true, "memlock_ok": true, "npu_driver_ok": false}}
        exit /b 1
    )
)
if "%1"=="list" (
    echo {list_json}
    exit /b 0
)
exit /b 0
"""
    else:
        script_path = os.path.join(directory, "mock_flm.sh")
        if broken_version:
            content = """#!/bin/bash
echo "Error parsing arguments: unrecognised option '--json'"
exit 1
"""
        else:
            content = f"""#!/bin/bash
MOCK_VERSION="{version}"
MOCK_VALIDATE_READY="{'true' if validate_ready else 'false'}"

case "$1" in
    version) echo '{{"version": "'"$MOCK_VERSION"'"}}' ;;
    validate)
        if [ "$MOCK_VALIDATE_READY" = "true" ]; then
            echo '{{"ready":true,"amd_device_found":true,"all_fw_ok":true,"kernel_ok":true,"memlock_ok":true,"npu_driver_ok":true}}'
        else
            echo '{{"ready":false,"amd_device_found":true,"all_fw_ok":true,"kernel_ok":true,"memlock_ok":true,"npu_driver_ok":false}}'
            exit 1
        fi ;;
    list)
        if [ "$2" = "--json" ]; then
            echo '{list_json}'
        fi ;;
esac
"""

    with open(script_path, "w", newline=("\r\n" if IS_WINDOWS else "\n")) as f:
        f.write(content)

    if not IS_WINDOWS:
        os.chmod(script_path, os.stat(script_path).st_mode | stat.S_IEXEC)

    return script_path


class FlmStatusTests(unittest.TestCase):
    """Comprehensive tests for FLM status detection and API behavior."""

    @classmethod
    def setUpClass(cls):
        parser = argparse.ArgumentParser(description="Test FLM status detection")
        parser.add_argument(
            "--server-binary",
            type=str,
            default=get_default_server_binary(),
            help="Path to server binary",
        )
        args, _ = parser.parse_known_args()
        cls.server_binary = args.server_binary
        cls.server_version = get_server_version(cls.server_binary)
        print(f"[SETUP] Using server version: {cls.server_version}")

        # Read required FLM version from backend_versions.json
        global REQUIRED_FLM_VERSION
        try:
            server_dir = os.path.dirname(cls.server_binary)
            bv_path = os.path.join(server_dir, "resources", "backend_versions.json")
            with open(bv_path) as f:
                bv = json.load(f)
            REQUIRED_FLM_VERSION = bv.get("flm", {}).get("npu", "v0.9.35")
        except Exception:
            REQUIRED_FLM_VERSION = "v0.9.35"
        print(f"[SETUP] Required FLM version: {REQUIRED_FLM_VERSION}")

        stop_server(cls.server_binary)

    # ------------------------------------------------------------------ #
    #  Server lifecycle context manager
    # ------------------------------------------------------------------ #

    @contextmanager
    def _server(self, hardware, mock_flm_path=None):
        """Start server with mock hardware + optional mock FLM.

        Yields nothing; test code makes API calls directly while inside the
        context.  Server is stopped and temp dirs cleaned up on exit.
        """
        temp_cache_dir = tempfile.mkdtemp(prefix="lemonade_flm_test_")
        process = None

        try:
            # Write mock hardware_info.json
            cache_file = os.path.join(temp_cache_dir, "hardware_info.json")
            cache_data = {
                "version": self.server_version,
                "hardware": hardware,
            }
            with open(cache_file, "w") as f:
                json.dump(cache_data, f, indent=2)

            env = os.environ.copy()
            env["LEMONADE_CACHE_DIR"] = temp_cache_dir
            env.pop("LEMONADE_CI_MODE", None)

            if mock_flm_path is not None:
                env["LEMONADE_MOCK_FLM_PATH"] = mock_flm_path
            else:
                env.pop("LEMONADE_MOCK_FLM_PATH", None)

            cmd = [self.server_binary, "serve", "--no-tray", "--log-level", "debug"]
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=env,
            )

            wait_for_server()
            yield

        finally:
            # Stop server
            try:
                subprocess.run(
                    [self.server_binary, "stop"],
                    capture_output=True,
                    text=True,
                    timeout=30,
                    env=os.environ.copy(),
                )
                time.sleep(2)
            except Exception:
                pass
            if process:
                try:
                    process.terminate()
                    process.wait(timeout=5)
                except Exception:
                    try:
                        process.kill()
                    except Exception:
                        pass
            shutil.rmtree(temp_cache_dir, ignore_errors=True)

    # ------------------------------------------------------------------ #
    #  API helpers
    # ------------------------------------------------------------------ #

    def _get_system_info(self):
        r = requests.get(f"{BASE_URL}/system-info", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(r.status_code, 200)
        return r.json()

    def _get_flm_npu(self, data):
        self.assertIn("recipes", data)
        self.assertIn("flm", data["recipes"])
        self.assertIn("backends", data["recipes"]["flm"])
        self.assertIn("npu", data["recipes"]["flm"]["backends"])
        return data["recipes"]["flm"]["backends"]["npu"]

    def _get_models(self, show_all=True):
        url = f"{BASE_URL}/models"
        if show_all:
            url += "?show_all=true"
        r = requests.get(url, timeout=TIMEOUT_DEFAULT)
        self.assertEqual(r.status_code, 200)
        return r.json()

    def _get_flm_model_names(self, show_all=True):
        """Return list of model names ending in -FLM from the models endpoint."""
        data = self._get_models(show_all=show_all)
        models = data.get("data", data.get("models", []))
        return [m["id"] for m in models if m.get("id", "").endswith("-FLM")]

    def _post_load(self, model_name):
        r = requests.post(
            f"{BASE_URL}/load",
            json={"model_name": model_name},
            timeout=TIMEOUT_DEFAULT,
        )
        return r

    def _post_pull(self, model_name):
        r = requests.post(
            f"{BASE_URL}/pull",
            json={"model": model_name},
            timeout=TIMEOUT_DEFAULT,
        )
        return r

    def _post_install(self, recipe, backend):
        r = requests.post(
            f"{BASE_URL}/install",
            json={"recipe": recipe, "backend": backend},
            timeout=TIMEOUT_DEFAULT,
        )
        return r

    # ------------------------------------------------------------------ #
    #  Scenario 1: unsupported (no NPU hardware)
    # ------------------------------------------------------------------ #

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_unsupported_system_info(self):
        """No XDNA2 NPU -> state=unsupported, empty action."""
        with self._server(NO_NPU_HARDWARE):
            data = self._get_system_info()
            npu = self._get_flm_npu(data)

            self.assertEqual(npu["state"], "unsupported")
            self.assertEqual(npu["action"], "")
            print(f"  [OK] unsupported system-info: {npu['message']}")

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_unsupported_models(self):
        """No XDNA2 NPU -> no FLM models visible."""
        with self._server(NO_NPU_HARDWARE):
            flm_models = self._get_flm_model_names()
            self.assertEqual(
                flm_models, [], f"Expected no FLM models, got {flm_models}"
            )
            print("  [OK] unsupported models: no FLM models listed")

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_unsupported_load(self):
        """No XDNA2 NPU -> loading -FLM model gives not-found with FLM hint."""
        with self._server(NO_NPU_HARDWARE):
            r = self._post_load(FAKE_FLM_MODEL)
            self.assertIn(r.status_code, (400, 404, 500))
            body = r.json()
            error_msg = json.dumps(body).lower()
            self.assertIn(
                "not found", error_msg, f"Expected 'not found' in error: {body}"
            )
            self.assertIn("flm", error_msg, f"Expected FLM hint in error: {body}")
            print(f"  [OK] unsupported load: got FLM hint in error")

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_unsupported_pull(self):
        """No XDNA2 NPU -> pulling -FLM model fails (model not registered)."""
        with self._server(NO_NPU_HARDWARE):
            r = self._post_pull(FAKE_FLM_MODEL)
            self.assertIn(r.status_code, (400, 404, 500))
            print(f"  [OK] unsupported pull: status={r.status_code}")

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_unsupported_install(self):
        """No XDNA2 NPU -> install flm:npu fails with unsupported error."""
        with self._server(NO_NPU_HARDWARE):
            r = self._post_install("flm", "npu")
            self.assertEqual(r.status_code, 500)
            body = r.json()
            error_msg = body.get("error", "").lower()
            self.assertIn(
                "not supported", error_msg, f"Expected 'not supported' in error: {body}"
            )
            print(f"  [OK] unsupported install: {body.get('error', '')[:80]}")

    # ------------------------------------------------------------------ #
    #  Scenario 2: installable (NPU present, no FLM binary)
    # ------------------------------------------------------------------ #

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installable_system_info(self):
        """NPU present, no FLM binary -> state=installable."""
        with self._server(NPU_HARDWARE, mock_flm_path="none"):
            data = self._get_system_info()
            npu = self._get_flm_npu(data)

            self.assertEqual(
                npu["state"],
                "installable",
                f"Expected installable, got {npu['state']}: {npu}",
            )
            self.assertIn("not installed", npu["message"].lower())
            self.assertNotEqual(npu["action"], "")
            print(f"  [OK] installable system-info: {npu['message']}")

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installable_models(self):
        """NPU present, no FLM binary -> no FLM models visible."""
        with self._server(NPU_HARDWARE, mock_flm_path="none"):
            flm_models = self._get_flm_model_names()
            self.assertEqual(
                flm_models, [], f"Expected no FLM models, got {flm_models}"
            )
            print("  [OK] installable models: no FLM models listed")

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installable_load(self):
        """NPU present, no FLM -> loading -FLM model gives not-found with FLM hint."""
        with self._server(NPU_HARDWARE, mock_flm_path="none"):
            r = self._post_load(FAKE_FLM_MODEL)
            self.assertIn(r.status_code, (400, 404, 500))
            body = r.json()
            error_msg = json.dumps(body).lower()
            self.assertIn("not found", error_msg)
            self.assertIn(
                "not installed", error_msg, f"Expected 'not installed' FLM hint: {body}"
            )
            print(f"  [OK] installable load: got FLM hint in error")

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installable_pull(self):
        """NPU present, no FLM -> pulling -FLM model fails."""
        with self._server(NPU_HARDWARE, mock_flm_path="none"):
            r = self._post_pull(FAKE_FLM_MODEL)
            self.assertIn(r.status_code, (400, 404, 500))
            print(f"  [OK] installable pull: status={r.status_code}")

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installable_install(self):
        """NPU present, no FLM -> install flm:npu attempts installation (fails in CI)."""
        with self._server(NPU_HARDWARE, mock_flm_path="none"):
            r = self._post_install("flm", "npu")
            # On Windows, tries to download real installer (will fail in CI).
            # On Linux, throws "only supported on Windows".
            # Either way, we get 500.
            self.assertEqual(r.status_code, 500)
            body = r.json()
            error_msg = body.get("error", "").lower()
            # Should NOT say "not supported" (that's the unsupported case)
            self.assertNotIn(
                "not supported on this system",
                error_msg,
                f"installable should not get 'not supported': {body}",
            )
            print(f"  [OK] installable install: status=500 (expected in CI)")

    # ------------------------------------------------------------------ #
    #  Scenario 3: update_required (parseable old version)
    # ------------------------------------------------------------------ #

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_update_required_old_version_system_info(self):
        """NPU present, FLM v0.9.20 (old) -> state=update_required."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = create_mock_flm_script(
                temp_dir, version="0.9.20", validate_ready=True
            )
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                data = self._get_system_info()
                npu = self._get_flm_npu(data)

                self.assertEqual(
                    npu["state"],
                    "update_required",
                    f"Expected update_required, got {npu['state']}: {npu}",
                )
                self.assertIn("requires", npu["message"].lower())
                self.assertNotEqual(npu["action"], "")
                print(f"  [OK] update_required (old ver) system-info: {npu['message']}")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_update_required_old_version_models(self):
        """NPU present, FLM v0.9.20 -> no FLM models visible."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = create_mock_flm_script(
                temp_dir, version="0.9.20", validate_ready=True
            )
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                flm_models = self._get_flm_model_names()
                self.assertEqual(
                    flm_models, [], f"Expected no FLM models, got {flm_models}"
                )
                print("  [OK] update_required (old ver) models: no FLM models listed")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_update_required_old_version_load(self):
        """NPU present, FLM v0.9.20 -> load -FLM model gives not-found with requires hint."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = create_mock_flm_script(
                temp_dir, version="0.9.20", validate_ready=True
            )
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                r = self._post_load(FAKE_FLM_MODEL)
                self.assertIn(r.status_code, (400, 404, 500))
                body = r.json()
                error_msg = json.dumps(body).lower()
                self.assertIn("not found", error_msg)
                self.assertIn(
                    "requires", error_msg, f"Expected 'requires' FLM hint: {body}"
                )
                print(f"  [OK] update_required (old ver) load: got version hint")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_update_required_old_version_install(self):
        """NPU present, FLM v0.9.20 -> install flm:npu attempts upgrade."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = create_mock_flm_script(
                temp_dir, version="0.9.20", validate_ready=True
            )
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                r = self._post_install("flm", "npu")
                # Attempts real install/upgrade, will fail in CI
                self.assertEqual(r.status_code, 500)
                body = r.json()
                error_msg = body.get("error", "").lower()
                self.assertNotIn("not supported on this system", error_msg)
                print(
                    f"  [OK] update_required (old ver) install: status=500 (expected)"
                )
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    # ------------------------------------------------------------------ #
    #  Scenario 4: update_required (unknown version — FLM too old for --json)
    # ------------------------------------------------------------------ #

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_update_required_unknown_version_system_info(self):
        """NPU present, FLM exists but version unparseable -> state=update_required."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = create_mock_flm_script(temp_dir, broken_version=True)
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                data = self._get_system_info()
                npu = self._get_flm_npu(data)

                self.assertEqual(
                    npu["state"],
                    "update_required",
                    f"Expected update_required, got {npu['state']}: {npu}",
                )
                self.assertIn(
                    "unknown",
                    npu["message"].lower(),
                    f"Expected 'unknown' in message: {npu['message']}",
                )
                self.assertNotEqual(npu["action"], "")
                print(
                    f"  [OK] update_required (unknown ver) system-info: {npu['message']}"
                )
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_update_required_unknown_version_models(self):
        """NPU present, FLM version unparseable -> no FLM models visible."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = create_mock_flm_script(temp_dir, broken_version=True)
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                flm_models = self._get_flm_model_names()
                self.assertEqual(
                    flm_models, [], f"Expected no FLM models, got {flm_models}"
                )
                print(
                    "  [OK] update_required (unknown ver) models: no FLM models listed"
                )
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_update_required_unknown_version_load(self):
        """NPU present, FLM version unparseable -> load -FLM model gives hint."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = create_mock_flm_script(temp_dir, broken_version=True)
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                r = self._post_load(FAKE_FLM_MODEL)
                self.assertIn(r.status_code, (400, 404, 500))
                body = r.json()
                error_msg = json.dumps(body).lower()
                self.assertIn("not found", error_msg)
                # Should mention FLM is not ready with version info
                self.assertIn(
                    "unknown", error_msg, f"Expected 'unknown' version hint: {body}"
                )
                print(f"  [OK] update_required (unknown ver) load: got version hint")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    # ------------------------------------------------------------------ #
    #  Scenario 5: action_required (correct version, validate fails)
    # ------------------------------------------------------------------ #

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_action_required_system_info(self):
        """NPU present, correct version, validate fails -> state=action_required."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            version = REQUIRED_FLM_VERSION.lstrip("v")
            mock_path = create_mock_flm_script(
                temp_dir, version=version, validate_ready=False
            )
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                data = self._get_system_info()
                npu = self._get_flm_npu(data)

                self.assertEqual(
                    npu["state"],
                    "action_required",
                    f"Expected action_required, got {npu['state']}: {npu}",
                )
                self.assertNotEqual(npu["action"], "")
                print(f"  [OK] action_required system-info: {npu['message']}")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_action_required_models(self):
        """NPU present, validate fails -> no FLM models visible."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            version = REQUIRED_FLM_VERSION.lstrip("v")
            mock_path = create_mock_flm_script(
                temp_dir, version=version, validate_ready=False
            )
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                flm_models = self._get_flm_model_names()
                self.assertEqual(
                    flm_models, [], f"Expected no FLM models, got {flm_models}"
                )
                print("  [OK] action_required models: no FLM models listed")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_action_required_load(self):
        """NPU present, validate fails -> load -FLM model gives not-found with hint."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            version = REQUIRED_FLM_VERSION.lstrip("v")
            mock_path = create_mock_flm_script(
                temp_dir, version=version, validate_ready=False
            )
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                r = self._post_load(FAKE_FLM_MODEL)
                self.assertIn(r.status_code, (400, 404, 500))
                body = r.json()
                error_msg = json.dumps(body).lower()
                self.assertIn("not found", error_msg)
                self.assertIn("flm", error_msg, f"Expected FLM hint in error: {body}")
                print(f"  [OK] action_required load: got FLM hint")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_action_required_install(self):
        """NPU present, validate fails -> install flm:npu attempts install."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            version = REQUIRED_FLM_VERSION.lstrip("v")
            mock_path = create_mock_flm_script(
                temp_dir, version=version, validate_ready=False
            )
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                r = self._post_install("flm", "npu")
                # Attempts install (will fail in CI), but should not say "not supported"
                self.assertEqual(r.status_code, 500)
                body = r.json()
                error_msg = body.get("error", "").lower()
                self.assertNotIn("not supported on this system", error_msg)
                print(f"  [OK] action_required install: status=500 (expected)")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    # ------------------------------------------------------------------ #
    #  Scenario 6: installed (all checks pass)
    # ------------------------------------------------------------------ #

    def _installed_mock_path(self, temp_dir):
        """Create a mock FLM for the 'installed' scenario."""
        version = REQUIRED_FLM_VERSION.lstrip("v")
        return create_mock_flm_script(
            temp_dir,
            version=version,
            validate_ready=True,
            list_json='{"models":[{"name":"test-model:1b","footprint":1.0,"label":["llm"]}]}',
        )

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installed_system_info(self):
        """All checks pass -> state=installed, empty action, version present."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = self._installed_mock_path(temp_dir)
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                data = self._get_system_info()
                npu = self._get_flm_npu(data)

                self.assertEqual(
                    npu["state"],
                    "installed",
                    f"Expected installed, got {npu['state']}: {npu}",
                )
                self.assertEqual(npu["action"], "")
                self.assertIn("version", npu)
                self.assertNotEqual(npu["version"], "")
                print(f"  [OK] installed system-info: version={npu['version']}")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installed_models(self):
        """All checks pass -> FLM models visible in listing."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = self._installed_mock_path(temp_dir)
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                flm_models = self._get_flm_model_names()
                self.assertIn(
                    MOCK_FLM_MODEL_NAME,
                    flm_models,
                    f"Expected {MOCK_FLM_MODEL_NAME} in {flm_models}",
                )
                print(f"  [OK] installed models: {flm_models}")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installed_load(self):
        """All checks pass -> loading FLM model gets past not-found check.

        The mock FLM can't actually serve, so load will fail — but the error
        should be a load failure, NOT a model-not-found or FLM-not-ready error.
        """
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = self._installed_mock_path(temp_dir)
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                r = self._post_load(MOCK_FLM_MODEL_NAME)
                body = r.json()
                error_msg = json.dumps(body).lower()
                # Model IS in registry, so we should NOT get "not found"
                self.assertNotIn(
                    "not found", error_msg, f"Model should be found in registry: {body}"
                )
                # Should NOT say "FLM is not ready"
                self.assertNotIn(
                    "flm is not ready", error_msg, f"FLM should be ready: {body}"
                )
                print(f"  [OK] installed load: model found, status={r.status_code}")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installed_pull(self):
        """All checks pass -> pulling FLM model attempts flm pull (not readiness error).

        Mock FLM doesn't support 'pull', so it will fail — but the error
        should NOT be 'FLM is not ready'.
        """
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = self._installed_mock_path(temp_dir)
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                r = self._post_pull(MOCK_FLM_MODEL_NAME)
                body = r.json()
                error_msg = json.dumps(body).lower()
                # Should NOT say "FLM is not ready"
                self.assertNotIn(
                    "flm is not ready", error_msg, f"FLM should be ready: {body}"
                )
                print(
                    f"  [OK] installed pull: no readiness error, status={r.status_code}"
                )
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(IS_X86_64, "FLM tests require x86_64")
    def test_installed_install(self):
        """All checks pass -> install flm:npu succeeds (already installed)."""
        temp_dir = tempfile.mkdtemp(prefix="lemonade_mock_flm_")
        try:
            mock_path = self._installed_mock_path(temp_dir)
            with self._server(NPU_HARDWARE, mock_flm_path=mock_path):
                r = self._post_install("flm", "npu")
                self.assertEqual(
                    r.status_code,
                    200,
                    f"Expected 200 for already-installed, got {r.status_code}: {r.json()}",
                )
                body = r.json()
                self.assertEqual(body.get("status"), "success")
                print(f"  [OK] installed install: already installed, 200")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
