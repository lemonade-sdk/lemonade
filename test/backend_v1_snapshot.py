"""Capture and compare the unchanged v1 API and CLI using an isolated CPU server."""

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
from urllib.parse import quote

from backend_catalog_http import Server, definition, prepare, request
from backend_definition_inventory import production_inventory
from backend_schema_cli import Fixture

REPO = Path(__file__).resolve().parents[1]
SOURCE_SHA = "410b5b4e9f9c7bdfcd1031ea6911795a8cce09ae"


def capture(lemond, cli, source_root=REPO):
    result = {"source_sha": SOURCE_SHA, "platform": platform.system()}
    result["file_sha256"] = {
        str(path.relative_to(source_root)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for path in [
            source_root / "schemas/backend-definition-v1.schema.json",
            source_root / "src/cpp/resources/defaults.json",
            *sorted((source_root / "src/cpp/resources/backends").glob("*.json")),
        ]
    }
    with tempfile.TemporaryDirectory(prefix="lemon-v1-snapshot-") as temporary:
        root = Path(temporary)
        prepare(root)
        env = os.environ.copy()
        env.pop("LEMONADE_ADMIN_API_KEY", None)
        env["LEMONADE_API_KEY"] = "regular-test-key"
        env["XDG_CONFIG_HOME"] = str(root / "client")
        env["NO_COLOR"] = "1"
        with Server(lemond, root, "--builtin-backends-only") as server:

            def api(path, **kwargs):
                status, body, headers = request(server.port, path, **kwargs)
                return {"status": status, "body": body, "etag": headers.get("ETag")}

            result["schema"] = api("/v1/backends/schema")
            schema = result["schema"]["body"]
            result["definitions"] = {
                row["definition_id"]: row["definition_hash"]
                for row in schema["recipes"]
                if row["definition_id"] is not None
            }
            assert set(result["definitions"]) == production_inventory(
                source_root
            ), "ExactMixedDefinitionSet"
            result["filtered"] = {
                row["recipe"]: api("/v1/backends/schema?recipe=" + quote(row["recipe"]))
                for row in schema["recipes"]
            }
            result["conditional"] = api(
                "/v1/backends/schema",
                headers={"If-None-Match": result["schema"]["etag"]},
            )
            result["unauthenticated"] = api("/v1/backends/schema", key=None)
            result["defaults"] = api("/internal/config/defaults", key="admin-test-key")
            result["recipe_metadata"] = api("/v1/system-info", key="admin-test-key")[
                "body"
            ]["recipes"]

            def run(*args, offline=False):
                command = [str(cli), "--no-discovery"]
                if not offline:
                    command += ["--host", "127.0.0.1", "--port", str(server.port)]
                process = subprocess.run(
                    command + list(args),
                    capture_output=True,
                    text=True,
                    env=env,
                    timeout=30,
                )
                return {
                    "status": process.returncode,
                    "stdout": process.stdout,
                    "stderr": process.stderr,
                }

            result["cli_backends"] = run("backends", "--all")
            result["cli_schema"] = run("backends", "schema")
            result["cli_validation"] = {}
            valid = definition("snapshot-fixture", "snapshot_batch")
            invalid = dict(valid, schema_version=2)
            cases = {
                "valid": json.dumps(valid),
                "v2": json.dumps(invalid),
                "duplicate": '{"schema_version":1,"id":"a","id":"b"}',
                "malformed": "{",
            }
            for name, text in cases.items():
                candidate = root / "candidate.json"
                candidate.write_text(text)
                result["cli_validation"][name] = {
                    "online": run("backends", "validate", str(candidate)),
                    "offline": run(
                        "backends",
                        "validate",
                        str(candidate),
                        "--offline",
                        offline=True,
                    ),
                }
    with tempfile.TemporaryDirectory(prefix="lemon-v1-original-bytes-") as temporary:
        fixture = Fixture()
        try:
            path = Path(temporary) / "candidate.json"
            original = b'{ "schema_version": 2, "id":"first", "id":"second" }\n'
            path.write_bytes(original)
            env = os.environ.copy()
            env.pop("LEMONADE_ADMIN_API_KEY", None)
            env["LEMONADE_API_KEY"] = "fixture-key"
            process = subprocess.run(
                [
                    str(cli),
                    "--no-discovery",
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(fixture.port),
                    "backends",
                    "validate",
                    str(path),
                ],
                capture_output=True,
                text=True,
                env=env,
                timeout=30,
            )
            posted = [body for method, _, body, _ in fixture.calls if method == "POST"]
            assert process.returncode == 0 and posted == [original]
            result["original_bytes"] = {
                "bytes": original.decode(),
                "status": process.returncode,
                "stdout": process.stdout,
                "stderr": process.stderr,
            }
        finally:
            fixture.close()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["capture", "compare"])
    parser.add_argument("--lemond", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    actual = (
        json.dumps(
            capture(args.lemond.resolve(), args.cli.resolve()), indent=2, sort_keys=True
        )
        + "\n"
    )
    if args.mode == "capture":
        assert args.out is not None
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "snapshot.json").write_text(actual)
        print(f"Captured v1 baseline in {args.out / 'snapshot.json'}")
    else:
        assert args.baseline is not None
        expected = (args.baseline / "snapshot.json").read_text()
        if expected != actual:
            print(
                "".join(
                    difflib.unified_diff(
                        expected.splitlines(True),
                        actual.splitlines(True),
                        fromfile="baseline",
                        tofile="current",
                    )
                )
            )
            raise SystemExit(1)
        print("v1 API, eight digests, CLI, defaults and recipe metadata: identical")


if __name__ == "__main__":
    main()
