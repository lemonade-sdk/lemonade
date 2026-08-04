"""Select which inference-test matrix legs must run for a set of changed files.

Reads a newline-separated changed-file list from stdin and writes GitHub
Actions outputs (exe_matrix, deb_matrix, run_exe, run_deb, run_dmg, selected).

Classification is conservative: a file selects nothing only if it matches an
explicit safe rule, and any file that matches no rule at all selects every
engine — including under .github/. Engine-specific rules select just that
engine's legs.

This runs from the PR head, so a PR that edits this script also changes its own
gating. That is accepted: fork PRs get a read-only token here, and the repo
requires review, so a self-skipping rewrite has to get past a human either way.
"""

import argparse
import fnmatch
import json
import sys

WORKFLOW_FILE = ".github/workflows/cpp_server_build_test_release.yml"

# A backend is a folder of sources plus the matching folder of headers.
BACKEND_DIRS = (
    "src/cpp/server/backends/",
    "src/cpp/include/lemon/backends/",
)

# Workflows that define or guard the inference legs. Every other workflow is
# safe; test_workflows_that_touch_inference_are_listed keeps this honest.
INFERENCE_WORKFLOWS = frozenset(
    [
        WORKFLOW_FILE,
        ".github/workflows/routing_schema_tests.yml",
    ]
)

TEST_DIR = "test/"

# fnmatch's "*" spans "/", so these match at any depth.
SAFE_PATTERNS = [
    "docs/*",
    "*.md",
    "mkdocs.yml",
    "examples/*",
    "src/app/*",
    "src/web-app/*",
    "src/cpp/tray/*",
    "test/app/*",
    "test/cpp/*",
    ".pre-commit-config.yaml",
    "LICENSE",
]

# Everything under .github/ that cannot reach an inference job. Anything else
# there — the matrix, composite actions, this script — is unrecognized and
# therefore selects every engine.
GITHUB_SAFE_PATTERNS = [
    ".github/workflows/*",
    ".github/ISSUE_TEMPLATE/*",
    ".github/CODEOWNERS",
    ".github/dependabot.yml",
    ".github/runners.json",
    ".github/*.md",
]

BACKEND_DIR_TO_ENGINES = {
    # server_router.py loads a GGUF embedding model through llamacpp to back
    # the semantic_similarity classifier, so llamacpp reaches the router leg.
    "llamacpp": ["llamacpp", "omni", "router-onnxruntime"],
    "whispercpp": ["whisper", "omni"],
    "fastflowlm": ["flm", "flm-whisper"],
    "ryzenai": ["ryzenai"],
    "moonshine": ["moonshine"],
    "onnxruntime": ["classify-onnxruntime", "router-onnxruntime"],
    # The omni collection bundles an llamacpp planner with SD-Turbo, Whisper,
    # and kokoro components, so each of those reaches the omni leg too. That
    # component list lives in the collection manifest on Hugging Face, not in
    # this repo, so nothing here catches it being re-composed.
    "sdcpp": ["stable-diffusion", "omni"],
    "thinksound": ["audio-gen-thinksound"],
    "acestep": ["audio-gen-acestep"],
    "kokoro": ["text-to-speech", "omni"],
    "trellis": ["3d-trellis"],
    "openmoss": ["tts-openmoss"],
    "cloud": ["router-onnxruntime"],
    "vllm": [],
}

DMG_ENGINES = {"llamacpp", "whisper", "text-to-speech"}

# Sentinel, not a string, so a rule value can never be mistaken for it.
ALL = object()


def derive_script_rules(matrix):
    """Map each test script to the legs that run it, read off the matrix.

    Every leg names the script it runs, so this direction of the mapping is
    not a judgement call and is never hand-maintained.
    """
    rules = {}
    for legs in matrix.values():
        for leg in legs:
            rules.setdefault(f"test/{leg['script']}", set()).add(leg["name"])
    return rules


def _matches_any(path, patterns):
    # fnmatchcase, not fnmatch: fnmatch normcases the path, so on macOS a local
    # run would classify "DOCS/x.md" as safe while the Linux runner would not.
    return any(fnmatch.fnmatchcase(path, p) for p in patterns)


def _is_non_leg_test_script(path, script_rules):
    """True for a test script at the top of test/ that no matrix leg runs.

    Those belong to the always-on CLI/endpoints jobs. Deriving this from the
    matrix rather than listing the scripts means one that later becomes a
    leg's script is picked up by script_rules first, on its own.
    """
    if not path.startswith(TEST_DIR) or path in script_rules:
        return False
    name = path[len(TEST_DIR) :]
    return (
        "/" not in name
        and name.endswith(".py")
        and name.startswith(("server_", "test_"))
    )


def classify_file(path, script_rules):
    """Return the engines a single changed file selects: no engines, or ALL."""
    if path in INFERENCE_WORKFLOWS:
        return ALL
    if path in script_rules:
        return script_rules[path]
    if path.startswith(".github/"):
        return [] if _matches_any(path, GITHUB_SAFE_PATTERNS) else ALL
    if _matches_any(path, SAFE_PATTERNS) or _is_non_leg_test_script(path, script_rules):
        return []
    for prefix in BACKEND_DIRS:
        if not path.startswith(prefix):
            continue
        remainder = path[len(prefix) :]
        if "/" not in remainder:
            return ALL
        backend_dir = remainder.split("/", 1)[0]
        return BACKEND_DIR_TO_ENGINES.get(backend_dir, ALL)
    return ALL


def select_engines(changed_files, event_name, script_rules):
    if event_name != "pull_request" or not changed_files:
        return ALL
    selected = set()
    for path in changed_files:
        engines = classify_file(path, script_rules)
        if engines is ALL:
            return ALL
        selected.update(engines)
    return selected


def validate_mapping(matrix):
    """Raise if BACKEND_DIR_TO_ENGINES and the matrix have drifted apart.

    Runs on every invocation, not just under test, because both failure modes
    are silent: a rule naming a leg that no longer exists selects nothing, and
    a leg no backend folder names stops running when its backend changes.
    Failing here fails the aggregate required check.
    """
    all_engines = {leg["name"] for legs in matrix.values() for leg in legs}
    by_backend = set()
    for engines in BACKEND_DIR_TO_ENGINES.values():
        by_backend.update(engines)

    problems = set()
    # `selected` is a space-joined line in $GITHUB_OUTPUT, so whitespace in a
    # leg name would corrupt it — or append another key entirely.
    for name in all_engines:
        if not name or any(char.isspace() for char in name):
            problems.add(f"matrix leg name {name!r} must be non-empty and unspaced")
    for name in by_backend - all_engines:
        problems.add(f"a backend rule selects unknown engine {name!r}")
    for name in all_engines - by_backend:
        problems.add(f"no backend folder selects matrix leg {name!r}")
    for name in DMG_ENGINES - all_engines:
        problems.add(f"DMG_ENGINES lists unknown engine {name!r}")
    if problems:
        raise SystemExit(
            "inference engine selection rules are stale:\n  "
            + "\n  ".join(sorted(problems))
        )


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
    validate_mapping(matrix)

    changed_files = [line.strip() for line in sys.stdin if line.strip()]
    selection = select_engines(
        changed_files, args.event_name, derive_script_rules(matrix)
    )
    outputs = build_outputs(selection, matrix)

    print(f"event: {args.event_name}, changed files: {len(changed_files)}")
    print(f"selected engines: {outputs['selected']}")
    with open(args.output, "a", encoding="utf-8") as f:
        for key, value in outputs.items():
            f.write(f"{key}={value}\n")


if __name__ == "__main__":
    main()
