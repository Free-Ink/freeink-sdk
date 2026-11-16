#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-picco.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -DFREEINK_DEVICE_PICCO=1 -DARDUINO_USB_CDC_ON_BOOT=1 \
  -Ipicco_stubs -I../../include -I../../../BoardConfig/include \
  test_toggle_switch.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_toggle_switch"
"$BUILD_DIR/test_toggle_switch"
