#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-metalio.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -DFREEINK_DEVICE_METALIO_EINK4=1 -DARDUINO_USB_CDC_ON_BOOT=1 \
  -Imetalio_stubs -I../../include -I../../../BoardConfig/include \
  test_metalio.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_metalio"
"$BUILD_DIR/test_metalio"
