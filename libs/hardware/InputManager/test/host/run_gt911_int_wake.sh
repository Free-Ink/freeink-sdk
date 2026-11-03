#!/bin/sh
# X4 Pro GT911 INT wake against a model controller, with and without
# FREEINK_GT911_INT_WAKE.
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-gt911.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
for flag in 0 1; do
  c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter \
    -DFREEINK_DEVICE_X4PRO=1 -DARDUINO_USB_CDC_ON_BOOT=1 -DFREEINK_GT911_INT_WAKE=$flag \
    -Igt911_stubs -Ipicco_stubs -I../../include -I../../../BoardConfig/include \
    test_gt911_int_wake.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_gt911_int_wake_$flag"
  "$BUILD_DIR/test_gt911_int_wake_$flag"
done
