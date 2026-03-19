#!/usr/bin/env python3
"""
Show pipeline_tag and tags for HuggingFace models.

Strips quant/file specifiers (e.g. "org/model-GGUF:Q4_K_M" → "org/model-GGUF")
and deduplicates checkpoints when scanning model registries.

Tracks HF API rate limits via response headers and waits automatically.
Set HF_TOKEN env var for higher rate limits (1000 vs 500 req/5min).

Usage:
    # Single model
    python test/hf_model_tags.py HauhauCS/Qwen3.5-35B-A3B-Uncensored-HauhauCS-Aggressive

    # All checkpoints from server_models.json
    python test/hf_model_tags.py --all

    # Single recipe from server_models.json
    python test/hf_model_tags.py --llamacpp
    python test/hf_model_tags.py --sd-cpp
    python test/hf_model_tags.py --whispercpp
    python test/hf_model_tags.py --kokoro
    python test/hf_model_tags.py --ryzenai-llm
    python test/hf_model_tags.py --experience

    # Include user_models.json (from LEMONADE_CACHE_DIR or ~/.cache/lemonade/)
    python test/hf_model_tags.py --all --user
    python test/hf_model_tags.py --llamacpp --user
    python test/hf_model_tags.py --all --user --user-models-path /path/to/user_models.json

    # Tag summary per recipe (inclusive list of all tags seen for each recipe)
    python test/hf_model_tags.py --all --summary
    python test/hf_model_tags.py --llamacpp --summary

    # Full detection (file extensions, name patterns, classifyModel result)
    python test/hf_model_tags.py --detect some-org/some-model
    python test/hf_model_tags.py --llamacpp --detect

    # Combine freely
    python test/hf_model_tags.py --llamacpp --kokoro --user --summary --detect some-org/some-model
"""

import argparse
import json
import os
import re
import sys
import time
from collections import defaultdict
from pathlib import Path

import requests

MODELS_JSON = Path(__file__).resolve().parent.parent / "src/cpp/resources/server_models.json"

ALL_RECIPES = ["llamacpp", "sd-cpp", "whispercpp", "kokoro", "flm", "ryzenai-llm", "experience"]

# Format tags — file serialization formats
FORMAT_TAGS = {"gguf", "onnx", "safetensors", "bin", "flm", "q4nx"}

# Task/pipeline tags — what the model does (mirrors recipeCompatibility.ts)
TASK_TAGS = {
    # LLM
    "text-generation", "conversational", "text2text-generation", "image-text-to-text",
    # Image
    "text-to-image", "image-to-image", "image-to-video", "image-to-3d",
    "image-text-to-image", "image-text-to-video", "unconditional-image-generation",
    "image-segmentation", "object-detection", "depth-estimation", "mask-generation",
    "zero-shot-object-detection",
    # Audio
    "automatic-speech-recognition", "text-to-speech", "audio-text-to-text",
    "text-to-audio", "audio-to-audio", "voice-activity-detection",
    # Video
    "text-to-video", "text-to-3d", "video-to-video",
    # Embedding/reranking
    "sentence-similarity", "feature-extraction", "text-ranking",
    # Other NLP
    "fill-mask", "question-answering", "summarization", "translation",
    "text-classification", "token-classification", "zero-shot-classification",
    "table-question-answering",
}

# Library/framework tags
LIBRARY_TAGS = {
    "transformers", "transformers.js", "diffusers", "sentence-transformers",
    "onnxruntime", "pytorch", "tensorflow", "jax", "flax", "keras",
    "llama.cpp", "ctranslate2", "mlx", "vllm", "openvino", "coreml",
    "tensorrt", "tflite", "rust", "paddlepaddle", "spacy", "fastai",
    "flair", "adapter-transformers", "timm", "open_clip",
}

# Prefixed tags we filter into their own buckets
PREFIXED_CATEGORIES = ("license:", "arxiv:", "base_model:", "region:", "doi:", "dataset:")

# ---------------------------------------------------------------------------
# Mirrors recipeCompatibility.ts — TASK_RECIPE_MAP
# ---------------------------------------------------------------------------
TASK_RECIPE_MAP = [
    {
        "pipelineTags": ["text-to-image", "image-to-image"],
        "hfTags": ["stable-diffusion", "text-to-image", "diffusers", "image-generation", "image-editing"],
        "namePatterns": [re.compile(r"stable-diffusion", re.I), re.compile(r"\bflux\b", re.I), re.compile(r"\bsdxl\b", re.I)],
        "recipe": "sd-cpp",
        "modelType": "image",
        "label": "sd.cpp",
    },
    {
        "pipelineTags": ["automatic-speech-recognition"],
        "hfTags": ["whisper"],
        "namePatterns": [re.compile(r"whisper", re.I)],
        "recipe": "whispercpp",
        "modelType": "audio",
        "label": "whisper.cpp",
    },
    {
        "pipelineTags": ["text-to-speech", "text-to-audio"],
        "hfTags": ["tts", "kokoro"],
        "namePatterns": [re.compile(r"kokoro", re.I)],
        "recipe": "kokoro",
        "modelType": "tts",
        "label": "Kokoro",
    },
    {
        "pipelineTags": ["sentence-similarity", "feature-extraction"],
        "hfTags": ["sentence-transformers", "nomic-embed", "embedding", "embeddings"],
        "namePatterns": [re.compile(r"embed", re.I), re.compile(r"nomic", re.I)],
        "recipe": "llamacpp",
        "modelType": "embedding",
        "label": "llama.cpp",
    },
    {
        "pipelineTags": ["text-ranking"],
        "hfTags": ["reranker", "cross-encoder", "reranking"],
        "namePatterns": [re.compile(r"rerank", re.I)],
        "recipe": "llamacpp",
        "modelType": "reranking",
        "label": "llama.cpp",
    },
]

LLM_PIPELINE_TAGS = ["text-generation", "conversational", "text2text-generation", "image-text-to-text"]

RECIPE_FORMATS = {
    "llamacpp": ["gguf"],
    "sd-cpp": ["safetensors"],
    "whispercpp": ["bin"],
    "kokoro": ["onnx"],
    "flm": ["flm"],
    "ryzenai-llm": ["onnx"],
}

# Rate limit tracking
_rate_limit_remaining = None
_rate_limit_reset = None


# ---------------------------------------------------------------------------
# Detection logic — mirrors detectBackend() + classifyModel() from ModelManager.tsx
# ---------------------------------------------------------------------------

def scan_file_extensions(siblings: list[dict]) -> dict:
    """Scan siblings file list for format-relevant extensions."""
    files = [s.get("rfilename", "").lower() for s in siblings]
    return {
        "gguf": [f for f in files if f.endswith(".gguf")],
        "onnx": [f for f in files if f.endswith(".onnx") or f.endswith(".onnx_data")],
        "safetensors": [f for f in files if f.endswith(".safetensors")],
        "bin": [f for f in files if f.endswith(".bin")],
        "flm": [f for f in files if f.endswith(".flm")],
    }


def check_name_patterns(model_id: str) -> list[dict]:
    """Check model ID against TASK_RECIPE_MAP name patterns. Returns matching mappings."""
    id_lower = model_id.lower()
    matches = []
    for mapping in TASK_RECIPE_MAP:
        for pat in mapping["namePatterns"]:
            if pat.search(id_lower):
                matches.append({"recipe": mapping["recipe"], "label": mapping["label"],
                                "modelType": mapping["modelType"], "pattern": pat.pattern})
                break
    return matches


def has_required_format(recipe: str, tags: list[str], ext_scan: dict) -> bool:
    """Check format gate — tags first, file extension fallback."""
    formats = RECIPE_FORMATS.get(recipe)
    if not formats:
        return True
    for fmt in formats:
        if fmt in tags:
            return True
        if ext_scan.get(fmt):
            return True
    return False


def classify_model(model_id: str, pipeline_tag: str | None, tags: list[str], ext_scan: dict) -> dict:
    """
    Python port of classifyModel() from recipeCompatibility.ts.
    Returns {recipe, modelType, label, level, reason, source}.
    source indicates which pass matched: 'pipeline_tag', 'hf_tags', 'name_pattern',
    'format_fallback', or 'none'.
    """
    id_lower = model_id.lower()
    tag_set = set(tags)

    has_gguf = bool(ext_scan["gguf"]) or "gguf" in tag_set
    has_onnx = bool(ext_scan["onnx"]) or "onnx" in tag_set
    has_flm = bool(ext_scan["flm"]) or "flm" in tag_set
    has_bin = bool(ext_scan["bin"])

    # --- Pass 1: pipeline_tag ---
    if pipeline_tag:
        for m in TASK_RECIPE_MAP:
            if pipeline_tag in m["pipelineTags"] and has_required_format(m["recipe"], tags, ext_scan):
                return {"recipe": m["recipe"], "modelType": m["modelType"], "label": m["label"],
                        "level": "supported", "reason": f'pipeline_tag "{pipeline_tag}" → {m["label"]}',
                        "source": "pipeline_tag"}

        if pipeline_tag in LLM_PIPELINE_TAGS:
            if has_gguf:
                return {"recipe": "llamacpp", "modelType": "llm", "label": "llama.cpp",
                        "level": "supported", "reason": f'pipeline_tag "{pipeline_tag}" + GGUF',
                        "source": "pipeline_tag"}
            if has_onnx:
                return {"recipe": "ryzenai-llm", "modelType": "llm", "label": "RyzenAI",
                        "level": "likely", "reason": f'pipeline_tag "{pipeline_tag}" + ONNX',
                        "source": "pipeline_tag"}

        if pipeline_tag not in LLM_PIPELINE_TAGS:
            return {"recipe": "", "modelType": "unknown", "label": pipeline_tag,
                    "level": "incompatible", "reason": f'pipeline_tag "{pipeline_tag}" unsupported',
                    "source": "pipeline_tag"}

    # --- Pass 2: HF tags ---
    for m in TASK_RECIPE_MAP:
        if any(t in tag_set for t in m["hfTags"]) and has_required_format(m["recipe"], tags, ext_scan):
            matched = [t for t in m["hfTags"] if t in tag_set]
            return {"recipe": m["recipe"], "modelType": m["modelType"], "label": m["label"],
                    "level": "likely", "reason": f"hf_tags [{', '.join(matched)}] → {m['label']}",
                    "source": "hf_tags"}

    # --- Pass 3: Name patterns ---
    for m in TASK_RECIPE_MAP:
        for pat in m["namePatterns"]:
            if pat.search(id_lower) and has_required_format(m["recipe"], tags, ext_scan):
                return {"recipe": m["recipe"], "modelType": m["modelType"], "label": m["label"],
                        "level": "experimental", "reason": f"name /{pat.pattern}/ → {m['label']}",
                        "source": "name_pattern"}

    # --- Pass 4: Format-only fallbacks ---
    if has_flm or id_lower.startswith("fastflowlm/") or "flm" in tag_set:
        return {"recipe": "flm", "modelType": "llm", "label": "FastFlowLM",
                "level": "likely", "reason": "FLM files or tags",
                "source": "format_fallback"}

    if has_onnx:
        label = "RyzenAI"
        if "npu" in tag_set or "-ryzenai-npu" in id_lower:
            label = "RyzenAI NPU"
        elif "hybrid" in tag_set or "-ryzenai-hybrid" in id_lower:
            label = "RyzenAI Hybrid"
        elif "igpu" in tag_set:
            label = "RyzenAI iGPU"
        return {"recipe": "ryzenai-llm", "modelType": "llm", "label": label,
                "level": "likely", "reason": "ONNX files detected",
                "source": "format_fallback"}

    if has_gguf:
        return {"recipe": "llamacpp", "modelType": "llm", "label": "llama.cpp",
                "level": "experimental", "reason": "GGUF present, no task metadata",
                "source": "format_fallback"}

    return {"recipe": "", "modelType": "unknown", "label": "Unknown",
            "level": "incompatible", "reason": "No compatible format or metadata",
            "source": "none"}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def get_user_models_path(override: str = None) -> Path:
    if override:
        return Path(override)
    cache_dir = os.environ.get("LEMONADE_CACHE_DIR")
    if cache_dir:
        return Path(cache_dir) / "user_models.json"
    return Path.home() / ".cache" / "lemonade" / "user_models.json"


def strip_quant_specifier(checkpoint: str) -> str:
    """Strip quant/file specifiers after ':' (e.g. 'org/model-GGUF:Q4_K_M' → 'org/model-GGUF')."""
    return checkpoint.split(":")[0] if ":" in checkpoint else checkpoint


def classify_tags(meta: dict) -> dict:
    """Classify a model's tags into categories."""
    pipeline_tag = meta.get("pipeline_tag")
    tags = meta.get("tags", [])
    tag_set = set(tags)

    formats = sorted(FORMAT_TAGS & tag_set)
    tasks = sorted(TASK_TAGS & tag_set)
    libraries = sorted(LIBRARY_TAGS & tag_set)
    known = FORMAT_TAGS | TASK_TAGS | LIBRARY_TAGS
    other = [t for t in tags if t not in known and not t.startswith(PREFIXED_CATEGORIES)]

    return {
        "pipeline_tag": pipeline_tag,
        "formats": formats,
        "tasks": tasks,
        "libraries": libraries,
        "other": other,
    }


def _hf_get(url: str) -> requests.Response | None:
    """GET with rate limiting and HF_TOKEN."""
    global _rate_limit_remaining, _rate_limit_reset

    if _rate_limit_remaining is not None and _rate_limit_remaining <= 1:
        wait = (_rate_limit_reset or 60) + 1
        print(f"  [rate limited — waiting {wait}s]", flush=True)
        time.sleep(wait)

    headers = {}
    hf_token = os.environ.get("HF_TOKEN")
    if hf_token:
        headers["Authorization"] = f"Bearer {hf_token}"

    try:
        r = requests.get(url, headers=headers, timeout=15)

        if "X-RateLimit-Remaining" in r.headers:
            _rate_limit_remaining = int(r.headers["X-RateLimit-Remaining"])
        if "X-RateLimit-Reset" in r.headers:
            try:
                reset_time = int(r.headers["X-RateLimit-Reset"])
                _rate_limit_reset = max(0, reset_time - int(time.time()))
            except ValueError:
                pass

        if r.status_code == 429:
            retry_after = int(r.headers.get("Retry-After", 60))
            print(f"  [429 rate limited — waiting {retry_after}s]", flush=True)
            time.sleep(retry_after + 1)
            return _hf_get(url)

        return r
    except requests.RequestException as e:
        print(f"  ERROR: {e}")
        return None


def fetch_model_meta(model_id: str) -> dict | None:
    api_id = strip_quant_specifier(model_id)
    r = _hf_get(f"https://huggingface.co/api/models/{api_id}")
    if r is None:
        return None
    if r.status_code == 404:
        print(f"  NOT FOUND: {model_id}")
        return None
    try:
        r.raise_for_status()
    except requests.HTTPError as e:
        print(f"  ERROR fetching {model_id}: {e}")
        return None
    return r.json()


def fetch_siblings(model_id: str) -> list[dict] | None:
    """Fetch the file tree for a model (siblings list from model metadata)."""
    meta = fetch_model_meta(model_id)
    if not meta:
        return None
    return meta.get("siblings", [])


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def print_model(model_id: str, meta: dict, recipe: str = None, detect: bool = False):
    c = classify_tags(meta)

    prefix = f"[{recipe}] " if recipe else ""
    print(f"\n{prefix}{model_id}")
    print(f"  pipeline_tag: {c['pipeline_tag'] or '(none)'}")
    print(f"  formats:      {', '.join(c['formats']) if c['formats'] else '(none)'}")
    print(f"  tasks:        {', '.join(c['tasks']) if c['tasks'] else '(none)'}")
    print(f"  libraries:    {', '.join(c['libraries']) if c['libraries'] else '(none)'}")
    if c["other"]:
        print(f"  other:        {', '.join(c['other'])}")

    if detect:
        siblings = meta.get("siblings", [])
        ext_scan = scan_file_extensions(siblings)
        ext_summary = {fmt: len(files) for fmt, files in ext_scan.items() if files}
        print(f"  extensions:   {ext_summary if ext_summary else '(none)'}")

        name_matches = check_name_patterns(model_id)
        if name_matches:
            parts = [f"{m['recipe']} (/{m['pattern']}/)" for m in name_matches]
            print(f"  name match:   {', '.join(parts)}")
        else:
            print(f"  name match:   (none)")

        tags = meta.get("tags", [])
        result = classify_model(model_id, meta.get("pipeline_tag"), tags, ext_scan)
        source = result["source"]
        level = result["level"]
        print(f"  classify:     {result['recipe'] or '(none)'} / {result['modelType']}"
              f" [{level}] via {source} — {result['reason']}")


def print_summary(summary: dict):
    """Print inclusive tag summary per recipe."""
    print(f"\n{'=' * 60}")
    print("TAG SUMMARY BY RECIPE")
    print(f"{'=' * 60}")

    for recipe in sorted(summary.keys()):
        data = summary[recipe]
        count = data["count"]
        print(f"\n[{recipe}] ({count} model{'s' if count != 1 else ''})")

        for category in ("pipeline_tags", "formats", "tasks", "libraries", "other"):
            tags = sorted(data[category])
            if not tags:
                tags = ["(none)"]
            label = category.replace("_", " ").rjust(14)
            if data[f"{category}_none_count"] > 0 and tags[0] != "(none)":
                tags.insert(0, f"(none)×{data[f'{category}_none_count']}")
            print(f"  {label}: {', '.join(tags)}")


def load_checkpoints(registry: dict, recipe_filter: set | None) -> dict:
    """Load and deduplicate checkpoints from a model registry."""
    seen = {}
    for _name, entry in registry.items():
        recipe = entry.get("recipe", "?")
        if recipe_filter and recipe not in recipe_filter:
            continue
        cp = entry.get("checkpoint", "")
        cp_base = strip_quant_specifier(cp)
        if cp_base and cp_base not in seen:
            seen[cp_base] = recipe
    return seen


def main():
    parser = argparse.ArgumentParser(
        description="Show HF model pipeline_tag and tags",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("models", nargs="*", help="HuggingFace model IDs (org/name)")
    parser.add_argument("--all", action="store_true", help="All recipes from server_models.json")
    parser.add_argument("--user", action="store_true", help="Include user_models.json")
    parser.add_argument("--user-models-path", type=str, default=None,
                        help="Override user_models.json path (default: LEMONADE_CACHE_DIR or ~/.cache/lemonade/)")
    parser.add_argument("--summary", action="store_true", help="Print inclusive tag summary per recipe")
    parser.add_argument("--detect", action="store_true",
                        help="Show file extension scan, name pattern matches, and classifyModel result")

    # Per-recipe flags
    for recipe in ALL_RECIPES:
        parser.add_argument(f"--{recipe}", action="store_true", help=f"Only {recipe} models from server_models.json")

    args = parser.parse_args()

    # Determine which recipes are selected
    selected_recipes = {r for r in ALL_RECIPES if getattr(args, r.replace("-", "_"), False)}
    use_registry = args.all or bool(selected_recipes)
    recipe_filter = selected_recipes if selected_recipes else None  # None = all

    if not args.models and not use_registry and not args.user:
        parser.print_help()
        sys.exit(1)

    # Collect results for summary mode
    summary = defaultdict(lambda: {
        "count": 0,
        "pipeline_tags": set(), "pipeline_tags_none_count": 0,
        "formats": set(), "formats_none_count": 0,
        "tasks": set(), "tasks_none_count": 0,
        "libraries": set(), "libraries_none_count": 0,
        "other": set(), "other_none_count": 0,
    })

    def process_model(model_id: str, recipe: str = None):
        meta = fetch_model_meta(model_id)
        if not meta:
            return
        print_model(model_id, meta, recipe=recipe, detect=args.detect)

        if args.summary and recipe:
            c = classify_tags(meta)
            s = summary[recipe]
            s["count"] += 1
            if c["pipeline_tag"]:
                s["pipeline_tags"].add(c["pipeline_tag"])
            else:
                s["pipeline_tags_none_count"] += 1
            for cat in ("formats", "tasks", "libraries", "other"):
                if c[cat]:
                    s[cat].update(c[cat])
                else:
                    s[f"{cat}_none_count"] += 1

    # Named models first
    for model_id in args.models:
        process_model(model_id)

    # server_models.json checkpoints
    if use_registry:
        with open(MODELS_JSON) as f:
            registry = json.load(f)

        checkpoints = load_checkpoints(registry, recipe_filter)

        label = "server_models.json"
        if recipe_filter:
            label += f" [{', '.join(sorted(recipe_filter))}]"
        print(f"\n{'=' * 60}")
        print(f"{label}: {len(checkpoints)} unique checkpoints")
        print(f"{'=' * 60}")

        for checkpoint, recipe in sorted(checkpoints.items()):
            process_model(checkpoint, recipe=recipe)

    # user_models.json
    if args.user:
        user_path = get_user_models_path(args.user_models_path)
        if user_path.exists():
            with open(user_path) as f:
                user_registry = json.load(f)

            checkpoints = load_checkpoints(user_registry, recipe_filter)

            print(f"\n{'=' * 60}")
            print(f"user_models.json: {len(checkpoints)} unique checkpoints")
            print(f"{'=' * 60}")

            for checkpoint, recipe in sorted(checkpoints.items()):
                process_model(checkpoint, recipe=recipe)
        else:
            print(f"\n  user_models.json not found at {user_path}")

    # Print summary if requested
    if args.summary and summary:
        print_summary(summary)


if __name__ == "__main__":
    main()
