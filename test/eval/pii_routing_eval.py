"""
PII routing evaluator.

Registers the PII router policy against a running Lemonade server, POSTs each
case from cases.jsonl, compares the actual routing decision to the expected one,
and prints a confusion matrix with leak rate, over-route rate, and Fbeta score.
Every run is also backed up to <corpus-dir>/runs/ as a timestamped text log
(line-buffered and flushed per write, so it's safe to tail mid-run) plus a
JSON summary (counts, metrics, wrong/risk/fallback cases). A heartbeat line
is logged every --progress-every cases regardless of --verbose, so a large
corpus doing live inference doesn't look stalled between the startup banner
and the final summary. FAIL/ERROR/LLM_FALLBACK/UNEXPECTED-risk cases are
always logged live as they happen (not gated behind --verbose) since those
are what you'd actually want to see while debugging; --verbose additionally
prints a line for every PASS.

Usage:
    python test/eval/pii_routing_eval.py [--base-url URL] [--corpus-dir DIR] [--policy FILE] [--beta FLOAT] [--verbose] [--limit N] [--progress-every N] [--log-dir DIR] [--no-log-file]

Defaults:
    --base-url       http://localhost:13305
    --corpus-dir     test/conformance/routing/1/l2_pii_regex
    --policy         policy.json  (relative to --corpus-dir, or an absolute path)
    --beta           2.0  (recall-weighted: missing PII is more costly than over-routing)
    --limit          0  (evaluate all cases; pass e.g. 20 for a quick smoke test)
    --progress-every 50  (0 disables the heartbeat)
    --log-dir        <corpus-dir>/runs/  (pass --no-log-file to skip the backup)

Examples:
    # Regex policy against hand-crafted corpus
    python test/eval/pii_routing_eval.py --verbose

    # LLM policy against the same hand-crafted corpus
    python test/eval/pii_routing_eval.py --policy policy_llm.json --verbose

    # LLM policy against Nemotron corpus
    python test/eval/pii_routing_eval.py \\
        --corpus-dir test/conformance/routing/1/l2_pii_nemotron \\
        --policy ../l2_pii_regex/policy_llm.json --verbose
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

import requests

# Case fixtures (note fields, LLM rationale text) can carry arbitrary Unicode
# (em-dashes, curly quotes, non-English text) that Windows' default cp1252
# console encoding can't represent, crashing print() mid-run. Force UTF-8 with
# graceful fallback instead of relying on every string printed to stay ASCII.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--base-url", default="http://localhost:13305", help="Lemonade server base URL"
    )
    p.add_argument(
        "--corpus-dir",
        default=str(
            Path(__file__).parent.parent
            / "conformance"
            / "routing"
            / "1"
            / "l2_pii_regex"
        ),
        help="Directory containing policy.json and cases.jsonl",
    )
    p.add_argument(
        "--policy",
        default="policy.json",
        help="Policy file name (relative to --corpus-dir) or absolute path. "
        "Defaults to policy.json. Use policy_llm.json for the LLM router.",
    )
    p.add_argument(
        "--beta",
        type=float,
        default=2.0,
        help="Beta for Fbeta score (beta>1 weights recall over precision)",
    )
    p.add_argument("--verbose", action="store_true", help="Print per-case results")
    p.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Only evaluate the first N cases (0 = all, the default). Cases in "
        "the corpus are pre-shuffled by the builder, so a small N still gives a "
        "random-but-reproducible mix for a smoke test before a full run.",
    )
    p.add_argument(
        "--timeout",
        type=int,
        default=60,
        help="HTTP timeout per request in seconds. "
        "Increase for LLM policy (router LLM generates before routing).",
    )
    p.add_argument(
        "--progress-every",
        type=int,
        default=50,
        help="Log a heartbeat every N cases regardless of --verbose (0 disables). "
        "On a large corpus, per-case --verbose output is too noisy but a quiet "
        "log is indistinguishable from a stalled run without this.",
    )
    p.add_argument(
        "--log-dir",
        default=None,
        help="Directory to back up the full run output (text log + JSON summary). "
        "Defaults to <corpus-dir>/runs/. Files are timestamped so runs don't collide.",
    )
    p.add_argument(
        "--no-log-file",
        action="store_true",
        help="Skip writing the run log / JSON summary backup.",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# Server helpers
# ---------------------------------------------------------------------------


def wait_for_server(base_url: str, timeout: int = 30) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = requests.get(f"{base_url}/health", timeout=5)
            if r.status_code == 200:
                return
        except requests.exceptions.ConnectionError:
            pass
        time.sleep(1)
    print(
        f"ERROR: server not reachable at {base_url} after {timeout}s", file=sys.stderr
    )
    sys.exit(1)


def register_policy(base_url: str, policy: dict, timeout: int) -> None:
    r = requests.post(f"{base_url}/v1/pull", json=policy, timeout=timeout)
    if r.status_code not in (200, 201):
        print(
            f"ERROR: failed to register policy: {r.status_code} {r.text}",
            file=sys.stderr,
        )
        sys.exit(1)


def chat_completion(
    base_url: str, request_body: dict, timeout: int, model: str
) -> dict:
    endpoint = f"{base_url}/v1/chat/completions"
    payload = dict(request_body)
    # Corpus fixtures bake in a fixed request["model"] (typically whatever
    # policy built the corpus), which is independent of --policy. Always
    # target the policy actually registered this run, or a corpus built
    # against one policy would silently test whatever OTHER policy happens
    # to already be registered under that name instead.
    payload["model"] = model
    payload["route_trace"] = True
    payload["max_tokens"] = 1
    r = requests.post(endpoint, json=payload, timeout=timeout)
    r.raise_for_status()
    return r.json()


# ---------------------------------------------------------------------------
# Case classification
# ---------------------------------------------------------------------------

RISK_NOTE_MARKERS = ("RISK",)

# Canonical-ID prefixes the router resolves candidate/route_to names to at
# policy-registration time (see src/cpp/include/lemon/canonical_id.h). Fixture
# files author bare names, but a live server reports the canonical form (e.g.
# "user.Qwen3.5-9B-NoThinking") once the target model is resolved, so model
# names must be compared prefix-insensitively.
CANONICAL_PREFIXES = ("user.", "extra.", "builtin.")


def strip_canonical_prefix(name: str) -> str:
    for prefix in CANONICAL_PREFIXES:
        if name.startswith(prefix):
            return name[len(prefix) :]
    return name


def same_model(a: str, b: str) -> bool:
    return strip_canonical_prefix(a) == strip_canonical_prefix(b)


# The `routing.router` (LLM router) sugar desugars to a classifier with id
# "__router" (see desugar_routing_router in routing_policy_parser.cpp), but
# ClassifierBandCondition traces it under "classifier:" + id, never the bare
# id (see routing_policy.cpp). Any code that looks for the bare "__router"
# string in a trace entry's condition can never match.
ROUTER_TRACE_CONDITION = "classifier:__router"


def is_risk_case(case: dict) -> bool:
    note = case.get("note", "")
    return any(m in note for m in RISK_NOTE_MARKERS)


def is_llm_policy(policy: dict) -> bool:
    """True when the policy uses an LLM router (routing.router.type == 'llm')."""
    return policy.get("routing", {}).get("router", {}).get("type") == "llm"


def privacy_route(policy: dict) -> str:
    """Return the candidate that PII should route to (the local/private model).

    For rules-based policy: the route_to of the 'pii-detected' rule.
    For LLM policy: default_model (the LLM defaults to local when uncertain,
    so the private model is the default, not the exception).
    """
    if is_llm_policy(policy):
        return policy.get("routing", {}).get("default_model", "")
    for rule in policy.get("routing", {}).get("rules", []):
        if rule.get("id") == "pii-detected":
            return rule["route_to"]
    # Fallback: any non-default candidate
    routing = policy.get("routing", {})
    default = routing.get("default_model", "")
    candidates = routing.get("candidates", [])
    non_default = [c for c in candidates if c != default]
    return non_default[0] if non_default else ""


def classify_case(case: dict, local_model: str) -> str:
    """
    Returns one of: TP, TN, FP, FN, or RISK_FN / RISK_FP
    based on pii_category and expected route_to.
    """
    pii = case.get("pii_category", "none")
    expected_route = case["decision"]["route_to"]
    has_pii = pii != "none"
    routes_local = expected_route == local_model

    if is_risk_case(case):
        if has_pii and not routes_local:
            return "RISK_FN"
        return "RISK_FP"

    if has_pii and routes_local:
        return "TP"
    if not has_pii and expected_route != local_model:
        return "TN"
    if not has_pii and routes_local:
        return "FP"
    # has_pii and not routes_local
    return "FN"


# ---------------------------------------------------------------------------
# Evaluation
# ---------------------------------------------------------------------------


def evaluate(args: argparse.Namespace) -> None:
    corpus_dir = Path(args.corpus_dir)
    policy_arg = Path(args.policy)
    policy_path = policy_arg if policy_arg.is_absolute() else corpus_dir / policy_arg
    cases_path = corpus_dir / "cases.jsonl"

    if not policy_path.exists():
        print(f"ERROR: policy file not found at {policy_path}", file=sys.stderr)
        sys.exit(1)
    if not cases_path.exists():
        print(f"ERROR: cases.jsonl not found at {cases_path}", file=sys.stderr)
        sys.exit(1)

    policy = json.loads(policy_path.read_text(encoding="utf-8"))
    cases = [
        json.loads(line)
        for line in cases_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    if args.limit and args.limit > 0:
        cases = cases[: args.limit]

    log_file_handle = None
    log_path: Path | None = None
    json_path: Path | None = None
    if not args.no_log_file:
        log_dir = Path(args.log_dir) if args.log_dir else corpus_dir / "runs"
        log_dir.mkdir(parents=True, exist_ok=True)
        run_id = f"{policy_path.stem}_{time.strftime('%Y%m%d-%H%M%S')}"
        log_path = log_dir / f"{run_id}.log"
        json_path = log_dir / f"{run_id}.json"
        # buffering=1 (line-buffered) plus an explicit flush below: on a long
        # run (thousands of cases, live inference) the file must be tailable
        # as it goes, not sit in a buffer until close() at the very end.
        log_file_handle = log_path.open("w", encoding="utf-8", buffering=1)

    def log(msg: str = "") -> None:
        print(msg, flush=True)
        if log_file_handle:
            log_file_handle.write(msg + "\n")
            log_file_handle.flush()

    try:
        log(f"Corpus : {corpus_dir}")
        log(f"Policy : {policy_path.name}")
        log(f"Cases  : {len(cases)}")
        log(f"Server : {args.base_url}")
        log(f"Beta   : {args.beta}")
        log()

        wait_for_server(args.base_url, timeout=15)
        register_policy(args.base_url, policy, args.timeout)
        local_model = privacy_route(policy)
        log(f"Policy '{policy['model_name']}' registered.")
        log(f"Privacy route target: '{local_model}'\n")

        llm_policy = is_llm_policy(policy)

        # Counters
        counts: dict[str, int] = {
            "TP": 0,
            "TN": 0,
            "FP": 0,
            "FN": 0,
            "RISK_FN": 0,
            "RISK_FP": 0,
            "ERROR": 0,
            "LLM_FALLBACK": 0,  # LLM policy only: router LLM produced bad output, server fell back
        }
        wrong_cases: list[dict] = []
        risk_cases: list[dict] = []
        llm_fallback_cases: list[dict] = []

        start_time = time.time()
        for idx, case in enumerate(cases, start=1):
            if args.progress_every and idx % args.progress_every == 0:
                elapsed = time.time() - start_time
                rate = idx / elapsed if elapsed > 0 else 0.0
                log(
                    f"  ... {idx}/{len(cases)} cases processed "
                    f"({elapsed:.0f}s elapsed, {rate:.2f} cases/s)"
                )

            name = case.get("name", "?")
            expected_route = case["decision"]["route_to"]
            expected_rule = case["decision"].get("matched_rule", "")
            kind = classify_case(case, local_model)

            try:
                resp = chat_completion(
                    args.base_url,
                    case["request"],
                    args.timeout,
                    model=policy["model_name"],
                )
            except Exception as exc:
                counts["ERROR"] += 1
                log(f"  [ERROR] {name}: {exc}")
                continue

            actual_decision = resp.get("x_lemonade_route", {})
            actual_route = actual_decision.get("route_to", "")
            actual_rule = actual_decision.get("matched_rule", "")
            actual_default_used = actual_decision.get("default_used", False)

            # Detect silent LLM fallback: default_used=true but the __router
            # classifier trace entry is absent, meaning the LLM produced malformed
            # JSON or an unrecognised candidate name and the engine fell open.
            llm_fallback = False
            if llm_policy and actual_default_used:
                trace = actual_decision.get("trace", [])
                router_fired = any(
                    t.get("condition") == ROUTER_TRACE_CONDITION for t in trace
                )
                if not router_fired:
                    llm_fallback = True
                    counts["LLM_FALLBACK"] += 1
                    llm_fallback_cases.append(
                        {"name": name, "kind": kind, "actual_route": actual_route}
                    )
                    log(
                        f"  [LLM_FALLBACK] {name}: router LLM produced bad output, fell open to default"
                    )
                    continue

            passed = same_model(actual_route, expected_route)

            if kind in ("RISK_FN", "RISK_FP"):
                risk_cases.append(
                    {
                        "name": name,
                        "kind": kind,
                        "expected": expected_route,
                        "actual": actual_route,
                        "passed": passed,
                        "note": case.get("note", ""),
                    }
                )
                if not passed:
                    # A risk case documents an accepted, known gap (see its
                    # note). An UNEXPECTED flip is a behavior change worth
                    # seeing live even without --verbose.
                    log(
                        f"  [UNEXPECTED][{kind}] {name}: expected={expected_route} actual={actual_route}"
                    )
                elif args.verbose:
                    log(
                        f"  [PASS][{kind}] {name}: expected={expected_route} actual={actual_route}"
                    )
                continue

            counts[kind] += 1 if passed else 0

            if not passed:
                wrong_cases.append(
                    {
                        "name": name,
                        "kind": kind,
                        "expected_route": expected_route,
                        "actual_route": actual_route,
                        "expected_rule": expected_rule,
                        "actual_rule": actual_rule,
                        "note": case.get("note", ""),
                    }
                )
                rationale = ""
                if llm_policy:
                    trace = actual_decision.get("trace", [])
                    for t in trace:
                        if t.get("condition") == ROUTER_TRACE_CONDITION and t.get(
                            "rationale"
                        ):
                            rationale = f" rationale='{t['rationale']}'"
                            break
                pii = case.get("pii_category", "none")
                log(
                    f"  [FAIL][{kind}] {name} (pii={pii}): expected={expected_route} actual={actual_route}{rationale}"
                )
            else:
                if args.verbose:
                    rationale = ""
                    if llm_policy:
                        trace = actual_decision.get("trace", [])
                        for t in trace:
                            if t.get("condition") == ROUTER_TRACE_CONDITION and t.get(
                                "rationale"
                            ):
                                rationale = f" rationale='{t['rationale']}'"
                                break
                    log(
                        f"  [PASS][{kind}] {name}: route={actual_route} rule={actual_rule or 'default'}{rationale}"
                    )

        log(
            f"\nFinished processing {len(cases)} cases in {time.time() - start_time:.0f}s.\n"
        )

        # For counts we need totals per class regardless of pass/fail
        # Re-count class totals from the case list
        class_totals: dict[str, int] = {"TP": 0, "TN": 0, "FP": 0, "FN": 0}
        for case in cases:
            k = classify_case(case, local_model)
            if k in class_totals:
                class_totals[k] += 1

        # Passed counts per class (counts dict holds pass counts above)
        tp_pass = counts["TP"]
        tn_pass = counts["TN"]
        fp_pass = counts["FP"]
        fn_pass = counts["FN"]

        # Confusion matrix in routing terms:
        # We want to know: of cases where the model was sensitive (TP class expected local),
        # how many actually went local (correct) vs cloud (leak).
        # And of benign cases (TN class expected cloud), how many over-routed to local.

        total_sensitive = class_totals["TP"]  # true PII cases - should go local
        total_benign = class_totals["TN"]  # clean cases - should go cloud

        # A "leaked" case is a TP case that the router sent to cloud
        leaked = class_totals["TP"] - tp_pass  # TP class but failed = went cloud
        over_routed = class_totals["TN"] - tn_pass  # TN class but failed = went local

        leak_rate = leaked / total_sensitive if total_sensitive else 0.0
        over_route_rate = over_routed / total_benign if total_benign else 0.0

        # Fbeta: precision = TP_pass / (TP_pass + over_routed), recall = TP_pass / total_sensitive
        precision = (
            tp_pass / (tp_pass + over_routed) if (tp_pass + over_routed) > 0 else 0.0
        )
        recall = tp_pass / total_sensitive if total_sensitive > 0 else 0.0
        beta = args.beta
        fbeta = (
            (1 + beta**2) * precision * recall / (beta**2 * precision + recall)
            if (precision + recall) > 0
            else 0.0
        )

        log("=" * 60)
        log("CONFUSION MATRIX (non-risk cases)")
        log("=" * 60)
        log(f"  PII -> local   (TP, correct)    : {tp_pass:3d} / {total_sensitive}")
        log(f"  PII -> cloud   (LEAK, FN error) : {leaked:3d} / {total_sensitive}")
        log(f"  benign -> cloud (TN, correct)   : {tn_pass:3d} / {total_benign}")
        log(f"  benign -> local (FP, over-route): {over_routed:3d} / {total_benign}")
        log(f"  errors (HTTP/parse)            : {counts['ERROR']}")
        if llm_policy:
            log(f"  LLM fallbacks (bad JSON/name)  : {counts['LLM_FALLBACK']}")
        log()
        log("METRICS")
        log("-" * 40)
        log(
            f"  Leak rate (FN / sensitive)     : {leak_rate:.1%}  <- primary (lower is better)"
        )
        log(f"  Over-route rate (FP / benign)  : {over_route_rate:.1%}")
        log(f"  Precision                      : {precision:.3f}")
        log(f"  Recall                         : {recall:.3f}")
        log(f"  F{beta:.0f} score                    : {fbeta:.3f}")
        log()

        if risk_cases:
            log(
                f"RISK CASES ({len(risk_cases)} documented gaps - not counted in metrics)"
            )
            log("-" * 40)
            for rc in risk_cases:
                status = "expected" if rc["passed"] else "UNEXPECTED"
                log(f"  [{status}][{rc['kind']}] {rc['name']}: actual={rc['actual']}")
                if not rc["passed"]:
                    log(f"    note: {rc['note']}")
            log()

        if wrong_cases:
            log(f"UNEXPECTED FAILURES ({len(wrong_cases)})")
            log("-" * 40)
            for wc in wrong_cases:
                log(f"  [{wc['kind']}] {wc['name']}")
                log(
                    f"    expected route={wc['expected_route']} rule={wc['expected_rule'] or 'default'}"
                )
                log(
                    f"    actual   route={wc['actual_route']}   rule={wc['actual_rule'] or 'default'}"
                )
                log(f"    note: {wc['note']}")
            log()

        if llm_policy and llm_fallback_cases:
            log(
                f"LLM FALLBACKS ({len(llm_fallback_cases)} - router LLM produced bad output)"
            )
            log("-" * 40)
            for lf in llm_fallback_cases:
                log(f"  [{lf['kind']}] {lf['name']}: fell open to {lf['actual_route']}")
            log()

        log("=" * 60)
        overall = leaked == 0 and counts["ERROR"] == 0 and counts["LLM_FALLBACK"] == 0
        result = "PASS - zero leaks" if overall else "FAIL - see above"
        log(f"RESULT: {result}")
        log("=" * 60)

        if json_path:
            summary = {
                "run_id": run_id,
                "corpus_dir": str(corpus_dir),
                "policy": str(policy_path),
                "base_url": args.base_url,
                "beta": args.beta,
                "n_cases": len(cases),
                "counts": counts,
                "class_totals": class_totals,
                "leaked": leaked,
                "over_routed": over_routed,
                "leak_rate": leak_rate,
                "over_route_rate": over_route_rate,
                "precision": precision,
                "recall": recall,
                "fbeta": fbeta,
                "result": result,
                "wrong_cases": wrong_cases,
                "risk_cases": risk_cases,
                "llm_fallback_cases": llm_fallback_cases,
            }
            json_path.write_text(
                json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8"
            )
            log(f"\nFull log     -> {log_path}")
            log(f"JSON summary -> {json_path}")

        sys.exit(0 if overall else 1)
    finally:
        if log_file_handle:
            log_file_handle.close()


if __name__ == "__main__":
    evaluate(parse_args())
