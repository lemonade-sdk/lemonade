import copy
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from urllib.parse import urlparse

from backend_catalog_http import definition, free_port

SCHEMA = {
    "schema_version": 1,
    "control_version": 1,
    "definition_schema_version": 1,
    "catalog_id": "sha256:" + "a" * 64,
    "feature_manifest_id": "sha256:" + "b" * 64,
    "recipes": [
        {
            "recipe": "server-only",
            "options": [
                {
                    "name": "server_batch",
                    "value_type": "integer",
                    "default_value": 2,
                    "minimum": -1,
                    "maximum": 8,
                },
                {
                    "name": "server_enabled",
                    "value_type": "boolean",
                    "default_value": False,
                },
                {"name": "server_label", "value_type": "string", "default_value": ""},
                {
                    "name": "server_ratio",
                    "value_type": "number",
                    "default_value": 0.5,
                    "enum": [0, 0.5, 1],
                },
                {"name": "ctx_size", "value_type": "integer", "default_value": -1},
                {"name": "merge_args", "value_type": "boolean", "default_value": True},
                {"name": "pinned", "value_type": "boolean", "default_value": False},
            ],
        }
    ],
}


class Fixture:
    def __init__(self):
        self.mode = "compatible"
        self.schema = copy.deepcopy(SCHEMA)
        self.calls = []
        self.downloaded = True
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def reply(self, code, body):
                raw = json.dumps(body).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_GET(self):
                owner.calls.append(("GET", self.path, None, time.time()))
                path = urlparse(self.path).path
                if path.startswith("/api/v1/models/"):
                    self.reply(
                        200,
                        {
                            "id": "remote-model",
                            "recipe": "server-only",
                            "downloaded": owner.downloaded,
                            "labels": ["chat"],
                        },
                    )
                elif path == "/api/v1/health":
                    self.reply(200, {"max_models": {}, "pinned_models": {}})
                elif path == "/api/v1/backends/schema":
                    if owner.mode == "old":
                        self.reply(404, {"error": "Not found"})
                    elif owner.mode.startswith("auth"):
                        self.reply(
                            int(owner.mode[4:]), {"error": "Invalid or missing API key"}
                        )
                    elif owner.mode == "unknown":
                        self.reply(
                            404,
                            {
                                "error": {
                                    "message": "Unknown backend recipe: server-only",
                                    "type": "invalid_request_error",
                                    "code": "unknown_recipe",
                                }
                            },
                        )
                    else:
                        self.reply(200, owner.schema)
                else:
                    self.reply(404, {"error": "Unexpected request"})

            def do_POST(self):
                raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                owner.calls.append(("POST", self.path, raw, time.time()))
                if self.path == "/api/v1/backends/validate":
                    if owner.mode == "old":
                        self.reply(404, {"error": "Not found"})
                    elif owner.mode.startswith("auth"):
                        self.reply(
                            int(owner.mode[4:]), {"error": "Invalid or missing API key"}
                        )
                    else:
                        valid = owner.mode != "invalid"
                        self.reply(
                            200 if valid else 400,
                            {
                                "valid": valid,
                                "validation": "server",
                                "catalog_id": SCHEMA["catalog_id"],
                                "diagnostics": [],
                            },
                        )
                elif self.path == "/api/v1/load":
                    self.reply(200, {"status": "success"})
                else:
                    self.reply(500, {"error": "Unexpected side effect"})

        self.http = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.http.server_port
        self.thread = threading.Thread(target=self.http.serve_forever)
        self.thread.start()

    def close(self):
        self.http.shutdown()
        self.http.server_close()
        self.thread.join()


def main(binary):
    checks = 0
    fixture = Fixture()

    def check(condition, message):
        nonlocal checks
        assert condition, message
        checks += 1

    with tempfile.TemporaryDirectory(prefix="lemon-schema-cli-") as temporary:
        root = Path(temporary)
        env = os.environ.copy()
        env.pop("LEMONADE_ADMIN_API_KEY", None)
        env["LEMONADE_API_KEY"] = "fixture-key"
        env["XDG_CONFIG_HOME"] = str(root / "client")
        marker = root / "agent-started"
        agent = root / "claude"
        agent.write_text(
            "#!/usr/bin/env python3\nimport pathlib,time\npathlib.Path("
            + repr(str(marker))
            + ").write_text(str(time.time()))\ntime.sleep(0.4)\n"
        )
        agent.chmod(0o755)
        env["PATH"] = str(root) + os.pathsep + env.get("PATH", "")

        def run(*args, target=True, input_text=None):
            fixture.calls.clear()
            command = [str(binary)]
            if target:
                command += [
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(fixture.port),
                    "--no-discovery",
                ]
            return subprocess.run(
                command + list(args),
                input=input_text,
                text=True,
                capture_output=True,
                env=env,
                timeout=12,
            )

        def no_posts():
            return not any(method == "POST" for method, *_ in fixture.calls)

        try:
            result = run(
                "load",
                "remote-model",
                "--ctx-size",
                "4096",
                "--no-merge-args",
                "--pinned",
            )
            check(result.returncode == 0, result.stderr)
            check(
                json.loads(fixture.calls[-1][2])
                == {
                    "model_name": "remote-model",
                    "save_options": False,
                    "ctx_size": 4096,
                    "merge_args": False,
                    "pinned": True,
                },
                "hand-authored existing named request",
            )
            check(
                not any("backends/schema" in path for _, path, *_ in fixture.calls),
                "named-only has no mandatory schema request",
            )
            help_text = run("load", "--help").stdout
            for text in (
                "--ctx-size",
                "Context size for the model",
                "--no-merge-args",
                "Merge global and model arguments",
                "--pinned",
                "Pin the model to prevent auto-eviction",
            ):
                check(text in help_text, "existing named help: " + text)
            check("KEY=JSON_VALUE" in help_text, "generic grammar exposed")
            result = run(
                "load",
                "--option",
                "server_batch=4",
                "remote-model",
                "--option",
                "server_enabled=false",
                "--option",
                'server_label="a=b"',
                "--option",
                "server_ratio=0",
            )
            check(result.returncode == 0, result.stderr)
            check(
                json.loads(fixture.calls[-1][2])
                == {
                    "model_name": "remote-model",
                    "save_options": False,
                    "server_batch": 4,
                    "server_enabled": False,
                    "server_label": "a=b",
                    "server_ratio": 0,
                },
                "exact typed raw JSON for recipe absent locally",
            )
            check(
                type(json.loads(fixture.calls[-1][2])["server_ratio"]) is int,
                "number contract preserves integer assignment",
            )
            for assignment in (
                'server_label=""',
                "server_batch=-1",
                "server_batch=0",
                "server_batch=null",
                "ctx_size=-1",
                "merge_args=false",
                "pinned=false",
            ):
                result = run("load", "remote-model", "--option", assignment)
                key, value = assignment.split("=", 1)
                check(
                    result.returncode == 0
                    and json.loads(fixture.calls[-1][2])[key] == json.loads(value),
                    "typed literal or default-valued unprovided flag: " + assignment,
                )
            for assignments in (
                ("server_batch=2", "server_batch=2"),
                ("=2",),
                ("server_batch=",),
                ("server_batch=2 junk",),
                ("server_batch=NaN",),
                ("server_batch=Infinity",),
                ("server_batch=1e10000",),
                ("server_batch=18446744073709551616",),
                ("server_batch=-9223372036854775809",),
                ("server_batch",),
            ):
                args = ["load", "remote-model"]
                for value in assignments:
                    args += ["--option", value]
                result = run(*args)
                check(
                    result.returncode == 2 and not fixture.calls,
                    "local grammar rejects before network: " + str(assignments),
                )
            for named, assignment in (
                (["--ctx-size", "-1"], "ctx_size=-1"),
                (["--no-merge-args"], "merge_args=false"),
                (["--pinned"], "pinned=true"),
                (["--save-options"], "save_options=true"),
                ([], 'model_name="other"'),
            ):
                result = run("load", "remote-model", *named, "--option", assignment)
                check(
                    result.returncode == 2 and not fixture.calls,
                    "explicit named/generic collision including aliases",
                )
            result = run(
                "launch",
                "claude",
                "--model",
                "remote-model",
                "--option",
                'model_name="other"',
            )
            check(
                result.returncode == 2 and not fixture.calls and not marker.exists(),
                "launch model flag collides before network",
            )
            for assignment in (
                "server_batch=2.0",
                "server_batch=9",
                'server_batch="2"',
                "server_batch=false",
                "server_batch=[]",
                "server_batch={}",
                "server_ratio=2",
                "missing=4",
                " server_batch=2",
            ):
                result = run("load", "remote-model", "--option", assignment)
                check(
                    result.returncode == 2 and no_posts(),
                    "server contract rejection before side effects: " + assignment,
                )
            for field in (
                "schema_version",
                "control_version",
                "definition_schema_version",
            ):
                fixture.schema[field] = 2
                fixture.downloaded = False
                result = run("run", "remote-model", "--option", "server_batch=4")
                check(
                    result.returncode == 1
                    and no_posts()
                    and "Incompatible" in result.stderr,
                    "unsupported version before pull/load/browser",
                )
                fixture.schema[field] = 1
            fixture.downloaded = True
            fixture.schema["recipes"][0]["options"][0]["value_type"] = "object"
            result = run("load", "remote-model", "--option", "server_batch=4")
            check(
                result.returncode == 1 and no_posts(),
                "unknown value type is incompatibility",
            )
            fixture.schema = copy.deepcopy(SCHEMA)
            for mode, message in (
                ("old", "Server schema support required"),
                ("auth401", "API key"),
                ("auth403", "Authentication failure"),
                ("unknown", "unknown_recipe"),
            ):
                fixture.mode = mode
                for command in (
                    ("load", "remote-model"),
                    ("run", "remote-model"),
                    ("launch", "claude", "--model", "remote-model"),
                ):
                    result = run(*command, "--option", "server_batch=4")
                    check(
                        result.returncode == 1
                        and no_posts()
                        and message in result.stderr
                        and not marker.exists(),
                        "compatibility matrix without side effects: "
                        + mode
                        + str(command),
                    )
                result = run("backends", "schema")
                check(
                    result.returncode == 1 and message in result.stderr,
                    "schema command compatibility matrix",
                )
            fixture.mode = "old"
            result = run("load", "remote-model", "--ctx-size", "1024")
            check(
                result.returncode == 0
                and not any("schema" in path for _, path, *_ in fixture.calls),
                "old server named-only path unchanged",
            )
            fixture.mode = "compatible"
            result = run("backends", "schema", "server/only +")
            check(
                result.returncode == 0 and json.loads(result.stdout) == SCHEMA,
                "schema stdout is complete JSON",
            )
            check(
                fixture.calls[0][1] == "/api/v1/backends/schema?recipe=server/only+%2B",
                "exact URL-encoded recipe filter: " + repr(fixture.calls),
            )
            file = root / "candidate.json"
            raw = b'{"schema_version":2,"id":"first","id":"second"}'
            file.write_bytes(raw)
            result = run("backends", "validate", str(file))
            check(
                result.returncode == 0 and fixture.calls[-1][2] == raw,
                "server validation sends original bytes without local rejection",
            )
            fixture.mode = "invalid"
            result = run("backends", "validate", str(file))
            check(
                result.returncode == 2 and json.loads(result.stdout)["valid"] is False,
                "server invalid definition exit 2",
            )
            for mode in ("old", "auth401", "auth403"):
                fixture.mode = mode
                check(
                    run("backends", "validate", str(file)).returncode == 1,
                    "validation transport/auth exit 1",
                )
            fixture.mode = "compatible"
            result = run("backends", "validate", str(file), "--offline", target=False)
            check(
                result.returncode == 2 and not fixture.calls,
                "offline invalid skips discovery/network",
            )
            check(
                result.stderr
                == "Offline validation only; server catalog collisions and executable bindings\nhave not been validated by the connected server.\n",
                "exact offline warning",
            )
            file.write_text(json.dumps(definition("offline-fixture", "offline_batch")))
            result = run("backends", "validate", str(file), "--offline", target=False)
            check(
                result.returncode == 0
                and json.loads(result.stdout)["validation"] == "offline"
                and not fixture.calls,
                "offline provisional success without network",
            )
            candidate = definition("offline-fixture", "offline_batch")
            candidate["metadata"]["options"][0]["unset_values"] = [[]]
            file.write_text(json.dumps(candidate))
            result = run("backends", "validate", str(file), "--offline", target=False)
            check(
                result.returncode == 2 and not fixture.calls,
                "v1 rejects nonscalar unset values",
            )
            file.write_bytes(b" " * (1024 * 1024 + 1))
            check(
                run("backends", "validate", str(file)).returncode == 2
                and not fixture.calls,
                "bounded file read before network",
            )
            check(
                run("backends", "validate", str(root / "absent")).returncode == 2
                and not fixture.calls,
                "unreadable definition exit 2",
            )
            fixture.schema["recipes"][0]["options"][0]["unset_values"] = []
            result = run("load", "remote-model", "--option", "server_batch=null")
            check(
                result.returncode == 0
                and json.loads(fixture.calls[-1][2])["server_batch"] is None,
                "REST null clearing is retained at CLI request boundary",
            )
            fixture.schema = copy.deepcopy(SCHEMA)
            for assignments, expected in (
                (("server_batch=4",), {"server_batch": 4}),
                (("server_batch=null",), {"server_batch": None}),
                (
                    ("server_enabled=false", 'server_label=""', "server_ratio=0"),
                    {"server_enabled": False, "server_label": "", "server_ratio": 0},
                ),
            ):
                arguments = ["run", "remote-model", "--chat-cli"]
                for assignment in assignments:
                    arguments += ["--option", assignment]
                result = run(
                    *arguments,
                    input_text="/model other-model\n/load remote-model\n/model remote-model\n/exit\n",
                )
                check(result.returncode == 0, result.stderr)
                bodies = [
                    json.loads(body)
                    for method, _, body, _ in fixture.calls
                    if method == "POST"
                ]
                original = {
                    "model_name": "remote-model",
                    "save_options": False,
                    **expected,
                }
                check(
                    bodies
                    == [
                        original,
                        original,
                        {"model_name": "other-model", "save_options": False},
                        original,
                        original,
                    ],
                    "generic values survive startup and same-model reloads only: "
                    + repr(bodies),
                )
            for arguments, first in (
                (("run", "remote-model", "--chat-cli"), {}),
                (
                    ("run", "remote-model", "--chat-cli", "--ctx-size", "1024"),
                    {"ctx_size": 1024},
                ),
                (("chat", "remote-model"), None),
            ):
                result = run(*arguments, input_text="/exit\n")
                check(result.returncode == 0, result.stderr)
                bodies = [
                    json.loads(body)
                    for method, _, body, _ in fixture.calls
                    if method == "POST"
                ]
                empty = {"model_name": "remote-model", "save_options": False}
                check(
                    bodies
                    == ([{**empty, **first}, empty] if first is not None else [empty])
                    and not any("schema" in path for _, path, *_ in fixture.calls),
                    "commands without generic options keep their requests",
                )
            result = run(
                "run",
                "remote-model",
                "--chat-cli",
                "--save-options",
                "--option",
                "server_batch=4",
                input_text="/exit\n",
            )
            check(result.returncode == 0, result.stderr)
            check(
                [
                    json.loads(body)
                    for method, _, body, _ in fixture.calls
                    if method == "POST"
                ]
                == [
                    {
                        "model_name": "remote-model",
                        "save_options": True,
                        "server_batch": 4,
                    },
                    {
                        "model_name": "remote-model",
                        "save_options": False,
                        "server_batch": 4,
                    },
                ],
                "only the explicit initial save persists options",
            )
            if os.name != "nt":
                result = run(
                    "launch",
                    "claude",
                    "--model",
                    "remote-model",
                    "--option",
                    "server_batch=4",
                )
                check(result.returncode == 0 and marker.exists(), result.stderr)
                posts = [call for call in fixture.calls if call[0] == "POST"]
                check(
                    len(posts) == 1
                    and json.loads(posts[0][2])
                    == {
                        "model_name": "remote-model",
                        "save_options": False,
                        "server_batch": 4,
                    },
                    "launch background raw option body",
                )
                schema_time = next(
                    stamp
                    for _, path, _, stamp in fixture.calls
                    if "backends/schema" in path
                )
                check(
                    schema_time < posts[0][3]
                    and schema_time < float(marker.read_text()),
                    "launch preflight precedes background work and agent start",
                )
                result = run(
                    "launch",
                    "claude",
                    "--model",
                    "remote-model",
                    "--option",
                    "server_batch=null",
                )
                check(result.returncode == 0, result.stderr)
                check(
                    [
                        json.loads(body)
                        for method, _, body, _ in fixture.calls
                        if method == "POST"
                    ]
                    == [
                        {
                            "model_name": "remote-model",
                            "save_options": False,
                            "server_batch": None,
                        }
                    ],
                    "launch background load retains explicit null without saving",
                )
            engine_port = free_port()
            nonexistent = root / "unread-model.fixture"
            engine_args = [
                "--port",
                str(engine_port),
                "--batch",
                "3",
                "--tag",
                "harness",
                "--model-fixture",
                str(nonexistent),
            ]
            engine = subprocess.Popen(
                [
                    sys.executable,
                    str(Path(__file__).with_name("fake_catalog_engine.py")),
                    *engine_args,
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                for _ in range(100):
                    try:
                        with urllib.request.urlopen(
                            f"http://127.0.0.1:{engine_port}/health", timeout=1
                        ):
                            break
                    except OSError:
                        time.sleep(0.02)
                body = {
                    "messages": [{"role": "user", "content": "fixture"}],
                    "stream": False,
                }
                req = urllib.request.Request(
                    f"http://127.0.0.1:{engine_port}/chat",
                    data=json.dumps(body).encode(),
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(req, timeout=2) as response:
                    answer = json.load(response)
                check(
                    answer
                    == {
                        "id": "fake",
                        "object": "chat.completion",
                        "choices": [
                            {
                                "index": 0,
                                "message": {
                                    "role": "assistant",
                                    "content": "fake batch=3 tag=harness",
                                },
                                "finish_reason": "stop",
                            }
                        ],
                        "usage": {
                            "prompt_tokens": 1,
                            "completion_tokens": 1,
                            "total_tokens": 2,
                        },
                    },
                    "independent deterministic harness response",
                )
                observations = [
                    json.loads(line)
                    for line in nonexistent.with_suffix(".observations.jsonl")
                    .read_text()
                    .splitlines()
                ]
                check(
                    observations
                    == [
                        {"event": "argv", "argv": engine_args},
                        {"event": "request", "path": "/chat", "body": body},
                    ],
                    "independent harness argv and request",
                )
                check(
                    not nonexistent.exists(),
                    "harness never reads or creates a model fixture",
                )
            finally:
                engine.terminate()
                engine.wait(timeout=5)
            closed_port = free_port()
            result = subprocess.run(
                [
                    str(binary),
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(closed_port),
                    "--no-discovery",
                    "load",
                    "remote-model",
                    "--option",
                    "server_batch=4",
                ],
                text=True,
                capture_output=True,
                env=env,
                timeout=8,
            )
            check(result.returncode == 1, "absent server has no offline fallback")
        finally:
            fixture.close()
    print(f"{checks}/{checks} frozen CLI checks passed")


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
