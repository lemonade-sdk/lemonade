# Request Logging (PostgreSQL)

Lemonade can persist HTTP request metadata to PostgreSQL for client auditing and troubleshooting. This is useful for identifying which clients send inference requests, especially calls that set `keep_alive` or pin models in VRAM.

## Prerequisites

- `lemond` built with libpq support (default when `libpq` development headers are installed)
- PostgreSQL 16+ (local install or Docker)

Linux packages:

```bash
# Debian/Ubuntu
sudo apt install libpq-dev

# Fedora/RHEL
sudo dnf install postgresql-devel
```

macOS:

```bash
brew install libpq
export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig"
```

## Quick start with Docker

From the repository root:

```bash
./examples/start-request-log-db.sh
```

This starts PostgreSQL with a random host port (Docker assigns an available port) and prints the connection URL.

Suggested env drop-in for systemd (`/etc/lemonade/conf.d/request-log.conf`):

```ini
LEMONADE_REQUEST_LOG_ENABLED=true
LEMONADE_REQUEST_LOG_DATABASE_URL=postgresql://lemonade:change-me@127.0.0.1:<PORT>/lemonade_logs
LEMONADE_REQUEST_LOG_RETENTION_DAYS=30
LEMONADE_LOG_PROMPTS=false
```

Restart `lemond` after adding the env file.

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `LEMONADE_REQUEST_LOG_ENABLED` | `false` | Master switch for database request logging |
| `LEMONADE_REQUEST_LOG_DATABASE_URL` | (empty) | PostgreSQL URL, e.g. `postgresql://user:pass@host:5432/lemonade_logs` |
| `LEMONADE_REQUEST_LOG_RETENTION_DAYS` | `30` | Retention policy (see below) |
| `LEMONADE_LOG_PROMPTS` | `false` | When `true`, store prompt/message content in `redacted_body` |

If logging is enabled but PostgreSQL is unreachable, Lemonade logs a warning and continues serving requests.

## Retention behavior

| Value | Behavior |
|-------|----------|
| `-1` | Keep rows forever (purge disabled) |
| `0` | Write rows normally, but purge **all** rows on each hourly purge cycle |
| `N > 0` | Delete rows older than `N` days during hourly purge |

## Privacy and redaction

- Authorization headers are never stored.
- JSON body fields matching sensitive key names (`api_key`, `token`, `password`, `secret`, etc.) are replaced with `[REDACTED]`.
- When `LEMONADE_LOG_PROMPTS=false` (default), only character counts are stored for `prompt` and `messages`.
- Full prompt/message content is stored only when `LEMONADE_LOG_PROMPTS=true`.

## Review API

All endpoints are registered under the standard quad-prefix paths (`/api/v0/`, `/api/v1/`, `/v0/`, `/v1/`).

| Endpoint | Description |
|----------|-------------|
| `GET /api/v1/request-log/recent?limit=100` | Newest entries (max 1000) |
| `GET /api/v1/request-log/search?model=&client_ip=&path=&since=&keep_alive=` | Filtered search |
| `GET /api/v1/request-log/stats?since=24h` | Aggregates for the time window |

When `LEMONADE_API_KEY` is set, these endpoints require Bearer authentication (admin key also accepted).

### Example queries

Recent entries:

```bash
curl -s 'http://127.0.0.1:13305/api/v1/request-log/recent?limit=20' \
  -H "Authorization: Bearer $LEMONADE_API_KEY" | jq .
```

Find Ollama unload requests (`keep_alive=0`):

```bash
curl -s 'http://127.0.0.1:13305/api/v1/request-log/search?keep_alive=0&limit=50' \
  -H "Authorization: Bearer $LEMONADE_API_KEY" | jq .
```

Stats for the last hour:

```bash
curl -s 'http://127.0.0.1:13305/api/v1/request-log/stats?since=1h' \
  -H "Authorization: Bearer $LEMONADE_API_KEY" | jq .
```

Generate a sample request to verify logging:

```bash
curl -s -X POST http://127.0.0.1:13305/api/chat \
  -H 'Content-Type: application/json' \
  -d '{"model":"llama3.2","messages":[],"keep_alive":0}'
```

## SQL examples

Connect to the database:

Use the connection URL printed by `./examples/start-request-log-db.sh`, for example:

```bash
psql postgresql://lemonade:change-me@127.0.0.1:<PORT>/lemonade_logs
```

Clients sending `keep_alive`:

```sql
SELECT created_at, client_ip, path, model, keep_alive
FROM request_logs
WHERE keep_alive IS NOT NULL
ORDER BY created_at DESC
LIMIT 50;
```

Load requests with pinned models:

```sql
SELECT created_at, client_ip, model, redacted_body->'_meta'->>'pinned' AS pinned
FROM request_logs
WHERE path LIKE '%/load'
  AND redacted_body->'_meta'->>'pinned' = 'true'
ORDER BY created_at DESC;
```

## Build

```bash
./setup.sh
cmake --build --preset default
```

The binary is `build/lemond`.

To disable request-log support at compile time:

```bash
cmake -DLEMONADE_REQUEST_LOG=OFF --preset default
```

## Safe systemd upgrade

```bash
./examples/start-request-log-db.sh

sudo tee /etc/lemonade/conf.d/request-log.conf <<'EOF'
LEMONADE_REQUEST_LOG_ENABLED=true
LEMONADE_REQUEST_LOG_DATABASE_URL=postgresql://lemonade:change-me@127.0.0.1:<PORT>/lemonade_logs
LEMONADE_REQUEST_LOG_RETENTION_DAYS=30
LEMONADE_LOG_PROMPTS=false
EOF

Use the `<PORT>` value printed by `./examples/start-request-log-db.sh`.

sudo systemctl stop lemond.service
sudo cp build/lemond /usr/bin/lemond
sudo systemctl daemon-reload
sudo systemctl start lemond.service
sudo systemctl status lemond.service
```

## Patch workflow for upstream rebases

Keep changes on a feature branch and export a patch:

```bash
git checkout -b feature/request-log-db
# ... commit changes ...
git format-patch main --stdout > request-log.patch
```

Apply on a fresh branch:

```bash
git checkout -b feature/request-log-db main
git apply request-log.patch
```

## Limitations

- Streaming/chunked responses may report `response_body_bytes=0` because the body is not buffered.
- WebSocket traffic (`/realtime`, `/logs/stream`) is not logged by this subsystem.

## Manual schema init

The server creates the schema automatically on startup. You can also apply [`sql/request_logs_init.sql`](../../sql/request_logs_init.sql) manually.
