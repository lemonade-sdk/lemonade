"""
PII routing evaluator.

Registers the PII router policy against a running Lemonade server, POSTs each
case from cases.jsonl, compares the actual routing decision to the expected one,
and prints a confusion matrix with leak rate, over-route rate, and Fbeta score.

Usage:
    python test/eval/pii_routing_eval.py [--base-url URL] [--corpus-dir DIR] [--beta FLOAT] [--verbose]

Defaults:
    --base-url   http://localhost:13305
    --corpus-dir test/conformance/routing/1/l2_pii_regex
    --beta       2.0  (recall-weighted: missing PII is more costly than over-routing)
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

import requests

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
        "--beta",
        type=float,
        default=2.0,
        help="Beta for Fbeta score (beta>1 weights recall over precision)",
    )
    p.add_argument("--verbose", action="store_true", help="Print per-case results")
    p.add_argument(
        "--timeout", type=int, default=60, help="HTTP timeout per request (seconds)"
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


def chat_completion(base_url: str, request_body: dict, timeout: int) -> dict:
    endpoint = f"{base_url}/v1/chat/completions"
    payload = dict(request_body)
    payload["route_trace"] = True
    payload["max_tokens"] = 1
    r = requests.post(endpoint, json=payload, timeout=timeout)
    r.raise_for_status()
    return r.json()


# ---------------------------------------------------------------------------
# Case classification
# ---------------------------------------------------------------------------

RISK_NOTE_MARKERS = ("RISK",)


def is_risk_case(case: dict) -> bool:
    note = case.get("note", "")
    return any(m in note for m in RISK_NOTE_MARKERS)


def privacy_route(policy: dict) -> str:
    """Return the candidate that PII rules route to (the local/private model)."""
    for rule in policy.get("routing", {}).get("rules", []):
        if rule.get("id") == "pii-detected":
            return rule["route_to"]
    # Fall back to any non-default candidate
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
    policy_path = corpus_dir / "policy.json"
    cases_path = corpus_dir / "cases.jsonl"

    if not policy_path.exists():
        print(f"ERROR: policy.json not found at {policy_path}", file=sys.stderr)
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

    print(f"Corpus : {corpus_dir}")
    print(f"Cases  : {len(cases)}")
    print(f"Server : {args.base_url}")
    print(f"Beta   : {args.beta}")
    print()

    wait_for_server(args.base_url, timeout=15)
    register_policy(args.base_url, policy, args.timeout)
    local_model = privacy_route(policy)
    print(f"Policy '{policy['model_name']}' registered.")
    print(f"Privacy route target: '{local_model}'\n")

    # Counters
    counts: dict[str, int] = {
        "TP": 0,
        "TN": 0,
        "FP": 0,
        "FN": 0,
        "RISK_FN": 0,
        "RISK_FP": 0,
        "ERROR": 0,
    }
    wrong_cases: list[dict] = []
    risk_cases: list[dict] = []

    for case in cases:
        name = case.get("name", "?")
        expected_route = case["decision"]["route_to"]
        expected_rule = case["decision"].get("matched_rule", "")
        kind = classify_case(case, local_model)

        try:
            resp = chat_completion(args.base_url, case["request"], args.timeout)
        except Exception as exc:
            counts["ERROR"] += 1
            if args.verbose:
                print(f"  [ERROR] {name}: {exc}")
            continue

        actual_decision = resp.get("x_lemonade_route", {})
        actual_route = actual_decision.get("route_to", "")
        actual_rule = actual_decision.get("matched_rule", "")
        passed = actual_route == expected_route

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
            if args.verbose:
                status = "PASS" if passed else "FAIL"
                print(
                    f"  [{status}][{kind}] {name}: expected={expected_route} actual={actual_route}"
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
            if args.verbose:
                print(
                    f"  [FAIL][{kind}] {name}: expected={expected_route} actual={actual_route}"
                )
        else:
            if args.verbose:
                print(
                    f"  [PASS][{kind}] {name}: route={actual_route} rule={actual_rule or 'default'}"
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

    total_sensitive = class_totals["TP"]  # true PII cases — should go local
    total_benign = class_totals["TN"]  # clean cases — should go cloud

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

    print("=" * 60)
    print("CONFUSION MATRIX (non-risk cases)")
    print("=" * 60)
    print(f"  PII → local   (TP, correct)    : {tp_pass:3d} / {total_sensitive}")
    print(f"  PII → cloud   (LEAK, FN error) : {leaked:3d} / {total_sensitive}")
    print(f"  benign → cloud (TN, correct)   : {tn_pass:3d} / {total_benign}")
    print(f"  benign → local (FP, over-route): {over_routed:3d} / {total_benign}")
    print(f"  errors (HTTP/parse)            : {counts['ERROR']}")
    print()
    print("METRICS")
    print("-" * 40)
    print(
        f"  Leak rate (FN / sensitive)     : {leak_rate:.1%}  ← primary (lower is better)"
    )
    print(f"  Over-route rate (FP / benign)  : {over_route_rate:.1%}")
    print(f"  Precision                      : {precision:.3f}")
    print(f"  Recall                         : {recall:.3f}")
    print(f"  F{beta:.0f} score                    : {fbeta:.3f}")
    print()

    if risk_cases:
        print(
            f"RISK CASES ({len(risk_cases)} documented gaps — not counted in metrics)"
        )
        print("-" * 40)
        for rc in risk_cases:
            status = "expected" if rc["passed"] else "UNEXPECTED"
            print(f"  [{status}][{rc['kind']}] {rc['name']}: actual={rc['actual']}")
            if not rc["passed"]:
                print(f"    note: {rc['note']}")
        print()

    if wrong_cases:
        print(f"UNEXPECTED FAILURES ({len(wrong_cases)})")
        print("-" * 40)
        for wc in wrong_cases:
            print(f"  [{wc['kind']}] {wc['name']}")
            print(
                f"    expected route={wc['expected_route']} rule={wc['expected_rule'] or 'default'}"
            )
            print(
                f"    actual   route={wc['actual_route']}   rule={wc['actual_rule'] or 'default'}"
            )
            print(f"    note: {wc['note']}")
        print()

    print("=" * 60)
    overall = leaked == 0 and counts["ERROR"] == 0
    result = "PASS — zero leaks" if overall else "FAIL — see above"
    print(f"RESULT: {result}")
    print("=" * 60)

    sys.exit(0 if overall else 1)


if __name__ == "__main__":
    evaluate(parse_args())
