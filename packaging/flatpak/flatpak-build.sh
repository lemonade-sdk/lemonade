#!/usr/bin/env bash
set -euo pipefail

# Lemonade Flatpak online from-source build helper
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/_flatpak_build"
DIST_DIR="${ROOT_DIR}/dist"
CONFIG_FILE="${ROOT_DIR}/packaging/flatpak/aetherpak.yaml"
MANIFEST="${ROOT_DIR}/packaging/flatpak/ai.lemonadeserver.app.yaml"

mkdir -p "${DIST_DIR}"

# 1. Build Flatpak (prefer aetherpak CLI if detected, fallback to flatpak-builder)
if command -v aetherpak >/dev/null 2>&1; then
    echo "=== Building Flatpak with AetherPak CLI ==="
    aetherpak build \
        --manifest "${MANIFEST}" \
        --config "${CONFIG_FILE}" \
        --repo-path "${DIST_DIR}/flatpak-repo"
    echo "AetherPak build complete."
elif command -v flatpak-builder >/dev/null 2>&1; then
    echo "=== Running flatpak-builder from-source build ==="
    DBUS_WRAPPER=""
    if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-run-session >/dev/null 2>&1; then
        DBUS_WRAPPER="dbus-run-session --"
    fi

    $DBUS_WRAPPER flatpak-builder \
        --force-clean \
        --user \
        --disable-rofiles-fuse \
        --install-deps-from=flathub \
        --repo="${DIST_DIR}/flatpak-repo" \
        "${BUILD_DIR}" \
        "${MANIFEST}"

    echo "=== Creating standalone Flatpak bundle ==="
    flatpak build-bundle "${DIST_DIR}/flatpak-repo" "${DIST_DIR}/ai.lemonadeserver.app.flatpak" ai.lemonadeserver.app
    echo "Flatpak bundle created at: ${DIST_DIR}/ai.lemonadeserver.app.flatpak"
else
    echo "Neither aetherpak nor flatpak-builder is installed on this system."
    exit 1
fi
