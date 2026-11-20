#!/bin/sh
# Flatpak entry point: lemonade-tray owns the lifecycle. The wrapper exists only
# to forward the desktop entry's %u links, which the tray CLI cannot accept.
set -u

TRAY=/app/bin/lemonade-tray
APP=/app/bin/lemonade-app

if [ "$#" -gt 0 ]; then
    if command -v "$1" >/dev/null 2>&1; then
        exec "$@"
    fi
    mkdir -p "$HOME/.cache/lemonade"
    "$TRAY" --spawn-server --silent >>"$HOME/.cache/lemonade/tray.log" 2>&1 &
    exec "$APP" "$@"
fi

exec "$TRAY" --spawn-server --launch-app
