#!/usr/bin/env python3
"""Translate the upstream AI Toolbox Cockpit catalog into Lemonade's registries.

Donato Capitella's cockpit (github.com/kyuz0/ai-toolbox-cockpit) curates the
container toolboxes for AMD Strix Halo and Radeon AI PRO R9700: which image runs
which engine, which device passthrough it needs, and which model files each
engine can actually load. That catalog is the best curation source that exists
for this hardware, so Lemonade translates it at build time rather than fetching
it at runtime.

Three jobs, one per subcommand:

  pins      Resolve every committed image pin's tag to the registry's current
            digest. ``--check`` fails when a pinned tag has disappeared upstream
            (the CI drift gate); ``--write`` moves the digests forward (the
            weekly refresh PR).

  models    Regenerate the DS4 and Halogen entries of server_models.json from
            the catalog. These are a deterministic mapping, so they are owned by
            this script and marked with ``catalog_source``. The ROCmFPX entries
            are a human's pick of one GGUF per model family and are left alone.

  report    Diff the catalog against what Lemonade ships: new or retired toolbox
            tags, new models, changed launch defaults, changed Halogen bundles.
            Prints a Markdown summary for a PR body. Never edits anything.

Usage:
    python docs/tools/gen_toolbox_catalog.py pins --check
    python docs/tools/gen_toolbox_catalog.py pins --write
    python docs/tools/gen_toolbox_catalog.py models --check
    python docs/tools/gen_toolbox_catalog.py report
"""

import argparse
import collections
import json
import os
import sys
import urllib.error
import urllib.request

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BACKEND_VERSIONS = os.path.join(
    REPO_ROOT, "src", "cpp", "resources", "backend_versions.json"
)
SERVER_MODELS = os.path.join(REPO_ROOT, "src", "cpp", "resources", "server_models.json")

CATALOG_BASE = "https://raw.githubusercontent.com/kyuz0/ai-toolbox-cockpit/main/ai_toolbox_cockpit/assets"

# Entries this script owns in server_models.json. Anything else in the file is
# hand-curated and never touched.
CATALOG_MARKER = "catalog_source"
CATALOG_MARKER_VALUE = "ai-toolbox-cockpit"

# Catalog artifact roles that are companion files rather than loadable models.
COMPANION_ROLES = {"dspark_support", "vision_encoder"}

# Lemonade model names for catalog entries. The catalog's ids encode the full
# quant recipe, which makes an unreadable name in the model list, so the display
# name is a human call. An id with no entry here falls back to a slug of its
# filename and shows up in `report` as a new model for someone to name.
NAME_OVERRIDES = {
    "ds4-deepseek-v4-flash-iq2xxs-w2q2k-aprojq8-sexpq8-outq8-chat-v2-imatrix-0731-gguf": "DeepSeek-V4-Flash-IQ2XXS-DS4",
    "ds4-deepseek-v4-flash-layers37-42q4kexperts-otherexpertlayersiq2xxsgateup-q2kdown-"
    "aprojq8-sexpq8-outq8-chat-v2-imatrix-fixed-0731-gguf": "DeepSeek-V4-Flash-Hybrid-Q2Q4-DS4",
    "ds4-deepseek-v4-flash-q4kexperts-f16hc-f16compressor-f16indexer-q8attn-q8shared-"
    "q8out-chat-v2-imatrix-0731-gguf": "DeepSeek-V4-Flash-Q4-DS4",
    "ds4-deepseek-v4-flash-mxfp4experts-f16hc-f16compressor-f16indexer-q8attn-q8shared-"
    "q8out-chat-v2-mxfp4-0731-gguf": "DeepSeek-V4-Flash-MXFP4-DS4",
    "ds4-deepseek-v4-flash-vision-exp-iq2xxs-w2q2k-aprojq8-sexpq8-outq8-gguf": "DeepSeek-V4-Flash-Vision-IQ2XXS-DS4",
    "ds4-deepseek-v4-flash-vision-exp-layers37-42q4kexperts-otherexpertlayersiq2xxsgateup-"
    "q2kdown-aprojq8-sexpq8-outq8-gguf": "DeepSeek-V4-Flash-Vision-Hybrid-Q2Q4-DS4",
    "ds4-deepseek-v4-flash-vision-exp-mxfp4experts-f16hc-f16compressor-f16indexer-q8attn-"
    "q8shared-q8out-gguf": "DeepSeek-V4-Flash-Vision-MXFP4-DS4",
    "ds4-glm-5-3-flash-q2-gguf": "GLM-5.3-Flash-Q2-DS4",
    "ds4-glm-5-3-flash-q4-k-gguf": "GLM-5.3-Flash-Q4_K-DS4",
    "ds4-deepseek-v4-1-flash-q2-gguf": "DeepSeek-V4.1-Flash-Q2-DS4",
    "qwen38-flash-next-w4b-quality": "Qwen3.8-Flash-Next-W4B-Halogen",
    "qwen38-flash-next-w4b-speed": "Qwen3.8-Flash-Next-W4B-Speed-Halogen",
    "qwen38-flash-next-w4b-quality-vision": "Qwen3.8-Flash-Next-W4B-Vision-Halogen",
    "qwen38-flash-next-w4b-speed-vision": "Qwen3.8-Flash-Next-W4B-Speed-Vision-Halogen",
}

USER_AGENT = {"User-Agent": "lemonade-toolbox-catalog/1.0"}
MANIFEST_ACCEPT = ", ".join(
    [
        "application/vnd.oci.image.index.v1+json",
        "application/vnd.oci.image.manifest.v1+json",
        "application/vnd.docker.distribution.manifest.list.v2+json",
        "application/vnd.docker.distribution.manifest.v2+json",
    ]
)


# --------------------------------------------------------------------------
# Registry access
# --------------------------------------------------------------------------


def _get(url, headers=None, timeout=60):
    request = urllib.request.Request(url, headers={**USER_AGENT, **(headers or {})})
    return urllib.request.urlopen(request, timeout=timeout)


def split_repository(repository):
    """Split "docker.io/kyuz0/x" into ("docker.io", "kyuz0/x")."""
    head, _, rest = repository.partition("/")
    if "." in head or ":" in head:
        return head, rest
    return "docker.io", repository


def registry_host(registry):
    return "registry-1.docker.io" if registry == "docker.io" else registry


def registry_token(registry, repo):
    if registry == "docker.io":
        url = (
            "https://auth.docker.io/token?service=registry.docker.io"
            f"&scope=repository:{repo}:pull"
        )
    elif registry == "ghcr.io":
        url = f"https://ghcr.io/token?scope=repository:{repo}:pull"
    else:
        return None
    with _get(url) as response:
        return json.load(response).get("token")


def resolve_digest(repository, tag):
    """Current manifest digest and compressed size for repository:tag.

    Returns (digest, compressed_bytes). Raises on a tag that no longer exists.
    """
    registry, repo = split_repository(repository)
    headers = {"Accept": MANIFEST_ACCEPT}
    token = registry_token(registry, repo)
    if token:
        headers["Authorization"] = "Bearer " + token
    url = f"https://{registry_host(registry)}/v2/{repo}/manifests/{tag}"
    with _get(url, headers) as response:
        digest = response.headers.get("Docker-Content-Digest")
        body = json.load(response)
    size = sum(layer.get("size", 0) for layer in body.get("layers", []))
    return digest, size


# --------------------------------------------------------------------------
# Catalog access
# --------------------------------------------------------------------------


def load_catalog(catalog_dir=None):
    """Return (toolboxes, models). Reads local files when catalog_dir is given."""
    documents = {}
    for name in ("toolboxes.json", "models.json"):
        if catalog_dir:
            with open(os.path.join(catalog_dir, name), encoding="utf-8") as handle:
                documents[name] = json.load(handle)
        else:
            with _get(f"{CATALOG_BASE}/{name}") as response:
                documents[name] = json.load(response)
    return documents["toolboxes.json"], documents["models.json"]


def load_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle, object_pairs_hook=collections.OrderedDict)


def write_json(path, data, indent):
    """Rewrite a registry file in the indentation it already uses."""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(data, indent=indent, ensure_ascii=False) + "\n")


def committed_pins(versions):
    """Yield (recipe, variant, arch, pin_dict) for every committed image pin."""
    for recipe, recipe_node in versions.items():
        if not isinstance(recipe_node, dict):
            continue
        for variant, variant_node in recipe_node.items():
            if not isinstance(variant_node, dict):
                continue
            if "repository" in variant_node:
                yield recipe, variant, "", variant_node
                continue
            for arch, pin in variant_node.items():
                if isinstance(pin, dict) and "repository" in pin:
                    yield recipe, variant, arch, pin


# --------------------------------------------------------------------------
# pins
# --------------------------------------------------------------------------


def command_pins(args):
    versions = load_json(BACKEND_VERSIONS)
    drift, missing = [], []

    for recipe, variant, arch, pin in committed_pins(versions):
        label = f"{recipe}:{variant}" + (f"/{arch}" if arch else "")
        try:
            digest, size = resolve_digest(pin["repository"], pin["tag"])
        except urllib.error.HTTPError as error:
            missing.append(
                f"{label} - {pin['repository']}:{pin['tag']} → HTTP {error.code}"
            )
            continue
        except Exception as error:  # network trouble is not drift
            print(f"WARN {label}: {error}", file=sys.stderr)
            continue

        if digest != pin["digest"]:
            drift.append((label, pin["digest"], digest))
            if args.write:
                pin["digest"] = digest
                pin["compressed_bytes"] = size

    for line in missing:
        print(f"GONE  {line}")
    for label, old, new in drift:
        print(f"DRIFT {label}\n        {old}\n     -> {new}")
    if not drift and not missing:
        print("All image pins match the registry.")

    if args.write and drift:
        write_json(BACKEND_VERSIONS, versions, indent=2)
        print(
            f"\nUpdated {len(drift)} pin(s) in {os.path.relpath(BACKEND_VERSIONS, REPO_ROOT)}"
        )

    if args.check:
        # A moved digest is expected: upstream rebuilds these images daily, and
        # the refresh workflow is what moves the pin. Only a tag that no longer
        # exists is a failure, because that pin can never be installed again.
        return 1 if missing else 0
    return 0


# --------------------------------------------------------------------------
# models
# --------------------------------------------------------------------------


def slugify(text):
    out = []
    for char in text:
        out.append(char if char.isalnum() or char in "-." else "-")
    slug = "".join(out)
    while "--" in slug:
        slug = slug.replace("--", "-")
    return slug.strip("-")


def ds4_entries(models):
    """DS4 model entries derived from the catalog, keyed by Lemonade model name."""
    entries = collections.OrderedDict()
    for model in models["backends"]["ds4"]["models"]:
        if model.get("artifact_role") in COMPANION_ROLES:
            continue
        filename = model["filename"]
        if ".gguf.part" in filename:
            # Split checkpoints need a concatenation step Lemonade does not have.
            continue

        base = filename[: -len(".gguf")] if filename.endswith(".gguf") else filename
        name = NAME_OVERRIDES.get(model["id"], slugify(base) + "-DS4")
        entry = collections.OrderedDict(
            [
                ("checkpoint", f"{model['repo']}:{filename}"),
                ("recipe", "ds4"),
                ("suggested", bool(model.get("recommended"))),
                ("labels", ["chat", "reasoning"]),
            ]
        )
        if "vision" in model.get("family", ""):
            entry["labels"] = ["chat", "reasoning", "vision"]
        entry["size"] = round(float(model.get("size_gb", 0.0)), 2)
        entry["min_resident_gb"] = 16.0

        defaults = model.get("server_defaults") or {}
        recipe_options = collections.OrderedDict()
        if defaults.get("standalone_ctx"):
            recipe_options["ctx_size"] = int(defaults["standalone_ctx"])
        extra_args = []
        if defaults.get("prefill_chunk"):
            extra_args += ["--prefill-chunk", str(int(defaults["prefill_chunk"]))]
        if defaults.get("dist_prefill_chunk"):
            extra_args += [
                "--dist-prefill-chunk",
                str(int(defaults["dist_prefill_chunk"])),
            ]
        if defaults.get("dist_prefill_window"):
            extra_args += [
                "--dist-prefill-window",
                str(int(defaults["dist_prefill_window"])),
            ]
        if extra_args:
            recipe_options["ds4_args"] = " ".join(extra_args)
        if recipe_options:
            entry["recipe_options"] = recipe_options

        entry[CATALOG_MARKER] = CATALOG_MARKER_VALUE
        entries[name] = entry
    return entries


def halogen_entries(models):
    entries = collections.OrderedDict()
    for model in models["backends"]["halogen"]["models"]:
        name = NAME_OVERRIDES.get(model["id"], "Halogen-" + slugify(model["id"]))
        files = {item["path"]: item["size_bytes"] for item in model.get("files", [])}
        total_gb = round(sum(files.values()) / 1e9, 2)

        entry = collections.OrderedDict(
            [
                ("checkpoint", f"{model['repo']}:{model['checkpoint']}"),
                ("recipe", "halogen"),
                ("suggested", bool(model.get("recommended"))),
                (
                    "labels",
                    ["chat", "reasoning"]
                    + (["vision"] if model.get("vision_tower") else []),
                ),
                ("size", total_gb),
                # The checkpoint is mapped from disk rather than copied, so the
                # 115 GiB file is not the memory requirement. What must be
                # resident is the weights the engine keeps in the device pool:
                # it measures 67.7 GiB for this bundle at startup and reports
                # exactly that before refusing to run. Recorded as the floor so
                # Lemonade's own size filter gives that answer up front instead
                # of starting a container that dies in two seconds.
                ("min_resident_gb", 68.0),
                ("halogen_overlay", model["overlay"]),
                ("halogen_tokenizer", model.get("tokenizer_dir", "tokenizer")),
                # Recorded for provenance: Lemonade's downloader tracks the repo's
                # default branch, so this is what the catalog validated against
                # rather than a pin Lemonade can enforce today.
                ("halogen_revision", model.get("revision", "")),
            ]
        )
        if model.get("vision_tower"):
            entry["halogen_vision_tower"] = model["vision_tower"]
        entry[CATALOG_MARKER] = CATALOG_MARKER_VALUE
        entries[name] = entry
    return entries


def command_models(args):
    _, models = load_catalog(args.catalog_dir)
    registry = load_json(SERVER_MODELS)

    generated = collections.OrderedDict()
    generated.update(ds4_entries(models))
    generated.update(halogen_entries(models))

    owned = {
        name
        for name, entry in registry.items()
        if isinstance(entry, dict) and entry.get(CATALOG_MARKER) == CATALOG_MARKER_VALUE
    }

    added = [name for name in generated if name not in registry]
    removed = sorted(owned - set(generated))
    changed = [
        name
        for name, entry in generated.items()
        if name in registry and registry[name] != entry
    ]

    for name in added:
        print(f"ADD     {name}")
    for name in changed:
        print(f"CHANGE  {name}")
    for name in removed:
        print(f"REMOVE  {name}")
    if not (added or changed or removed):
        print("server_models.json catalog entries are up to date.")

    if args.write and (added or changed or removed):
        for name in removed:
            registry.pop(name, None)
        registry.update(generated)
        # Keep the file sorted the way it already is: entries appended at the end.
        write_json(SERVER_MODELS, registry, indent=4)
        print(f"\nUpdated {os.path.relpath(SERVER_MODELS, REPO_ROOT)}")

    if args.check:
        return 1 if (added or changed or removed) else 0
    return 0


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------


def command_report(args):
    toolboxes, models = load_catalog(args.catalog_dir)
    versions = load_json(BACKEND_VERSIONS)

    pinned_images = {
        (pin["repository"], pin["tag"]) for _, _, _, pin in committed_pins(versions)
    }
    pinned_repos = {repository for repository, _ in pinned_images}

    # The platforms Lemonade supports out of this catalog.
    wanted_platforms = {"strix-halo", "r9700"}
    wanted_ids = set()
    for platform in toolboxes.get("platforms", []):
        if platform.get("id") in wanted_platforms:
            wanted_ids.update(platform.get("toolbox_ids", []))

    print("## Toolbox catalog diff\n")

    print("### Toolbox images upstream that Lemonade does not pin\n")
    unpinned = []
    for toolbox in toolboxes.get("toolboxes", []):
        if toolbox.get("id") not in wanted_ids:
            continue
        repository, _, tag = toolbox["image"].rpartition(":")
        if (repository, tag) in pinned_images:
            continue
        unpinned.append(
            f"- `{toolbox['id']}` - `{toolbox['image']}` "
            f"({toolbox.get('backend')}, {toolbox.get('maturity')}): {toolbox.get('description', '')}"
        )
    print("\n".join(unpinned) if unpinned else "_None._")

    print("\n### Pinned tags the catalog does not list\n")
    print(
        "Either the catalog retired the tag, or Lemonade pins it deliberately "
        "(the catalog tracks one DS4 image per platform, and its Halogen pin trails "
        "Peonist's releases). `pins --check` is what proves a tag still exists.\n"
    )
    catalog_images = {
        tuple(toolbox["image"].rsplit(":", 1))
        for toolbox in toolboxes.get("toolboxes", [])
        if ":" in toolbox["image"]
    }
    retired = [
        f"- `{repository}:{tag}`"
        for repository, tag in sorted(pinned_images)
        if repository in pinned_repos and (repository, tag) not in catalog_images
    ]
    print("\n".join(retired) if retired else "_None._")

    print("\n### Model entries this script would change\n")
    args_copy = argparse.Namespace(
        catalog_dir=args.catalog_dir, write=False, check=False
    )
    command_models(args_copy)

    print("\n### ROCmFPX-capable repositories in the catalog\n")
    print(
        "These need a human to pick one GGUF per family; this script does not generate them.\n"
    )
    rocmfpx = [
        f"- `{model['repo']}`"
        for model in models["backends"]["llama_cpp"]["models"]
        if "rocmfp" in model["repo"].lower() or "rocmi" in model["repo"].lower()
    ]
    print("\n".join(rocmfpx) if rocmfpx else "_None._")

    return 0


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--catalog-dir",
        help="Read toolboxes.json/models.json from this directory instead of GitHub",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    pins = subparsers.add_parser(
        "pins", help="Resolve committed image pins against the registry"
    )
    pins.add_argument(
        "--write", action="store_true", help="Move digests forward in place"
    )
    pins.add_argument(
        "--check", action="store_true", help="Exit non-zero if a pinned tag is gone"
    )
    pins.set_defaults(func=command_pins)

    models = subparsers.add_parser(
        "models", help="Regenerate DS4 and Halogen registry entries"
    )
    models.add_argument("--write", action="store_true", help="Write the entries")
    models.add_argument(
        "--check", action="store_true", help="Exit non-zero if entries are stale"
    )
    models.set_defaults(func=command_models)

    report = subparsers.add_parser(
        "report", help="Markdown diff of catalog vs what we ship"
    )
    report.set_defaults(func=command_report)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
