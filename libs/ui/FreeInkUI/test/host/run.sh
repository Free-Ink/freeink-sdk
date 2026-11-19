#!/bin/sh
# Builds and runs the FreeInkUI host tests. No device or PlatformIO needed —
# the library is freestanding C++17.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeinkui-tests"
mkdir -p "$BUILD_DIR"
# Once as 1-bit panels build it, once with the 16-level draw path compiled in.
for GRAY16 in 0 1; do
  c++ -std=c++17 -pthread -Wall -Wextra -Werror -DFREEINK_UI_GRAY16=$GRAY16 -I../../include ../../src/FreeInkUI.cpp test_freeinkui.cpp -o "$BUILD_DIR/test_freeinkui"
  "$BUILD_DIR/test_freeinkui"
done
