"""Exercise explicit admission negotiation and private CPU-only candidates."""

import json
import shutil
from pathlib import Path
import sys
import tempfile

from backend_catalog_http import Server, definition, prepare, request

REPO = Path(__file__).resolve().parents[1]


def root_definition(identity="v2-root"):
    value = json.loads(
        (REPO / "test/fixtures/backend-v2/export.synthetic.json").read_text()
    )
    value["id"] = identity
    value["recipe"] = identity
    merge = value["merge"]
    del merge["hooks"]
    del merge["artifacts"]
    merge["launch"]["prepare_order"] = ["backend", "artifacts", "program", "ports"]
    del merge["operations"][0]["request"]["hooks"]
    del merge["operations"][0]["response"]
    return value


def main(binary):
    checks = 0

    def check(condition, message):
        nonlocal checks
        assert condition, message
        checks += 1

    with tempfile.TemporaryDirectory(prefix="lemon-negotiation-") as temporary:
        root = Path(temporary)
        config = prepare(root)
        (config / "backends.d").mkdir()
        (config / "backends.d/legacy.json").write_text(json.dumps(definition()))
        with Server(binary, root) as server:
            for prefix in ("/api/v0", "/api/v1", "/v0", "/v1"):
                endpoint = prefix + "/backends/schema"
                status, legacy, headers = request(server.port, endpoint)
                check(status == 200, "legacy response")
                status, explicit, explicit_headers = request(
                    server.port, endpoint + "?representation=1"
                )
                check(
                    status == 200
                    and explicit == legacy
                    and headers["ETag"] == explicit_headers["ETag"],
                    "explicit 1 equals omitted body and ETag",
                )
                status, rich, rich_headers = request(
                    server.port, endpoint + "?representation=2"
                )
                check(status == 200 and rich["schema_version"] == 2, "explicit v2")
                check(
                    rich["catalog_id"] == legacy["catalog_id"]
                    and rich["legacy_feature_manifest_id"]
                    == legacy["feature_manifest_id"]
                    and rich["accepted_definition_versions"] == [1, 2]
                    and rich["executable_definition_versions"] == [1]
                    and rich["acceptance_stage"] == "declaration_authorization",
                    "honest versions and separate identities",
                )
                check(
                    rich["feature_manifest_id"] != legacy["feature_manifest_id"]
                    and rich_headers["ETag"] != headers["ETag"]
                    and rich_headers["Cache-Control"] == "private, no-cache",
                    "representation-specific manifest and cache identity",
                )
                for query in (
                    "representation=",
                    "representation=0",
                    "representation=3",
                    "representation=2&representation=2",
                    "representation=1&representation=2",
                    "representation=1&%72epresentation=1",
                ):
                    status, body, _ = request(server.port, endpoint + "?" + query)
                    check(
                        status == 400
                        and body["error"]["code"] == "invalid_representation",
                        "reject invalid representation " + query,
                    )
                for version, other in ((1, rich_headers), (2, headers)):
                    target = endpoint + f"?representation={version}"
                    status, _, _ = request(
                        server.port, target, headers={"If-None-Match": other["ETag"]}
                    )
                    check(status == 200, "cross representation conditional is not 304")
                status, _, _ = request(
                    server.port,
                    endpoint + "?representation=2",
                    headers={"If-None-Match": rich_headers["ETag"]},
                )
                check(status == 304, "matching v2 ETag")
                status, _, _ = request(
                    server.port, endpoint + "?representation=2", key=None
                )
                check(status == 401, "v2 authentication enforced")
                status, body, filtered_headers = request(
                    server.port, endpoint + "?representation=2&recipe=http-alpha"
                )
                check(
                    status == 200
                    and len(body["recipes"]) == 1
                    and body["recipes"][0]["input_schema_version"] == 1
                    and body["catalog_v2_id"] == rich["catalog_v2_id"]
                    and filtered_headers["ETag"] != rich_headers["ETag"],
                    "filtered v2 ETag and honest v1 input version",
                )
                status, body, _ = request(
                    server.port, endpoint + "?representation=2&recipe=absent"
                )
                check(
                    status == 404 and body["error"]["code"] == "unknown_recipe",
                    "unknown recipe",
                )
                candidate = root_definition()
                status, body, _ = request(
                    server.port,
                    prefix + "/backends/validate",
                    body=json.dumps(candidate).encode(),
                )
                check(
                    status == 400
                    and body["valid"] is False
                    and body["stages"]
                    == dict(
                        resolved=True,
                        authorized=True,
                        catalog_admitted=True,
                        executable=False,
                    )
                    and body["diagnostics"][0]["code"]
                    == "unsupported_execution_version",
                    "admission success is not executable validation",
                )
                candidate = dict(
                    schema_version=2,
                    id="missing-child",
                    recipe="missing-child",
                    extends={"external": "external-not-installed"},
                )
                status, body, _ = request(
                    server.port,
                    prefix + "/backends/validate",
                    body=json.dumps(candidate).encode(),
                )
                check(
                    status == 400
                    and body["diagnostics"][0]["code"] == "missing_parent"
                    and body["diagnostics"][0]["pointer"] == "/extends/external",
                    "missing external dependency surfaced",
                )
                candidate["extends"] = {"external": "http-alpha"}
                status, body, _ = request(
                    server.port,
                    prefix + "/backends/validate",
                    body=json.dumps(candidate).encode(),
                )
                check(
                    status == 400
                    and body["diagnostics"][0]["code"] == "unsupported_parent_version",
                    "existing v1 parent distinguished",
                )
                status, unchanged, unchanged_headers = request(server.port, endpoint)
                check(
                    status == 200
                    and unchanged == legacy
                    and unchanged_headers["ETag"] == headers["ETag"],
                    "candidate never publishes",
                )
            raw = json.dumps(rich)
            check(
                all(
                    key not in raw
                    for key in (
                        '"grants"',
                        '"authority"',
                        '"params"',
                        '"launch"',
                        '"extends"',
                        '"policy"',
                        '"argv"',
                    )
                ),
                "public projection excludes executable and authority internals",
            )
    with tempfile.TemporaryDirectory(prefix="lemon-frozen-resources-") as temporary:
        root = Path(temporary)
        isolated = root / "bin"
        isolated.mkdir()
        copied = isolated / binary.name
        shutil.copy2(binary, copied)
        shutil.copytree(binary.parent / "resources", isolated / "resources")
        config = prepare(root)
        pins_path = isolated / "resources/backend_versions.json"
        pins = json.loads(pins_path.read_text())
        pins["initial-frozen-pin"] = {"cpu": "v1"}
        pins_path.write_text(json.dumps(pins))
        managed = definition("managed-legacy", "managed_value")
        managed["install"] = {
            "strategy": "github_release",
            "pin_recipe": "initial-frozen-pin",
            "cases": [
                {"backend": ["cpu"], "repo": "fixture/repo", "filename": "fake.zip"}
            ],
        }
        (config / "backends.d").mkdir()
        (config / "backends.d/managed.json").write_text(json.dumps(managed))
        with Server(copied, root) as server:
            defaults_path = isolated / "resources/defaults.json"
            defaults = json.loads(defaults_path.read_text())
            defaults["frozen-only-section"] = {}
            defaults_path.write_text(json.dumps(defaults))
            candidate = definition("frozen-candidate", "frozen_value")
            candidate["metadata"]["config_section"] = "frozen-only-section"
            status, body, _ = request(
                server.port,
                "/v1/backends/validate",
                body=json.dumps(candidate).encode(),
            )
            check(
                status == 200 and body["valid"],
                "candidate retains frozen defaults and reservations",
            )
            pins_path = isolated / "resources/backend_versions.json"
            pins = json.loads(pins_path.read_text())
            del pins["initial-frozen-pin"]
            pins["frozen-only-pin"] = {"cpu": "v99"}
            pins_path.write_text(json.dumps(pins))
            candidate["install"] = {
                "strategy": "github_release",
                "pin_recipe": "frozen-only-pin",
                "cases": [
                    {"backend": ["cpu"], "repo": "fixture/repo", "filename": "fake.zip"}
                ],
            }
            status, body, _ = request(
                server.port,
                "/v1/backends/validate",
                body=json.dumps(candidate).encode(),
            )
            check(
                status == 400
                and any(
                    row["code"] == "required"
                    and row["pointer"] == "/install/default_versions"
                    for row in body["diagnostics"]
                ),
                "candidate retains frozen version pins",
            )
            status, body, _ = request(
                server.port,
                "/v1/backends/validate",
                body=json.dumps(root_definition()).encode(),
            )
            check(
                status == 400 and body["stages"]["catalog_admitted"],
                "v2 private graph retains frozen pins",
            )
    print(f"{checks} checks passed")


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
