"""
Container backend smoke test: drives one container backend end to end through
one container tool: install, load, chat, unload, a killed lemond and
uninstall. Unlike the other suites, it starts and stops lemond itself, since it
kills lemond.

Usage:
    python test/server_containers.py --lemond PATH --backend llamacpp:nathanw \
        --tool podman --mode rootless
"""

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

import requests
from openai import OpenAI

from utils.server_base import (
    load_model,
    pull_model_with_retry,
    unload_model,
    wait_for_server,
)
from utils.test_models import ENDPOINT_TEST_MODEL, TIMEOUT_MODEL_OPERATION

LABEL = "ai.lemonade"

args = None


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def tool(*command):
    return subprocess.run(
        [args.tool, *command], capture_output=True, text=True, check=False
    )


def labeled(kind):
    """Names of every container (stopped ones too) or network with the label."""
    if kind == "container":
        command = ["ps", "-a", "--format", "{{.Names}}"]
    else:
        command = ["network", "ls", "--format", "{{.Name}}"]
    return tool(*command, "--filter", f"label={LABEL}").stdout.split()


class ContainerSmokeTest(unittest.TestCase):
    lemond = None
    port = None
    cache_dir = None
    log = None
    # The <image>@<digest> lemond ran, read from its container.
    image = None

    @classmethod
    def setUpClass(cls):
        cls.port = free_port()
        cls.cache_dir = tempfile.mkdtemp(prefix="lemonade-smoke-")
        os.environ["LEMONADE_PORT"] = str(cls.port)
        cls.log = open(
            os.path.join(os.environ.get("RUNNER_TEMP", cls.cache_dir), "lemond.log"),
            "a",
            encoding="utf-8",
        )
        cls.start_lemond()

    @classmethod
    def tearDownClass(cls):
        cls.stop_lemond()
        cls.log.close()

    @classmethod
    def start_lemond(cls):
        cls.lemond = subprocess.Popen(
            [args.lemond, cls.cache_dir, "--port", str(cls.port)],
            stdout=cls.log,
            stderr=subprocess.STDOUT,
        )
        wait_for_server(port=cls.port, timeout=60)

    @classmethod
    def stop_lemond(cls):
        if cls.lemond and cls.lemond.poll() is None:
            cls.lemond.terminate()
            cls.lemond.wait(timeout=60)

    def url(self, path):
        return f"http://localhost:{self.port}/api/v1/{path}"

    def cli(self, *command):
        result = subprocess.run(
            [args.cli_binary, *command], capture_output=True, text=True, check=False
        )
        print(result.stdout, result.stderr)
        self.assertEqual(result.returncode, 0, f"lemonade {' '.join(command)} failed")

    def test_01_tool_mode(self):
        if args.tool == "podman":
            rootless = tool("info", "--format", "{{.Host.Security.Rootless}}").stdout
        else:
            options = tool("info", "--format", "{{.SecurityOptions}}").stdout
            rootless = str("rootless" in options)
        self.assertEqual(rootless.strip().lower(), str(args.mode == "rootless").lower())

    def test_02_install(self):
        self.cli("config", "set", "enable_containers=true", "--yes")
        self.cli("backends", "install", args.backend, "--force")

    def test_03_load_and_chat(self):
        pull_model_with_retry(ENDPOINT_TEST_MODEL, port=self.port)
        recipe, backend = args.backend.split(":")
        response = load_model(
            ENDPOINT_TEST_MODEL, port=self.port, **{f"{recipe}_backend": backend}
        )
        self.assertEqual(response.status_code, 200, response.text)

        containers = labeled("container")
        self.assertEqual(len(containers), 1, containers)
        self.assertTrue(containers[0].startswith(f"lemonade-{recipe}-{backend}-"))
        type(self).image = tool(
            "inspect", "--format", "{{.Config.Image}}", containers[0]
        ).stdout.strip()
        # lemond publishes a loopback port only when it detects rootless Podman,
        # so this checks lemond's own view of the mode, not just the tool's.
        published = tool("port", containers[0]).stdout.strip()
        if args.mode == "rootless":
            self.assertIn("-> 127.0.0.1:", published)
        else:
            self.assertEqual(published, "")

        client = OpenAI(
            base_url=self.url(""), api_key="lemonade", timeout=TIMEOUT_MODEL_OPERATION
        )
        messages = [{"role": "user", "content": "Say hello."}]
        completion = client.chat.completions.create(
            model=ENDPOINT_TEST_MODEL, messages=messages, max_completion_tokens=8
        )
        self.assertTrue(completion.choices[0].message.content)

        streamed = ""
        for chunk in client.chat.completions.create(
            model=ENDPOINT_TEST_MODEL,
            messages=messages,
            max_completion_tokens=8,
            stream=True,
        ):
            if chunk.choices and chunk.choices[0].delta.content:
                streamed += chunk.choices[0].delta.content
        self.assertTrue(streamed)

    def test_04_unload_leaves_nothing(self):
        unload_model(ENDPOINT_TEST_MODEL, port=self.port)
        self.assertEqual(labeled("container"), [])
        self.assertEqual(labeled("network"), [])

    def test_05_killed_lemond_leaves_no_container(self):
        if args.tool == "podman" and args.mode == "rootful":
            # The container belongs to the Podman service, which outlives a
            # killed client and keeps it running.
            self.skipTest("rootful Podman keeps the container after lemond dies")
        recipe, backend = args.backend.split(":")
        response = load_model(
            ENDPOINT_TEST_MODEL, port=self.port, **{f"{recipe}_backend": backend}
        )
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(len(labeled("container")), 1)

        self.lemond.kill()
        self.lemond.wait(timeout=60)
        deadline = time.time() + 60
        while labeled("container") and time.time() < deadline:
            time.sleep(1)
        self.assertEqual(labeled("container"), [])

        # The network stays until the same model loads again.
        for network in labeled("network"):
            tool("network", "rm", network)
        self.start_lemond()

    def test_06_uninstall(self):
        self.cli("backends", "uninstall", args.backend)
        self.assertNotEqual(tool("image", "inspect", self.image).returncode, 0)


def main():
    global args
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lemond", required=True, help="Path to lemond")
    parser.add_argument("--cli-binary", default="lemonade", help="Path to lemonade")
    parser.add_argument(
        "--backend",
        required=True,
        help="Container backend as recipe:backend, e.g. llamacpp:nathanw",
    )
    parser.add_argument("--tool", required=True, choices=["podman", "docker"])
    parser.add_argument("--mode", required=True, choices=["rootless", "rootful"])
    args = parser.parse_args()

    suite = unittest.TestLoader().loadTestsFromTestCase(ContainerSmokeTest)
    result = unittest.TextTestRunner(verbosity=2, failfast=True).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
