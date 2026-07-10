#!/usr/bin/env bash
# Compile and run the host-side LIN protocol unit tests. No ESP32/ESP-IDF/hardware needed.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$ROOT/esphome/components/linbus"
OUT="${TMPDIR:-/tmp}/lin_host_test"

CXX="${CXX:-g++}"

"$CXX" -std=c++17 -O0 -g -Wall \
  -I "$HERE/stubs" -I "$SRC" \
  "$HERE/test_lin_protocol.cpp" \
  "$SRC/LinProtocolHandler.cpp" \
  -o "$OUT"

exec "$OUT"
