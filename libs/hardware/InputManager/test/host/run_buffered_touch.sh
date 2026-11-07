#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-buffered-touch.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
for DEVICE in METALIO_EINK4 METALIO_EINK4_PLUS; do
  c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DFREEINK_DEVICE_${DEVICE}=1 -DARDUINO_USB_CDC_ON_BOOT=1 \
    -Imetalio_stubs -I../../include -I../../../BoardConfig/include \
    test_buffered_touch.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_touch"
  "$BUILD_DIR/test_touch"
done
