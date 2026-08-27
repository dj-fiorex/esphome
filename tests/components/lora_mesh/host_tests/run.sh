#!/usr/bin/env bash
# Build and run the lora_mesh host-side behavior tests.
# Usage: tests/components/lora_mesh/host_tests/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/lora_mesh_host_tests"
mkdir -p "$BUILD_DIR"

# Focused tests exercise the three internal seams selected in ADR-0012.
c++ -std=gnu++20 -g -Wall -Wextra -Wno-unused-parameter -DESPHOME_LOG_LEVEL=1 \
  -I"$HERE/stub_include" -I"$ROOT" \
  "$HERE/test_route_table.cpp" \
  "$HERE/stubs.cpp" \
  "$ROOT/esphome/components/lora_mesh/route_table.cpp" \
  -o "$BUILD_DIR/test_route_table"

"$BUILD_DIR/test_route_table"

c++ -std=gnu++20 -g -Wall -Wextra -Wno-unused-parameter -DESPHOME_LOG_LEVEL=1 \
  -I"$HERE/stub_include" -I"$ROOT" \
  "$HERE/test_packet_admission.cpp" \
  "$HERE/stubs.cpp" \
  "$ROOT/esphome/components/lora_mesh/packet_admission.cpp" \
  -o "$BUILD_DIR/test_packet_admission"

"$BUILD_DIR/test_packet_admission"

c++ -std=gnu++20 -g -Wall -Wextra -Wno-unused-parameter -DESPHOME_LOG_LEVEL=1 \
  -I"$HERE/stub_include" -I"$ROOT" \
  "$HERE/test_outbound_airtime.cpp" \
  "$HERE/stubs.cpp" \
  "$ROOT/esphome/components/lora_mesh/outbound_airtime.cpp" \
  -o "$BUILD_DIR/test_outbound_airtime"

"$BUILD_DIR/test_outbound_airtime"

c++ -std=gnu++20 -g -Wall -Wextra -Wno-unused-parameter -DESPHOME_LOG_LEVEL=5 -DLORA_MESH_LINK_SIM \
  -I"$HERE/stub_include" -I"$ROOT" \
  "$HERE/test_link_sim_admission.cpp" \
  "$HERE/stubs.cpp" \
  "$ROOT/esphome/components/lora_mesh/lora_mesh.cpp" \
  "$ROOT/esphome/components/lora_mesh/outbound_airtime.cpp" \
  "$ROOT/esphome/components/lora_mesh/packet_admission.cpp" \
  "$ROOT/esphome/components/lora_mesh/route_table.cpp" \
  -o "$BUILD_DIR/test_link_sim_admission"

"$BUILD_DIR/test_link_sim_admission"

# stub_include shadows esphome/core/defines.h (must come before $ROOT).
c++ -std=gnu++20 -g -Wall -Wextra -Wno-unused-parameter -DESPHOME_LOG_LEVEL=1 \
  -I"$HERE/stub_include" -I"$ROOT" \
  "$HERE/test_lora_mesh.cpp" \
  "$HERE/stubs.cpp" \
  "$ROOT/esphome/components/lora_mesh/lora_mesh.cpp" \
  "$ROOT/esphome/components/lora_mesh/outbound_airtime.cpp" \
  "$ROOT/esphome/components/lora_mesh/packet_admission.cpp" \
  "$ROOT/esphome/components/lora_mesh/route_table.cpp" \
  -o "$BUILD_DIR/test_lora_mesh"

exec "$BUILD_DIR/test_lora_mesh"
