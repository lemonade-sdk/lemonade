#!/usr/bin/env bash
# Builds and runs one adapter_spec job test without building lemond:
#   bash test/cpp/adapter_spec/run.sh ap2
# JSON_INCLUDE overrides where nlohmann/json.hpp is found; by default it is looked up in the _deps folder
# of BUILD_DIR (default: <repo>/build), where CMake fetched it.
set -euo pipefail
job="${1:?usage: run.sh <job, e.g. ap2>}"
root="$(cd "$(dirname "$0")/../../.." && pwd)"
inc="${JSON_INCLUDE:-${BUILD_DIR:-$root/build}/_deps/json-src/include}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -std=c++20 -Wall -Wextra -Werror -O0 -g -I"$inc" -I"$root/src/cpp/server/backends/adapter" \
    "$root/test/cpp/adapter_spec/test_${job}.cpp" "$root/src/cpp/server/backends/adapter/adapter_spec.cpp" \
    -o "$out/test_${job}"
"$out/test_${job}"
