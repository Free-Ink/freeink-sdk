#!/bin/sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INPUT_MANAGER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HARDWARE_DIR=$(CDPATH= cd -- "$INPUT_MANAGER_DIR/.." && pwd)
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-murphy-m4.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM
CXX=${CXX:-c++}

for batch1 in 0 1; do
  "$CXX" -std=c++17 -Wall -Wextra -Werror -O2 \
    -DFREEINK_MURPHY_M4_BATCH1="$batch1" \
    -I"$HARDWARE_DIR/BoardConfig/include" -I"$INPUT_MANAGER_DIR/include" \
    "$SCRIPT_DIR/test_murphy_m4.cpp" -o "$BUILD_DIR/test_murphy_m4_$batch1"
  "$BUILD_DIR/test_murphy_m4_$batch1"
done