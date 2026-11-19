#!/usr/bin/env python3
"""HTTP authorization regressions for backend argument configuration; no inference."""

import argparse
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request

from utils.test_models import get_default_lemond_binary

SERVER_BINARY = get_default_lemond_binary()
REGULAR_KEY = "backend-args-test-regular"
ADMIN_KEY = "backend-args-test-admin"
PREFIXES = ("/api/v0", "/api/v1", "/v0", "/v1")


class IsolatedServer:
    def __init__(self, root, regular_key, admin_key):
        self.root = Path(root)
        self.config = self.root / "config"
        self.cache = self.root / "cache"
        self.config.mkdir()
        self.cache.mkdir()
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        env = os.environ.copy()
        for name, value in (
            ("LEMONADE_API_KEY", regular_key),
            ("LEMONADE_ADMIN_API_KEY", admin_key),
        ):
            env.pop(name, None)
            if value is not None:
                env[name] = value
        env["HF_HUB_CACHE"] = str(self.root / "hf")
        self.log = (self.root / "server.log").open("w")
        self.proc = subprocess.Popen(
            [
                SERVER_BINARY,
                str(self.cache),
                str(self.config),
                "--port",
                str(self.port),
                "--host",
                "127.0.0.1",
                "--no-broadcast",
                "--log-file",
                "disabled",
            ],
            env=env,
            stdout=self.log,
            stderr=self.log,
        )
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self.close()
                raise RuntimeError((self.root / "server.log").read_text()[-4000:])
            try:
                if self.request("GET", "/live")[0] == 200:
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.05)
        self.close()
        raise RuntimeError("Isolated server did not become ready")

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.log.close()

    def request(self, method, path, body=None, key=None):
        headers = {"Content-Type": "application/json"}
        if key is not None:
            headers["Authorization"] = f"Bearer {key}"
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}",
            data=None if body is None else json.dumps(body).encode(),
            headers=headers,
            method=method,
        )
        try:
            response = urllib.request.urlopen(req, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, response.read().decode()

    def saved_state(self):
        return {
            name: (
                (self.config / name).read_bytes()
                if (self.config / name).exists()
                else None
            )
            for name in ("user_models.json", "recipe_options.json", "jobs.json")
        }


@contextlib.contextmanager
def isolated_server(regular_key=REGULAR_KEY, admin_key=ADMIN_KEY):
    with tempfile.TemporaryDirectory(prefix="lemonade-backend-args-auth-") as root:
        server = IsolatedServer(root, regular_key, admin_key)
        try:
            yield server
        finally:
            server.close()


class BackendArgsAuthorizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not Path(SERVER_BINARY).is_file():
            raise RuntimeError(
                f"Build lemond first or pass --server-binary: {SERVER_BINARY}"
            )

    def assert_forbidden_without_writes(self, server, path, body):
        before = server.saved_state()
        status, text = server.request("POST", path, body, REGULAR_KEY)
        self.assertEqual(status, 403, text[:1000])
        self.assertEqual(server.saved_state(), before)

    def test_regular_key_cannot_supply_load_arguments(self):
        with isolated_server() as server:
            for prefix in PREFIXES:
                for key in ("llamacpp_args", "whispercpp_args"):
                    for value in ("--threads 1", "", None):
                        with self.subTest(prefix=prefix, key=key, value=value):
                            self.assert_forbidden_without_writes(
                                server,
                                f"{prefix}/load",
                                {"model_name": "user.DoesNotExist", key: value},
                            )
            self.assert_forbidden_without_writes(
                server,
                "/v1/load",
                {"model_name": "user.DoesNotExist", "merge_args": False},
            )

    def test_regular_key_cannot_register_or_import_arguments(self):
        with isolated_server() as server:
            for route in ("models/register", "pull"):
                for block in ("recipe_options", "image_defaults", "audio_defaults"):
                    for value in ("--threads 1", "", None):
                        with self.subTest(route=route, block=block, value=value):
                            self.assert_forbidden_without_writes(
                                server,
                                f"/v1/{route}",
                                {
                                    "model_name": "user.AuthBoundary",
                                    "recipe": "llamacpp",
                                    block: {"llamacpp_args": value},
                                },
                            )
            self.assert_forbidden_without_writes(
                server,
                "/v1/pull",
                {
                    "model_name": "user.AuthCollection",
                    "recipe": "collection.omni",
                    "components": ["user.AuthChild"],
                    "models": [
                        {
                            "model_name": "user.AuthChild",
                            "recipe": "llamacpp",
                            "recipe_options": {"llamacpp_args": "--threads 1"},
                        }
                    ],
                },
            )

    def test_admin_can_register_arguments_and_regular_can_register_without_them(self):
        with isolated_server() as server:
            for key, options in (
                (REGULAR_KEY, {}),
                (ADMIN_KEY, {"recipe_options": {"llamacpp_args": "--threads 1"}}),
            ):
                status, text = server.request(
                    "POST",
                    "/v1/models/register",
                    {
                        "model_name": "user.AuthAccepted",
                        "recipe": "llamacpp",
                        **options,
                    },
                    key,
                )
                self.assertEqual(status, 200, text[:1000])
            state = server.saved_state()
            self.assertIn(b"--threads 1", b"".join(v or b"" for v in state.values()))
            for value in ("--threads 2", "", None):
                with self.subTest(value=value):
                    self.assert_forbidden_without_writes(
                        server,
                        "/v1/models/user.AuthAccepted/options",
                        {"llamacpp_args": value},
                    )
            status, text = server.request(
                "POST",
                "/v1/models/user.AuthAccepted/options",
                {"llamacpp_args": "--threads 2"},
                ADMIN_KEY,
            )
            self.assertEqual(status, 200, text[:1000])
            self.assertEqual(json.loads(text)["saved"]["llamacpp_args"], "--threads 2")
            for key, options, expected in (
                (REGULAR_KEY, {}, 404),
                (ADMIN_KEY, {"llamacpp_args": "--threads 1"}, 404),
                ("wrong-key", {}, 401),
            ):
                status, text = server.request(
                    "POST",
                    "/v1/load",
                    {"model_name": "user.DoesNotExist", **options},
                    key,
                )
                self.assertEqual(status, expected, text[:1000])

    def test_regular_key_cannot_clear_saved_arguments_indirectly(self):
        with isolated_server() as server:
            name = "user.SavedAuthBoundary"
            status, text = server.request(
                "POST",
                "/v1/models/register",
                {"model_name": name, "recipe": "llamacpp"},
                ADMIN_KEY,
            )
            self.assertEqual(status, 200, text[:1000])
            path = f"/v1/models/{name}/options"
            status, text = server.request(
                "POST",
                path,
                {"llamacpp_args": "--threads 1"},
                ADMIN_KEY,
            )
            self.assertEqual(status, 200, text[:1000])
            status, text = server.request(
                "POST",
                path,
                {"ctx_size": 1024},
                REGULAR_KEY,
            )
            self.assertEqual(status, 200, text[:1000])
            self.assertEqual(json.loads(text)["saved"]["llamacpp_args"], "--threads 1")
            before = server.saved_state()
            status, text = server.request("DELETE", path, key=REGULAR_KEY)
            self.assertEqual(status, 403, text[:1000])
            self.assertEqual(server.saved_state(), before)
            self.assert_forbidden_without_writes(
                server,
                "/v1/load",
                {"model_name": name, "save_options": True},
            )
            status, text = server.request("DELETE", path, key=ADMIN_KEY)
            self.assertEqual(status, 200, text[:1000])
            status, text = server.request(
                "POST",
                path,
                {"ctx_size": 1024},
                REGULAR_KEY,
            )
            self.assertEqual(status, 200, text[:1000])
            status, text = server.request("DELETE", path, key=REGULAR_KEY)
            self.assertEqual(status, 200, text[:1000])

    def test_regular_key_cannot_remove_registered_arguments_by_replacing_model(self):
        with isolated_server() as server:
            name = "user.RegistryAuthBoundary"
            status, text = server.request(
                "POST",
                "/v1/models/register",
                {
                    "model_name": name,
                    "recipe": "llamacpp",
                    "recipe_options": {"llamacpp_args": "--threads 1"},
                },
                ADMIN_KEY,
            )
            self.assertEqual(status, 200, text[:1000])
            for options in ({}, {"recipe_options": {}}, {"recipe_options": None}):
                with self.subTest(options=options):
                    self.assert_forbidden_without_writes(
                        server,
                        "/v1/models/register",
                        {"model_name": name, "recipe": "llamacpp", **options},
                    )

    def test_regular_key_cannot_choose_remote_collection_manifests(self):
        with isolated_server() as server:
            for route in ("models/register", "pull"):
                for checkpoint in (
                    {"checkpoint": "example/remote-collection"},
                    {"checkpoints": {"main": "example/remote-collection"}},
                ):
                    with self.subTest(route=route, checkpoint=checkpoint):
                        self.assert_forbidden_without_writes(
                            server,
                            f"/v1/{route}",
                            {
                                "model_name": "user.RemoteAuthBoundary",
                                "recipe": "collection.omni",
                                **checkpoint,
                            },
                        )
            status, text = server.request(
                "POST",
                "/v1/models/register",
                {
                    "model_name": "user.RemoteAuthBoundary",
                    "recipe": "collection.omni",
                    "checkpoint": "example/remote-collection",
                },
                ADMIN_KEY,
            )
            self.assertEqual(status, 200, text[:1000])
            for checkpoint in (
                {"checkpoint": "example/replacement-collection"},
                {"checkpoints": {"main": "example/replacement-collection"}},
            ):
                self.assert_forbidden_without_writes(
                    server,
                    "/v1/pull",
                    {"model_name": "user.RemoteAuthBoundary", **checkpoint},
                )
            for source_key in ("source", "registry_source"):
                self.assert_forbidden_without_writes(
                    server,
                    "/v1/pull",
                    {"model_name": "user.RemoteAuthBoundary", source_key: "modelscope"},
                )

    def test_collection_source_change_requires_admin_with_explicit_recipe(self):
        with isolated_server() as server:
            name = "user.MaskedRemoteCollection"
            status, text = server.request(
                "POST",
                "/v1/models/register",
                {
                    "model_name": name,
                    "recipe": "collection.omni",
                    "checkpoint": "example/remote-collection",
                },
                ADMIN_KEY,
            )
            self.assertEqual(status, 200, text[:1000])
            self.assert_forbidden_without_writes(
                server,
                "/v1/pull",
                {"model_name": name, "recipe": "llamacpp", "source": "modelscope"},
            )

    def test_regular_key_cannot_submit_jobs_with_backend_arguments(self):
        with isolated_server() as server:
            for wrapped in (False, True):
                for value in ("--threads 2", "", None, "${inputs.args}"):
                    step = {
                        "id": "load",
                        "op": "load",
                        "params": {
                            "model": "user.DoesNotExist",
                            "llamacpp_args": value,
                        },
                    }
                    body = {"steps": [step]}
                    if wrapped:
                        body = {"definition": body}
                    body["inputs"] = {"args": "--threads 2"}
                    with self.subTest(wrapped=wrapped, value=value):
                        self.assert_forbidden_without_writes(server, "/v1/jobs", body)
            self.assert_forbidden_without_writes(
                server,
                "/v1/jobs",
                {
                    "steps": [
                        {
                            "id": "load",
                            "op": "load",
                            "params": {
                                "model": "user.DoesNotExist",
                                "merge_args": False,
                            },
                        }
                    ]
                },
            )
            self.assert_forbidden_without_writes(
                server,
                "/v1/jobs",
                {
                    "steps": [
                        {
                            "id": "load",
                            "op": "load",
                            "status": "completed",
                            "params": {
                                "model": "user.DoesNotExist",
                                "llamacpp_args": "--threads 2",
                            },
                        }
                    ]
                },
            )
            for key, options in (
                (REGULAR_KEY, {}),
                (ADMIN_KEY, {"llamacpp_args": "--threads 2"}),
            ):
                status, text = server.request(
                    "POST",
                    "/v1/jobs",
                    {
                        "steps": [
                            {
                                "id": "load",
                                "op": "load",
                                "params": {
                                    "model": "user.DoesNotExist",
                                    **options,
                                },
                            }
                        ]
                    },
                    key,
                )
                self.assertEqual(status, 202, text[:1000])

    def test_single_key_and_no_key_compatibility(self):
        for regular_key, admin_key, token in (
            (REGULAR_KEY, None, REGULAR_KEY),
            (REGULAR_KEY, "", REGULAR_KEY),
            (REGULAR_KEY, REGULAR_KEY, REGULAR_KEY),
            (None, None, None),
            (None, ADMIN_KEY, ADMIN_KEY),
        ):
            with self.subTest(regular=regular_key, admin=admin_key):
                with isolated_server(regular_key, admin_key) as server:
                    status, text = server.request(
                        "POST",
                        "/v1/models/register",
                        {
                            "model_name": "user.AuthCompatible",
                            "recipe": "llamacpp",
                            "recipe_options": {"llamacpp_args": "--threads 1"},
                        },
                        token,
                    )
                    self.assertEqual(status, 200, text[:1000])
                    status, text = server.request(
                        "POST",
                        "/v1/jobs",
                        {
                            "steps": [
                                {
                                    "id": "load",
                                    "op": "load",
                                    "params": {
                                        "model": "user.DoesNotExist",
                                        "llamacpp_args": "--threads 2",
                                    },
                                }
                            ]
                        },
                        token,
                    )
                    self.assertEqual(status, 202, text[:1000])

    def test_admin_only_configuration_requires_key_for_arguments(self):
        with isolated_server(None, ADMIN_KEY) as server:
            status, text = server.request(
                "POST",
                "/v1/models/register",
                {"model_name": "user.PublicRegistration", "recipe": "llamacpp"},
            )
            self.assertEqual(status, 200, text[:1000])
            before = server.saved_state()
            status, text = server.request(
                "POST",
                "/v1/models/register",
                {
                    "model_name": "user.PublicRegistration",
                    "recipe": "llamacpp",
                    "recipe_options": {"llamacpp_args": "--threads 1"},
                },
            )
            self.assertEqual(status, 403, text[:1000])
            self.assertEqual(server.saved_state(), before)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-binary", default=SERVER_BINARY)
    args, remaining = parser.parse_known_args()
    SERVER_BINARY = args.server_binary
    unittest.main(argv=[__file__, *remaining])
