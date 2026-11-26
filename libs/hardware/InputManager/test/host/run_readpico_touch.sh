#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-readpico-touch.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -DFREEINK_DEVICE_READPICO=1 -DARDUINO_USB_CDC_ON_BOOT=1 \
  -Imetalio_stubs -I../../include -I../../../BoardConfig/include \
  -I../../../BoardReadPico/include \
  test_readpico_touch.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_readpico_touch"
"$BUILD_DIR/test_readpico_touch"
