"""
Registry invariant: every model in server_models.json declares a deployment mode
its backend can actually serve.

A model's deployment mode (which backend endpoint it is routed to) comes from a
single label. Nothing in the C++ ingest path can recover a missing one for a
built-in model, and a model without it silently disappears from the chat model
pickers, so it is checked here instead.

The mode must also be one the recipe's backend serves. `supported_modes` in each
backend descriptor is the same list the C++ registration path validates against;
BackendModeContractTest proves that list matches the capability interfaces the
backend implements.

Run standalone: python test/test_server_models_labels.py
"""

import json
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SERVER_MODELS = REPO_ROOT / "src" / "cpp" / "resources" / "server_models.json"
BACKENDS_DIR = REPO_ROOT / "src" / "cpp" / "include" / "lemon" / "backends"
MODEL_TYPES_H = REPO_ROOT / "src" / "cpp" / "include" / "lemon" / "model_types.h"

# Mirrors find_deployment_mode() in src/cpp/include/lemon/model_types.h. Labels
# that map to the same mode are interchangeable there and here.
DEPLOYMENT_LABELS = {
    "chat": "LLM",
    "embeddings": "EMBEDDING",
    "embedding": "EMBEDDING",
    "reranking": "RERANKING",
    "transcription": "TRANSCRIPTION",
    "image": "IMAGE",
    "tts": "TTS",
    "audio-generation": "AUDIO_GENERATION",
    "classification": "CLASSIFICATION",
    "classifier": "CLASSIFICATION",
    "3d": "MESH",
}

RECIPE_FIELD = re.compile(r"/\*recipe\*/\s*\"([^\"]*)\"")
SUPPORTED_MODES_FIELD = re.compile(r"/\*supported_modes\*/\s*\{([^}]*)\}")


def load_recipe_modes():
    """Map each recipe to the deployment modes its backend descriptor declares."""
    recipe_modes = {}
    for header in sorted(BACKENDS_DIR.glob("*/*.h")):
        text = header.read_text(encoding="utf-8")
        recipe = RECIPE_FIELD.search(text)
        modes = SUPPORTED_MODES_FIELD.search(text)
        if recipe and modes:
            recipe_modes[recipe.group(1)] = re.findall(r"\"([^\"]+)\"", modes.group(1))
    return recipe_modes


def check_label_table_is_current():
    """The C++ classifier must not grow a deployment label this script misses."""
    source = MODEL_TYPES_H.read_text(encoding="utf-8")
    body = source.split("inline bool find_deployment_mode")[1].split("\n}")[0]
    declared = set(re.findall(r"label == \"([^\"]+)\"", body))
    unknown = declared - set(DEPLOYMENT_LABELS)
    if unknown:
        return [
            "model_types.h names deployment labels this script does not know "
            f"about: {sorted(unknown)}. Add them to DEPLOYMENT_LABELS."
        ]
    return []


def check_models(models, recipe_modes):
    errors = []
    for name, entry in sorted(models.items()):
        labels = entry.get("labels", [])
        recipe = entry.get("recipe", "")
        declared = [label for label in labels if label in DEPLOYMENT_LABELS]
        modes = {DEPLOYMENT_LABELS[label] for label in declared}

        if not modes:
            supported = recipe_modes.get(recipe) or ["chat"]
            errors.append(
                f"{name}: no modality label. Recipe '{recipe}' deploys as "
                f"'{supported[0]}' — add it to the model's \"labels\" array."
            )
            continue

        if len(modes) > 1:
            errors.append(
                f"{name}: labels name more than one modality ({sorted(modes)}); "
                "a model deploys in exactly one mode."
            )
            continue

        # An illegal mode claim is refused rather than repaired, so a built-in
        # carrying one is skipped at startup and vanishes from the catalog.
        supported = recipe_modes.get(recipe)
        if supported is None:
            continue
        unservable = [label for label in declared if label not in supported]
        if unservable:
            errors.append(
                f"{name}: recipe '{recipe}' cannot serve {sorted(unservable)}; "
                f"it supports {supported}."
            )
    return errors


def main():
    models = json.loads(SERVER_MODELS.read_text(encoding="utf-8"))
    recipe_modes = load_recipe_modes()
    if not recipe_modes:
        print(f"FAIL: no backend descriptors found under {BACKENDS_DIR}")
        return 1

    errors = check_label_table_is_current()
    errors += check_models(models, recipe_modes)

    if errors:
        print(f"FAIL: {len(errors)} problem(s) in {SERVER_MODELS.name}\n")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(f"PASS: all {len(models)} models in {SERVER_MODELS.name} declare a modality")
    return 0


if __name__ == "__main__":
    sys.exit(main())
