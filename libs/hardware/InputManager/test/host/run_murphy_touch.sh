#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-murphy-touch.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
# murphy_stubs first: its Wire.h shadows the Picco one; the rest of the platform stubs are shared.
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -DFREEINK_DEVICE_MURPHY=1 -DARDUINO_USB_CDC_ON_BOOT=1 \
  -Imurphy_stubs -Ipicco_stubs -I../../include -I../../../BoardConfig/include \
  test_murphy_touch.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_murphy_touch"
"$BUILD_DIR/test_murphy_touch"
