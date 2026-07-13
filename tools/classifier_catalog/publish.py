"""Publish the validated ONNX classifier catalog to the lemonade-sdk HF org.

For each model: (re-)export from source via export.py (clean provenance),
benchmark on CPU, write a model card (original-model link + post-export
validation data), include export.py, and upload to lemonade-sdk/<repo>.

    conda run -n lmxclf python tools/classifier_catalog/publish.py [--only <repo> ...] [--dry-run]
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from huggingface_hub import HfApi, snapshot_download

HERE = Path(__file__).parent
EXPORT = HERE / "export.py"
STAGING = HERE / "publish_staging"
ORG = "lemonade-sdk"

# Publication is allowlist-only and FAILS CLOSED: a model is uploaded only if
# the source repo's declared license matches `license` below at publish time
# (mirrors and exports alike), and — for from-source exports — only if parity
# vs the original PyTorch model passes.
#
# Evaluated and EXCLUDED — do not re-add without a redistribution grant:
#   iiiorg/piiranha-v1-detect-personal-information      cc-by-nc-nd-4.0 (ND: no derivatives)
#   facebook/roberta-hate-speech-dynabench-r4-target    no declared license
#   testsavantai/prompt-injection-defender-base-v0-onnx no declared license
MODELS = [
    dict(
        repo="bert-finetuned-phishing-ONNX",
        source="ealvaradob/bert-finetuned-phishing",
        task="text-classification",
        license="apache-2.0",
    ),
    dict(
        repo="phishing-email-detection-distilbert-ONNX",
        source="cybersectony/phishing-email-detection-distilbert_v2.4.1",
        task="text-classification",
        license="apache-2.0",
    ),
    dict(
        repo="Llama-Prompt-Guard-2-86M-ONNX",
        source="meta-llama/Llama-Prompt-Guard-2-86M",
        task="text-classification",
        license="llama4",
        llama_license=True,
    ),
]

MAX_SCORE_DELTA = 1e-4
MIN_TOKEN_AGREEMENT = 0.999


def resolve_source_license(api: HfApi, source: str) -> str:
    """Return the source repo's effective license id; abort if undeterminable."""
    try:
        card = api.model_info(source).card_data
    except Exception as e:  # noqa: BLE001
        sys.exit(f"REFUSING to publish: license lookup failed for {source}: {e}")
    license_id = card.get("license") if card else None
    if license_id == "other":
        license_id = card.get("license_name") if card else None
    if not license_id:
        sys.exit(f"REFUSING to publish: {source} declares no license")
    return license_id


def check_parity_gate(m: dict, parity: dict) -> None:
    if m.get("onnx_only"):
        return
    if m["task"] == "text-classification":
        delta = parity.get("max_score_delta")
        if delta is None or delta > MAX_SCORE_DELTA:
            sys.exit(
                f"REFUSING to publish {m['repo']}: parity max_score_delta={delta} "
                f"(limit {MAX_SCORE_DELTA})"
            )
    else:
        agreement = parity.get("token_label_agreement")
        if agreement is None or agreement < MIN_TOKEN_AGREEMENT:
            sys.exit(
                f"REFUSING to publish {m['repo']}: token_label_agreement={agreement} "
                f"(minimum {MIN_TOKEN_AGREEMENT})"
            )


def benchmark(model_dir: Path):
    import onnxruntime as ort
    from transformers import AutoTokenizer

    sess = ort.InferenceSession(
        str(next(model_dir.glob("*.onnx"))), providers=["CPUExecutionProvider"]
    )
    tok = AutoTokenizer.from_pretrained(model_dir)
    in_names = {i.name for i in sess.get_inputs()}
    enc = tok(
        "Please review the attached quarterly report before the meeting.",
        return_tensors="np",
        truncation=True,
    )
    feeds = {k: v for k, v in enc.items() if k in in_names}
    for _ in range(3):
        sess.run(None, feeds)
    ts = []
    for _ in range(50):
        t = time.perf_counter()
        sess.run(None, feeds)
        ts.append((time.perf_counter() - t) * 1000)
    ts.sort()
    return round(ts[len(ts) // 2], 2), int(next(iter(enc.values())).shape[-1])


def write_manifest_from_config(model_dir: Path, task: str):
    cfg = json.loads((model_dir / "config.json").read_text())
    id2label = {int(k): v for k, v in cfg["id2label"].items()}
    (model_dir / "manifest.json").write_text(
        json.dumps(
            {
                "task": task,
                "id2label": id2label,
                "score_normalization": "softmax",
                "token_aggregation": None if task == "text-classification" else "max",
            },
            indent=2,
        )
    )
    return id2label


def model_card(m, license_id, parity, p50_ms, seq_len, id2label) -> str:
    src = m["source"]
    fm = ["---"]
    if m.get("llama_license"):
        fm += ["license: other", "license_name: llama4", "license_link: LICENSE"]
    elif license_id:
        fm.append(f"license: {license_id}")
    fm += [
        f"base_model: {src}",
        "library_name: onnx",
        f"pipeline_tag: {'token-classification' if m['task']=='token-classification' else 'text-classification'}",
        "tags:",
        "  - onnx",
        "  - lemonade",
        "  - text-classification",
        "---",
        "",
    ]
    labels = ", ".join(f"`{v}`" for v in id2label.values())
    if m.get("onnx_only"):
        val = (
            f"The source repo [`{src}`](https://huggingface.co/{src}) ships ONNX only "
            "(no PyTorch weights), so this is a **mirror** of the author's ONNX. It is "
            "load- and inference-checked (produces valid label scores); there is no "
            "from-source parity comparison because there is no reference PyTorch model."
        )
    else:
        key = (
            "max_score_delta"
            if m["task"] == "text-classification"
            else "token_label_agreement"
        )
        v = parity.get(key)
        line = (
            f"max softmax delta **{v}** (0 = identical)"
            if key == "max_score_delta"
            else f"per-token label agreement **{v}** (1.0 = identical)"
        )
        val = (
            f"Exported from source with 🤗 Optimum and **validated against the original "
            f"PyTorch model** on fixtures (ONNX Runtime CPU vs HF): {line}."
        )
    if m.get("llama_license"):
        license_section = (
            f"**Built with Llama.** This is an ONNX derivative of [`{src}`]"
            f"(https://huggingface.co/{src}), licensed under the **Llama 4 Community "
            "License Agreement**, Copyright © Meta Platforms, Inc. All Rights Reserved. "
            "A copy of the license and the Acceptable Use Policy are included in this repo "
            "(`LICENSE`, `USE_POLICY`); your use is subject to those terms."
        )
    else:
        license_section = f"Follows the base model [`{src}`](https://huggingface.co/{src}); refer to it for terms."
    body = [
        f"# {m['repo']}",
        "",
        f"ONNX export of [`{src}`](https://huggingface.co/{src}), packaged for the "
        "[Lemonade](https://github.com/lemonade-sdk/lemonade) router classifier backend "
        "([`ort-server`](https://github.com/lemonade-sdk/ort-server)).",
        "",
        f"- **Base model:** [`{src}`](https://huggingface.co/{src})",
        f"- **Task:** {m['task']}",
        f"- **Labels:** {labels}",
        "",
        "## Files",
        "",
        "| file | purpose |",
        "|------|---------|",
        "| `model.onnx` | the exported model (`input_ids`/`attention_mask` → logits) |",
        "| `tokenizer.json` | the original HuggingFace tokenizer |",
        "| `manifest.json` | task / labels / normalization for ort-server |",
        "| `export.py` | the exact script used to produce & validate these files |",
        "",
        "## Validation after export",
        "",
        val,
        "",
        f"CPU-EP latency (ONNX Runtime, single input): **~{p50_ms} ms** p50 @ {seq_len} tokens.",
        "",
        "## Reproduce",
        "",
        "```bash",
        'pip install "optimum[onnxruntime]" transformers torch onnxruntime sentencepiece',
        f"python export.py {src} ./out --task {m['task']}",
        "```",
        "",
        "See `validation.json` for the recorded parity result.",
        "",
        "## License",
        "",
        license_section,
    ]
    return "\n".join(fm) + "\n".join(body) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    api = HfApi()
    STAGING.mkdir(parents=True, exist_ok=True)

    for m in MODELS:
        if args.only and m["repo"] not in args.only:
            continue
        print(f"\n=== {m['repo']} ===", flush=True)

        license_id = resolve_source_license(api, m["source"])
        if license_id != m["license"]:
            sys.exit(
                f"REFUSING to publish {m['repo']}: source license {license_id!r} "
                f"!= expected {m['license']!r}"
            )

        d = STAGING / m["repo"]
        d.mkdir(parents=True, exist_ok=True)
        parity = {}

        if m.get("onnx_only"):
            snapshot_download(
                m["source"],
                local_dir=str(d),
                repo_type="model",
                allow_patterns=["*.onnx", "*.json", "*.txt", "*.model"],
            )
            id2label = write_manifest_from_config(d, m["task"])
        else:
            subprocess.run(
                [sys.executable, str(EXPORT), m["source"], str(d), "--task", m["task"]],
                check=True,
            )
            parity = json.loads((d / "validation.json").read_text())
            check_parity_gate(m, parity)
            id2label = json.loads((d / "manifest.json").read_text())["id2label"]
            id2label = {int(k): v for k, v in id2label.items()}

        (d / "export.py").write_text(
            EXPORT.read_text(encoding="utf-8"), encoding="utf-8"
        )
        # Llama community license requires redistributing the LICENSE + Acceptable
        # Use Policy alongside derivatives; refuse to publish without them.
        if m.get("llama_license"):
            snapshot_download(
                m["source"],
                local_dir=str(d),
                repo_type="model",
                allow_patterns=["LICENSE*", "USE_POLICY*"],
            )
            if not (list(d.glob("LICENSE*")) and list(d.glob("USE_POLICY*"))):
                sys.exit(
                    f"REFUSING to publish {m['repo']}: LICENSE/USE_POLICY "
                    "not present in source repo"
                )
        p50, seq_len = benchmark(d)

        (d / "README.md").write_text(
            model_card(m, license_id, parity, p50, seq_len, id2label), encoding="utf-8"
        )

        print(f"parity={parity or 'n/a (onnx-only)'} p50={p50}ms license={license_id}")
        if args.dry_run:
            print("dry-run: not uploading")
            continue

        repo_id = f"{ORG}/{m['repo']}"
        api.create_repo(repo_id, repo_type="model", exist_ok=True)
        api.upload_folder(
            repo_id=repo_id,
            folder_path=str(d),
            commit_message="Publish ONNX export for Lemonade ort-server",
        )
        print(f"published https://huggingface.co/{repo_id}", flush=True)


if __name__ == "__main__":
    main()
