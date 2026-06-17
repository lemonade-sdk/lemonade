#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
COMPOSE_FILE="${REPO_ROOT}/examples/docker-compose.request-log.yml"
SERVICE_NAME="lemonade-request-log-db"
DB_HOST="127.0.0.1"
DB_PORT="5433"
DB_USER="lemonade"
DB_PASSWORD="change-me"
DB_NAME="lemonade_logs"
DATABASE_URL="postgresql://${DB_USER}:${DB_PASSWORD}@${DB_HOST}:${DB_PORT}/${DB_NAME}"

if ! command -v docker >/dev/null 2>&1; then
    echo "Error: docker is required but not installed." >&2
    exit 1
fi

if docker compose version >/dev/null 2>&1; then
    COMPOSE=(docker compose)
elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE=(docker-compose)
else
    echo "Error: docker compose or docker-compose is required." >&2
    exit 1
fi

echo "Starting PostgreSQL request log database..."
"${COMPOSE[@]}" -f "${COMPOSE_FILE}" up -d

echo "Waiting for PostgreSQL to accept connections on ${DB_HOST}:${DB_PORT}..."
ready=0
for _ in $(seq 1 60); do
    if "${COMPOSE[@]}" -f "${COMPOSE_FILE}" exec -T "${SERVICE_NAME}" \
        pg_isready -U "${DB_USER}" -d "${DB_NAME}" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
done

if [[ "${ready}" -ne 1 ]]; then
    echo "Error: PostgreSQL did not become ready in time." >&2
    exit 1
fi

cat <<EOF

PostgreSQL request log database is ready.

Connection URL:
  ${DATABASE_URL}

Suggested Lemonade env drop-in (/etc/lemonade/conf.d/request-log.conf):

LEMONADE_REQUEST_LOG_ENABLED=true
LEMONADE_REQUEST_LOG_DATABASE_URL=${DATABASE_URL}
LEMONADE_REQUEST_LOG_RETENTION_DAYS=30
LEMONADE_LOG_PROMPTS=false

EOF
