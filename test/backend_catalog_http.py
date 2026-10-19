"""CPU-only catalog HTTP, bootstrap, and persistence acceptance checks."""

import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def definition(identity="http-alpha", option="http_batch"):
    return {
        "schema_version": 1,
        "id": identity,
        "recipe": identity,
        "kind": "local",
        "metadata": {
            "display_name": "HTTP Alpha",
            "server_name": "http-fixture",
            "binary": "SEEDED_PRIVATE_BINARY",
            "default_device": "cpu",
            "bin_variants": ["cpu"],
            "options": [
                {
                    "name": option,
                    "value_type": "integer",
                    "default_value": 2,
                    "minimum": -1,
                    "maximum": 8,
                }
            ],
        },
        "install": {"strategy": "external_only"},
        "launch": {
            "prepare_order": ["artifacts", "backend", "program", "ports"],
            "backend": {"fixed": "cpu"},
            "program": {"source": "external_or_managed", "install_policy": "always"},
            "ports": [{"name": "http", "reject_zero": True, "error": "No port"}],
            "argv": [{"args": ["SEEDED_PRIVATE_ARGV"]}],
            "environment": {
                "preset": "inherit",
                "set": [{"name": "PRIVATE_TOKEN", "value": "SEEDED_PRIVATE_TOKEN"}],
            },
            "working_directory": "inherit",
            "readiness": {"port": "http", "path": "/health", "timeout": 5},
            "watchdog": {"port": "http", "path": "/health", "policy": "none"},
            "output": {"inherit_levels": ["debug"], "filter": False},
        },
        "operations": [
            {
                "operation": "chat_completion",
                "delivery": "buffered",
                "model_modes": ["chat"],
                "transport": {
                    "kind": "http_json",
                    "method": "POST",
                    "path": "/chat",
                    "port": "http",
                    "timeout": 5,
                    "response_codec": "json.v1",
                },
                "request": {"base": "identity"},
            }
        ],
    }


def request(port, path, key="regular-test-key", body=None, headers=None):
    fields = dict(headers or {})
    if key is not None:
        fields["Authorization"] = "Bearer " + key
    if body is not None:
        fields["Content-Type"] = "application/json"
        if not isinstance(body, bytes):
            body = json.dumps(body).encode()
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}", data=body, headers=fields
    )
    try:
        response = urllib.request.urlopen(req, timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = response.read()
        return response.status, json.loads(data) if data else None, response.headers


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Server:
    def __init__(self, binary, root, *args, env_overrides=None):
        self.root = Path(root)
        self.port = free_port()
        self.log = tempfile.TemporaryFile(mode="w+b")
        env = os.environ.copy()
        env.update(
            LEMONADE_API_KEY="regular-test-key",
            LEMONADE_ADMIN_API_KEY="admin-test-key",
        )
        env.update(env_overrides or {})
        self.process = subprocess.Popen(
            [
                str(binary),
                str(self.root / "cache"),
                str(self.root / "config"),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--no-broadcast",
                "--log-file",
                "disabled",
                *args,
            ],
            stdout=self.log,
            stderr=self.log,
            env=env,
        )

    def ready(self):
        for _ in range(150):
            if self.process.poll() is not None:
                self.log.seek(0)
                raise AssertionError(self.log.read().decode(errors="replace")[-4000:])
            try:
                if request(self.port, "/v1/health")[0] == 200:
                    return self
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.1)
        raise AssertionError("Server readiness timeout")

    def close(self):
        if self.process.poll() is None:
            try:
                request(self.port, "/internal/shutdown", key="admin-test-key", body={})
                self.process.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                self.process.terminate()
                self.process.wait(timeout=10)
        self.log.close()

    def __enter__(self):
        try:
            return self.ready()
        except BaseException:
            self.close()
            raise

    def __exit__(self, *_):
        self.close()


def prepare(root):
    config = root / "config"
    config.mkdir(parents=True)
    (config / "config.json").write_text(
        json.dumps(
            {
                "offline": True,
                "auto_check_model_updates": False,
                "disable_model_filtering": True,
                "inhibit_suspend": False,
                "log_file": "disabled",
                "broadcast": False,
            }
        )
    )
    return config


def main(binary):
    checks = 0

    def check(condition, message):
        nonlocal checks
        assert condition, message
        checks += 1

    with tempfile.TemporaryDirectory(prefix="lemon-catalog-http-") as temporary:
        root = Path(temporary)
        config = prepare(root)
        directory = config / "backends.d"
        directory.mkdir()
        alpha = definition()
        beta = definition("http-beta", "beta_batch")
        (directory / "a.json").write_text(json.dumps(alpha))
        (directory / "z.json").write_text(json.dumps(beta))
        (directory / "ignored.txt").write_text("{")
        fixture = root / "tiny.fixture"
        fixture.write_bytes(b"fake catalog fixture")
        (config / "user_models.json").write_text(
            json.dumps(
                {"http_fixture": {"recipe": "http-alpha", "checkpoint": str(fixture)}}
            )
        )
        with Server(binary, root) as server:
            status, schema, headers = request(server.port, "/v1/backends/schema")
            check(status == 200, "schema success")
            catalog_id = schema["catalog_id"]
            check(
                [row["recipe"] for row in schema["recipes"]][-2:]
                == ["http-alpha", "http-beta"],
                "discovery order",
            )
            check(
                all(
                    marker not in json.dumps(schema)
                    for marker in (
                        "SEEDED_PRIVATE",
                        "PRIVATE_TOKEN",
                        str(root),
                        "argv",
                        "environment",
                        "install",
                    )
                ),
                "allowlisted export excludes private definition details",
            )
            expected = {
                "recipe": "http-alpha",
                "definition_id": "http-alpha",
                "definition_hash": "sha256:"
                + hashlib.sha256(
                    json.dumps(alpha, sort_keys=True, separators=(",", ":")).encode()
                ).hexdigest(),
                "source": "external",
                "backend_option": None,
                "display_name": "HTTP Alpha",
                "supported_modes": ["chat"],
                "options": [
                    {
                        "name": "http_batch",
                        "value_type": "integer",
                        "default_value": 2,
                        "minimum": -1,
                        "maximum": 8,
                        "unset_values": [None],
                    },
                    {
                        "name": "merge_args",
                        "value_type": "boolean",
                        "default_value": True,
                    },
                    {
                        "name": "auto_evict",
                        "value_type": "boolean",
                        "default_value": None,
                    },
                    {
                        "name": "evict_idle_timeout",
                        "value_type": "integer",
                        "default_value": 300,
                    },
                    {
                        "name": "downsize_idle_timeout",
                        "value_type": "integer",
                        "default_value": 60,
                    },
                    {
                        "name": "evict_weight_factor",
                        "value_type": "number",
                        "default_value": 1.0,
                    },
                    {"name": "pinned", "value_type": "boolean", "default_value": False},
                    {
                        "name": "auto_update",
                        "value_type": "boolean",
                        "default_value": None,
                    },
                ],
                "operations_complete": True,
                "operations": [
                    {
                        "operation": "chat_completion",
                        "delivery": "buffered",
                        "model_modes": ["chat"],
                        "provenance": "implemented",
                    }
                ],
                "support": [],
            }
            for option in expected["options"][1:]:
                option["unset_values"] = [None, -1, "", "auto"]
            check(schema["recipes"][-2] == expected, "hand-authored exact external row")
            for prefix in ("/api/v0/", "/api/v1/", "/v0/", "/v1/"):
                path = prefix + "backends/schema"
                check(request(server.port, path)[1] == schema, "quad prefix schema")
                check(
                    request(server.port, path + "?recipe=http-alpha")[1]["recipes"]
                    == [expected],
                    "exact recipe filter",
                )
                for query, code in (
                    ("?recipe=", 400),
                    ("?recipe=x&recipe=x", 400),
                    ("?recipe=absent", 404),
                ):
                    result = request(server.port, path + query)
                    check(result[0] == code, f"filter error {path}{query}: {result}")
                    if code == 404:
                        check(
                            result[1]
                            == {
                                "error": {
                                    "message": "Unknown backend recipe: absent",
                                    "type": "invalid_request_error",
                                    "code": "unknown_recipe",
                                }
                            },
                            "defined unknown_recipe payload",
                        )
                for key in (None, "wrong", "regular-test-key", "admin-test-key"):
                    status, payload, _ = request(
                        server.port,
                        path,
                        key=key,
                        headers={"If-None-Match": headers["ETag"]},
                    )
                    check(
                        status
                        == (
                            304
                            if key in ("regular-test-key", "admin-test-key")
                            else 401
                        ),
                        "auth before conditional response",
                    )
                    if status == 401:
                        check(
                            payload == {"error": "Invalid or missing API key"},
                            "existing auth error payload",
                        )
                    check(
                        request(
                            server.port,
                            prefix + "backends/validate",
                            key=key,
                            body=alpha,
                        )[0]
                        == (
                            200
                            if key in ("regular-test-key", "admin-test-key")
                            else 401
                        ),
                        "validation auth",
                    )
                check(
                    request(
                        server.port,
                        path,
                        key=None,
                        headers={"Authorization": "Basic abc"},
                    )[0]
                    == 401,
                    "malformed auth",
                )
                duplicate = (
                    json.dumps(alpha)
                    .replace('"id":', '"id":"duplicate","id":', 1)
                    .encode()
                )
                for body, code in (
                    (b"{", 400),
                    (duplicate, 400),
                    (b" " * (1024 * 1024 + 1), 413),
                ):
                    result = request(
                        server.port, prefix + "backends/validate", body=body
                    )
                    check(
                        result[0] == code and result[1]["valid"] is False,
                        "invalid original validation bytes",
                    )
            model_path = "/v1/models/user.http_fixture/options"
            for value in (-1, 0, 5):
                result = request(server.port, model_path, body={"http_batch": value})
                check(
                    result[0] == 200
                    and result[1]["saved"]["http_batch"] == value
                    and result[1]["effective"]["http_batch"] == value,
                    "typed saved and effective external options",
                )
            saved_before = (config / "recipe_options.json").read_bytes()
            for value in (9, 2.5, "2", False):
                check(
                    request(server.port, model_path, body={"http_batch": value})[0]
                    == 400,
                    "saved option contract rejection",
                )
                check(
                    (config / "recipe_options.json").read_bytes() == saved_before,
                    "invalid option writes nothing",
                )
            saved_before = (config / "recipe_options.json").read_bytes()
            rejected_load = request(
                server.port,
                "/v1/load",
                body={
                    "model_name": "user.http_fixture",
                    "http_batch": 9,
                    "save_options": True,
                },
            )
            check(
                rejected_load[0] != 200
                and (config / "recipe_options.json").read_bytes() == saved_before,
                "invalid request cannot persist options or start a backend",
            )
            result = request(server.port, model_path, body={"http_batch": None})
            check(
                result[0] == 200
                and "http_batch" not in result[1]["saved"]
                and result[1]["effective"]["http_batch"] == 2,
                "null clears external saved option",
            )
            before = {
                str(p.relative_to(root)): p.read_bytes()
                for p in root.rglob("*")
                if p.is_file()
            }
            changed = definition("http-alpha", "http_batch")
            changed["metadata"]["options"][0]["default_value"] = 7
            check(
                request(server.port, "/v1/backends/validate", body=changed)[1]["valid"]
                is True,
                "private matching substitution",
            )
            changed["recipe"] = "ds4"
            check(
                request(server.port, "/v1/backends/validate", body=changed)[1]["valid"]
                is False,
                "builtin cannot substitute",
            )
            check(
                before
                == {
                    str(p.relative_to(root)): p.read_bytes()
                    for p in root.rglob("*")
                    if p.is_file()
                },
                "validation writes no config/model/option files",
            )
            check(
                request(server.port, "/v1/backends/schema")[1] == schema,
                "validation publishes nothing",
            )
            (directory / "a.json").write_text("{")
            if os.name != "nt":
                server.process.send_signal(signal.SIGHUP)
            check(
                request(server.port, "/v1/backends/schema")[1] == schema,
                "file changes and SIGHUP leave frozen catalog",
            )
        before = (config / "config.json").read_bytes()
        failed = Server(binary, root)
        try:
            check(failed.process.wait(timeout=15) != 0, "invalid catalog fails startup")
            check(
                (config / "config.json").read_bytes() == before,
                "catalog failure precedes config consumption",
            )
            try:
                request(failed.port, "/v1/health")
                listening = True
            except (OSError, urllib.error.URLError):
                listening = False
            check(not listening, "invalid catalog never listens")
        finally:
            failed.close()
        with Server(binary, root, "--builtin-backends-only") as server:
            check(
                len(request(server.port, "/v1/backends/schema")[1]["recipes"]) == 17,
                "builtins-only skips broken external files",
            )
        (directory / "a.json").write_text(json.dumps(alpha, indent=4))
        with Server(binary, root) as server:
            check(
                request(server.port, "/v1/backends/schema")[1]["catalog_id"]
                == catalog_id,
                "restart digest excludes port/path and formatting",
            )
        alpha["metadata"]["options"].append(
            {"name": "constructor", "value_type": "string", "default_value": "cpu"}
        )
        alpha["launch"]["backend"] = {
            "option": "constructor",
            "fallback": "first_supported",
            "on_empty": {"operation": "load", "target": "fixture"},
        }
        (directory / "a.json").write_text(json.dumps(alpha))
        with Server(binary, root) as server:
            selected_schema = request(server.port, "/v1/backends/schema")[1]
            check(
                selected_schema["recipes"][-2]["backend_option"] == "constructor",
                "selector export without UI hints or suffix",
            )
            check(
                selected_schema["catalog_id"] != catalog_id,
                "selector metadata changes hash and ETag",
            )
            frozen_cli = os.environ.get("LEMONADE_FROZEN_CLI")
            if frozen_cli:
                cli_env = os.environ.copy()
                cli_env.pop("LEMONADE_ADMIN_API_KEY", None)
                cli_env["LEMONADE_API_KEY"] = "regular-test-key"
                cli_env["XDG_CONFIG_HOME"] = str(root / "cli-config")
                reply = subprocess.run(
                    [
                        frozen_cli,
                        "--host",
                        "127.0.0.1",
                        "--port",
                        str(server.port),
                        "--no-discovery",
                        "backends",
                        "schema",
                        "http-alpha",
                    ],
                    capture_output=True,
                    text=True,
                    env=cli_env,
                    timeout=12,
                )
                check(reply.returncode == 0, reply.stderr)
                check(
                    json.loads(reply.stdout)["recipes"][-1]["backend_option"]
                    == "constructor",
                    "frozen CLI accepts additive selector metadata",
                )
        alpha["metadata"]["options"].pop()
        alpha["launch"]["backend"] = {"fixed": "cpu"}
        (directory / "a.json").write_text(json.dumps(alpha))
        retired_optins = (
            "LEMONADE_THINKSOUND_CONFIGURED",
            "LEMONADE_DS4_CONFIGURED",
            "LEMONADE_RYZENAI_CONFIGURED",
        )
        for value in ("0", "1"):
            with Server(
                binary, root, env_overrides={key: value for key in retired_optins}
            ) as server:
                check(
                    request(server.port, "/v1/backends/schema")[1]["catalog_id"]
                    == catalog_id,
                    "retired configured opt-in environment does not affect catalog digest",
                )
        alpha["metadata"]["options"][0]["default_value"] = 3
        (directory / "a.json").write_text(json.dumps(alpha))
        with Server(binary, root) as server:
            check(
                request(server.port, "/v1/backends/schema")[1]["catalog_id"]
                != catalog_id,
                "restart adopts changed definition",
            )
    print(f"{checks}/{checks} catalog HTTP checks passed")


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
