#!/bin/sh
# Build a lemonade-server .apk for musl/Alpine.
#
# Run inside an alpine:latest container from the repo root, e.g.:
#
#   docker run --rm -v "$PWD:/src" -w /src alpine:latest \
#       sh packaging/alpine/build-apk.sh
#
# Produces the .apk(s) under ./dist/ (override with OUTDIR=/path).
set -eux

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUTDIR=${OUTDIR:-"$REPO_ROOT/dist"}

# Keep the packaged version in lockstep with the CMake project version.
VERSION=$(sed -n 's/^project(lemon_cpp VERSION \([0-9.]*\)).*/\1/p' "$REPO_ROOT/CMakeLists.txt")
[ -n "$VERSION" ] || { echo "could not parse project version from CMakeLists.txt" >&2; exit 1; }

# alpine-sdk brings abuild + build-base + git; abuild -r installs makedepends.
apk add --no-cache alpine-sdk

# abuild refuses to run without a signing key. Generate a throwaway one and
# install its public half so the freshly built .apk verifies.
if ! ls "$HOME"/.abuild/*.rsa >/dev/null 2>&1; then
	abuild-keygen -a -n -i
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Snapshot the working tree (includes uncommitted changes) minus build output.
STAGE="$WORK/src/lemonade-server-$VERSION"
mkdir -p "$STAGE"
cp -a "$REPO_ROOT"/. "$STAGE"/
rm -rf "$STAGE"/build "$STAGE"/build-* "$STAGE"/.git "$STAGE"/dist

BUILDDIR="$WORK/abuild"
mkdir -p "$BUILDDIR"
cp "$REPO_ROOT"/packaging/alpine/APKBUILD "$BUILDDIR"/
cp "$REPO_ROOT"/packaging/alpine/lemonade-server.pre-install "$BUILDDIR"/
cp "$REPO_ROOT"/packaging/alpine/lemonade-server.post-install "$BUILDDIR"/
sed -i "s/^pkgver=.*/pkgver=$VERSION/" "$BUILDDIR"/APKBUILD

tar -czf "$BUILDDIR/lemonade-server-$VERSION.tar.gz" -C "$WORK/src" "lemonade-server-$VERSION"

cd "$BUILDDIR"
abuild -F checksum
abuild -F -r

mkdir -p "$OUTDIR"
find "$HOME"/packages -name '*.apk' -exec cp -v {} "$OUTDIR"/ \;
echo "APK(s) written to $OUTDIR"
