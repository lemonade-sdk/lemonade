#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Setup GitHub repo secrets for PR-Agent (DeepSeek) and Qodo Merge workflows.
#
# Reads API keys from local files in ~/Documents/ and pushes them as
# encrypted GitHub Actions secrets via `gh secret set`.
#
# Usage:
#   bash .github/scripts/setup-repo-secrets.sh                  # current repo
#   bash .github/scripts/setup-repo-secrets.sh --repo owner/repo
#   bash .github/scripts/setup-repo-secrets.sh --dry-run        # preview only
#
# Key sources (edit paths below to match your setup):
#   ~/Documents/deepseek api key.txt   → DEEPSEEK_API_KEY
#   ~/Documents/qodo api key.txt       → QODO_API_KEY
#
# Prerequisites:
#   - gh CLI installed and authenticated (`gh auth status`)
#   - Write / admin access to the target repo
# ---------------------------------------------------------------------------
set -euo pipefail

REPO=""
DRY_RUN=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)     REPO="$2"; shift 2 ;;
    --dry-run)  DRY_RUN=true;  shift ;;
    *)          echo "Unknown: $1"; exit 1 ;;
  esac
done

# --- Config: key file paths -----------------------------------------------
DEEPSEEK_KEY_FILE="$HOME/Documents/deepseek api key.txt"
QODO_KEY_FILE="$HOME/Documents/qodo api key.txt"

# --- Checks ----------------------------------------------------------------
if ! command -v gh &>/dev/null; then
  echo "❌ gh CLI not found — install it from https://cli.github.com/"
  exit 1
fi

if ! gh auth status &>/dev/null; then
  echo "❌ gh CLI not authenticated — run 'gh auth login' first."
  exit 1
fi

GH_ARGS=()
[[ -n "$REPO" ]] && GH_ARGS+=(--repo "$REPO")

echo "🔍 Target: ${REPO:-$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null || echo 'current repo')}"

# --- Read keys -------------------------------------------------------------
read_key() {
  local path="$1" label="$2"
  if [[ ! -f "$path" ]]; then
    echo "⚠️  $label key file not found at: $path"
    return 1
  fi
  local val
  val="$(tr -d '[:space:]' < "$path")"
  if [[ -z "$val" ]]; then
    echo "⚠️  $label key file is empty: $path"
    return 1
  fi
  echo "$val"
}

echo ""
echo "📂 Reading keys from ~/Documents/ …"

DEEPSEEK_KEY="$(read_key "$DEEPSEEK_KEY_FILE" "DeepSeek")" || true
QODO_KEY="$(read_key "$QODO_KEY_FILE" "Qodo")" || true

# --- Set secrets -----------------------------------------------------------
set_secret() {
  local name="$1" value="$2"
  if [[ -z "$value" ]]; then
    echo "   ⏭️  Skipping $name (no value)"
    return
  fi
  if $DRY_RUN; then
    echo "   🏁 [DRY-RUN] gh secret set $name ${GH_ARGS[*]}"
  else
    echo "   🔐 Setting $name …"
    echo -n "$value" | gh secret set "$name" "${GH_ARGS[@]}"
    echo "   ✅ $name set"
  fi
}

echo ""
$DRY_RUN && echo "🏁 DRY RUN — no secrets will be written" || echo "🚀 Setting secrets …"
echo ""

set_secret "DEEPSEEK_API_KEY" "$DEEPSEEK_KEY"
set_secret "QODO_API_KEY" "$QODO_KEY"

# --- Summary ---------------------------------------------------------------
echo ""
if $DRY_RUN; then
  echo "🏁 Dry run complete. Run without --dry-run to apply."
else
  echo "✅ Done! Secrets are now available to GitHub Actions workflows."
  echo "   Verify: gh secret list ${GH_ARGS[*]}"
fi
