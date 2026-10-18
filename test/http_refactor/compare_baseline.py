"""Compare safe HTTP behavior of separate GUI3 and refactor lemond builds."""

from __future__ import annotations

import argparse
import json
import sys

from harness import Client, IsolatedLemond, find_lemond

SAFE_GETS = (
    "/api/v1/health",
    "/v0/health",
    "/v1/health",
    "/api/v1/models?show_all=true",
    "/api/v1/downloads",
    "/api/v1/jobs",
    "/internal/aliases",
    "/api/version",
    "/api/tags",
    "/internal/config/defaults",
)


def normalized(path: str, response):
    if response.status != 200:
        return {"status": response.status, "text": response.text[:400]}
    body = response.json
    if path.endswith("/health"):
        selected = (
            "status",
            "model_loaded",
            "max_models",
            "pinned_models",
            "pinned_helper_models",
        )
        return {"status": 200, "value": {key: body.get(key) for key in selected}}
    if path.startswith("/api/v1/models?"):
        data = body.get("data", [])
        return {
            "status": 200,
            "models": sorted((entry["id"], entry.get("recipe")) for entry in data),
        }
    if path == "/internal/config/defaults":
        selected = ("max_loaded_models", "offline", "auto_evict", "ctx_size")
        return {"status": 200, "defaults": {key: body.get(key) for key in selected}}
    return {"status": 200, "value": body}


def snapshots(client: Client):
    values = {}
    for path in SAFE_GETS:
        values[path] = normalized(path, client.get(path, timeout=30))
    for path, payload in (
        ("/api/v1/chat/completions", {"messages": [{"role": "user", "content": "hi"}]}),
        ("/api/v1/embeddings", {"input": "hello"}),
        ("/api/v1/responses", {"input": "hello"}),
    ):
        reply = client.post(path, data=payload)
        values["POST " + path + " missing-model"] = {"status": reply.status}
    reply = client.get("/api/v1/http-refactor-invalid-path")
    values["unknown-path"] = {"status": reply.status}
    reply = client.request("HEAD", "/api/v1/health")
    values["HEAD health"] = {"status": reply.status, "body_length": len(reply.body)}
    return values


def diff(baseline: dict, candidate: dict) -> dict:
    return {
        key: {"baseline": baseline.get(key), "candidate": candidate.get(key)}
        for key in baseline.keys() | candidate.keys()
        if baseline.get(key) != candidate.get(key)
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--baseline-lemond", required=True, help="Binary built from GUI3_squashed"
    )
    parser.add_argument(
        "--candidate-lemond",
        required=True,
        help="Binary from the PR #3771 + GUI3 merge",
    )
    args = parser.parse_args()
    first = find_lemond(args.baseline_lemond)
    second = find_lemond(args.candidate_lemond)
    with IsolatedLemond(first) as baseline:
        before = snapshots(baseline.api)
    with IsolatedLemond(second) as candidate:
        after = snapshots(candidate.api)
    changes = diff(before, after)
    if changes:
        print(json.dumps(changes, indent=2, sort_keys=True))
        print(f"\n{len(changes)} differences across {len(before)} contracts")
        return 1
    print(f"All {len(before)} common API contracts match the GUI3 baseline.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
