#!/usr/bin/env bash
# Starts lemond + tray + app for the Lemonade flatpak.
set -u

HOST="${LEMONADE_HOST:-127.0.0.1}"
PORT="${LEMONADE_PORT:-13305}"
DATA_ROOT="${LEMONADE_DATA_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/lemonade}"

# wildcard bind is reached over loopback
case "$HOST" in 0.0.0.0|::|"") CONNECT=127.0.0.1 ;; *) CONNECT="$HOST" ;; esac
HEALTH="http://$CONNECT:$PORT/api/v1/health"

# Wire any installed backend extension (extensions/backends/<name>) onto the
# environment so the server can discover and launch it.
for d in /app/extensions/backends/*/; do
  [ -d "$d" ] || continue
  d="${d%/}"
  [ -d "$d/bin" ] && export PATH="$d/bin:$PATH"
  [ -d "$d/lib" ] && export LD_LIBRARY_PATH="$d/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  # a backend bundling the XRT runtime (e.g. RyzenAI/NPU) ships lib/xrt
  [ -e "$d/lib/xrt" ] && export XILINX_XRT="$d"
done

# If the first argument is a command in the PATH, execute it directly
if [ $# -gt 0 ] && type -P "$1" >/dev/null 2>&1; then
  exec "$@"
fi

# second launch / deep link: forward to the running app, then exit
LOCK="$XDG_RUNTIME_DIR/app/$FLATPAK_ID/supervisor.lock"
mkdir -p "$(dirname "$LOCK")"
exec 9>"$LOCK"
flock -xn 9 || exec /app/bin/lemonade-app "$@"

LEMOND_PID=""
TRAY_PID=""
APP_PID=""

is_server_reachable() {
  if curl -s --connect-timeout 1 -m 2 -o /dev/null "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null; then
    return 0
  fi
  if curl -s --connect-timeout 1 -m 2 -o /dev/null "http://localhost:$PORT/api/v1/health" 2>/dev/null; then
    return 0
  fi
  return 1
}

# start bundled lemond unless one is already reachable
if [ "${LEMONADE_FLATPAK_FORCE_BUNDLED:-}" = 1 ] || ! is_server_reachable; then
  mkdir -p "$DATA_ROOT"
  /app/bin/lemond "$DATA_ROOT" --port "$PORT" --host "$HOST" &
  LEMOND_PID=$!
  for _ in $(seq 1 30); do
    is_server_reachable && break
    sleep 0.5
  done
fi

# only stop a lemond we started
cleanup() {
  [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null
  [ -n "$TRAY_PID" ] && kill "$TRAY_PID" 2>/dev/null
  if [ -n "$LEMOND_PID" ]; then
    curl -sf --max-time 5 -X POST "http://$CONNECT:$PORT/internal/shutdown" >/dev/null 2>&1
    kill "$LEMOND_PID" 2>/dev/null
  fi
  return 0
}
trap 'exit' INT TERM HUP
trap cleanup EXIT

/app/bin/lemonade-tray --host "$HOST" --port "$PORT" &
TRAY_PID=$!
/app/bin/lemonade-app "$@" &
APP_PID=$!

# tray-anchored: window can close without stopping the server; no tray -> app-anchored
sleep 1
if kill -0 "$TRAY_PID" 2>/dev/null; then
  wait "$TRAY_PID"
else
  TRAY_PID=""
  wait "$APP_PID"
fi
