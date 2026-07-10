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

MODELS = [
    dict(repo="bert-finetuned-phishing-ONNX",
         source="ealvaradob/bert-finetuned-phishing", task="text-classification"),
    dict(repo="roberta-hate-speech-dynabench-r4-ONNX",
         source="facebook/roberta-hate-speech-dynabench-r4-target", task="text-classification"),
    dict(repo="phishing-email-detection-distilbert-ONNX",
         source="cybersectony/phishing-email-detection-distilbert_v2.4.1", task="text-classification"),
    dict(repo="piiranha-v1-detect-personal-information-ONNX",
         source="iiiorg/piiranha-v1-detect-personal-information", task="token-classification"),
    dict(repo="prompt-injection-defender-base-v0-ONNX",
         source="testsavantai/prompt-injection-defender-base-v0-onnx",
         task="text-classification", onnx_only=True),
]


def benchmark(model_dir: Path):
    import onnxruntime as ort
    from transformers import AutoTokenizer
    sess = ort.InferenceSession(str(next(model_dir.glob("*.onnx"))), providers=["CPUExecutionProvider"])
    tok = AutoTokenizer.from_pretrained(model_dir)
    in_names = {i.name for i in sess.get_inputs()}
    enc = tok("Please review the attached quarterly report before the meeting.",
              return_tensors="np", truncation=True)
    feeds = {k: v for k, v in enc.items() if k in in_names}
    for _ in range(3):
        sess.run(None, feeds)
    ts = []
    for _ in range(50):
        t = time.perf_counter(); sess.run(None, feeds); ts.append((time.perf_counter() - t) * 1000)
    ts.sort()
    return round(ts[len(ts) // 2], 2), int(next(iter(enc.values())).shape[-1])


def write_manifest_from_config(model_dir: Path, task: str):
    cfg = json.loads((model_dir / "config.json").read_text())
    id2label = {int(k): v for k, v in cfg["id2label"].items()}
    (model_dir / "manifest.json").write_text(json.dumps({
        "task": task, "id2label": id2label, "score_normalization": "softmax",
        "token_aggregation": None if task == "text-classification" else "max",
    }, indent=2))
    return id2label


def model_card(m, license_id, parity, p50_ms, seq_len, id2label) -> str:
    src = m["source"]
    fm = ["---"]
    if license_id:
        fm.append(f"license: {license_id}")
    fm += [
        f"base_model: {src}",
        "library_name: onnx",
        f"pipeline_tag: {'token-classification' if m['task']=='token-classification' else 'text-classification'}",
        "tags:", "  - onnx", "  - lemonade", "  - text-classification", "---", "",
    ]
    labels = ", ".join(f"`{v}`" for v in id2label.values())
    if m.get("onnx_only"):
        val = (f"The source repo [`{src}`](https://huggingface.co/{src}) ships ONNX only "
               "(no PyTorch weights), so this is a **mirror** of the author's ONNX. It is "
               "load- and inference-checked (produces valid label scores); there is no "
               "from-source parity comparison because there is no reference PyTorch model.")
    else:
        key = "max_score_delta" if m["task"] == "text-classification" else "token_label_agreement"
        v = parity.get(key)
        line = (f"max softmax delta **{v}** (0 = identical)" if key == "max_score_delta"
                else f"per-token label agreement **{v}** (1.0 = identical)")
        val = (f"Exported from source with 🤗 Optimum and **validated against the original "
               f"PyTorch model** on fixtures (ONNX Runtime CPU vs HF): {line}.")
    body = [
        f"# {m['repo']}", "",
        f"ONNX export of [`{src}`](https://huggingface.co/{src}), packaged for the "
        "[Lemonade](https://github.com/lemonade-sdk/lemonade) router classifier backend "
        "([`ort-server`](https://github.com/lemonade-sdk/ort-server)).", "",
        f"- **Base model:** [`{src}`](https://huggingface.co/{src})",
        f"- **Task:** {m['task']}",
        f"- **Labels:** {labels}", "",
        "## Files", "",
        "| file | purpose |",
        "|------|---------|",
        "| `model.onnx` | the exported model (`input_ids`/`attention_mask` → logits) |",
        "| `tokenizer.json` | the original HuggingFace tokenizer |",
        "| `manifest.json` | task / labels / normalization for ort-server |",
        "| `export.py` | the exact script used to produce & validate these files |", "",
        "## Validation after export", "", val, "",
        f"CPU-EP latency (ONNX Runtime, single input): **~{p50_ms} ms** p50 @ {seq_len} tokens.", "",
        "## Reproduce", "",
        "```bash",
        'pip install "optimum[onnxruntime]" transformers torch onnxruntime sentencepiece',
        f"python export.py {src} ./out --task {m['task']}",
        "```", "",
        "See `validation.json` for the recorded parity result.", "",
        "## License", "",
        f"Follows the base model [`{src}`](https://huggingface.co/{src}); refer to it for terms.",
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
        d = STAGING / m["repo"]
        d.mkdir(parents=True, exist_ok=True)
        parity = {}

        if m.get("onnx_only"):
            snapshot_download(m["source"], local_dir=str(d), repo_type="model",
                              allow_patterns=["*.onnx", "*.json", "*.txt", "*.model"])
            id2label = write_manifest_from_config(d, m["task"])
        else:
            subprocess.run([sys.executable, str(EXPORT), m["source"], str(d),
                            "--task", m["task"]], check=True)
            parity = json.loads((d / "validation.json").read_text())
            id2label = json.loads((d / "manifest.json").read_text())["id2label"]
            id2label = {int(k): v for k, v in id2label.items()}

        (d / "export.py").write_text(EXPORT.read_text(encoding="utf-8"), encoding="utf-8")
        p50, seq_len = benchmark(d)

        license_id = None
        try:
            info = api.model_info(m["source"])
            license_id = (info.card_data.get("license") if info.card_data else None)
        except Exception as e:  # noqa: BLE001
            print("license lookup failed:", e)

        (d / "README.md").write_text(
            model_card(m, license_id, parity, p50, seq_len, id2label), encoding="utf-8")

        print(f"parity={parity or 'n/a (onnx-only)'} p50={p50}ms license={license_id}")
        if args.dry_run:
            print("dry-run: not uploading")
            continue

        repo_id = f"{ORG}/{m['repo']}"
        api.create_repo(repo_id, repo_type="model", exist_ok=True)
        api.upload_folder(repo_id=repo_id, folder_path=str(d),
                          commit_message="Publish ONNX export for Lemonade ort-server")
        print(f"published https://huggingface.co/{repo_id}", flush=True)


if __name__ == "__main__":
    main()
