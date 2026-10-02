"""
A/B two ort-server binaries on the same model directory, label by label.

Launches a baseline and a candidate ort-server per --model, sends both the
exact text lemond forwards to /classify for each sampled case (the last user
message) plus the synthetic probes in ort_parity_common.py, and diffs the
responses: HTTP status, label set, every score, and the routing decision the
l2_pii_onnx_* policies take from them (any non-"O" label >= --min-score).

Requests go to one server at a time, so the two never compete for cores and
the run is as deterministic as ONNX Runtime itself.

    python test/eval/ort_server_parity.py \\
        --baseline  <old>/ort-server.exe --candidate <new>/ort-server.exe \\
        --model mmbert=~/.cache/lemonade-onnx-models/mmbert32k-pii-onnx \\
        [--model pf-v2=DIR ...] [--sample 500] [--longest 100] [--non-ascii 150]

Writes <out-dir>/server_parity_<model>_<ts>.jsonl (one row per input) and a
combined server_parity_<ts>.json summary; exits 1 if any CORPUS case differs
beyond --tolerance or flips a routing decision. Probe differences are reported
but do not fail the run -- some are expected (see probe_texts()).
"""

import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).parent))
from ort_parity_common import (  # noqa: E402
    DEFAULT_CORPUS,
    detected_types,
    load_cases,
    parse_model_args,
    probe_texts,
    routes_local,
    select_sample,
)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class OrtServer:
    def __init__(self, binary: Path, model_dir: Path, log_path: Path):
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.log = open(log_path, "w", encoding="utf-8", errors="replace")
        self.proc = subprocess.Popen(
            [str(binary), "--model-path", str(model_dir), "--port", str(self.port)],
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )

    def wait_ready(self, timeout: float) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"exited with {self.proc.returncode}; see {self.log.name}"
                )
            try:
                if requests.get(f"{self.base}/health", timeout=2).ok:
                    return
            except requests.RequestException:
                pass
            time.sleep(0.5)
        raise RuntimeError(f"not ready after {timeout}s; see {self.log.name}")

    def classify(self, text: str, timeout: float) -> tuple[int, dict, float]:
        t0 = time.perf_counter()
        try:
            r = requests.post(
                f"{self.base}/classify", json={"text": text}, timeout=timeout
            )
            body = r.json() if r.content else {}
            status = r.status_code
        except requests.RequestException as e:
            status, body = 0, {"error": f"{type(e).__name__}: {e}"}
        return status, body, (time.perf_counter() - t0) * 1000

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()


def compare(a_status, a_body, b_status, b_body, min_score, tolerance) -> dict:
    row = {"status": [a_status, b_status]}
    a_labels, b_labels = a_body.get("labels"), b_body.get("labels")
    if a_status != 200 or b_status != 200 or a_labels is None or b_labels is None:
        row["errors"] = [a_body.get("error"), b_body.get("error")]
        row["agree"] = a_status == b_status
        return row
    keys = set(a_labels) | set(b_labels)
    diffs = {
        k: abs(a_labels.get(k, float("nan")) - b_labels.get(k, float("nan")))
        for k in keys
    }
    worst = max(diffs, key=lambda k: diffs[k] if diffs[k] == diffs[k] else float("inf"))
    row.update(
        {
            "same_label_set": set(a_labels) == set(b_labels),
            "max_abs_diff": diffs[worst],
            "worst_label": worst,
            "route_local": [
                routes_local(a_labels, min_score),
                routes_local(b_labels, min_score),
            ],
            "detected": [
                detected_types(a_labels, min_score),
                detected_types(b_labels, min_score),
            ],
        }
    )
    row["flip"] = row["route_local"][0] != row["route_local"][1]
    row["agree"] = (
        row["same_label_set"]
        and not row["flip"]
        and diffs[worst] == diffs[worst]
        and diffs[worst] <= tolerance
    )
    return row


def run_model(name, model_dir, inputs, args, out_dir, stamp) -> dict:
    rows_path = out_dir / f"server_parity_{name}_{stamp}.jsonl"
    a = OrtServer(
        args.baseline, model_dir, out_dir / f"server_parity_{name}_{stamp}.baseline.log"
    )
    b = OrtServer(
        args.candidate,
        model_dir,
        out_dir / f"server_parity_{name}_{stamp}.candidate.log",
    )
    try:
        a.wait_ready(args.startup_timeout)
        b.wait_ready(args.startup_timeout)
        print(
            f"[{name}] baseline :{a.port}  candidate :{b.port}  inputs={len(inputs)}",
            flush=True,
        )
        summary = {
            "model_dir": str(model_dir),
            "inputs": len(inputs),
            "corpus": 0,
            "probes": 0,
            "corpus_disagree": [],
            "probe_disagree": [],
            "flips": [],
            "max_abs_diff_corpus": 0.0,
            "status_mismatch": [],
            "ms_baseline": 0.0,
            "ms_candidate": 0.0,
        }
        t_start = time.time()
        with open(rows_path, "w", encoding="utf-8") as f:
            for i, item in enumerate(inputs, 1):
                a_st, a_body, a_ms = a.classify(item["text"], args.timeout)
                b_st, b_body, b_ms = b.classify(item["text"], args.timeout)
                row = {
                    "case": item["name"],
                    "why": item["why"],
                    "chars": len(item["text"]),
                    "ms": [round(a_ms, 1), round(b_ms, 1)],
                }
                row.update(
                    compare(a_st, a_body, b_st, b_body, args.min_score, args.tolerance)
                )
                f.write(json.dumps(row, ensure_ascii=False) + "\n")

                is_probe = item["why"] == "probe"
                summary["probes" if is_probe else "corpus"] += 1
                if not is_probe:
                    summary["ms_baseline"] += a_ms
                    summary["ms_candidate"] += b_ms
                    if (
                        "max_abs_diff" in row
                        and row["max_abs_diff"] == row["max_abs_diff"]
                    ):
                        summary["max_abs_diff_corpus"] = max(
                            summary["max_abs_diff_corpus"], row["max_abs_diff"]
                        )
                if not row["agree"]:
                    summary["probe_disagree" if is_probe else "corpus_disagree"].append(
                        item["name"]
                    )
                if row.get("flip"):
                    summary["flips"].append(item["name"])
                if row["status"][0] != row["status"][1]:
                    summary["status_mismatch"].append(item["name"])
                if is_probe or not row["agree"] or args.verbose:
                    tag = "OK  " if row["agree"] else "DIFF"
                    detail = (
                        f"max|d|={row['max_abs_diff']:.3g} route_local={row['route_local']}"
                        if "max_abs_diff" in row
                        else f"status={row['status']} " f"errors={row.get('errors')}"
                    )
                    print(
                        f"  [{tag}] {item['name']} ({item['why']}, {row['chars']} chars): "
                        f"{detail}",
                        flush=True,
                    )
                if i % args.progress_every == 0:
                    rate = i / (time.time() - t_start)
                    print(f"  ... {i}/{len(inputs)} ({rate:.2f}/s)", flush=True)
        n = max(summary["corpus"], 1)
        summary["ms_baseline"] = round(summary["ms_baseline"] / n, 1)
        summary["ms_candidate"] = round(summary["ms_candidate"] / n, 1)
        summary["rows"] = str(rows_path)
        return summary
    finally:
        a.stop()
        b.stop()


def parse_args():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--baseline", type=Path, required=True, help="Old ort-server binary")
    p.add_argument(
        "--candidate", type=Path, required=True, help="New ort-server binary"
    )
    p.add_argument(
        "--model", action="append", default=[], metavar="NAME=DIR", required=True
    )
    p.add_argument("--corpus-dir", type=Path, default=DEFAULT_CORPUS)
    p.add_argument(
        "--sample", type=int, default=500, help="Corpus cases per model (0 = all)"
    )
    p.add_argument("--longest", type=int, default=100)
    p.add_argument("--non-ascii", type=int, default=150)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--no-probes", action="store_true")
    p.add_argument("--min-score", type=float, default=0.5)
    p.add_argument(
        "--tolerance",
        type=float,
        default=0.0,
        help="Max |score difference| still counted as agreement (default: exact)",
    )
    p.add_argument("--timeout", type=float, default=300)
    p.add_argument("--startup-timeout", type=float, default=600)
    p.add_argument("--progress-every", type=int, default=50)
    p.add_argument(
        "--verbose", action="store_true", help="Print every input, not only diffs"
    )
    p.add_argument(
        "--out-dir", type=Path, default=None, help="Default: <corpus-dir>/runs"
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    models = parse_model_args(args.model)
    cases = select_sample(
        load_cases(args.corpus_dir),
        args.sample,
        args.longest,
        args.non_ascii,
        args.seed,
    )
    inputs = cases + (
        []
        if args.no_probes
        else [
            {"name": f"probe:{n}", "text": t, "why": "probe"} for n, t in probe_texts()
        ]
    )
    out_dir = args.out_dir or args.corpus_dir / "runs"
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")

    report = {
        "baseline": str(args.baseline),
        "candidate": str(args.candidate),
        "corpus": str(args.corpus_dir),
        "sample": [c["name"] for c in cases],
        "tolerance": args.tolerance,
        "min_score": args.min_score,
        "models": {},
    }
    failed = False
    for name, model_dir in models:
        s = run_model(name, model_dir, inputs, args, out_dir, stamp)
        report["models"][name] = s
        failed |= bool(
            s["corpus_disagree"]
            or [c for c in s["flips"] if not c.startswith("probe:")]
        )
        print(
            f"[{name}] corpus {s['corpus'] - len(s['corpus_disagree'])}/{s['corpus']} agree, "
            f"max|d|={s['max_abs_diff_corpus']:.3g}, flips={len(s['flips'])}, "
            f"probes disagree={s['probe_disagree']}, avg ms {s['ms_baseline']} -> "
            f"{s['ms_candidate']}\n",
            flush=True,
        )

    path = out_dir / f"server_parity_{stamp}.json"
    path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(
        f"RESULT: {'FAIL - corpus responses differ' if failed else 'PASS - corpus responses identical'}"
    )
    print(f"Report -> {path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
