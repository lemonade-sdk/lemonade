import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import requests

HOST = "127.0.0.1"
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_LEMOND_BINARY = None

ROUNDS = 5
CLIENT_THREADS = 8
WARMUP_SECONDS = 0.5
EXIT_TIMEOUT_SECONDS = 30
READY_TIMEOUT_SECONDS = 120
TRAFFIC_PATHS = ("/api/v1/system-info", "/api/v1/health", "/api/v1/models")


def find_lemond_binary():
    if _LEMOND_BINARY:
        return _LEMOND_BINARY
    env = os.environ.get("LEMOND_BINARY")
    if env:
        return env
    for candidate in ("build/lemond", "build-debug/lemond", "build-release/lemond"):
        path = os.path.join(REPO_ROOT, candidate)
        if os.path.isfile(path):
            return path
    raise FileNotFoundError(
        "could not locate the lemond binary; build it or pass --lemond-binary"
    )


def free_port():
    with socket.socket() as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


class ShutdownUnderLoadTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="lemond-shutdown-")
        self.proc = None
        self.stop_traffic = threading.Event()
        self.threads = []

    def tearDown(self):
        self.stop_traffic.set()
        for thread in self.threads:
            thread.join(timeout=10)
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        shutil.rmtree(self.root, ignore_errors=True)

    def start_server(self):
        self.port = free_port()
        cache_dir = os.path.join(self.root, "cache")
        config_dir = os.path.join(self.root, "config")
        os.makedirs(cache_dir)
        os.makedirs(config_dir)
        env = dict(os.environ)
        env["HOME"] = self.root
        env["XDG_CONFIG_HOME"] = os.path.join(self.root, "xdg-config")
        env["XDG_CACHE_HOME"] = os.path.join(self.root, "xdg-cache")
        env["XDG_DATA_HOME"] = os.path.join(self.root, "xdg-data")
        for key in [k for k in env if k.startswith("LEMONADE_")]:
            del env[key]
        self.log = open(os.path.join(self.root, "lemond.log"), "wb")
        self.proc = subprocess.Popen(
            [
                find_lemond_binary(),
                cache_dir,
                config_dir,
                "--port",
                str(self.port),
                "--host",
                HOST,
                "--no-broadcast",
                "--log-file",
                "disabled",
            ],
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=env,
        )
        deadline = time.time() + READY_TIMEOUT_SECONDS
        while time.time() < deadline:
            self.assertIsNone(self.proc.poll(), "lemond exited during startup")
            try:
                requests.get(f"http://{HOST}:{self.port}/live", timeout=1)
                return
            except requests.RequestException:
                time.sleep(0.05)
        self.fail("lemond did not become ready")

    def client_loop(self, index):
        sock = None
        n = 0
        while not self.stop_traffic.is_set():
            path = TRAFFIC_PATHS[(index + n) % len(TRAFFIC_PATHS)]
            n += 1
            try:
                if sock is None:
                    sock = socket.create_connection((HOST, self.port), timeout=5)
                sock.sendall(f"GET {path} HTTP/1.1\r\nHost: {HOST}\r\n\r\n".encode())
                if not sock.recv(65536):
                    raise OSError("connection closed")
            except OSError:
                if sock:
                    sock.close()
                sock = None
                time.sleep(0.01)
        if sock:
            sock.close()

    def test_shutdown_while_clients_are_active(self):
        for round_index in range(ROUNDS):
            with self.subTest(round=round_index):
                self.stop_traffic.clear()
                self.threads = []
                shutil.rmtree(self.root, ignore_errors=True)
                os.makedirs(self.root)
                self.start_server()
                for index in range(CLIENT_THREADS):
                    thread = threading.Thread(
                        target=self.client_loop, args=(index,), daemon=True
                    )
                    thread.start()
                    self.threads.append(thread)
                time.sleep(WARMUP_SECONDS)

                response = requests.post(
                    f"http://{HOST}:{self.port}/internal/shutdown", timeout=10
                )
                self.assertEqual(response.status_code, 200)

                try:
                    code = self.proc.wait(timeout=EXIT_TIMEOUT_SECONDS)
                except subprocess.TimeoutExpired:
                    self.fail("lemond did not exit after /internal/shutdown")
                finally:
                    self.stop_traffic.set()
                    for thread in self.threads:
                        thread.join(timeout=10)
                    self.log.close()
                self.assertEqual(
                    code, 0, f"lemond exited with {code} while clients were active"
                )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--lemond-binary", default=None)
    args, remaining = parser.parse_known_args()
    _LEMOND_BINARY = args.lemond_binary
    unittest.main(argv=[sys.argv[0]] + remaining)
