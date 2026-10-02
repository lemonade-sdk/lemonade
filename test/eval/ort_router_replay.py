"""
Replay a recorded ONNX-classifier router run through lemond's routing engine
and diff the decisions, case by case.

pii_routing_eval.py measures routing through /v1/chat/completions, which also
runs the routed model -- a local LLM and, for most policies, a cloud one. To
test a new ort-server build only the DECISION matters, so this script asks
POST /v1/routing/validate instead: the same RoutingPolicyEngine, the same
classifier service, the same lemond -> ort-server /classify hop, but no routed
model and no cloud key. Each decision is reduced to "routed to the privacy
target or not" and compared with what the recorded log says that case did, so
a log produced under a different default model still compares cleanly.

    python test/eval/ort_router_replay.py --base-url http://127.0.0.1:13306 \\
        --register user.mmbert32k-pii-onnx \\
        --policy test/conformance/routing/1/l2_pii_onnx_classifier/policy.json \\
        --historical-log <corpus>/runs/policy_smoke_20260819-100935.log

The case sample uses the same selection flags and defaults as
ort_server_parity.py, so the two scripts cover the same cases. The server must
already be running with its onnxruntime cpu_bin pointed at the build under
test, and --register expects the model directory staged in that server's HF
cache as models--<name without "user.">. Writes
<out-dir>/router_replay_<policy>_<ts>.jsonl + .json; exits 1 on any
disagreement with the recorded run.
"""

import argparse
import json
import re
import sys
import time
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).parent))
from ort_parity_common import (
    DEFAULT_CORPUS,
    load_cases,
    probe_texts,
    select_sample,
)  # noqa: E402

PASS_RE = re.compile(r"^\s*\[PASS\]\[(?:TP|TN|FP|FN)\] (\S+?): route=(\S+) rule=")
FAIL_RE = re.compile(
    r"^\s*\[FAIL\]\[(?:TP|TN|FP|FN)\] (\S+?) \(pii=[^)]*\): expected=\S+ actual=(\S+)"
)
TARGET_RE = re.compile(r"^Privacy route target: '([^']+)'")


def read_historical(log_path: Path) -> tuple[str, dict[str, str]]:
    """(privacy target, case -> route) from a pii_routing_eval.py log.

    A resumed run's log carries every earlier answer as a [replayed] line, so
    the final log of a resumed series is complete on its own.
    """
    target, routes = None, {}
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if target is None and (m := TARGET_RE.match(line)):
                target = m.group(1)
            elif m := PASS_RE.match(line) or FAIL_RE.match(line):
                routes.setdefault(m.group(1), m.group(2))
    if target is None:
        raise SystemExit(f"{log_path}: no 'Privacy route target' header")
    return target, routes


def register(base_url: str, name: str) -> None:
    # local_import is what `lemonade import` uses: the files must already sit in
    # lemond's HF cache as models--<name without "user.">. A bare local path in
    # `checkpoint` is parsed as a Hugging Face repo id.
    body = {"model_name": name, "recipe": "onnxruntime", "local_import": True}
    r = requests.post(f"{base_url}/v1/pull", json=body, timeout=600)
    if not r.ok:
        raise SystemExit(
            f"registering {name} failed: HTTP {r.status_code} {r.text[:300]}"
        )
    print(f"Registered {name} (local import)")


def classifier_trace(decision: dict) -> dict:
    """The deciding leaf when the rule matched, else the highest-scoring one."""
    leaves = [
        t
        for t in decision.get("trace", [])
        if str(t.get("condition", "")).startswith("classifier:")
    ]
    fired = [t for t in leaves if t.get("result")]
    scored = [t for t in leaves if t.get("score") is not None]
    if fired:
        return {"fired": fired[0].get("label"), "score": fired[0].get("score")}
    if scored:
        top = max(scored, key=lambda t: t["score"])
        return {"fired": None, "top_label": top.get("label"), "score": top["score"]}
    return {"fired": None, "score": None, "classifier_error": bool(leaves)}


def parse_args():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--base-url", default="http://127.0.0.1:13306")
    p.add_argument("--policy", type=Path, required=True)
    p.add_argument("--historical-log", type=Path, required=True)
    p.add_argument(
        "--register",
        action="append",
        default=[],
        metavar="MODEL",
        help="Register a staged onnxruntime model (local import); repeatable",
    )
    p.add_argument("--corpus-dir", type=Path, default=DEFAULT_CORPUS)
    p.add_argument("--sample", type=int, default=500, help="0 = every case in the log")
    p.add_argument("--longest", type=int, default=100)
    p.add_argument("--non-ascii", type=int, default=150)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--no-probes", action="store_true")
    p.add_argument("--timeout", type=float, default=600)
    p.add_argument("--progress-every", type=int, default=50)
    p.add_argument("--verbose", action="store_true")
    p.add_argument(
        "--out-dir", type=Path, default=None, help="Default: <corpus-dir>/runs"
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    policy = json.loads(args.policy.read_text(encoding="utf-8"))
    target, recorded = read_historical(args.historical_log)
    if target not in {r["route_to"] for r in policy["routing"]["rules"]}:
        raise SystemExit(
            f"log's privacy target {target!r} is not a route_to in {args.policy}"
        )
    for spec in args.register:
        register(args.base_url, spec)

    cases = [
        c
        for c in select_sample(
            load_cases(args.corpus_dir),
            args.sample,
            args.longest,
            args.non_ascii,
            args.seed,
        )
        if c["name"] in recorded
    ]
    inputs = cases + (
        []
        if args.no_probes
        else [
            {"name": f"probe:{n}", "text": t, "why": "probe"} for n, t in probe_texts()
        ]
    )
    out_dir = args.out_dir or args.corpus_dir / "runs"
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = f"router_replay_{args.policy.stem}_{time.strftime('%Y%m%d-%H%M%S')}"
    print(
        f"Policy {policy['model_name']} | privacy target {target} | "
        f"{len(recorded)} recorded cases, replaying {len(cases)} + {len(inputs) - len(cases)} probes"
    )

    summary = {
        "policy": str(args.policy),
        "historical_log": str(args.historical_log),
        "base_url": args.base_url,
        "privacy_target": target,
        "replayed": 0,
        "agree": 0,
        "disagree": [],
        "errors": [],
        "probes": {},
    }
    t0 = time.time()
    with open(out_dir / f"{stem}.jsonl", "w", encoding="utf-8") as f:
        for i, item in enumerate(inputs, 1):
            t_case = time.perf_counter()
            try:
                r = requests.post(
                    f"{args.base_url}/v1/routing/validate",
                    json={"policy": policy, "prompt": item["text"]},
                    timeout=args.timeout,
                )
                r.raise_for_status()
                decision = r.json()["decision"]
            except (requests.RequestException, KeyError, ValueError) as e:
                summary["errors"].append(item["name"])
                print(f"  [ERROR] {item['name']}: {e}", flush=True)
                continue
            ms = (time.perf_counter() - t_case) * 1000
            now_local = decision.get("route_to") == target
            row = {
                "case": item["name"],
                "why": item["why"],
                "route_to": decision.get("route_to"),
                "matched_rule": decision.get("matched_rule"),
                "ms": round(ms, 1),
                **classifier_trace(decision),
            }
            if item["why"] == "probe":
                summary["probes"][item["name"]] = {
                    "route_local": now_local,
                    **classifier_trace(decision),
                }
                print(
                    f"  [PROBE] {item['name']}: route_local={now_local} {classifier_trace(decision)}",
                    flush=True,
                )
            else:
                was_local = recorded[item["name"]] == target
                row["recorded_route"] = recorded[item["name"]]
                row["agree"] = now_local == was_local
                summary["replayed"] += 1
                if row["agree"]:
                    summary["agree"] += 1
                else:
                    summary["disagree"].append(item["name"])
                if not row["agree"] or args.verbose:
                    print(
                        f"  [{'OK  ' if row['agree'] else 'DIFF'}] {item['name']} ({item['why']}): "
                        f"now={'local' if now_local else 'default'} recorded="
                        f"{'local' if was_local else 'default'} {classifier_trace(decision)} "
                        f"[{ms:.0f}ms]",
                        flush=True,
                    )
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
            if i % args.progress_every == 0:
                print(
                    f"  ... {i}/{len(inputs)} ({i / (time.time() - t0):.2f}/s)",
                    flush=True,
                )

    summary["seconds"] = round(time.time() - t0, 1)
    (out_dir / f"{stem}.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    ok = not summary["disagree"] and not summary["errors"]
    print(
        f"\nDecisions matching the recorded run: {summary['agree']}/{summary['replayed']}"
        f"  (errors {len(summary['errors'])})"
    )
    print(f"RESULT: {'PASS' if ok else 'FAIL'}")
    print(f"Rows -> {out_dir / (stem + '.jsonl')}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
