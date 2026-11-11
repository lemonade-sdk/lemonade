"""
Agent manager tests: `lemonade agents` and `lemonade launch pi` through the
per-user agent host.

Expects a running server (like server_cli2.py). Each test runs the CLI with
an isolated HOME and XDG directories, so the agent host it starts, the pi it
installs and pi's config all live in a temporary directory. Releases come
from a fixture server on loopback (LEMONADE_AGENT_RELEASE_BASE_URL) and are
pinned by a fixture agent_versions.json (LEMONADE_AGENT_VERSIONS).

Usage:
    python server_agents.py [--cli-binary PATH] [--api-key KEY]
"""

import argparse
import hashlib
import http.server
import io
import json
import os
import platform
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest

import requests

from utils.server_base import _auth_headers
from utils.test_models import (
    ENDPOINT_TEST_MODEL,
    PORT,
    TIMEOUT_DEFAULT,
    get_default_cli_binary,
)

IS_WINDOWS = platform.system() == "Windows"
FIXTURE_TAG = "v1.5.0"
FIXTURE_VERSION = "1.5.0"

_config = {"cli_binary": None, "api_key": None}

FAKE_PI = """#!/usr/bin/env python3
import json, os, sys
if sys.argv[1:] == ["--version"]:
    probes = os.environ.get("PI_TEST_VERSION_PROBES")
    if probes:
        try:
            with open(f"/proc/{{os.getppid()}}/comm", encoding="utf-8") as f:
                parent = f.read().strip()
        except OSError:
            parent = "unknown"
        with open(probes, "a", encoding="utf-8") as f:
            f.write(parent + "\\n")
    print("{version}")
    sys.exit(0)
capture = os.environ.get("PI_TEST_CAPTURE")
if capture:
    with open(capture, "w", encoding="utf-8") as f:
        json.dump({{
            "binary": os.path.realpath(sys.argv[0]),
            "argv": sys.argv[1:],
            "cwd": os.getcwd(),
            "env": {{k: os.environ.get(k, "") for k in [
                "LEMONADE_API_KEY", "PI_CODING_AGENT_DIR", "PI_SKIP_VERSION_CHECK",
                "PI_SELF_UPDATE_COMMAND"]}},
        }}, f)
print("fake-pi-ok")
"""


def auth_headers():
    if _config["api_key"]:
        return {"Authorization": f"Bearer {_config['api_key']}"}
    return _auth_headers()


def host_asset():
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x64"
    os_name = {"Linux": "linux", "Darwin": "darwin"}.get(platform.system(), "windows")
    return f"pi-{os_name}-{arch}.tar.gz"


def build_release(label):
    """A tarball laid out like pi's: everything under pi/."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as tar:
        for name, data, mode in [
            ("pi/pi", FAKE_PI.format(version=FIXTURE_VERSION + label), 0o755),
            ("pi/theme/dark.json", "{}", 0o644),
        ]:
            info = tarfile.TarInfo(name)
            payload = data.encode()
            info.size = len(payload)
            info.mode = mode
            tar.addfile(info, io.BytesIO(payload))
    return buffer.getvalue()


class ReleaseServer:
    """Serves fixture releases at /<tag>/<asset> on loopback."""

    def __init__(self, releases):
        releases_ref = releases
        server = self
        self.delay = 0

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                time.sleep(server.delay)
                body = releases_ref.get(self.path.lstrip("/"))
                if body is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class RecordingServer:
    """Records every request's headers; answers 503 like a lemond with no host."""

    def __init__(self, host):
        seen = self.seen = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def _answer(self):
                seen.append({k.lower(): v for k, v in self.headers.items()})
                self.send_response(503)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"error":"none","code":"agent_host_unavailable"}')

            do_GET = do_POST = do_DELETE = _answer

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer((host, 0), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class FakeHost:
    """An agent host that answers list and drops its connection on launch."""

    def __init__(self, token):
        from websockets.sync.client import connect

        headers = {}
        if _config["api_key"]:
            headers["Authorization"] = f"Bearer {_config['api_key']}"
        self.ws = connect(
            f"ws://127.0.0.1:{PORT}/v1/agents/host", additional_headers=headers
        )
        self.ws.send(
            json.dumps(
                {
                    "type": "hello",
                    "protocol": 1,
                    "token": token,
                    "version": "test",
                    "platform": "test",
                    "persistent": False,
                }
            )
        )
        assert json.loads(self.ws.recv())["type"] == "welcome"
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        try:
            for message in self.ws:
                frame = json.loads(message)
                if frame.get("type") != "request":
                    continue
                if frame["route"] == "lemonade.agents_launch":
                    self.ws.close()
                    return
                self.ws.send(
                    json.dumps(
                        {
                            "type": "response",
                            "id": frame["id"],
                            "status": 200,
                            "body": {"agents": [], "host": {}},
                        }
                    )
                )
        except Exception:  # pylint: disable=broad-except
            pass


def non_loopback_address():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            address = s.getsockname()[0]
        return None if address.startswith("127.") else address
    except OSError:
        return None


@unittest.skipIf(IS_WINDOWS, "fixture pi is a POSIX script; Windows runs the C++ tests")
class AgentManagerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.releases = {f"{FIXTURE_TAG}/{host_asset()}": build_release("")}
        cls.release_server = ReleaseServer(cls.releases)

    @classmethod
    def tearDownClass(cls):
        cls.release_server.close()

    def setUp(self):
        self.home = tempfile.mkdtemp(prefix="lemonade-agents-")
        self.stub_bin = os.path.join(self.home, "stub-bin")
        os.makedirs(self.stub_bin)
        self.versions_file = os.path.join(self.home, "agent_versions.json")
        self.write_pins(self.releases[f"{FIXTURE_TAG}/{host_asset()}"])

    def tearDown(self):
        self.stop_host()
        shutil.rmtree(self.home, ignore_errors=True)

    # Helpers

    def write_pins(self, release_bytes):
        digest = hashlib.sha256(release_bytes).hexdigest()
        with open(self.versions_file, "w", encoding="utf-8") as f:
            json.dump(
                {
                    "pi": {"version": FIXTURE_TAG},
                    "checksums": {
                        "github": {
                            "earendil-works/pi": {
                                FIXTURE_TAG: {host_asset(): "sha256:" + digest}
                            }
                        }
                    },
                },
                f,
            )

    def env(self, extra_path=None):
        env = os.environ.copy()
        for name in ["PI_CODING_AGENT_DIR", "LEMONADE_ADMIN_API_KEY"]:
            env.pop(name, None)
        env["HOME"] = self.home
        env["XDG_CONFIG_HOME"] = os.path.join(self.home, ".config")
        env["XDG_CACHE_HOME"] = os.path.join(self.home, ".cache")
        env["XDG_DATA_HOME"] = os.path.join(self.home, ".local", "share")
        env["XDG_STATE_HOME"] = os.path.join(self.home, ".local", "state")
        env["XDG_BIN_HOME"] = os.path.join(self.home, ".local", "bin")
        paths = (
            [self.stub_bin]
            + ([extra_path] if extra_path else [])
            + ["/usr/bin", "/bin"]
        )
        env["PATH"] = os.pathsep.join(paths)
        env["LEMONADE_AGENT_RELEASE_BASE_URL"] = self.release_server.url
        env["LEMONADE_AGENT_VERSIONS"] = self.versions_file
        env["LEMONADE_AGENT_HOST_IDLE_SECONDS"] = "10"
        env["PI_TEST_CAPTURE"] = os.path.join(self.home, "capture.json")
        env["PI_TEST_VERSION_PROBES"] = os.path.join(self.home, "version-probes.txt")
        return env

    def cli(self, args, env=None, timeout=TIMEOUT_DEFAULT, binary=None, target=True):
        cmd = [binary or _config["cli_binary"]]
        if target:
            cmd += ["--port", str(PORT)]
        if _config["api_key"]:
            cmd += ["--api-key", _config["api_key"]]
        cmd += args
        result = subprocess.run(
            cmd,
            env=env or self.env(),
            capture_output=True,
            text=True,
            timeout=timeout,
            stdin=subprocess.DEVNULL,
            cwd=self.home,
        )
        print(f"$ {' '.join(cmd)}\n{result.stdout}{result.stderr}")
        return result

    @property
    def state_dir(self):
        if sys.platform == "darwin":
            return os.path.join(
                self.home, "Library", "Application Support", "lemonade", "agent-host"
            )
        return os.path.join(self.home, ".local", "state", "lemonade", "agent-host")

    @property
    def shim(self):
        return os.path.join(self.home, ".local", "bin", "pi")

    @property
    def payload_root(self):
        if sys.platform == "darwin":
            return os.path.join(
                self.home, "Library", "Application Support", "lemonade", "agents", "pi"
            )
        return os.path.join(self.home, ".local", "share", "lemonade", "agents", "pi")

    def token(self):
        with open(os.path.join(self.state_dir, "token"), encoding="utf-8") as f:
            return f.read().strip()

    def stop_host(self):
        info = os.path.join(self.state_dir, "host.json")
        try:
            with open(info, encoding="utf-8") as f:
                os.kill(json.load(f)["pid"], signal.SIGTERM)
        except (OSError, ValueError, KeyError):
            pass

    def write_external_pi(self, directory, version="1.4.0"):
        path = os.path.join(directory, "pi")
        with open(path, "w", encoding="utf-8") as f:
            f.write(FAKE_PI.format(version=version))
        os.chmod(path, 0o755)
        return path

    def install(self):
        result = self.cli(["agents", "install", "pi"])
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def capture(self):
        with open(os.path.join(self.home, "capture.json"), encoding="utf-8") as f:
            return json.load(f)

    def pi_json(self, name, default=None):
        path = os.path.join(self.home, ".pi", "agent", name)
        if default is not None and not os.path.exists(path):
            return default
        with open(path, encoding="utf-8") as f:
            return json.load(f)

    # Tests

    def test_010_list_starts_a_host(self):
        result = self.cli(["agents", "list"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout, r"pi\s+1\.5\.0\s+-\s+none")
        self.assertEqual(
            os.stat(os.path.join(self.state_dir, "token")).st_mode & 0o777, 0o600
        )

    def test_015_a_second_host_keeps_the_running_hosts_log(self):
        self.cli(["agents", "list"])
        log = os.path.join(self.state_dir, "agent-host.log")
        with open(log, encoding="utf-8") as f:
            before = f.read()
        host = os.path.join(
            os.path.dirname(_config["cli_binary"]), "lemonade-agent-host"
        )
        subprocess.run([host], env=self.env(), timeout=TIMEOUT_DEFAULT, check=True)
        with open(log, encoding="utf-8") as f:
            self.assertEqual(f.read()[: len(before)], before)

    def test_015b_a_new_host_keeps_the_previous_hosts_log(self):
        self.cli(["agents", "list"])
        log = os.path.join(self.state_dir, "agent-host.log")
        with open(log, encoding="utf-8") as f:
            before = f.read()
        with open(os.path.join(self.state_dir, "host.json"), encoding="utf-8") as f:
            pid = json.load(f)["pid"]
        self.stop_host()
        deadline = time.time() + 10
        while True:
            try:
                os.kill(pid, 0)
            except OSError:
                break
            self.assertLess(time.time(), deadline, "the host didn't exit")
            time.sleep(0.1)
        self.cli(["agents", "list"])
        with open(log + ".1", encoding="utf-8") as f:
            self.assertTrue(f.read().startswith(before))

    def test_016_list_reports_the_shim_dir_on_path_before_install(self):
        bin_dir = os.path.join(self.home, ".local", "bin")
        os.makedirs(bin_dir)
        result = self.cli(["agents", "list"], env=self.env(bin_dir))
        self.assertEqual(result.returncode, 0, result.stderr)
        listing = requests.get(
            f"http://127.0.0.1:{PORT}/api/v1/agents",
            headers={**auth_headers(), "X-Lemonade-Agent-Token": self.token()},
            timeout=TIMEOUT_DEFAULT,
        ).json()
        shim = listing["agents"][0]["shim"]
        self.assertEqual(shim["state"], "absent")
        self.assertTrue(shim["on_path"])

    def test_020_install_writes_payload_shim_and_config(self):
        result = self.install()
        self.assertIn("pi-", result.stdout)
        self.assertIn("Installed pi 1.5.0", result.stdout)
        with open(self.shim, encoding="utf-8") as f:
            self.assertIn("lemonade-agent-shim v1", f.read())
        version = subprocess.run(
            [self.shim, "--version"], capture_output=True, text=True, check=True
        )
        self.assertEqual(version.stdout.strip(), FIXTURE_VERSION)
        provider = self.pi_json("models.json")["providers"]["Lemonade"]
        self.assertEqual(provider["baseUrl"], f"http://127.0.0.1:{PORT}/v1")
        self.assertFalse(
            os.path.exists(os.path.join(self.home, ".pi", "agent", "settings.json")),
            "install must not pick pi's default model",
        )
        listing = self.cli(["agents"])
        self.assertRegex(listing.stdout, r"pi\s+1\.5\.0\s+1\.5\.0\s+managed")
        again = self.install()
        self.assertIn("Already installed", again.stdout)

    def test_021_list_says_why_the_managed_pi_isnt_the_one_on_path(self):
        self.install()
        result = self.cli(["agents", "list"])
        self.assertIn("isn't on your PATH", result.stdout)
        external = self.write_external_pi(self.stub_bin)
        bin_dir = os.path.join(self.home, ".local", "bin")
        result = self.cli(["agents", "list"], env=self.env(extra_path=bin_dir))
        self.assertIn(external + " comes before " + bin_dir, result.stdout)

    def test_022_concurrent_installs_one_is_busy(self):
        self.cli(["agents", "list"])
        self.release_server.delay = 2
        try:
            cmd = [_config["cli_binary"], "--port", str(PORT)]
            if _config["api_key"]:
                cmd += ["--api-key", _config["api_key"]]
            procs = [
                subprocess.Popen(
                    cmd + ["agents", "install", "pi"],
                    env=self.env(),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    stdin=subprocess.DEVNULL,
                )
                for _ in range(2)
            ]
            results = [
                p.communicate(timeout=TIMEOUT_DEFAULT) + (p.returncode,) for p in procs
            ]
        finally:
            self.release_server.delay = 0
        codes = sorted(r[2] for r in results)
        self.assertEqual(codes, [0, 1], results)
        busy = next(r for r in results if r[2] == 1)
        self.assertIn("already being installed", busy[1])

    def test_024_persistent_host_takes_over(self):
        self.cli(["agents", "list"])
        with open(os.path.join(self.state_dir, "host.json"), encoding="utf-8") as f:
            on_demand = json.load(f)["pid"]
        old_token = self.token()
        host = os.path.join(
            os.path.dirname(_config["cli_binary"]), "lemonade-agent-host"
        )
        env = self.env()
        if _config["api_key"]:
            env["LEMONADE_API_KEY"] = _config["api_key"]
        persistent = subprocess.Popen(
            [host, "--persistent", "--lemond", f"http://127.0.0.1:{PORT}"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            deadline = time.time() + 15
            while time.time() < deadline:
                try:
                    os.kill(on_demand, 0)
                except OSError:
                    break
                time.sleep(0.2)
            else:
                self.fail("the on-demand host didn't hand over")
            deadline = time.time() + 15
            info = {}
            while time.time() < deadline:
                try:
                    with open(
                        os.path.join(self.state_dir, "host.json"), encoding="utf-8"
                    ) as f:
                        info = json.load(f)
                except (OSError, ValueError):
                    pass
                if info.get("pid") == persistent.pid:
                    break
                time.sleep(0.2)
            self.assertTrue(info.get("persistent"))
            self.assertNotEqual(self.token(), old_token)
            result = self.cli(["agents", "list"])
            self.assertEqual(result.returncode, 0, result.stderr)
        finally:
            persistent.terminate()
            persistent.wait(timeout=10)

    def test_025_checksum_mismatch_installs_nothing(self):
        self.write_pins(b"something else")
        result = self.cli(["agents", "install", "pi"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum", result.stderr)
        self.assertFalse(
            os.path.exists(os.path.join(self.payload_root, FIXTURE_VERSION))
        )
        self.assertFalse(os.path.exists(self.shim))

    def test_030_foreign_shim_is_kept(self):
        os.makedirs(os.path.dirname(self.shim))
        with open(self.shim, "w", encoding="utf-8") as f:
            f.write("#!/bin/sh\necho mine\n")
        result = self.install()
        self.assertIn("isn't Lemonade's", result.stderr)
        with open(self.shim, encoding="utf-8") as f:
            self.assertEqual(f.read(), "#!/bin/sh\necho mine\n")
        self.cli(["agents", "uninstall", "pi"])
        self.assertTrue(os.path.exists(self.shim), "uninstall removed a foreign shim")

    def test_040_configure_mcp_touches_only_our_entry(self):
        mcp_dir = os.path.join(self.home, ".pi", "agent")
        os.makedirs(mcp_dir)
        with open(os.path.join(mcp_dir, "mcp.json"), "w", encoding="utf-8") as f:
            json.dump({"mcpServers": {"other": {"url": "http://x/mcp"}}}, f)
        self.assertEqual(self.cli(["agents", "configure", "pi", "--mcp"]).returncode, 0)
        servers = self.pi_json("mcp.json")["mcpServers"]
        self.assertEqual(servers["lemonade"]["url"], f"http://127.0.0.1:{PORT}/mcp")
        self.assertIn("other", servers)
        self.assertEqual(
            self.cli(["agents", "configure", "pi", "--no-mcp"]).returncode, 0
        )
        servers = self.pi_json("mcp.json")["mcpServers"]
        self.assertNotIn("lemonade", servers)
        self.assertIn("other", servers)

    def test_050_launch_uses_the_managed_pi(self):
        self.install()
        workdir = os.path.join(self.home, "project")
        os.makedirs(workdir)
        env = self.env()
        result = subprocess.run(
            [
                _config["cli_binary"],
                "--port",
                str(PORT),
                "launch",
                "pi",
                "--model",
                ENDPOINT_TEST_MODEL,
                "--codemode",
                "--agent-args",
                "--print hi",
            ]
            + (["--api-key", _config["api_key"]] if _config["api_key"] else []),
            env=env,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_DEFAULT,
            cwd=workdir,
            stdin=subprocess.DEVNULL,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = self.capture()
        self.assertEqual(
            payload["binary"], os.path.join(self.payload_root, FIXTURE_VERSION, "pi")
        )
        argv = payload["argv"]
        self.assertEqual(
            argv[:4], ["--provider", "Lemonade", "--model", ENDPOINT_TEST_MODEL]
        )
        self.assertEqual(argv[4:], ["--tools", "+codemode", "--print", "hi"])
        self.assertEqual(os.path.realpath(payload["cwd"]), os.path.realpath(workdir))
        self.assertEqual(payload["env"]["PI_SKIP_VERSION_CHECK"], "1")
        self.assertEqual(
            payload["env"]["PI_CODING_AGENT_DIR"],
            os.path.join(self.home, ".pi", "agent"),
        )
        settings = self.pi_json("settings.json")
        self.assertEqual(settings["defaultModel"], ENDPOINT_TEST_MODEL)

    def test_055_launch_follows_the_clients_pi_home(self):
        """The host writes the config pi will read, even when the client points pi elsewhere."""
        self.install()
        env = self.env()
        custom = os.path.join(self.home, "custom-pi-home")
        env["PI_CODING_AGENT_DIR"] = custom
        result = self.cli(["launch", "pi", "--model", ENDPOINT_TEST_MODEL], env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.path.exists(os.path.join(custom, "models.json")))
        self.assertEqual(self.capture()["env"]["PI_CODING_AGENT_DIR"], custom)

    def test_060_launch_without_pi_fails_off_a_tty(self):
        result = self.cli(["launch", "pi", "--model", ENDPOINT_TEST_MODEL])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lemonade agents install pi", result.stderr)

    def test_065_launch_uses_an_external_pi(self):
        external_dir = os.path.join(self.home, "npm-bin")
        os.makedirs(external_dir)
        external = self.write_external_pi(external_dir)
        result = self.cli(
            ["launch", "pi", "--model", ENDPOINT_TEST_MODEL], env=self.env(external_dir)
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = self.capture()
        self.assertEqual(payload["binary"], os.path.realpath(external))
        self.assertEqual(payload["env"]["PI_SKIP_VERSION_CHECK"], "")

    def test_066_host_never_runs_an_external_pi(self):
        """The version check for --codemode runs in the CLI, not in the host."""
        external_dir = os.path.join(self.home, "npm-bin")
        os.makedirs(external_dir)
        self.write_external_pi(external_dir, version="1.0.4")
        result = self.cli(
            ["launch", "pi", "--model", ENDPOINT_TEST_MODEL, "--codemode"],
            env=self.env(external_dir),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--codemode needs pi 1.1 or newer (found 1.0.4)", result.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.home, "capture.json")))
        with open(os.path.join(self.home, "version-probes.txt"), encoding="utf-8") as f:
            parents = f.read().split()
        self.assertTrue(parents, "nobody checked pi's version")
        if os.path.exists("/proc/self/comm"):
            self.assertNotIn("lemonade-agent-", parents)

    def test_070_uninstall_keeps_pi_home(self):
        self.install()
        result = self.cli(["agents", "uninstall", "pi"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(os.path.exists(self.payload_root))
        self.assertFalse(os.path.exists(self.shim))
        self.assertTrue(
            os.path.exists(os.path.join(self.home, ".pi", "agent", "models.json"))
        )

    def test_080_wrong_or_missing_token(self):
        self.cli(["agents", "list"])
        url = f"http://127.0.0.1:{PORT}/api/v1/agents"
        missing = requests.get(url, headers=auth_headers(), timeout=TIMEOUT_DEFAULT)
        self.assertEqual(missing.status_code, 400)
        self.assertEqual(missing.json()["code"], "agent_token_missing")
        wrong = requests.get(
            url,
            headers={**auth_headers(), "X-Lemonade-Agent-Token": "0" * 64},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(wrong.status_code, 503)
        right = requests.get(
            url,
            headers={**auth_headers(), "X-Lemonade-Agent-Token": self.token()},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(right.status_code, 200)
        if _config["api_key"]:
            unauthorized = requests.get(
                url,
                headers={"X-Lemonade-Agent-Token": self.token()},
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(unauthorized.status_code, 401)

    def test_085_host_rejects_bad_launch_input(self):
        self.cli(["agents", "list"])
        headers = {**auth_headers(), "X-Lemonade-Agent-Token": self.token()}
        base = f"http://127.0.0.1:{PORT}/api/v1/agents/pi"
        empty_model = requests.post(
            f"{base}/launch",
            json={"model": "", "cwd": self.home},
            headers=headers,
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertIn('"code":"invalid_argument"', empty_model.text)
        relative = requests.post(
            f"{base}/configure",
            json={"agent_dir": "relative-dir"},
            headers=headers,
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(relative.status_code, 400)
        self.assertEqual(relative.json()["code"], "invalid_argument")
        self.assertFalse(os.path.exists(os.path.join(self.home, "relative-dir")))

    def test_090_remote_target_never_gets_the_token(self):
        address = non_loopback_address()
        if address is None:
            self.skipTest("no non-loopback address")
        self.cli(["agents", "list"])  # leaves a token file behind
        recorder = RecordingServer(address)
        try:
            env = self.env()
            result = self.cli(
                ["--host", address, "--port", str(recorder.port), "agents", "list"],
                env=env,
                target=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("on this machine", result.stderr)
            self.cli(
                ["--host", address, "--port", str(recorder.port), "status"],
                env=env,
                target=False,
            )
            self.assertTrue(
                all(
                    "x-lemonade-agent-token" not in headers for headers in recorder.seen
                )
            )
        finally:
            recorder.close()

    def test_096_launch_falls_back_when_the_host_drops(self):
        token = "ab" * 32
        os.makedirs(self.state_dir)
        with open(os.path.join(self.state_dir, "token"), "w", encoding="utf-8") as f:
            f.write(token + "\n")
        FakeHost(token)
        external = self.write_external_pi(self.stub_bin)
        result = self.cli(["launch", "pi", "--model", ENDPOINT_TEST_MODEL])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("without the agent host", result.stderr)
        self.assertEqual(self.capture()["binary"], os.path.realpath(external))

    def test_091_a_name_starting_with_127_is_not_loopback(self):
        self.cli(["agents", "list"])
        result = self.cli(
            ["--host", "127.0.0.1.invalid", "--port", str(PORT), "agents", "list"],
            target=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("on this machine", result.stderr)

    def test_095_launch_falls_back_without_the_host_binary(self):
        lonely = os.path.join(self.home, "cli-only")
        os.makedirs(lonely)
        cli_copy = os.path.join(lonely, "lemonade")
        shutil.copy2(_config["cli_binary"], cli_copy)
        resources = os.path.join(os.path.dirname(_config["cli_binary"]), "resources")
        if os.path.isdir(resources):
            os.symlink(resources, os.path.join(lonely, "resources"))
        external = self.write_external_pi(self.stub_bin)
        result = self.cli(
            ["launch", "pi", "--model", ENDPOINT_TEST_MODEL], binary=cli_copy
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("without the agent host", result.stderr)
        self.assertEqual(self.capture()["binary"], os.path.realpath(external))


def main():
    parser = argparse.ArgumentParser(description="Test lemonade agents")
    parser.add_argument("--cli-binary", default=get_default_cli_binary())
    parser.add_argument("--api-key", default=None)
    args, rest = parser.parse_known_args()
    _config["cli_binary"] = os.path.abspath(args.cli_binary)
    _config["api_key"] = args.api_key
    requests.get(
        f"http://127.0.0.1:{PORT}/live", timeout=TIMEOUT_DEFAULT
    ).raise_for_status()
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)


if __name__ == "__main__":
    main()
