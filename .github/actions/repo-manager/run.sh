#!/usr/bin/env bash
# Runs inside the repo-manager container: bring up a lemond serving the review model,
# point pi at it, and run the one command the job asked for. Mounted from the checkout
# rather than baked into the image so that changing this does not need a new image.
set -euo pipefail

# The container has its own network namespace, so nothing else on the runner can be
# holding this, and the port never has to be negotiated with the other jobs on the box.
PORT=8000

# --no-broadcast because a lemond that answers UDP discovery would be found by the CLIs
# other jobs run, and the bridge network is not isolation enough to rely on.
"$LEMOND" /cache --port "$PORT" --host 127.0.0.1 --no-broadcast \
    > /logs/lemond.stdout.log 2> /logs/lemond.stderr.log &
LEMOND_PID=$!

stop_lemond() {
    curl -sf -X POST "http://127.0.0.1:$PORT/internal/shutdown" > /dev/null 2>&1 || true
    sleep 2
    kill "$LEMOND_PID" 2>/dev/null || true
}
trap stop_lemond EXIT

for _ in $(seq 1 60); do
    curl -sf "http://127.0.0.1:$PORT/api/v1/health" > /dev/null 2>&1 && break
    kill -0 "$LEMOND_PID" 2>/dev/null || { cat /logs/lemond.stderr.log; exit 1; }
    sleep 2
done
curl -sf "http://127.0.0.1:$PORT/api/v1/health" > /dev/null

response=$(curl -sf -X POST "http://127.0.0.1:$PORT/api/v1/pull" \
    -H "Content-Type: application/json" \
    -d "{\"model_name\": \"${REPO_MANAGER_MODEL}\"}")
echo "$response"
test "$(printf '%s' "$response" | jq -r '.status // ""')" = "success"
du -sh /cache

export REPO_MANAGER_LEMONADE_URL="http://127.0.0.1:$PORT/v1"
export REPO_MANAGER_PI_MODEL="Lemonade/${REPO_MANAGER_MODEL}"
repo-manager pi setup

# $COMMAND is a command line, not one argument.
# shellcheck disable=SC2086
repo-manager $COMMAND --repo lemonade-sdk/lemonade --checkout checkout --state state
