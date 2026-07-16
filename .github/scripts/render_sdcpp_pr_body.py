#!/usr/bin/env python3
"""Render a stable-diffusion.cpp update PR body with inline validation images.

The validation workflow downloads artifacts named ``sdcpp-validation-*`` with
JSON summaries and PNGs. This script normalizes those PNGs into a small evidence
folder that is committed to the auto-update branch, then writes a PR body that
links to the committed images so GitHub renders them directly in the PR.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
from pathlib import Path
from typing import Any

DEFAULT_VALIDATED_LABELS = ("cpu", "vulkan", "rocm-stable")


def parse_csv(value: str | None) -> list[str]:
    if not value:
        return []
    return [item.strip() for item in value.split(",") if item.strip()]


def safe_slug(value: str) -> str:
    chars: list[str] = []
    for char in value.lower():
        if char.isalnum():
            chars.append(char)
        elif char in {"-", "_", "."}:
            chars.append(char)
        else:
            chars.append("-")
    return "".join(chars).strip("-")


def read_records(labels: list[str]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for label in labels:
        path = Path(f"sdcpp_validation_{label}.json")
        if not path.exists():
            continue
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, list):
            raise SystemExit(f"{path} must contain a JSON array")
        for item in data:
            if not isinstance(item, dict):
                raise SystemExit(f"{path} contains a non-object record")
            item.setdefault("label", label)
            item["artifact"] = f"sdcpp-validation-{label}"
            records.append(item)
    return records


def resolve_image_path(row: dict[str, Any]) -> Path | None:
    raw = row.get("image_path")
    if not raw:
        return None

    candidates = [Path(str(raw)), Path(str(raw).replace("\\", os.sep))]
    label = str(row.get("label", ""))
    filename = Path(str(raw).replace("\\", "/")).name
    if label and filename:
        candidates.append(Path(f"sdcpp-images-{label}") / filename)

    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def copy_images(records: list[dict[str, Any]], evidence_dir: Path) -> int:
    images_dir = evidence_dir / "images"
    images_dir.mkdir(parents=True, exist_ok=True)
    copied = 0
    seen_names: set[str] = set()

    for row in records:
        if not row.get("pass"):
            continue
        source = resolve_image_path(row)
        if source is None:
            continue

        label = safe_slug(str(row.get("label", "backend")))
        model = safe_slug(str(row.get("model", "model")))
        size = safe_slug(str(row.get("size", "size")))
        name = f"sdcpp-{label}-{model}-{size}.png"
        if name in seen_names:
            suffix = 2
            stem = name[:-4]
            while f"{stem}-{suffix}.png" in seen_names:
                suffix += 1
            name = f"{stem}-{suffix}.png"
        seen_names.add(name)

        dest = images_dir / name
        shutil.copyfile(source, dest)
        row["pr_image_path"] = dest.as_posix()
        copied += 1
    return copied


def seconds(row: dict[str, Any]) -> float:
    for key in ("request_elapsed_s", "elapsed_s"):
        try:
            return float(row.get(key, 0) or 0)
        except (TypeError, ValueError):
            continue
    return 0.0


def image_url(repository: str, image_ref: str, path: str) -> str:
    quoted_path = "/".join(
        part.replace("#", "%23").replace(" ", "%20") for part in path.split("/")
    )
    quoted_ref = image_ref.replace("#", "%23").replace(" ", "%20")
    return f"https://raw.githubusercontent.com/{repository}/{quoted_ref}/{quoted_path}"


def group_summary(
    records: list[dict[str, Any]], labels: list[str]
) -> dict[str, dict[str, Any]]:
    summary: dict[str, dict[str, Any]] = {}
    for label in labels:
        summary[label] = {"total": 0, "passed": 0, "elapsed": 0.0}
    for row in records:
        label = str(row.get("label", ""))
        item = summary.setdefault(label, {"total": 0, "passed": 0, "elapsed": 0.0})
        item["total"] += 1
        if row.get("pass"):
            item["passed"] += 1
        item["elapsed"] += seconds(row)
    return summary


def assert_expected_images(
    records: list[dict[str, Any]],
    labels: list[str],
    models: list[str],
    sizes: list[str],
) -> None:
    """Fail if any expected backend/model/size image is missing or failed."""
    if not models:
        raise SystemExit("No expected validation models were provided")
    if not sizes:
        raise SystemExit("No expected validation sizes were provided")

    rows: dict[tuple[str, str, str], dict[str, Any]] = {}
    for row in records:
        key = (
            str(row.get("label", "")),
            str(row.get("model", "")),
            str(row.get("size", "")),
        )
        rows[key] = row

    missing: list[str] = []
    failed: list[str] = []
    missing_image: list[str] = []
    for label in labels:
        for model in models:
            for size in sizes:
                key = (label, model, size)
                row = rows.get(key)
                item = f"{label} / {model} / {size}"
                if row is None:
                    missing.append(item)
                elif not row.get("pass"):
                    failed.append(f"{item}: {row.get('error', 'marked failed')}")
                elif not row.get("pr_image_path"):
                    missing_image.append(item)

    if missing or failed or missing_image:
        details: list[str] = []
        if missing:
            details.append("Missing validation records:\n  - " + "\n  - ".join(missing))
        if failed:
            details.append("Failed validation records:\n  - " + "\n  - ".join(failed))
        if missing_image:
            details.append(
                "Missing copied validation images:\n  - " + "\n  - ".join(missing_image)
            )
        raise SystemExit(
            "Expected validation image evidence is incomplete.\n" + "\n".join(details)
        )


def render_body(
    args: argparse.Namespace, records: list[dict[str, Any]], copied: int
) -> str:
    base_backends = parse_csv(args.base_update_backends)
    cuda_backends = parse_csv(args.cuda_update_backends)
    updated = base_backends + cuda_backends
    labels = parse_csv(args.validated_labels) or list(DEFAULT_VALIDATED_LABELS)
    unvalidated = [backend for backend in updated if backend not in labels]
    prompt = next((str(row.get("prompt")) for row in records if row.get("prompt")), "")
    summary = group_summary(records, labels)

    timing_scope = "request wall time"
    if any(row.get("warmup") for row in records):
        timing_scope = "timed request wall time after untimed per-model warm-up"

    lines = [
        "This updates Lemonade's existing `sd-cpp` backend pins using the appropriate release stream for each backend.",
        "",
        f"- Base sd-cpp release (`leejet/stable-diffusion.cpp`): `{args.base_release}`",
        f"- CUDA sd-cpp release (`lemonade-sdk/stable-diffusion.cpp`): `{args.cuda_release}`",
        f"- Updated base pins: `{', '.join(base_backends)}` -> `{args.base_release}`",
        f"- Updated CUDA pins: `{', '.join(cuda_backends)}` -> `{args.cuda_release}`",
        f"- Validated backends: `{', '.join(labels)}`",
        "- Updated but not validated here: "
        + (f"`{', '.join(unvalidated)}`" if unvalidated else "none"),
        f"- Timing: `{timing_scope}`; per-image timings are listed below.",
        "",
        "CUDA assets are resolved independently from the Lemonade fork. They do not need to share the same tag as the upstream leejet release.",
        "",
        "Validated with the same prompt and seed on each available runner:",
        "",
        f"- Prompt: `{prompt or args.prompt}`",
        f"- Seed / steps: `{args.seed}` / `{args.steps}`",
        f"- Models: `{args.models}`",
        f"- Sizes: `{args.sizes}`",
        "",
        "| Backend | Result | Total timed request | Evidence |",
        "|---|---:|---:|---|",
    ]

    for label in labels:
        item = summary.get(label, {"total": 0, "passed": 0, "elapsed": 0.0})
        result = (
            f"{item['passed']}/{item['total']} images" if item["total"] else "missing"
        )
        elapsed = f"{item['elapsed']:.1f}s" if item["total"] else "-"
        lines.append(f"| {label} | {result} | {elapsed} | `sdcpp-validation-{label}` |")

    if unvalidated:
        lines.append("")
        lines.append(
            "Unvalidated pins are intentionally updated but not exercised in this workflow run."
        )

    lines += [
        "",
        "## Per-image validation evidence",
        "",
        "| Backend | Model | Size | Time | Image |",
        "|---|---|---:|---:|---|",
    ]

    for row in sorted(
        records,
        key=lambda r: (
            str(r.get("label", "")),
            str(r.get("model", "")),
            str(r.get("size", "")),
        ),
    ):
        label = str(row.get("label", ""))
        model = str(row.get("model", ""))
        size = str(row.get("size", ""))
        elapsed = f"{seconds(row):.3f}s" if row.get("pass") else "failed"
        if row.get("pr_image_path"):
            src = image_url(args.repository, args.image_ref, str(row["pr_image_path"]))
            alt = f"{label} {model} {size}"
            image = f'<img src="{src}" alt="{alt}" width="220">'
        elif row.get("error"):
            image = f"`{row['error']}`"
        else:
            image = "missing image"
        lines.append(f"| {label} | `{model}` | `{size}` | {elapsed} | {image} |")

    lines += [
        "",
        f"Rendered `{copied}` committed validation image(s) in this PR body.",
        "CUDA is pinned from `lemonade-sdk/stable-diffusion.cpp` but CUDA validation remains disabled until a matching runner exists.",
        "Metal is pinned when updated, but not validated in this workflow because there is no macOS matrix leg.",
        "`sd-cpp.rocm-nightly` is not touched.",
    ]
    return "\n".join(lines) + "\n"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-release", required=True)
    parser.add_argument("--cuda-release", required=True)
    parser.add_argument("--base-update-backends", required=True)
    parser.add_argument("--cuda-update-backends", required=True)
    parser.add_argument(
        "--validated-labels", default=",".join(DEFAULT_VALIDATED_LABELS)
    )
    parser.add_argument(
        "--repository",
        default=os.environ.get("GITHUB_REPOSITORY", "lemonade-sdk/lemonade"),
    )
    parser.add_argument(
        "--image-ref",
        required=True,
        help="Branch or commit ref that will contain committed evidence images",
    )
    parser.add_argument("--evidence-dir", required=True)
    parser.add_argument("--output", default="pr_body.md")
    parser.add_argument("--prompt", default=os.environ.get("SDCPP_TEST_PROMPT", ""))
    parser.add_argument("--seed", default=os.environ.get("SDCPP_TEST_SEED", "12345"))
    parser.add_argument("--steps", default=os.environ.get("SDCPP_TEST_STEPS", "4"))
    parser.add_argument("--models", default="SD-Turbo-GGUF, Flux-2-Klein-4B")
    parser.add_argument("--sizes", default="512x256, 1024x1024")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    labels = parse_csv(args.validated_labels) or list(DEFAULT_VALIDATED_LABELS)
    records = read_records(labels)
    evidence_dir = Path(args.evidence_dir)
    copied = copy_images(records, evidence_dir)

    assert_expected_images(
        records, labels, parse_csv(args.models), parse_csv(args.sizes)
    )

    body = render_body(args, records, copied)
    Path(args.output).write_text(body, encoding="utf-8")
    print(f"Wrote {args.output} with {copied} inline image(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
