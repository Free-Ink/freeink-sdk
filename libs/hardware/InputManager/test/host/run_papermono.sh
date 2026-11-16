#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-papermono-input.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -DFREEINK_DEVICE_PAPERMONO=1 -DFREEINK_CAP_TOUCH=0 -DARDUINO_USB_CDC_ON_BOOT=1 \
  -Ipapermono_stubs -Imetalio_stubs -I../../include -I../../../BoardConfig/include \
  test_papermono.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_papermono"
"$BUILD_DIR/test_papermono"
