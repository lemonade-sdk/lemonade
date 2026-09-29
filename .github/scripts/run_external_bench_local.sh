#!/usr/bin/env bash
#
# Run the bring-your-own (external_openai) backend benchmark LOCALLY on a
# gfx1151 Linux box, replicating the CI `bench-external` job without GitHub
# Actions. Use this on a dedicated 128GB Strix Halo machine to bring up and
# validate a container backend (gufo, halogen, ...) before enabling it in CI.
#
# What it does, mirroring the CI job:
#   1. Downloads the latest lemonade embeddable (for the `lemonade` CLI) unless
#      you point it at an existing build.
#   2. Detects the container engine (docker or podman) and resolves GPU-access
#      group flags.
#   3. Sets a model cache dir OUTSIDE the repo so multi-GB checkpoints persist.
#   4. Runs validate_backend_bench.py, which launches the container, waits for
#      its OpenAI endpoint, benches via `lemonade bench --base-url`, tears down.
#
# Usage:
#   ./.github/scripts/run_external_bench_local.sh <fork_id> [--lemonade /path/to/lemonade]
#
# Examples:
#   ./.github/scripts/run_external_bench_local.sh gufo
#   ./.github/scripts/run_external_bench_local.sh gufo --lemonade ~/lemonade/build/lemonade
#
# Environment overrides:
#   HF_TOKEN            Hugging Face token for model downloads (recommended).
#   BENCH_CACHE_ROOT    Where model + HF caches live (default: ~/lemonade-bench-cache).
#   BENCH_OUTPUT        Where run-*.json results are written (default: ./ci/results).
#
set -euo pipefail

FORK_ID="${1:-}"
if [ -z "$FORK_ID" ]; then
  echo "Usage: $0 <fork_id> [--lemonade /path/to/lemonade]"
  echo "  e.g. $0 gufo"
  exit 1
fi
shift || true

LEMONADE_BIN=""
while [ $# -gt 0 ]; do
  case "$1" in
    --lemonade) LEMONADE_BIN="$2"; shift 2 ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

FORKS_JSON="src/cpp/resources/benchmark_forks.json"
DRIVER=".github/scripts/validate_backend_bench.py"
BENCH_CACHE_ROOT="${BENCH_CACHE_ROOT:-$HOME/lemonade-bench-cache}"
BENCH_OUTPUT="${BENCH_OUTPUT:-$REPO_ROOT/ci/results}"

echo "=============================================================="
echo " Local external backend bench"
echo "   fork:        $FORK_ID"
echo "   repo:        $REPO_ROOT"
echo "   cache root:  $BENCH_CACHE_ROOT"
echo "   output:      $BENCH_OUTPUT"
echo "=============================================================="

# ---------------------------------------------------------------------------
# 1. lemonade CLI — reuse a provided build, or download the latest embeddable.
# ---------------------------------------------------------------------------
if [ -z "$LEMONADE_BIN" ]; then
  for c in "build/lemonade" "build/Release/lemonade"; do
    if [ -x "$c" ]; then LEMONADE_BIN="$REPO_ROOT/$c"; break; fi
  done
fi
if [ -z "$LEMONADE_BIN" ]; then
  echo "--- Downloading latest lemonade embeddable (for the CLI) ---"
  AUTH=()
  [ -n "${GH_TOKEN:-}" ] && AUTH=(-H "Authorization: Bearer $GH_TOKEN")
  RELEASE=$(curl -sf "${AUTH[@]}" -H "Accept: application/vnd.github+json" \
    "https://api.github.com/repos/lemonade-sdk/lemonade/releases/latest")
  ASSET_URL=$(echo "$RELEASE" | python3 -c "import sys,json; a=json.load(sys.stdin)['assets']; print(next((x['browser_download_url'] for x in a if 'embeddable' in x['name'] and 'ubuntu' in x['name'] and 'x64' in x['name']),'NOT_FOUND'))")
  if [ "$ASSET_URL" = "NOT_FOUND" ]; then echo "ERROR: no ubuntu embeddable asset"; exit 1; fi
  mkdir -p build
  curl -sfL "$ASSET_URL" | tar -xz --strip-components=1 -C build/
  chmod +x build/lemonade build/lemond 2>/dev/null || true
  LEMONADE_BIN="$REPO_ROOT/build/lemonade"
fi
test -x "$LEMONADE_BIN" || { echo "ERROR: lemonade CLI not found/executable: $LEMONADE_BIN"; exit 1; }
echo "lemonade CLI: $LEMONADE_BIN"

# ---------------------------------------------------------------------------
# 2. Container engine detection + GPU-access group flags (same logic as CI).
# ---------------------------------------------------------------------------
echo "--- container engine ---"
echo "whoami: $(whoami)  groups: $(id -nG)"
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  ENGINE=docker
elif command -v podman >/dev/null 2>&1; then
  ENGINE=podman
else
  echo "ERROR: no usable container engine (docker or podman)."
  echo "Install one: 'sudo apt-get install -y podman crun' (rootless, no daemon)."
  exit 1
fi
echo "engine: $ENGINE ($($ENGINE --version))"

ls -l /dev/kfd /dev/dri 2>&1 || { echo "ERROR: /dev/kfd or /dev/dri missing — no gfx1151 GPU passthrough"; exit 1; }

if [ "$ENGINE" = "podman" ]; then
  GPU_GROUPS="--group-add keep-groups"
else
  RGID=$(getent group render | cut -d: -f3 || true)
  VGID=$(getent group video  | cut -d: -f3 || true)
  GPU_GROUPS=""
  [ -n "$RGID" ] && GPU_GROUPS="$GPU_GROUPS --group-add $RGID"
  [ -n "$VGID" ] && GPU_GROUPS="$GPU_GROUPS --group-add $VGID"
fi
echo "GPU group flags: $GPU_GROUPS"

# ---------------------------------------------------------------------------
# 3. Caches OUTSIDE the repo so multi-GB checkpoints persist across runs.
# ---------------------------------------------------------------------------
export CONTAINER_ENGINE="$ENGINE"
export GPU_GROUP_FLAGS="$GPU_GROUPS"
export GUFO_MODELS_DIR="$BENCH_CACHE_ROOT/external-models/$FORK_ID"
export HF_HOME="$BENCH_CACHE_ROOT/hf-cache"
export LEMONADE_EXE="$LEMONADE_BIN"
mkdir -p "$GUFO_MODELS_DIR" "$HF_HOME" "$BENCH_OUTPUT"

if ! command -v hf >/dev/null 2>&1; then
  echo "--- installing hf CLI for model provisioning ---"
  python3 -m pip install --quiet --upgrade "huggingface_hub[cli]" || \
    pip install --quiet --upgrade "huggingface_hub[cli]" || \
    echo "WARN: hf CLI install failed; a fork's prepare_cmd may need it"
fi

# ---------------------------------------------------------------------------
# 4. Run the driver. --fork-filter forces an enabled:false fork to run, so you
#    can validate before flipping it on in benchmark_forks.json.
# ---------------------------------------------------------------------------
echo "--- running external backend bench ---"
python3 "$DRIVER" \
  --forks       "$FORKS_JSON" \
  --output      "$BENCH_OUTPUT" \
  --fork-filter "$FORK_ID"

echo "=============================================================="
echo " Done. Results under: $BENCH_OUTPUT/$FORK_ID/"
echo " Inspect a run file to confirm tps/tokens were captured:"
echo "   find '$BENCH_OUTPUT/$FORK_ID' -name 'run-*.json' | tail -1 | xargs cat"
echo "=============================================================="
