#!/bin/bash
# Build lemonade-server from source (native Debian packaging, /usr layout) and
# install it on this Ubuntu machine, replacing a PPA or prior .deb install.
#
# Usage (from repo root on Ubuntu):
#   ./scripts/deploy-ubuntu-local.sh [OPTIONS]
#
# Options:
#   --skip-deps     Skip apt build-dep
#   --skip-build    Reuse existing ../lemonade-server_*.deb
#   --no-restart    Install only; do not stop/start lemond.service
#   --hold          apt-mark hold lemonade-server after install
#   -h, --help      Show this help
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SKIP_DEPS=false
SKIP_BUILD=false
NO_RESTART=false
APT_HOLD=false

print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }

usage() {
    cat <<'EOF'
Build lemonade-server from source (native Debian packaging, /usr layout) and
install it on this Ubuntu machine, replacing a PPA or prior .deb install.

Usage (from repo root on Ubuntu):
  ./scripts/deploy-ubuntu-local.sh [OPTIONS]

Options:
  --skip-deps     Skip apt build-dep
  --skip-build    Reuse existing ../lemonade-server_*.deb
  --no-restart    Install only; do not stop/start lemond.service
  --hold          apt-mark hold lemonade-server after install
  -h, --help      Show this help
EOF
    exit 0
}

maybe_sudo() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

cleanup() {
    if [ -n "${REPO_ROOT:-}" ] && [ -d "${REPO_ROOT}/debian" ]; then
        print_info "Removing generated debian/ directory..."
        rm -rf "${REPO_ROOT}/debian"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-deps) SKIP_DEPS=true ;;
        --skip-build) SKIP_BUILD=true ;;
        --no-restart) NO_RESTART=true ;;
        --hold) APT_HOLD=true ;;
        -h|--help) usage ;;
        *)
            print_error "Unknown option: $1"
            echo "Run with --help for usage."
            exit 1
            ;;
    esac
    shift
done

if [[ "${OSTYPE:-}" != linux* ]]; then
    print_error "This script must run on Linux (Ubuntu)."
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    print_error "apt-get not found. This script is for Debian/Ubuntu only."
    exit 1
fi

if ! command -v git >/dev/null 2>&1; then
    print_error "git is required to determine the package version."
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "${REPO_ROOT}" ] || [ ! -f "${REPO_ROOT}/contrib/debian/control" ]; then
    print_error "Run this script from a Lemonade git checkout (contrib/debian/control not found)."
    exit 1
fi

cd "${REPO_ROOT}"
trap cleanup EXIT

if command -v snap >/dev/null 2>&1 && snap list lemonade-server >/dev/null 2>&1; then
    print_warning "Snap package 'lemonade-server' is installed and may conflict with the systemd .deb install."
    print_warning "Consider: sudo snap remove lemonade-server"
fi

if [ ! -d "contrib/debian" ]; then
    print_error "Missing contrib/debian packaging tree."
    exit 1
fi

print_info "Preparing Debian packaging metadata..."
rm -rf debian
cp -a contrib/debian debian

GIT_VERSION="$(git describe --tags --always)"
DEB_VERSION="${GIT_VERSION#v}"
if command -v lsb_release >/dev/null 2>&1; then
    UBUNTU_RELEASE="$(lsb_release -rs)"
    DEB_CODENAME="$(lsb_release -cs)"
    DEB_VERSION="${DEB_VERSION}~local${UBUNTU_RELEASE}"
else
    DEB_CODENAME="local"
    DEB_VERSION="${DEB_VERSION}~local"
fi
DEB_DATE="$(date -R)"

sed -e "s|@@DEB_VERSION@@|${DEB_VERSION}|g" \
    -e "s|@@DEB_CODENAME@@|${DEB_CODENAME}|g" \
    -e "s|@@DEB_DATE@@|${DEB_DATE}|g" \
    debian/changelog.in > debian/changelog

print_success "Package version: ${DEB_VERSION}"

if [ "${SKIP_DEPS}" = false ]; then
    print_info "Installing build dependencies (apt build-dep)..."
    maybe_sudo apt-get update
    maybe_sudo apt-get build-dep . -y
    print_success "Build dependencies installed"
else
    print_info "Skipping build dependencies (--skip-deps)"
fi

if [ "${SKIP_BUILD}" = false ]; then
    print_info "Building binary .deb (dpkg-buildpackage)..."
    dpkg-buildpackage -us -uc -b
    print_success "Package build finished"
else
    print_info "Skipping package build (--skip-build)"
fi

DEB_FILE="$(ls -t ../lemonade-server_*.deb 2>/dev/null | head -1 || true)"
if [ -z "${DEB_FILE}" ] || [ ! -f "${DEB_FILE}" ]; then
    PARENT_DIR="$(cd "${REPO_ROOT}/.." && pwd)"
    print_error "No lemonade-server .deb found in: ${PARENT_DIR}"
    print_error "Run without --skip-build, or build manually with dpkg-buildpackage."
    exit 1
fi

print_info "Using package: ${DEB_FILE}"

if [ "${NO_RESTART}" = false ]; then
    print_info "Stopping lemond.service (if running)..."
    maybe_sudo systemctl stop lemond.service 2>/dev/null || true
fi

print_info "Installing package (replaces PPA or prior install)..."
maybe_sudo apt-get install -y "${DEB_FILE}"
print_success "Package installed"

if [ "${NO_RESTART}" = false ]; then
    print_info "Enabling and starting lemond.service..."
    maybe_sudo systemctl daemon-reload
    maybe_sudo systemctl enable lemond.service
    maybe_sudo systemctl start lemond.service
    print_success "lemond.service started"
else
    print_info "Skipping service restart (--no-restart)"
fi

if [ "${APT_HOLD}" = true ]; then
    print_info "Holding lemonade-server to prevent apt upgrade from overwriting local build..."
    maybe_sudo apt-mark hold lemonade-server
    print_success "lemonade-server marked as held"
fi

if [ "${NO_RESTART}" = false ]; then
    print_info "Waiting for server health check..."
    HEALTH_OK=false
    for _ in $(seq 1 30); do
        if curl -sf "http://127.0.0.1:13305/health" >/dev/null 2>&1; then
            HEALTH_OK=true
            break
        fi
        if curl -sf "http://127.0.0.1:13305/v1/health" >/dev/null 2>&1; then
            HEALTH_OK=true
            break
        fi
        sleep 1
    done

    if [ "${HEALTH_OK}" = true ]; then
        print_success "Health check passed (http://127.0.0.1:13305)"
    else
        print_warning "Health check did not succeed within 30s."
        print_warning "Inspect logs: journalctl -u lemond.service -n 50 --no-pager"
    fi
fi

echo ""
echo "=========================================="
print_success "Deploy completed"
echo "=========================================="
echo ""
print_info "Installed package:"
dpkg -l lemonade-server 2>/dev/null | tail -1 || true
echo ""
print_info "Binary: $(command -v lemond 2>/dev/null || echo 'lemond not in PATH')"
print_info "Config/data: /var/lib/lemonade/.cache/lemonade/ (preserved across upgrades)"
echo ""
if [ "${APT_HOLD}" = false ]; then
    print_warning "PPA still enabled? A future 'apt upgrade' may replace this build with the PPA version."
    print_warning "To pin the local build: sudo apt-mark hold lemonade-server"
    print_warning "Or re-run with: $0 --hold"
fi
