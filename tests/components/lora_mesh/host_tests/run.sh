#!/usr/bin/env bash
# Build and run the lora_mesh host-side behavior tests.
# Usage: tests/components/lora_mesh/host_tests/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/lora_mesh_host_tests"
mkdir -p "$BUILD_DIR"

# stub_include shadows esphome/core/defines.h (must come before $ROOT).
c++ -std=gnu++20 -g -Wall -Wextra -Wno-unused-parameter -DESPHOME_LOG_LEVEL=1 \
  -I"$HERE/stub_include" -I"$ROOT" \
  "$HERE/test_lora_mesh.cpp" \
  "$HERE/stubs.cpp" \
  "$ROOT/esphome/components/lora_mesh/lora_mesh.cpp" \
  -o "$BUILD_DIR/test_lora_mesh"

exec "$BUILD_DIR/test_lora_mesh"
