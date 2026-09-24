#!/bin/sh
# Builds and runs the ContentProtection credential tests. No device or
# PlatformIO needed -- Credential.cpp is freestanding C++17.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/contentprotection-tests"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

c++ -std=c++17 -Wall -Wextra -Werror -I../../include \
  ../../src/Credential.cpp test_credential.cpp \
  -o "$BUILD_DIR/test_credential"

"$BUILD_DIR/test_credential"
