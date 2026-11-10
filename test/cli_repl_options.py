import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from backend_catalog_http import Server, prepare, request


def main(server_binary, cli_binary):
    checks = 0
    child_pids = set()

    def check(condition, message):
        nonlocal checks
        assert condition, message
        checks += 1

    with tempfile.TemporaryDirectory(prefix="lemon-repl-options-") as temporary:
        root = Path(temporary)
        config = prepare(root)
        directory = config / "backends.d"
        directory.mkdir()
        harness = Path(__file__).with_name("fake_catalog_engine.py").resolve()
        source = Path(__file__).with_name("fixtures") / "catalog-fourth.json"
        definition = json.loads(source.read_text())
        definition["launch"]["argv"][0]["args"].insert(0, str(harness))
        (directory / "fourth.json").write_text(json.dumps(definition))
        settings = json.loads((config / "config.json").read_text())
        settings["catalog_fourth"] = {"cpu_bin": sys.executable}
        (config / "config.json").write_text(json.dumps(settings))
        tiny = root / "tiny model.fixture"
        tiny.write_bytes(b"deterministic CPU fixture\n")
        (config / "user_models.json").write_text(
            json.dumps(
                {
                    "CatalogFourth": {
                        "recipe": "catalog-fourth",
                        "checkpoint": str(tiny),
                        "source": "local_path",
                        "labels": ["chat"],
                    }
                }
            )
        )
        model = "user.CatalogFourth"
        options_path = "/v1/models/" + model + "/options"
        trace = tiny.with_suffix(".observations.jsonl")
        env = os.environ.copy()
        env.pop("LEMONADE_ADMIN_API_KEY", None)
        env["LEMONADE_API_KEY"] = "regular-test-key"
        env["XDG_CONFIG_HOME"] = str(root / "client")

        with Server(server_binary, root) as server:

            def cli(*arguments, input_text=None):
                result = subprocess.run(
                    [
                        str(cli_binary),
                        "--host",
                        "127.0.0.1",
                        "--port",
                        str(server.port),
                        "--no-discovery",
                        *arguments,
                    ],
                    input=input_text,
                    text=True,
                    capture_output=True,
                    env=env,
                    timeout=20,
                )
                check(
                    result.returncode == 0,
                    f"{arguments}: {result.stderr}\n{result.stdout}",
                )
                tasks = Path(f"/proc/{server.process.pid}/task")
                if tasks.exists():
                    for task in tasks.iterdir():
                        children = task / "children"
                        if children.exists():
                            child_pids.update(map(int, children.read_text().split()))
                return result.stdout

            def saved_options():
                status, body, _ = request(server.port, options_path)
                check(status == 200, "options endpoint success")
                return body

            def saved_bytes():
                path = config / "recipe_options.json"
                return path.read_bytes() if path.exists() else None

            def check_engine(expected):
                events = [json.loads(line) for line in trace.read_text().splitlines()]
                argv = next(
                    event["argv"]
                    for event in reversed(events)
                    if event["event"] == "argv"
                )
                check(
                    argv
                    == [
                        "--port",
                        argv[1],
                        "--batch",
                        str(expected),
                        "--tag",
                        "initial",
                        "--model-fixture",
                        str(tiny),
                    ]
                    and argv[1].isdigit()
                    and 0 < int(argv[1]) < 65536,
                    f"expected fake batch {expected}, argv: {argv}",
                )
                return events

            def chat(expected, saved, *arguments, reload=False):
                before = saved_bytes()
                commands = f"/unload {model}\n/load {model}\n" if reload else ""
                output = cli(
                    "run",
                    model,
                    "--chat-cli",
                    *arguments,
                    input_text=commands + "fixture\n/exit\n",
                )
                events = check_engine(expected)
                check(
                    f"fake batch={expected} tag=initial" in output,
                    "exact deterministic reply reaches REPL",
                )
                check(
                    events[-1]
                    == {
                        "event": "request",
                        "path": "/chat",
                        "body": {
                            "model": model,
                            "messages": [{"role": "user", "content": "fixture"}],
                            "stream": True,
                            "stream_options": {"include_usage": True},
                        },
                    },
                    "exact fake upstream request",
                )
                options = saved_options()
                check(
                    options["saved"].get("catalog_fourth_batch") == saved
                    and options["defaults"]["catalog_fourth_batch"] == 2
                    and options["effective"]["catalog_fourth_batch"]
                    == (2 if saved is None else saved),
                    "saved/default/effective options remain independent of transient load",
                )
                if "--save-options" not in arguments:
                    check(
                        saved_bytes() == before, "transient run writes no saved options"
                    )
                cli("unload", model)

            initial = saved_options()
            check(
                "catalog_fourth_batch" not in initial["saved"]
                and initial["effective"]["catalog_fourth_batch"] == 2,
                "fresh option inherits schema default 2",
            )
            before = saved_bytes()
            cli("load", model, "--option", "catalog_fourth_batch=4")
            check_engine(4)
            check(saved_bytes() == before, "transient load writes no saved options")
            cli("unload", model)
            chat(4, None, "--option", "catalog_fourth_batch=4")
            chat(4, None, "--option", "catalog_fourth_batch=4", reload=True)
            check(
                request(server.port, options_path, body={"catalog_fourth_batch": 5})[0]
                == 200,
                "options endpoint explicitly saves 5",
            )
            chat(4, 5, "--option", "catalog_fourth_batch=4")
            chat(2, 5, "--option", "catalog_fourth_batch=null", reload=True)
            chat(5, 5)
            chat(6, 6, "--save-options", "--option", "catalog_fourth_batch=6")
            check(
                tiny.read_bytes() == b"deterministic CPU fixture\n", "fixture unchanged"
            )
        check(server.process.returncode == 0, "owned server exits cleanly")
        if sys.platform.startswith("linux"):
            deadline = time.monotonic() + 3
            while any(Path(f"/proc/{pid}").exists() for pid in child_pids):
                if time.monotonic() >= deadline:
                    break
                time.sleep(0.02)
            check(bool(child_pids), "owned fake engine processes observed")
            check(
                all(not Path(f"/proc/{pid}").exists() for pid in child_pids),
                "all owned fake engine processes reaped",
            )
    print(f"{checks}/{checks} CLI REPL fake-engine checks passed")


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
