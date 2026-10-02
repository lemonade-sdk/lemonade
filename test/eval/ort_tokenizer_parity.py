"""
Full-corpus input_ids parity between ort-server's old and new tokenizer layers.

ort-server moved from mlc-ai/tokenizers-cpp (HF `tokenizers` 0.21.4, per its
Cargo.lock) to third_party/tok_ffi (HF `tokenizers` 0.22.2). On the encoder
path that swap is the ONLY change: mask, segment ids, pad stripping, the
budget cut and the aggregation are the same C++. So if every case yields the
same input_ids under both layers, the model sees the same tensor and -- with
the same ONNX Runtime DLL -- produces the same scores. That makes this check
exhaustive where a server A/B (ort_server_parity.py) can only sample.

Each layer is reproduced as ort-server drives it:

  old  Tokenizer.from_str(tokenizer.json), tokenizer.json truncation LEFT ON
       (tokenizers-cpp never touched it), encode(add_special_tokens=True)
  new  same, but with truncation CLEARED (tok_new calls with_truncation(None))

then both go through ort-server's own post-tokenizer steps (strip trailing
padding, cut to max_length keeping the last token). Each layer runs in its own
interpreter because the two crate versions cannot share one process:

    python -m venv venv-tok021 && venv-tok021/Scripts/pip install tokenizers==0.21.4
    python test/eval/ort_tokenizer_parity.py --old-python venv-tok021/Scripts/python.exe \\
        --model mmbert=~/.cache/lemonade-onnx-models/mmbert32k-pii-onnx --model pplx=...

--new-python defaults to this interpreter, whose `tokenizers` must be 0.22.2.
Writes <out-dir>/tokenizer_parity_<ts>.json and exits 1 on any mismatch.
"""

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
import time
from array import array
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ort_parity_common import (  # noqa: E402
    DEFAULT_CORPUS,
    load_cases,
    ort_encoder_input_ids,
    ort_max_length,
    ort_pad_id,
    parse_model_args,
    probe_texts,
)

EXPECTED = {"old": "0.21.4", "new": "0.22.2"}


def ids_digest(ids: list[int]) -> str:
    return hashlib.sha1(array("I", ids).tobytes()).hexdigest()


def run_worker(args) -> None:
    import tokenizers
    from tokenizers import Tokenizer

    texts = json.loads(Path(args.texts).read_text(encoding="utf-8"))
    model_dir = Path(args.model_dir)
    blob = (model_dir / "tokenizer.json").read_text(encoding="utf-8")
    tok = Tokenizer.from_str(blob)
    if args.layer == "new":
        tok.no_truncation()
    max_len = ort_max_length(model_dir)
    pad_id = ort_pad_id(json.loads(blob))

    out = {
        "tokenizers": tokenizers.__version__,
        "max_length": max_len,
        "pad_id": pad_id,
        "file_truncation": tok.truncation,
        "rows": {},
    }
    # One encode per text, never encode_batch: ort-server encodes a single
    # sequence, and a batch would pad BatchLongest tokenizers to the batch max.
    for name, text in texts.items():
        enc = tok.encode(text, add_special_tokens=True)
        ids = ort_encoder_input_ids(enc.ids, max_len, pad_id)
        row = {"n": len(ids), "raw_n": len(enc.ids), "sha": ids_digest(ids)}
        if args.full:
            row["ids"] = ids
        out["rows"][name] = row
    Path(args.out).write_text(json.dumps(out), encoding="utf-8")


def spawn(
    python: str,
    layer: str,
    model_dir: Path,
    texts_path: Path,
    out_path: Path,
    full: bool = False,
) -> dict:
    cmd = [
        python,
        __file__,
        "--worker",
        "--layer",
        layer,
        "--model-dir",
        str(model_dir),
        "--texts",
        str(texts_path),
        "--out",
        str(out_path),
    ]
    if full:
        cmd.append("--full")
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise SystemExit(f"{layer} worker failed on {model_dir}:\n{proc.stderr}")
    return json.loads(out_path.read_text(encoding="utf-8"))


def first_diff(a: list[int], b: list[int]) -> int:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b))


def parse_args():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--layer", choices=["old", "new"], help=argparse.SUPPRESS)
    p.add_argument("--model-dir", help=argparse.SUPPRESS)
    p.add_argument("--texts", help=argparse.SUPPRESS)
    p.add_argument("--out", help=argparse.SUPPRESS)
    p.add_argument("--full", action="store_true", help=argparse.SUPPRESS)

    p.add_argument("--old-python", help="Interpreter with tokenizers==0.21.4")
    p.add_argument(
        "--new-python",
        default=sys.executable,
        help="Interpreter with tokenizers==0.22.2 (default: this one)",
    )
    p.add_argument(
        "--model",
        action="append",
        default=[],
        metavar="NAME=DIR",
        help="Model directory to check; repeatable",
    )
    p.add_argument("--corpus-dir", type=Path, default=DEFAULT_CORPUS)
    p.add_argument("--no-probes", action="store_true", help="Skip the synthetic probes")
    p.add_argument(
        "--out-dir", type=Path, default=None, help="Default: <corpus-dir>/runs"
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    if args.worker:
        run_worker(args)
        return 0
    if not args.old_python or not args.model:
        raise SystemExit("--old-python and at least one --model are required")

    models = parse_model_args(args.model)
    texts = {c["name"]: c["text"] for c in load_cases(args.corpus_dir)}
    n_corpus = len(texts)
    if not args.no_probes:
        texts.update({f"probe:{name}": text for name, text in probe_texts()})

    out_dir = args.out_dir or args.corpus_dir / "runs"
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    report = {
        "corpus": str(args.corpus_dir),
        "corpus_cases": n_corpus,
        "probes": len(texts) - n_corpus,
        "models": {},
    }
    failed = False

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        texts_path = tmp / "texts.json"
        texts_path.write_text(json.dumps(texts), encoding="utf-8")
        for name, model_dir in models:
            t0 = time.time()
            old = spawn(args.old_python, "old", model_dir, texts_path, tmp / "old.json")
            new = spawn(args.new_python, "new", model_dir, texts_path, tmp / "new.json")
            for layer, res in (("old", old), ("new", new)):
                if res["tokenizers"] != EXPECTED[layer]:
                    print(
                        f"  WARNING: {layer} layer ran tokenizers {res['tokenizers']}, "
                        f"expected {EXPECTED[layer]}"
                    )

            mismatched = [
                n for n in texts if old["rows"][n]["sha"] != new["rows"][n]["sha"]
            ]
            truncated_old = sum(
                1 for n in texts if old["rows"][n]["raw_n"] < new["rows"][n]["raw_n"]
            )
            longest = max(texts, key=lambda n: new["rows"][n]["raw_n"])
            details = []
            if mismatched:
                subset = {n: texts[n] for n in mismatched}
                sub_path = tmp / "subset.json"
                sub_path.write_text(json.dumps(subset), encoding="utf-8")
                old_full = spawn(
                    args.old_python, "old", model_dir, sub_path, tmp / "of.json", True
                )
                new_full = spawn(
                    args.new_python, "new", model_dir, sub_path, tmp / "nf.json", True
                )
                for n in mismatched:
                    a, b = old_full["rows"][n]["ids"], new_full["rows"][n]["ids"]
                    i = first_diff(a, b)
                    details.append(
                        {
                            "case": n,
                            "old_len": len(a),
                            "new_len": len(b),
                            "first_diff_at": i,
                            "old_ids": a[max(0, i - 3) : i + 5],
                            "new_ids": b[max(0, i - 3) : i + 5],
                        }
                    )

            corpus_mismatch = [n for n in mismatched if not n.startswith("probe:")]
            probe_mismatch = [n for n in mismatched if n.startswith("probe:")]
            failed |= bool(corpus_mismatch)
            report["models"][name] = {
                "model_dir": str(model_dir),
                "tokenizers_old": old["tokenizers"],
                "tokenizers_new": new["tokenizers"],
                "max_length": new["max_length"],
                "pad_id": new["pad_id"],
                "file_truncation": old["file_truncation"],
                "identical": len(texts) - len(mismatched),
                "checked": len(texts),
                "corpus_mismatches": len(corpus_mismatch),
                "probe_mismatches": probe_mismatch,
                "inputs_cut_by_file_truncation": truncated_old,
                "longest_raw_tokens": {
                    "case": longest,
                    "tokens": new["rows"][longest]["raw_n"],
                },
                "mismatch_details": details,
                "seconds": round(time.time() - t0, 1),
            }
            r = report["models"][name]
            print(
                f"{name:12s} tokenizers {r['tokenizers_old']} -> {r['tokenizers_new']}  "
                f"max_length={r['max_length']}  file_truncation="
                f"{(r['file_truncation'] or {}).get('max_length')}"
            )
            print(
                f"{'':12s} identical input_ids: {r['identical']}/{r['checked']}  "
                f"(corpus mismatches {r['corpus_mismatches']}, probe mismatches "
                f"{len(probe_mismatch)}{': ' + ', '.join(probe_mismatch) if probe_mismatch else ''})"
            )
            print(
                f"{'':12s} longest input: {r['longest_raw_tokens']['tokens']} tokens "
                f"({r['longest_raw_tokens']['case']}); cut by tokenizer.json truncation (old layer only): "
                f"{truncated_old}  [{r['seconds']}s]"
            )
            for d in details[:5]:
                print(
                    f"{'':14s}{d['case']}: len {d['old_len']} -> {d['new_len']}, "
                    f"first diff at {d['first_diff_at']}: {d['old_ids']} vs {d['new_ids']}"
                )

    path = out_dir / f"tokenizer_parity_{stamp}.json"
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(
        f"\nRESULT: {'FAIL - corpus input_ids differ' if failed else 'PASS - corpus input_ids identical'}"
    )
    print(f"Report -> {path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
