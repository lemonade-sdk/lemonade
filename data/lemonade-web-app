#!/bin/bash

set -e

LEMONADE_HOST="localhost"
LEMONADE_PORT="8000"

if [ -f /etc/lemonade/lemonade.conf ]; then
    source /etc/lemonade/lemonade.conf
fi
if [ -f ~/.config/lemonade/lemonade.conf ]; then
    source ~/.config/lemonade/lemonade.conf
fi

URL="http://${LEMONADE_HOST}:${LEMONADE_PORT}/"

# Prefer chromium based browsers otherwise xdg-open
if [ "$(echo "${LEMONADE_PREFER_CHROMIUM:-true}" | tr '[:upper:]' '[:lower:]')" = "true" ]; then
    if command -v google-chrome &> /dev/null; then
        google-chrome --app="$URL"
    elif command -v microsoft-edge-stable &> /dev/null; then
        microsoft-edge-stable --app="$URL"
    elif command -v chromium &> /dev/null; then
        chromium --app="$URL"
    elif command -v chromium-browser &> /dev/null; then
        chromium-browser --app="$URL"
    else
        xdg-open "$URL"
    fi
else
    xdg-open "$URL"
fi
