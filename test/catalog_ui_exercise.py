"""CPU-only served UI exercise with installed Chrome; no browser dependency is added."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from backend_catalog_http import Server, free_port, prepare, request


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--browser", default=shutil.which("google-chrome"))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)
    transcript = {
        "stages": [],
        "commands": [],
        "checks": 0,
        "browser_gate": "unverified",
    }
    checks = 0

    def check(condition, message):
        nonlocal checks
        assert condition, message
        checks += 1

    artifacts = json.loads(
        (repo / "test/fixtures/catalog-fourth-freeze.json").read_text()
    )["artifacts"]
    artifacts = {name: digest(repo / name) for name in artifacts}
    for file in (repo / "build/resources/web-app").rglob("*"):
        if file.is_file():
            artifacts[str(file.relative_to(repo))] = digest(file)
    transcript["freeze"] = {
        "at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True
        ).strip(),
        "artifacts": artifacts,
        "scope": "server, CLI, actual web assets, shipped schema/resources, CPU harness",
    }
    for name in (
        "build/lemond",
        "build/lemonade",
        "build/resources/web-app/renderer.bundle.js",
    ):
        data = (repo / name).read_bytes()
        check(
            all(
                value not in data
                for value in (
                    b"catalog-fourth",
                    b"catalog_fourth_batch",
                    b"ui-postfreeze-probe",
                    b"ui_probe_batch",
                )
            ),
            "probe identities absent from frozen product: " + name,
        )
    (args.output / "freeze.json").write_text(json.dumps(transcript["freeze"], indent=2))
    env = os.environ.copy()
    env.update(
        LEMONADE_API_KEY="regular-test-key", LEMONADE_ADMIN_API_KEY="admin-test-key"
    )
    process = browser = log = browser_log = None
    pids = set()
    with tempfile.TemporaryDirectory(prefix="lemon-ui-") as temporary:
        root = Path(temporary)
        config = prepare(root)
        settings = json.loads((config / "config.json").read_text())
        flm = root / "fake-flm"
        flm.write_text(
            "#!/usr/bin/env python3\nimport json, sys\n"
            "if sys.argv[1:] == ['version', '--json']:\n"
            "    print(json.dumps({'version': '99.0.0'}))\n"
            "elif sys.argv[1:] == ['list', '--json']:\n"
            "    print(json.dumps({'models': [{'name': 'Legacy', 'installed': True}]}))\n"
            "else:\n    sys.exit('Unexpected fake FLM invocation')\n"
        )
        flm.chmod(0o755)
        settings["flm"] = {"npu_bin": str(flm)}
        for section in ("catalog_fourth", "ui_probe"):
            settings[section] = {"cpu_bin": str(repo / "test/fake_catalog_engine.py")}
        (config / "config.json").write_text(json.dumps(settings))
        port, debug_port = free_port(), free_port()
        manifest = {"port": port, "debugPort": debug_port, "output": str(args.output)}
        manifest_path = root / "manifest.json"
        manifest_path.write_text(json.dumps(manifest))

        def start():
            nonlocal process, log
            log = tempfile.TemporaryFile()
            process = subprocess.Popen(
                [
                    str(repo / "build/lemond"),
                    str(root / "cache"),
                    str(config),
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(port),
                    "--no-broadcast",
                    "--log-file",
                    "disabled",
                ],
                stdout=log,
                stderr=log,
                env=env,
            )
            for _ in range(150):
                if process.poll() is not None:
                    log.seek(0)
                    raise AssertionError(log.read().decode(errors="replace")[-4000:])
                try:
                    if request(port, "/v1/health")[0] == 200:
                        return
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(0.1)
            raise AssertionError("lemond readiness timeout")

        def stop():
            nonlocal process, log
            if process is None:
                return
            tasks = Path(f"/proc/{process.pid}/task")
            for task in tasks.iterdir() if tasks.exists() else []:
                try:
                    pids.update(
                        int(pid) for pid in (task / "children").read_text().split()
                    )
                except FileNotFoundError:
                    pass
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=15)
            code = process.returncode
            log.seek(0)
            output = log.read().decode(errors="replace")[-4000:]
            process = None
            log.close()
            log = None
            check(code == 0, f"lemond exits cleanly: {code}: {output}")

        def drive(stage):
            result = subprocess.run(
                [
                    "node",
                    str(repo / "test/app/catalogBrowser.cjs"),
                    str(manifest_path),
                    stage,
                ],
                capture_output=True,
                text=True,
                timeout=90,
            )
            (args.output / (stage + ".log")).write_text(result.stdout + result.stderr)
            check(result.returncode == 0, stage + ": " + result.stdout + result.stderr)
            transcript["stages"].append(json.loads(result.stdout))

        try:
            start()
            check(
                len(request(port, "/v1/backends/schema")[1]["recipes"]) == 17,
                "baseline has no external definition",
            )
            html = urllib.request.urlopen(f"http://127.0.0.1:{port}/app").read()
            check(
                b"const connectionListeners" in html and b"window.api =" in html,
                "real injected browser bridge served",
            )
            if args.browser:
                browser_log = tempfile.TemporaryFile()
                browser = subprocess.Popen(
                    [
                        args.browser,
                        "--headless=new",
                        "--no-sandbox",
                        "--disable-gpu",
                        "--disable-software-rasterizer",
                        "--disable-features=Vulkan,WebGPU",
                        "--disable-dev-shm-usage",
                        "--no-first-run",
                        "--disable-background-timer-throttling",
                        "--disable-renderer-backgrounding",
                        "--disable-backgrounding-occluded-windows",
                        "--no-default-browser-check",
                        "--remote-allow-origins=*",
                        f"--remote-debugging-port={debug_port}",
                        f"--user-data-dir={root / 'browser'}",
                        "about:blank",
                    ],
                    stdout=browser_log,
                    stderr=browser_log,
                    start_new_session=True,
                )
                for _ in range(100):
                    try:
                        urllib.request.urlopen(
                            f"http://127.0.0.1:{debug_port}/json/version"
                        )
                        break
                    except (OSError, urllib.error.URLError):
                        time.sleep(0.1)
                drive("baseline")
            stop()
            frozen_config = digest(config / "config.json")
            transcript["freeze"]["at"] = datetime.datetime.now(
                datetime.timezone.utc
            ).isoformat()
            transcript["freeze"]["selected_config_sha256"] = frozen_config
            transcript["freeze"]["discovery_harness_sha256"] = digest(flm)
            transcript["freeze"]["configured_optins"] = {
                name: env.get(name, "0")
                for name in (
                    "LEMONADE_THINKSOUND_CONFIGURED",
                    "LEMONADE_DS4_CONFIGURED",
                    "LEMONADE_RYZENAI_CONFIGURED",
                )
            }
            (args.output / "freeze.json").write_text(
                json.dumps(transcript["freeze"], indent=2)
            )
            definition = json.loads(
                (repo / "test/fixtures/catalog-fourth.json").read_text()
            )
            directory = config / "backends.d"
            directory.mkdir()
            fourth = directory / "fourth.json"
            fourth.write_text(json.dumps(definition))
            # Authored after the complete artifact freeze, using installed primitives.
            probe = json.loads(
                json.dumps(definition)
                .replace("catalog-fourth", "ui-postfreeze-probe")
                .replace("catalog_fourth", "ui_probe")
            )
            probe["metadata"]["display_name"] = "UI Postfreeze Probe"
            probe["metadata"]["options"][0]["ui"]["label"] = "Probe batch"
            for name, kind, control, default, order in (
                ("text", "string", "text", "", 20),
                ("notes", "string", "textarea", "notes", 30),
                ("flag", "boolean", "checkbox", False, 40),
                ("choice", "boolean", "select", False, 50),
            ):
                option = {
                    "name": "ui_probe_" + name,
                    "value_type": kind,
                    "default_value": default,
                    "ui": {
                        "label": "Probe " + name,
                        "control": control,
                        "order": order,
                    },
                }
                if control == "select":
                    option["enum"] = [False, True]
                probe["metadata"]["options"].append(option)
            (directory / "probe.json").write_text(json.dumps(probe))
            tiny = root / "tiny.fixture"
            tiny.write_bytes(b"CPU fake fixture, no model weights\n")
            metadata = root / "context-metadata.gguf"

            def gguf_string(value):
                value = value.encode()
                return struct.pack("<Q", len(value)) + value

            metadata.write_bytes(
                b"GGUF"
                + struct.pack("<IQQ", 3, 0, 2)
                + gguf_string("general.architecture")
                + struct.pack("<I", 8)
                + gguf_string("llama")
                + gguf_string("llama.context_length")
                + struct.pack("<II", 4, 8192)
            )
            models = {
                "CatalogFourth": {
                    "recipe": "catalog-fourth",
                    "checkpoint": str(tiny),
                    "source": "local_path",
                    "labels": ["chat"],
                },
                "UiProbe": {
                    "recipe": "ui-postfreeze-probe",
                    "checkpoint": str(tiny),
                    "source": "local_path",
                    "labels": ["chat"],
                },
            }
            for recipe in json.loads(
                (repo / "test/app/fixtures/backend-catalog/legacy.json").read_text()
            )["recipes"]:
                if recipe == "flm":
                    continue
                models["Legacy-" + recipe] = {
                    "recipe": recipe,
                    "checkpoint": str(metadata if recipe == "llamacpp" else tiny),
                    "source": "local_path",
                    "labels": [],
                }
            (config / "user_models.json").write_text(json.dumps(models))
            start()
            if browser:
                for stage in ("fourth", "probe", "legacy", "dirty"):
                    drive(stage)
                definition["metadata"]["options"][0]["maximum"] = 4
                definition["metadata"]["options"][0]["default_value"] = 3
                fourth.write_text(json.dumps(definition))
                stop()
                start()
                for stage in ("restart", "incompatible", "auth"):
                    drive(stage)
                other_root = root / "other-server"
                prepare(other_root)
                with Server(repo / "build/lemond", other_root) as other:
                    manifest["otherPort"] = other.port
                    manifest_path.write_text(json.dumps(manifest))
                    drive("switch")
                transcript["browser_gate"] = (
                    "passed listed browser stages; native Tauri unverified"
                )
            else:
                transcript["unverified"] = [
                    "CSS layout",
                    "focus/keyboard",
                    "controlled editing",
                    "live React effects",
                    "Tauri webview",
                    "browser Save/Load/chat",
                ]
            cli_env = {**env, "XDG_CONFIG_HOME": str(root / "client")}
            for model, option in (
                ("user.CatalogFourth", "catalog_fourth_batch"),
                ("user.UiProbe", "ui_probe_batch"),
            ):
                configured = subprocess.run(
                    [
                        str(repo / "build/lemonade"),
                        "--host",
                        "127.0.0.1",
                        "--port",
                        str(port),
                        "--no-discovery",
                        "load",
                        model,
                        "--save-options",
                        "--option",
                        option + "=4",
                    ],
                    capture_output=True,
                    text=True,
                    env=cli_env,
                    timeout=20,
                )
                transcript["commands"].append(
                    {
                        "argv": configured.args,
                        "exit_code": configured.returncode,
                        "stdout": configured.stdout,
                        "stderr": configured.stderr,
                    }
                )
                check(
                    configured.returncode == 0,
                    "frozen CLI saves "
                    + model
                    + ": "
                    + configured.stdout
                    + configured.stderr,
                )
                saved = request(port, "/v1/models/" + model + "/options")[1]
                check(
                    saved["saved"][option] == 4 and saved["effective"][option] == 4,
                    "frozen CLI saved/effective integer 4: " + model,
                )
                result = subprocess.run(
                    [
                        str(repo / "build/lemonade"),
                        "--host",
                        "127.0.0.1",
                        "--port",
                        str(port),
                        "--no-discovery",
                        "run",
                        model,
                        "--chat-cli",
                        "--option",
                        option + "=4",
                    ],
                    input="frozen CLI probe\n/exit\n",
                    capture_output=True,
                    text=True,
                    env=cli_env,
                    timeout=20,
                )
                (args.output / (model + "-cli.log")).write_text(
                    result.stdout + result.stderr
                )
                transcript["commands"].append(
                    {
                        "argv": result.args,
                        "exit_code": result.returncode,
                        "stdout": result.stdout,
                        "stderr": result.stderr,
                    }
                )
                check(
                    result.returncode == 0
                    and "fake batch=4 tag=initial" in result.stdout,
                    "frozen CLI invokes "
                    + model
                    + ": "
                    + result.stdout
                    + result.stderr,
                )
            observations = [
                json.loads(line)
                for line in tiny.with_suffix(".observations.jsonl")
                .read_text()
                .splitlines()
            ]
            transcript["observations"] = observations
            check(
                all(
                    item["path"] == "/chat"
                    for item in observations
                    if item["event"] == "request"
                ),
                "exact fake upstream path",
            )
            check(
                all(
                    item["argv"]
                    == [
                        "--port",
                        item["argv"][1],
                        "--batch",
                        "4",
                        "--tag",
                        "initial",
                        "--model-fixture",
                        str(tiny),
                    ]
                    and item["argv"][1].isdigit()
                    and 0 < int(item["argv"][1]) < 65536
                    for item in observations
                    if item["event"] == "argv"
                ),
                "exact fake batch/tag argv",
            )
            expected_requests = []
            if browser:
                for model in ("CatalogFourth", "UiProbe"):
                    expected_requests.append(
                        {
                            "event": "request",
                            "path": "/chat",
                            "body": {
                                "model": model,
                                "messages": [
                                    {
                                        "role": "user",
                                        "content": "UI frozen user." + model,
                                    }
                                ],
                                "stream": True,
                            },
                        }
                    )
            for model in ("user.CatalogFourth", "user.UiProbe"):
                expected_requests.append(
                    {
                        "event": "request",
                        "path": "/chat",
                        "body": {
                            "model": model,
                            "messages": [
                                {"role": "user", "content": "frozen CLI probe"}
                            ],
                            "stream": True,
                            "stream_options": {"include_usage": True},
                        },
                    }
                )
            check(
                [item for item in observations if item["event"] == "request"]
                == expected_requests,
                "independent exact UI/CLI upstream request bodies",
            )
            stop()
            check(
                all(not Path(f"/proc/{pid}").exists() for pid in pids),
                "owned fake children reaped",
            )
            check(
                all(
                    digest(repo / name) == expected
                    for name, expected in artifacts.items()
                ),
                "all frozen artifacts unchanged",
            )
            transcript["reaped_child_pids"] = sorted(pids)
            check(
                digest(config / "config.json") == frozen_config,
                "selected server configuration unchanged after freeze",
            )
            check(
                digest(flm) == transcript["freeze"]["discovery_harness_sha256"],
                "CPU discovery harness unchanged after freeze",
            )
            transcript["checks"] = checks
        finally:
            try:
                stop()
            finally:
                if browser:
                    os.killpg(browser.pid, signal.SIGTERM)
                    browser.wait(timeout=10)
                    browser_log.close()
    (args.output / "transcript.json").write_text(
        json.dumps(transcript, indent=2) + "\n"
    )
    print(f"{checks}/{checks} UI exercise checks passed; {transcript['browser_gate']}")


if __name__ == "__main__":
    main()
