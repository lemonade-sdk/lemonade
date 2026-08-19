#!/usr/bin/env bash
# Vendors Rust dependencies for offline and distro packaging builds.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NONO_DIR="${REPO_ROOT}/src/rust/nono-c"

echo "Vendoring nono-c dependencies under ${NONO_DIR}..."

if ! command -v cargo >/dev/null 2>&1; then
    echo "Error: cargo is required to vendor Rust dependencies." >&2
    exit 1
fi

cd "${NONO_DIR}"
cargo vendor "${NONO_DIR}/vendor"

mkdir -p "${NONO_DIR}/.cargo"
cat << 'EOF' > "${NONO_DIR}/.cargo/config.toml"
[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "vendor"
EOF

echo "Successfully vendored dependencies into ${NONO_DIR}/vendor"
