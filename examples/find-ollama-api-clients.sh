#!/usr/bin/env bash
# Find local processes sending Ollama-compatible API traffic to Lemonade.
#
# Usage:
#   ./examples/find-ollama-api-clients.sh
#   LEMONADE_PORT=11434 ./examples/find-ollama-api-clients.sh
#   LEMONADE_BASE_URL=http://127.0.0.1:11434 ./examples/find-ollama-api-clients.sh

set -euo pipefail

PORT="${LEMONADE_PORT:-11434}"
BASE_URL="${LEMONADE_BASE_URL:-http://127.0.0.1:${PORT}}"

echo "=== Lemonade Ollama API client diagnostics ==="
echo "Port: ${PORT}"
echo "Base URL: ${BASE_URL}"
echo

echo "--- TCP clients connected to :${PORT} ---"
if command -v ss >/dev/null 2>&1; then
    ss -tnp 2>/dev/null | grep ":${PORT}" || echo "(no connections)"
elif command -v lsof >/dev/null 2>&1; then
    lsof -nP -iTCP:"${PORT}" -sTCP:ESTABLISHED 2>/dev/null || echo "(no connections)"
else
    echo "Install ss or lsof to list connected processes."
fi
echo

echo "--- Python processes (common ollama-python clients) ---"
if pgrep -a python >/dev/null 2>&1 || pgrep -a python3 >/dev/null 2>&1; then
    pgrep -a python 2>/dev/null || true
    pgrep -a python3 2>/dev/null || true
else
    echo "(no python processes)"
fi
echo

echo "--- Recent POST /api/chat from request-log API (if enabled) ---"
if command -v curl >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; then
    headers=()
    if [[ -n "${LEMONADE_API_KEY:-}" ]]; then
        headers=(-H "Authorization: Bearer ${LEMONADE_API_KEY}")
    elif [[ -n "${LEMONADE_ADMIN_API_KEY:-}" ]]; then
        headers=(-H "Authorization: Bearer ${LEMONADE_ADMIN_API_KEY}")
    fi
    response="$(curl -fsS "${headers[@]}" \
        "${BASE_URL}/api/v1/request-log/search?path=%25chat%25&limit=15" 2>/dev/null || true)"
    if [[ -n "${response}" ]]; then
        echo "${response}" | jq -r '.entries[]? | "\(.created_at) ip=\(.client_ip) model=\(.model) ua=\(.user_agent)"'
    else
        echo "Request log API unavailable (logging disabled, auth required, or wrong port)."
        echo "Query manually:"
        echo "  curl -s '${BASE_URL}/api/v1/request-log/search?path=%25chat%25&limit=15' | jq ."
    fi
else
    echo "Install curl and jq to query the request-log API."
fi
echo

echo "--- Tips ---"
cat <<'EOF'
1. user_agent "ollama-python/..." means a local Python script using the PyPI
   `ollama` package (pip install ollama), not Lemonade itself.
2. Default client host is http://127.0.0.1:11434 — same as Ollama's port.
3. Find the script:
     pgrep -af python
     sudo lsof -iTCP:11434 -sTCP:ESTABLISHED
     tr '\0' ' ' < /proc/<PID>/cmdline; ls -l /proc/<PID>/cwd
4. Stop it: kill <PID>, or reconfigure it to use Lemonade model names from
     `lemonade list`, or point OLLAMA_HOST at a real Ollama server.
5. To avoid accidental clients, run Lemonade on a non-Ollama port (e.g. 13305).
EOF
