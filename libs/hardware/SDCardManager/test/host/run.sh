#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-sd-stream.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} \
  -Istubs -I../../include test_stream.cpp ../../src/SDCardManager.cpp \
  -o "$BUILD_DIR/test_stream"
"$BUILD_DIR/test_stream"
c++ -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} -DUSE_BLOCK_DEVICE_INTERFACE=1 \
  -Istubs -I../../include test_stream.cpp ../../src/SDCardManager.cpp \
  -o "$BUILD_DIR/test_raw"
"$BUILD_DIR/test_raw"
c++ -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} -DFREEINK_SD_SDMMC=1 -DUSE_BLOCK_DEVICE_INTERFACE=1 \
  -Istubs -I../../include test_sdmmc.cpp ../../src/SDCardManager.cpp \
  -o "$BUILD_DIR/test_sdmmc"
"$BUILD_DIR/test_sdmmc"
