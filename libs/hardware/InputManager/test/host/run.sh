#!/bin/sh
# Builds and runs gesture math and raw button tests.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeink-input-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra -Werror test_multitouch_gesture_math.cpp -o "$BUILD_DIR/test_multitouch_gesture_math"
"$BUILD_DIR/test_multitouch_gesture_math"
c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter \
  -DFREEINK_DEVICE_ONEPAGE=1 -DFREEINK_CAP_TOUCH=0 \
  -Imetalio_stubs -I../../include -I../../../BoardConfig/include \
  test_onepage_buttons.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_onepage_buttons"
"$BUILD_DIR/test_onepage_buttons"
