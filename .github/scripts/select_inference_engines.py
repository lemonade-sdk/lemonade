"""Select which inference-test matrix legs must run for a set of changed files.

Reads a newline-separated changed-file list from stdin and writes GitHub
Actions outputs (exe_matrix, deb_matrix, run_exe, run_deb, run_dmg, selected).

Classification is conservative: a file selects nothing only if it matches an
explicit safe rule, and any file that matches no rule at all selects every
engine. Engine-specific rules select just that engine's legs.
"""

import argparse
import fnmatch
import json
import sys

WORKFLOW_FILE = ".github/workflows/cpp_server_build_test_release.yml"
BACKENDS_DIR = "src/cpp/server/backends/"

SAFE_PATTERNS = [
    "docs/*",
    "*.md",
    "**/*.md",
    "mkdocs.yml",
    "examples/*",
    "src/app/*",
    "src/web-app/*",
    "LICENSE",
]

# .github/ paths that DO affect inference tests; any other .github/ file is safe.
GITHUB_UNSAFE_PATTERNS = [
    WORKFLOW_FILE,
    ".github/actions/*",
    ".github/inference-matrix.json",
    ".github/scripts/*",
]

BACKEND_DIR_TO_ENGINES = {
    "llamacpp": ["llamacpp", "omni"],
    "whispercpp": ["whisper"],
    "fastflowlm": ["flm", "flm-whisper"],
    "ryzenai": ["ryzenai"],
    "moonshine": ["moonshine"],
    "onnxruntime": ["classify-onnxruntime", "router-onnxruntime"],
    "sdcpp": ["stable-diffusion"],
    "thinksound": ["audio-gen-thinksound"],
    "acestep": ["audio-gen-acestep"],
    "kokoro": ["text-to-speech"],
    "trellis": ["3d-trellis"],
    "openmoss": ["tts-openmoss"],
    "cloud": ["router-onnxruntime"],
    "vllm": [],
}

TEST_SCRIPT_TO_ENGINES = {
    "test/server_llm.py": ["llamacpp", "ryzenai", "flm"],
    "test/server_omni.py": ["omni"],
    "test/server_whisper.py": ["whisper", "flm-whisper"],
    "test/server_moonshine.py": ["moonshine"],
    "test/server_classify.py": ["classify-onnxruntime"],
    "test/server_router.py": ["router-onnxruntime"],
    "test/server_sd.py": ["stable-diffusion"],
    "test/server_audio_generation.py": ["audio-gen-thinksound", "audio-gen-acestep"],
    "test/server_tts.py": ["text-to-speech"],
    "test/server_tts_openmoss.py": ["tts-openmoss"],
    "test/server_3d.py": ["3d-trellis"],
}

DMG_ENGINES = {"llamacpp", "omni", "whisper", "text-to-speech"}

ALL = "ALL"


def _matches_any(path, patterns):
    return any(fnmatch.fnmatch(path, p) for p in patterns)


def classify_file(path):
    """Return the engines a single changed file selects: [], a list, or ALL."""
    if path.startswith(".github/"):
        if _matches_any(path, GITHUB_UNSAFE_PATTERNS):
            return ALL
        return []
    if _matches_any(path, SAFE_PATTERNS):
        return []
    if path.startswith(BACKENDS_DIR):
        remainder = path[len(BACKENDS_DIR) :]
        if "/" not in remainder:
            return ALL
        backend_dir = remainder.split("/", 1)[0]
        if backend_dir in BACKEND_DIR_TO_ENGINES:
            return BACKEND_DIR_TO_ENGINES[backend_dir]
        return ALL
    if path in TEST_SCRIPT_TO_ENGINES:
        return TEST_SCRIPT_TO_ENGINES[path]
    return ALL


def select_engines(changed_files, event_name):
    if event_name != "pull_request" or not changed_files:
        return ALL
    selected = set()
    for path in changed_files:
        engines = classify_file(path)
        if engines is ALL:
            return ALL
        selected.update(engines)
    return selected


def build_outputs(selection, matrix):
    all_engines = {leg["name"] for legs in matrix.values() for leg in legs}
    if selection is ALL:
        selection = all_engines
    exe = [leg for leg in matrix["exe"] if leg["name"] in selection]
    deb = [leg for leg in matrix["deb"] if leg["name"] in selection]
    run_dmg = bool(selection & DMG_ENGINES)
    return {
        "exe_matrix": json.dumps(exe, separators=(",", ":")),
        "deb_matrix": json.dumps(deb, separators=(",", ":")),
        "run_exe": "true" if exe else "false",
        "run_deb": "true" if deb else "false",
        "run_dmg": "true" if run_dmg else "false",
        "selected": " ".join(sorted(selection)) if selection else "none",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix-file", required=True)
    parser.add_argument("--event-name", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    with open(args.matrix_file, encoding="utf-8") as f:
        matrix = json.load(f)

    changed_files = [line.strip() for line in sys.stdin if line.strip()]
    selection = select_engines(changed_files, args.event_name)
    outputs = build_outputs(selection, matrix)

    print(f"event: {args.event_name}, changed files: {len(changed_files)}")
    print(f"selected engines: {outputs['selected']}")
    with open(args.output, "a", encoding="utf-8") as f:
        for key, value in outputs.items():
            f.write(f"{key}={value}\n")


if __name__ == "__main__":
    main()
