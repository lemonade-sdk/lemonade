"""
PII routing evaluator.

Registers the PII router policy against a running Lemonade server, POSTs each
case from cases.jsonl, compares the actual routing decision to the expected one,
and prints a confusion matrix with leak rate, over-route rate, and Fbeta score.
Every run is also backed up to <corpus-dir>/runs/ as a timestamped text log
(line-buffered and flushed per write, so it's safe to tail mid-run) plus a
JSON summary (counts, metrics, wrong/risk/fallback cases, timing). A heartbeat
line is logged every --progress-every cases regardless of --verbose, so a large
corpus doing live inference doesn't look stalled between the startup banner
and the final summary. FAIL/ERROR/LLM_FALLBACK/UNEXPECTED-risk cases are
always logged live as they happen (not gated behind --verbose) since those
are what you'd actually want to see while debugging; --verbose additionally
prints a line for every PASS.

Each case is a single POST to /v1/chat/completions (route_trace=True,
max_tokens=1): the server both makes the routing decision AND forwards to the
routed model in that one HTTP round trip, so total per-case latency conflates
"time to route" with "time for the routed model to process the input". There
is no server-side field that reports the router's own decision time in
isolation (see extract_model_timing_ms() below for exactly what IS available
and how the split is estimated) -- treat the routing/model split as an
estimate, not an instrumented measurement. e2e time is always exact (measured
client-side); it is only the routing-vs-model breakdown that's derived, and it
also absorbs one-time model-load latency on cold-cache cases (see the "Timing"
section comment above extract_model_timing_ms for details/measurements).

Timing is logged per-case (appended to the existing PASS/FAIL/risk/fallback
log lines) and summarized at the end in a TIMING section: sum/mean/median for
e2e, estimated routing, and routed-model time, plus how many cases had no
model-side timing available (e.g. cloud candidates, which don't report
inference duration -- see extract_model_timing_ms()). The full JSON summary
also gets a "timing" aggregate block and a per-case "timing_samples" array for
offline analysis (e.g. plotting a latency histogram).

Usage:
    python test/eval/pii_routing_eval.py [--base-url URL] [--corpus-dir DIR] [--policy FILE] [--beta FLOAT] [--verbose] [--limit N] [--timeout SECONDS] [--progress-every N] [--log-dir DIR] [--no-log-file]

Arguments:
    --base-url       Lemonade server base URL to test against. The server must
                      already be running (lemond); this script does not start it.
                      Default: http://localhost:13305
    --corpus-dir     Directory containing policy.json/policy_llm.json and
                      cases.jsonl. This is what controls corpus SIZE and
                      CONTENT -- there's no separate "corpus size" flag,
                      because the corpus is a fixed, pre-built fixture (see
                      stats.json alongside cases.jsonl in each corpus dir for
                      its composition). Use --limit to run a subset instead of
                      the whole file. Default: test/conformance/routing/1/l2_pii_regex
    --policy         Policy file to register and test. Relative to
                      --corpus-dir, or an absolute/relative-to-cwd path
                      elsewhere (e.g. ../l2_pii_regex/policy_llm.json to test
                      a different corpus dir's policy against this corpus).
                      Use policy.json for the regex router, policy_llm.json for
                      the LLM router. Default: policy.json
    --beta           Beta for the Fbeta score; beta>1 weights recall over
                      precision (missing real PII is worse than over-routing a
                      benign prompt to the local model). Default: 2.0
    --verbose        Print a log line for every PASS, not just
                      FAIL/ERROR/LLM_FALLBACK/UNEXPECTED-risk cases (those are
                      always printed). Each line includes the timing suffix.
    --limit          Only evaluate the first N cases (0 = all, the default).
                      Cases in the corpus are pre-shuffled by the builder, so a
                      small N (e.g. 20-50) still gives a random-but-reproducible
                      mix for a quick smoke test before committing to a full,
                      hours-long run. Default: 0
    --timeout        HTTP timeout per request, in seconds. Increase this for
                      LLM-router policies and/or large corpora -- the router
                      LLM has to generate a routing decision before the routed
                      model even starts on the actual request, so per-case
                      latency is higher than a plain completion. Default: 60
    --progress-every Log a heartbeat every N cases regardless of --verbose (0
                      disables it). Includes a running cases/sec rate so you
                      can project remaining time on a multi-hour run. Default: 50
    --log-dir        Directory to back up the full run output (text log + JSON
                      summary). Files are timestamped so runs don't collide.
                      Default: <corpus-dir>/runs/
    --no-log-file    Skip writing the run log / JSON summary backup (prints to
                      stdout only).
    --resume-from-log  Path to a previous run's .log file (same corpus dir and
                      policy). Cases already answered there (parsed from its
                      PASS/FAIL lines) are replayed from that recorded outcome
                      instead of re-queried, so an interrupted multi-hour run
                      can continue without redoing already-completed work or
                      double-counting it in the final report. ERROR lines are
                      deliberately NOT deduplicated -- those cases are retried.

Examples:
    # Regex policy against hand-crafted corpus
    python test/eval/pii_routing_eval.py --verbose

    # LLM policy against the same hand-crafted corpus
    python test/eval/pii_routing_eval.py --policy policy_llm.json --verbose

    # LLM policy against Nemotron corpus (2,500 cases; expect a multi-hour run
    # for a cloud default candidate -- use --limit for a smoke test first)
    python test/eval/pii_routing_eval.py \\
        --corpus-dir test/conformance/routing/1/l2_pii_nemotron \\
        --policy ../l2_pii_regex/policy_llm.json --limit 20 --verbose

    python test/eval/pii_routing_eval.py \\
        --corpus-dir test/conformance/routing/1/l2_pii_nemotron \\
        --policy ../l2_pii_regex/policy_llm.json --verbose
"""

import argparse
import json
import os
import re
import statistics
import sys
import time
from collections import Counter
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
    p.add_argument(
        "--resume-from-log",
        default=None,
        help="Path to a previous run's .log file for this same corpus+policy. "
        "Cases already answered there are replayed from the recorded outcome "
        "instead of re-queried. ERROR lines are not deduplicated (retried).",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# Server helpers
# ---------------------------------------------------------------------------


def wait_for_server(base_url: str, timeout: int = 30) -> None:
    # Bare "/health" isn't a registered route (see the quad-prefix registration
    # in server.cpp) -- it 200s anyway because it falls through to the web
    # app's SPA catch-all, which is up as soon as the process is listening and
    # says nothing about whether the router/backends are actually ready. Use
    # the real health endpoint.
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = requests.get(f"{base_url}/api/v1/health", timeout=5)
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
# Resume from a prior run's log
# ---------------------------------------------------------------------------
#
# These three shapes are the only case-result lines this script's own log()
# calls emit (see the matching f-strings in evaluate() below) -- non-risk
# PASS uses route=/rule=, non-risk FAIL adds the "(pii=...)" parenthetical
# and expected=/actual=, and RISK_*/UNEXPECTED lines share the FAIL-style
# expected=/actual= but without the parenthetical. ERROR and LLM_FALLBACK
# lines are deliberately not matched here -- those cases get retried on
# resume rather than replayed, since their prior outcome wasn't a real
# answer.

_PASS_NONRISK_RE = re.compile(
    r"^\s*\[PASS\]\[(?P<kind>[A-Z]+)\]\s+(?P<name>\S+):\s+"
    r"route=(?P<route>\S+)\s+rule=(?P<rule>\S+?)"
    r"(?:\s+rationale='[^']*')?\s*"
    r"\[e2e=(?P<e2e>[\d.]+)ms(?:\s+routing~=(?P<routing>[\d.]+)ms\s+model=(?P<model>[\d.]+)ms|\s+model=n/a)\]\s*$"
)
_FAIL_NONRISK_RE = re.compile(
    r"^\s*\[FAIL\]\[(?P<kind>[A-Z]+)\]\s+(?P<name>\S+)\s+\(pii=(?P<pii>[^)]*)\):\s+"
    r"expected=(?P<expected>\S+)\s+actual=(?P<actual>\S+)"
    r"(?:\s+rationale='[^']*')?\s*"
    r"\[e2e=(?P<e2e>[\d.]+)ms(?:\s+routing~=(?P<routing>[\d.]+)ms\s+model=(?P<model>[\d.]+)ms|\s+model=n/a)\]\s*$"
)
_RISK_RE = re.compile(
    r"^\s*\[(?P<status>PASS|UNEXPECTED)\]\[(?P<kind>RISK_\w+)\]\s+(?P<name>\S+):\s+"
    r"expected=(?P<expected>\S+)\s+actual=(?P<actual>\S+)"
    r"\s*\[e2e=(?P<e2e>[\d.]+)ms(?:\s+routing~=(?P<routing>[\d.]+)ms\s+model=(?P<model>[\d.]+)ms|\s+model=n/a)\]\s*$"
)


def parse_prior_log(path: Path) -> dict[str, dict]:
    """Case name -> replayed outcome, parsed from a previous run's text log."""
    results: dict[str, dict] = {}
    unrecognized = 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.lstrip()
        if not stripped.startswith("["):
            continue
        m = _PASS_NONRISK_RE.match(line)
        if m:
            gd = m.groupdict()
            results[gd["name"]] = {
                "kind": gd["kind"],
                "passed": True,
                "actual_route": gd["route"],
                "actual_rule": gd["rule"],
                "expected_route": None,
                "e2e_ms": float(gd["e2e"]),
                "routing_ms": float(gd["routing"]) if gd["routing"] else None,
                "model_ms": float(gd["model"]) if gd["model"] else None,
            }
            continue
        m = _FAIL_NONRISK_RE.match(line)
        if m:
            gd = m.groupdict()
            results[gd["name"]] = {
                "kind": gd["kind"],
                "passed": False,
                "actual_route": gd["actual"],
                "actual_rule": None,
                "expected_route": gd["expected"],
                "e2e_ms": float(gd["e2e"]),
                "routing_ms": float(gd["routing"]) if gd["routing"] else None,
                "model_ms": float(gd["model"]) if gd["model"] else None,
            }
            continue
        m = _RISK_RE.match(line)
        if m:
            gd = m.groupdict()
            results[gd["name"]] = {
                "kind": gd["kind"],
                "passed": gd["status"] == "PASS",
                "actual_route": gd["actual"],
                "actual_rule": None,
                "expected_route": gd["expected"],
                "e2e_ms": float(gd["e2e"]),
                "routing_ms": float(gd["routing"]) if gd["routing"] else None,
                "model_ms": float(gd["model"]) if gd["model"] else None,
            }
            continue
        if stripped.startswith("[ERROR]") or stripped.startswith("[LLM_FALLBACK]"):
            continue
        if stripped.startswith(("[PASS]", "[FAIL]", "[UNEXPECTED]")):
            unrecognized += 1
    if unrecognized:
        print(
            f"  (resume: {unrecognized} result line(s) in the prior log didn't match "
            "a known format and will be re-run, not replayed)",
            file=sys.stderr,
        )
    return results


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


def infer_local_model_from_corpus(cases: list[dict]) -> str | None:
    """Ground truth for "which candidate is local/private", read straight from
    the corpus instead of trusting policy structure: the most common route_to
    among non-risk cases that actually carry PII.

    privacy_route(policy) infers the local candidate from policy JSON (the
    'pii-detected' rule's route_to, or default_model for LLM policies), which
    is only correct if the policy file is configured consistently with the
    corpus it's being scored against. It's easy for the two to drift apart
    (e.g. a policy's routing.default_model gets copy-pasted from a sibling
    regex policy where default_model means "no rule matched" rather than "the
    router LLM's safe fallback", silently pointing the LLM's fallback at the
    cloud candidate; or a corpus's cases.jsonl keeps a stale local-model name
    after the policy's candidate gets renamed). Either drift makes every
    expected_route == local_model comparison in classify_case() wrong across
    the whole run without raising an error. Call this once per run and cross-
    check it against privacy_route(policy) so that kind of drift is a loud
    warning instead of a silently corrupted confusion matrix.
    """
    votes = Counter(
        case["decision"]["route_to"]
        for case in cases
        if case.get("pii_category", "none") != "none" and not is_risk_case(case)
    )
    if not votes:
        return None
    return votes.most_common(1)[0][0]


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
# Timing
# ---------------------------------------------------------------------------
#
# A case is one HTTP round trip: the server picks a route AND the routed model
# processes the request in the same request/response cycle (see
# route_decision_to_json in route_decision_response.cpp -- the decision JSON
# has no timing field). So the only thing directly measurable from the client
# is total wall-clock time per case (e2e_ms, below). To approximate the split
# the user actually wants -- how much of that was the router deciding vs. the
# routed model doing the work -- we lean on a field the ROUTED model's backend
# already puts in the response for its own telemetry:
#
#   - llama.cpp-backed candidates (e.g. any *-GGUF local model) return a
#     top-level "timings" object with prompt_ms + predicted_ms: the backend's
#     own prefill+decode time for the actual request (see the "llama-server
#     includes timing data in the response" handling in server.cpp).
#   - FastFlowLM-backed candidates report duration via "usage" fields
#     (prefill_duration_ttft seconds, decoding_speed_tps) instead.
#   - Cloud candidates (e.g. fireworks.*) report neither -- there's no
#     backend-side duration to read, so the split is left as "unavailable"
#     for those cases and only e2e time is known.
#
# model_ms above is real backend-reported time for the routed model's own
# request. routing_ms is *not* independently measured -- it's e2e_ms minus
# model_ms, i.e. "everything else": the router LLM's own classification call
# (for LLM-router policies) plus policy evaluation and HTTP/process overhead.
# It's a reasonable estimate, not an instrumented number; label it as such
# wherever it's printed.
#
# One-time model LOAD latency also lands in that "everything else" bucket: the
# first request that needs a candidate not already resident (cold start, or
# after the router's LRU evicts it) pays subprocess-start + weight-load time
# on top of routing, and that shows up entirely as routing_ms even though it
# has nothing to do with the router. Confirmed empirically: a cold Qwen3.5-2B
# load added ~19s to routing_ms on the first case, vs ~30-50ms once warm. On a
# multi-hundred-case run this is one or two outlier cases out of many and
# washes out of the mean, but don't read a single case's routing_ms as "how
# long the router took" without checking it wasn't also a cold load -- the
# per-case log line doesn't distinguish the two.


def extract_model_timing_ms(resp: dict) -> tuple[float | None, str]:
    """Return (model_ms, source) for the routed model's own inference time,
    read from whatever timing fields its backend put in the response.
    model_ms is None (source="unavailable") when the backend doesn't report
    one, which is expected/normal for cloud candidates.
    """
    timings = resp.get("timings")
    if (
        isinstance(timings, dict)
        and "prompt_ms" in timings
        and "predicted_ms" in timings
    ):
        try:
            return (
                float(timings["prompt_ms"]) + float(timings["predicted_ms"]),
                "llama_timings",
            )
        except (TypeError, ValueError):
            pass

    usage = resp.get("usage")
    if isinstance(usage, dict) and "prefill_duration_ttft" in usage:
        try:
            ttft_s = float(usage["prefill_duration_ttft"])
        except (TypeError, ValueError):
            return None, "unavailable"
        decode_s = 0.0
        tps = usage.get("decoding_speed_tps")
        completion_tokens = usage.get("completion_tokens")
        if tps and completion_tokens:
            try:
                decode_s = float(completion_tokens) / float(tps)
            except (TypeError, ValueError, ZeroDivisionError):
                decode_s = 0.0
        return (ttft_s + decode_s) * 1000.0, "flm_usage"

    return None, "unavailable"


def format_timing_suffix(
    e2e_ms: float, routing_ms: float | None, model_ms: float | None
) -> str:
    if routing_ms is None or model_ms is None:
        return f" [e2e={e2e_ms:.0f}ms model=n/a]"
    return f" [e2e={e2e_ms:.0f}ms routing~={routing_ms:.0f}ms model={model_ms:.0f}ms]"


def fmt_hms(seconds: float) -> str:
    seconds = int(seconds)
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    if h:
        return f"{h}h{m:02d}m{s:02d}s"
    if m:
        return f"{m}m{s:02d}s"
    return f"{s}s"


def summarize_timing(values: list[float]) -> dict[str, float]:
    if not values:
        return {"n": 0, "sum_ms": 0.0, "mean_ms": 0.0, "median_ms": 0.0, "max_ms": 0.0}
    return {
        "n": len(values),
        "sum_ms": sum(values),
        "mean_ms": statistics.mean(values),
        "median_ms": statistics.median(values),
        "max_ms": max(values),
    }


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

        inferred_local = infer_local_model_from_corpus(cases)
        if inferred_local and not same_model(inferred_local, local_model):
            log("!" * 70)
            log("WARNING: policy privacy route disagrees with corpus ground truth.")
            log(f"  privacy_route(policy) -> '{local_model}'")
            log(
                f"  corpus ground truth    -> '{inferred_local}' "
                "(most common route_to among PII cases)"
            )
            log(
                "  Using the corpus-derived value below, since that's what "
                "expected_route in cases.jsonl actually targets. This usually "
                "means routing.default_model (LLM policies) or a candidate "
                "name is stale/misconfigured relative to this corpus -- fix "
                "the policy or regenerate the corpus before trusting a full run."
            )
            log("!" * 70)
            local_model = inferred_local

        log(f"Policy '{policy['model_name']}' registered.")
        log(f"Privacy route target: '{local_model}'\n")

        prior_results: dict[str, dict] = {}
        if args.resume_from_log:
            prior_log_path = Path(args.resume_from_log)
            if not prior_log_path.exists():
                print(
                    f"ERROR: --resume-from-log path not found: {prior_log_path}",
                    file=sys.stderr,
                )
                sys.exit(1)
            prior_results = parse_prior_log(prior_log_path)
            log(
                f"Resuming from {prior_log_path.name}: {len(prior_results)} cases "
                "already answered there will be replayed (no re-query).\n"
            )

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
        timing_samples: list[dict] = []

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

            replay = prior_results.get(name)
            if replay is not None:
                e2e_ms = replay["e2e_ms"]
                model_ms = replay["model_ms"]
                routing_ms = replay["routing_ms"]
                actual_route = replay["actual_route"] or ""
                actual_rule = replay["actual_rule"] or ""
                passed = replay["passed"]
                timing_samples.append(
                    {
                        "name": name,
                        "kind": kind,
                        "e2e_ms": e2e_ms,
                        "routing_ms_est": routing_ms,
                        "model_ms": model_ms,
                        "model_timing_source": "replayed",
                    }
                )
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
                        log(
                            f"  [UNEXPECTED][{kind}] {name}: expected={expected_route} actual={actual_route} [replayed]"
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
                            "note": (
                                case.get("note", "") + " (replayed from prior log)"
                            ).strip(),
                        }
                    )
                    pii = case.get("pii_category", "none")
                    log(
                        f"  [FAIL][{kind}] {name} (pii={pii}): expected={expected_route} actual={actual_route} [replayed]"
                    )
                elif args.verbose:
                    log(
                        f"  [PASS][{kind}] {name}: route={actual_route} rule={actual_rule or 'default'} [replayed]"
                    )
                continue

            request_start = time.perf_counter()
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
            e2e_ms = (time.perf_counter() - request_start) * 1000.0

            model_ms, timing_source = extract_model_timing_ms(resp)
            routing_ms = (e2e_ms - model_ms) if model_ms is not None else None
            timing_samples.append(
                {
                    "name": name,
                    "kind": kind,
                    "e2e_ms": e2e_ms,
                    "routing_ms_est": routing_ms,
                    "model_ms": model_ms,
                    "model_timing_source": timing_source,
                }
            )
            timing_suffix = format_timing_suffix(e2e_ms, routing_ms, model_ms)

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
                        f"  [LLM_FALLBACK] {name}: router LLM produced bad output, fell open to default{timing_suffix}"
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
                        f"  [UNEXPECTED][{kind}] {name}: expected={expected_route} actual={actual_route}{timing_suffix}"
                    )
                elif args.verbose:
                    log(
                        f"  [PASS][{kind}] {name}: expected={expected_route} actual={actual_route}{timing_suffix}"
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
                    f"  [FAIL][{kind}] {name} (pii={pii}): expected={expected_route} actual={actual_route}{rationale}{timing_suffix}"
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
                        f"  [PASS][{kind}] {name}: route={actual_route} rule={actual_rule or 'default'}{rationale}{timing_suffix}"
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

        e2e_values = [t["e2e_ms"] for t in timing_samples]
        routing_values = [
            t["routing_ms_est"]
            for t in timing_samples
            if t["routing_ms_est"] is not None
        ]
        model_values = [
            t["model_ms"] for t in timing_samples if t["model_ms"] is not None
        ]
        e2e_stats = summarize_timing(e2e_values)
        routing_stats = summarize_timing(routing_values)
        model_stats = summarize_timing(model_values)
        n_no_split = e2e_stats["n"] - routing_stats["n"]

        log("=" * 60)
        log("TIMING (routing/model split is an ESTIMATE, see module docstring)")
        log("=" * 60)
        log(f"  Cases timed                    : {e2e_stats['n']}")
        log(
            f"  ...with a model-side split     : {routing_stats['n']} "
            f"({n_no_split} unavailable, e.g. cloud candidates)"
        )
        log(f"  E2E total (sum of per-case)    : {fmt_hms(e2e_stats['sum_ms'] / 1000)}")
        log(
            f"  E2E per case  avg / median / max: "
            f"{e2e_stats['mean_ms'] / 1000:.2f}s / {e2e_stats['median_ms'] / 1000:.2f}s / "
            f"{e2e_stats['max_ms'] / 1000:.2f}s"
        )
        if routing_stats["n"]:
            routing_share = (
                routing_stats["sum_ms"] / e2e_stats["sum_ms"]
                if e2e_stats["sum_ms"]
                else 0.0
            )
            model_share = (
                model_stats["sum_ms"] / e2e_stats["sum_ms"]
                if e2e_stats["sum_ms"]
                else 0.0
            )
            log(
                f"  Routing (router.model) avg/median: "
                f"{routing_stats['mean_ms'] / 1000:.2f}s / {routing_stats['median_ms'] / 1000:.2f}s "
                f"(~{routing_share:.1%} of split cases' e2e)"
            )
            log(
                f"  Routed-model eval avg/median   : "
                f"{model_stats['mean_ms'] / 1000:.2f}s / {model_stats['median_ms'] / 1000:.2f}s "
                f"(~{model_share:.1%} of split cases' e2e)"
            )
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
                "timing": {
                    "e2e_ms": e2e_stats,
                    "routing_ms_est": routing_stats,
                    "model_ms": model_stats,
                    "n_no_model_split": n_no_split,
                },
                "timing_samples": timing_samples,
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
