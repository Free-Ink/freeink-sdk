#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-chsc6x-touch.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -DFREEINK_DEVICE_MURPHY=1 -DARDUINO_USB_CDC_ON_BOOT=1 \
  -Ichsc6x_stubs -Ipicco_stubs -I../../include -I../../../BoardConfig/include \
  test_chsc6x_touch.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_chsc6x_touch"
"$BUILD_DIR/test_chsc6x_touch"
