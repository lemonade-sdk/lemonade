#!/bin/sh
# Flatpak entry point. lemonade-tray owns the lifecycle: it spawns lemond when
# none is reachable and opens the desktop app. The wrapper survives only because
# the desktop file needs %u, and the tray CLI rejects positional arguments.
set -u

if [ "$#" -gt 0 ]; then
    if command -v "$1" >/dev/null 2>&1; then
        exec "$@"
    fi
    mkdir -p "$HOME/.cache/lemonade"
    /app/bin/lemonade-tray --spawn-server --silent >>"$HOME/.cache/lemonade/tray.log" 2>&1 &
    exec /app/bin/lemonade-app "$@"
fi

exec /app/bin/lemonade-tray --spawn-server --launch-app
