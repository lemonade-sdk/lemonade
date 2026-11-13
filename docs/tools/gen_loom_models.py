"""Generate the HRX Loom rows of src/cpp/resources/server_models.json.

Every Hugging Face repository the ``loom-fun`` organization tags ``loom-serve``
becomes one ``recipe: "loom"`` row. A row's name, size and labels come from the
bundle itself: ``model.json`` names the GGUF origins, ``model.json.lock`` their
sizes, and ``info.json`` the features a serving gate verified.

Usage:
    python docs/tools/gen_loom_models.py [--check]

``--check`` exits non-zero when the committed rows differ from what the
organization publishes now, without modifying the file. Set ``HF_TOKEN`` to
raise the Hugging Face rate limit.
"""

import argparse
import json
import os
import sys
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "src" / "cpp" / "resources" / "server_models.json"
ORG = "loom-fun"
TAG = "loom-serve"
RECIPE = "loom"
HF = "https://huggingface.co"

FEATURE_LABELS = [
    ("image_input", "vision"),
    ("tool_calling", "tool-calling"),
    ("thinking", "reasoning"),
]
SPECULATIVE_LABELS = {"mtp": "mtp", "dflash": "dflash"}


def fetch_json(url: str):
    headers = {"User-Agent": "lemonade-gen-loom-models"}
    token = os.environ.get("HF_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    with urllib.request.urlopen(
        urllib.request.Request(url, headers=headers), timeout=60
    ) as response:
        return json.load(response)


def bundle_repos() -> list[str]:
    query = urllib.parse.urlencode({"author": ORG, "filter": TAG, "limit": 1000})
    return sorted(entry["id"] for entry in fetch_json(f"{HF}/api/models?{query}"))


def bundle_file(repo: str, name: str):
    return fetch_json(f"{HF}/{repo}/resolve/main/{name}")


def feature_on(features: dict, name: str) -> bool:
    feature = features.get(name) or {}
    return feature.get("status") == "yes"


def publisher(repo: str, card: dict) -> str:
    bundle = repo.split("/")[-1]
    for origin in (card.get("origins") or {}).values():
        owner = (origin or {}).get("repository", "").split("/")[0]
        if owner and bundle.startswith(owner + "-"):
            return owner
    return bundle.split("-")[0]


def base_name(card: dict, repo: str) -> str:
    # Bundles are named <GGUF publisher>-<model>-loom; the GGUF repository's own
    # name carries quant tags and publisher conventions a row should not.
    name = repo.split("/")[-1].removesuffix("-loom")
    return name.removeprefix(publisher(repo, card) + "-")


def chat_service(card: dict) -> bool:
    services = card.get("services") or {}
    return any(service.get("kind") == "text" for service in services.values())


def build_row(repo: str, card: dict, lock: dict, info: dict) -> dict:
    features = (info.get("card") or {}).get("features") or {}
    labels = ["chat"]
    labels += [label for key, label in FEATURE_LABELS if feature_on(features, key)]
    if "coder" in base_name(card, repo).lower():
        labels.append("coding")
    speculative = features.get("speculative_decoding") or {}
    if speculative.get("status") == "yes":
        method = (speculative.get("value") or {}).get("method", "")
        if method in SPECULATIVE_LABELS:
            labels.append(SPECULATIVE_LABELS[method])
    size_bytes = sum(entry.get("size", 0) for entry in lock.get("files", []))
    return {
        "checkpoint": repo,
        "recipe": RECIPE,
        "suggested": True,
        "labels": labels,
        "size": round(size_bytes / 1e9, 1),
    }


def generate_rows() -> dict:
    bundles = []
    for repo in bundle_repos():
        card = bundle_file(repo, "model.json")
        if not chat_service(card):
            print(f"skipping {repo}: no text service", file=sys.stderr)
            continue
        lock = bundle_file(repo, "model.json.lock")
        info = bundle_file(repo, "info.json")
        bundles.append((repo, card, build_row(repo, card, lock, info)))

    counts: dict[str, int] = {}
    for repo, card, _ in bundles:
        counts[base_name(card, repo)] = counts.get(base_name(card, repo), 0) + 1

    rows = {}
    for repo, card, row in bundles:
        name = base_name(card, repo)
        if counts[name] > 1:
            name = f"{name}-{publisher(repo, card)}"
        rows[f"{name}-Loom"] = row
    return dict(sorted(rows.items(), key=lambda item: item[0].lower()))


def merged_catalog(catalog: dict, rows: dict) -> dict:
    merged = {
        name: row
        for name, row in catalog.items()
        if not (isinstance(row, dict) and row.get("recipe") == RECIPE)
    }
    merged.update(rows)
    return merged


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    with open(CATALOG, encoding="utf-8", newline="") as stream:
        raw = stream.read()
    newline = "\r\n" if "\r\n" in raw else "\n"
    catalog = json.loads(raw)
    rows = generate_rows()
    text = json.dumps(merged_catalog(catalog, rows), indent=4, ensure_ascii=False)
    text = (text + "\n").replace("\n", newline)

    if args.check:
        if text != raw:
            print(
                f"{CATALOG.relative_to(ROOT)} is out of date; "
                "run python docs/tools/gen_loom_models.py",
                file=sys.stderr,
            )
            return 1
        print(f"{len(rows)} Loom rows are current")
        return 0

    with open(CATALOG, "w", encoding="utf-8", newline="") as stream:
        stream.write(text)
    print(f"wrote {len(rows)} Loom rows to {CATALOG.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
