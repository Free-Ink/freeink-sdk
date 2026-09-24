#!/bin/sh
# Builds and runs the FreeInkUI host tests. No device or PlatformIO needed —
# the library is freestanding C++17.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeinkui-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -pthread -Wall -Wextra -Werror -I../../include ../../src/FreeInkUI.cpp test_freeinkui.cpp -o "$BUILD_DIR/test_freeinkui"
"$BUILD_DIR/test_freeinkui"
