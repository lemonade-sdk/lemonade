import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.error

from backend_catalog_http import free_port, prepare, request

FROZEN_FILES = (
    "build/include/backend_capabilities_generated.h",
    "build/include/backend_definition_resources.h",
    "build/include/backend_descriptors_generated.h",
    "build/include/backend_factories_generated.h",
    "build/lemond",
    "build/lemonade",
    "schemas/backend-definition-v1.schema.json",
    "test/fake_catalog_engine.py",
)
FROZEN_TREES = ("build/resources", "src/cpp/resources")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def aggregate(artifacts):
    return hashlib.sha256(
        json.dumps(artifacts, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def frozen_artifacts(repo):
    names = list(FROZEN_FILES)
    for tree in FROZEN_TREES:
        names += [
            path.relative_to(repo).as_posix()
            for path in (repo / tree).rglob("*")
            if path.is_file()
        ]
    return {name: digest(repo / name) for name in sorted(names)}


def take_freeze(repo):
    def git(*arguments):
        return subprocess.check_output(["git", *arguments], cwd=repo, text=True).strip()

    artifacts = frozen_artifacts(repo)
    return {
        "frozen_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "commit": git("rev-parse", "HEAD"),
        "tree": git("rev-parse", "HEAD^{tree}"),
        "artifacts": artifacts,
        "artifact_count": len(artifacts),
        "aggregate_sha256": aggregate(artifacts),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--freeze",
        type=Path,
        required=True,
        help="artifact manifest to verify; written from the current build when absent",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    if args.freeze.exists():
        freeze = json.loads(args.freeze.read_text())
    else:
        freeze = take_freeze(repo)
        args.freeze.parent.mkdir(parents=True, exist_ok=True)
        args.freeze.write_text(json.dumps(freeze, indent=2) + "\n")
    server_binary = repo / "build/lemond"
    cli_binary = repo / "build/lemonade"
    harness = repo / "test/fake_catalog_engine.py"
    source = repo / "test/fixtures/catalog-fourth.json"
    checks = 0
    transcript = {"freeze": freeze, "commands": [], "stages": [], "observations": []}

    def check(condition, description):
        nonlocal checks
        assert condition, description
        checks += 1

    def unchanged():
        actual = {name: digest(repo / name) for name in freeze["artifacts"]}
        check(
            actual == freeze["artifacts"],
            "frozen executables/resources/schema/harness unchanged",
        )
        return aggregate(actual)

    unchanged()
    for binary in (server_binary, cli_binary):
        data = binary.read_bytes()
        check(
            b"catalog-fourth" not in data and b"catalog_fourth_batch" not in data,
            "fourth identities absent from frozen binary",
        )
    original = json.loads(source.read_text())
    check(
        original["recipe"] == "catalog-fourth"
        and original["metadata"]["options"][0]["name"] == "catalog_fourth_batch",
        "fourth data identity",
    )
    check(
        "hooks" not in original
        and "cli_flag" not in original["metadata"]["options"][0],
        "no hook or named product flag",
    )
    env = {
        key: value
        for key, value in os.environ.items()
        if not (key.startswith("LEMONADE_") and key.endswith("_API_KEY"))
    }
    env.update(
        LEMONADE_API_KEY="regular-test-key",
        LEMONADE_ADMIN_API_KEY="admin-test-key",
        LEMONADE_THINKSOUND_CONFIGURED="1",
        LEMONADE_DS4_CONFIGURED="1",
        LEMONADE_RYZENAI_CONFIGURED="1",
    )
    process = None
    log = None
    child_pids = set()
    with tempfile.TemporaryDirectory(prefix="lemon-fourth-") as temporary:
        root = Path(temporary)
        config = prepare(root)
        client = root / "client"
        client.mkdir()
        env["XDG_CONFIG_HOME"] = str(client)
        port = free_port()
        tiny = root / "tiny model 雪.fixture"
        trace = tiny.with_suffix(".observations.jsonl")
        model = "user.CatalogFourth"

        def start():
            nonlocal process, log
            log = tempfile.TemporaryFile(mode="w+b")
            process = subprocess.Popen(
                [
                    str(server_binary),
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
                    raise AssertionError(log.read().decode(errors="replace")[-5000:])
                try:
                    if request(port, "/v1/health")[0] == 200:
                        transcript["stages"].append(
                            {
                                "event": "server_started",
                                "pid": process.pid,
                                "port": port,
                            }
                        )
                        return
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(0.1)
            raise AssertionError("Server did not become ready")

        def children():
            if process is not None and process.poll() is None:
                tasks = Path(f"/proc/{process.pid}/task")
                if tasks.exists():
                    for task in tasks.iterdir():
                        path = task / "children"
                        if not path.exists():
                            continue
                        for pid in path.read_text().split():
                            cmdline = Path(f"/proc/{pid}/cmdline")
                            if (
                                cmdline.exists()
                                and str(harness).encode() in cmdline.read_bytes()
                            ):
                                child_pids.add(int(pid))

        def stop():
            nonlocal process, log
            if process is not None:
                children()
                if process.poll() is None:
                    try:
                        request(
                            port, "/internal/shutdown", key="admin-test-key", body={}
                        )
                        process.wait(timeout=12)
                    except (OSError, subprocess.TimeoutExpired):
                        process.terminate()
                        process.wait(timeout=8)
                check(process.returncode == 0, "server exits cleanly")
                transcript["stages"].append(
                    {
                        "event": "server_stopped",
                        "pid": process.pid,
                        "exit": process.returncode,
                    }
                )
                process = None
                log.close()
                log = None

        def cli(*arguments, input_text=None):
            command = [
                str(cli_binary),
                "--host",
                "127.0.0.1",
                "--port",
                str(port),
                "--no-discovery",
                *arguments,
            ]
            result = subprocess.run(
                command,
                input=input_text,
                text=True,
                capture_output=True,
                env=env,
                timeout=15,
            )
            transcript["commands"].append(
                {"argv": arguments, "exit": result.returncode}
            )
            check(
                result.returncode == 0,
                f"CLI command failed {arguments}: {result.stderr} {result.stdout}",
            )
            return result.stdout

        def schema():
            return json.loads(cli("backends", "schema", "catalog-fourth"))

        def observe(tag, prompt, generic=True):
            arguments = ["run", model, "--chat-cli"]
            if generic:
                arguments += ["--option", "catalog_fourth_batch=4"]
            output = cli(*arguments, input_text=prompt + "\n/exit\n")
            response = f"fake batch=4 tag={tag}"
            check(response in output, "exact fake reply reaches frozen CLI")
            observations = [json.loads(line) for line in trace.read_text().splitlines()]
            event = observations[-1]
            expected = {
                "event": "request",
                "path": "/chat",
                "body": {
                    "model": "user.CatalogFourth",
                    "messages": [{"role": "user", "content": prompt}],
                    "stream": True,
                    "stream_options": {"include_usage": True},
                },
            }
            check(event == expected, "hand-authored exact fake upstream request")
            argv = next(
                event["argv"]
                for event in reversed(observations)
                if event["event"] == "argv"
            )
            check(
                argv
                == [
                    "--port",
                    argv[1],
                    "--batch",
                    "4",
                    "--tag",
                    tag,
                    "--model-fixture",
                    str(tiny),
                ]
                and argv[1].isdigit()
                and 0 < int(argv[1]) < 65536,
                "hand-authored fake argv with allocated port",
            )
            children()
            transcript["observations"].append(
                {"argv": argv, "request": expected, "reply": response}
            )

        try:
            start()
            baseline = request(port, "/v1/backends/schema")[1]
            check(
                len(baseline["recipes"]) == 17
                and not any(
                    row["recipe"] == "catalog-fourth" for row in baseline["recipes"]
                ),
                "fourth absent before placement",
            )
            transcript["baseline_catalog_id"] = baseline["catalog_id"]
            stop()
            directory = config / "backends.d"
            directory.mkdir()
            runtime_definition = directory / "90-fourth.json"
            runtime_definition.write_bytes(source.read_bytes())
            tiny.write_bytes(b"deterministic CPU fixture\n")
            tiny_hash = digest(tiny)
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
            transcript["placed_at_utc"] = datetime.datetime.now(
                datetime.timezone.utc
            ).isoformat()
            check(
                not (client / "backends.d").exists(),
                "client has no external definitions",
            )
            start()
            first = schema()
            row = first["recipes"][0]
            check(
                row["recipe"] == "catalog-fourth"
                and row["source"] == "external"
                and row["options"][0]["name"] == "catalog_fourth_batch"
                and row["options"][0]["default_value"] == 2,
                "server-only schema through frozen CLI",
            )
            valid = json.loads(cli("backends", "validate", str(runtime_definition)))
            check(
                valid["valid"] is True
                and valid["validation"] == "server"
                and valid["definition_hash"] == row["definition_hash"],
                "server-authoritative validation through frozen CLI",
            )
            check(
                request(port, "/v1/models/" + model)[1]["downloaded"] is True,
                "tiny fixture is local and needs no pull",
            )
            cli(
                "config",
                "set",
                "catalog_fourth.cpu_bin=" + str(harness),
                "catalog_fourth.catalog_fourth_batch=3",
            )
            cli("load", model, "--save-options", "--option", "catalog_fourth_batch=4")
            saved = request(port, "/v1/models/" + model + "/options")[1]
            check(
                saved["saved"]["catalog_fourth_batch"] == 4
                and saved["defaults"]["catalog_fourth_batch"] == 3
                and saved["effective"]["catalog_fourth_batch"] == 4,
                "typed global/saved/effective values",
            )
            transcript["saved_options"] = {
                key: saved[key]["catalog_fourth_batch"]
                for key in ("saved", "defaults", "effective")
            }
            observe("initial", "fourth before")
            changed = json.loads(runtime_definition.read_text())
            changed["metadata"]["options"][0]["default_value"] = 7
            changed["launch"]["argv"][0]["args"][5] = "revised"
            runtime_definition.write_text(json.dumps(changed, indent=2) + "\n")
            check(schema() == first, "file edit leaves full frozen schema unchanged")
            cli("unload", model)
            observe("initial", "fourth while running")
            transcript["before_restart_catalog_id"] = first["catalog_id"]
            transcript["before_restart_definition_hash"] = row["definition_hash"]
            stop()
            start()
            after = schema()
            check(
                after["catalog_id"] != first["catalog_id"]
                and after["recipes"][0]["options"][0]["default_value"] == 7,
                "server-only restart adopts changed definition",
            )
            check(
                after["feature_manifest_id"] == first["feature_manifest_id"],
                "installed primitives unchanged",
            )
            observe("revised", "fourth after restart", generic=False)
            final_saved = request(port, "/v1/models/" + model + "/options")[1]
            check(
                final_saved["saved"]["catalog_fourth_batch"] == 4
                and final_saved["effective"]["catalog_fourth_batch"] == 4,
                "saved option survives restart",
            )
            check(
                str(harness) not in json.dumps(after)
                and str(root) not in json.dumps(after),
                "schema excludes configured paths",
            )
            check(digest(tiny) == tiny_hash, "tiny fixture unchanged")
            check(
                not (client / "backends.d").exists(), "client remains definition-free"
            )
            transcript["after_restart_catalog_id"] = after["catalog_id"]
            transcript["after_restart_definition_hash"] = after["recipes"][0][
                "definition_hash"
            ]
            transcript["after_aggregate_sha256"] = unchanged()
            stop()
            check(
                len(child_pids) == 3,
                "three owned fake engine processes observed: "
                + repr(sorted(child_pids)),
            )
            for pid in child_pids:
                check(not Path(f"/proc/{pid}").exists(), "owned fake engine was reaped")
            transcript["reaped_fake_engine_pids"] = sorted(child_pids)
            transcript["all_engine_observations"] = [
                json.loads(line) for line in trace.read_text().splitlines()
            ]
            transcript["outcome"] = (
                "4j server/CLI portion passed; full 4j pending maintainer-owned 4i and relevant platform evidence."
            )
            transcript["checks"] = {"passed": checks, "failed": 0}
        finally:
            if process is not None:
                stop()
    args.output.write_text(json.dumps(transcript, indent=2, ensure_ascii=False) + "\n")
    print(f"{checks}/{checks} frozen fourth-definition checks passed")
    print(transcript["outcome"])


if __name__ == "__main__":
    main()
