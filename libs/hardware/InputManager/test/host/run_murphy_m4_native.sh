#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INPUT_MANAGER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HARDWARE_DIR=$(CDPATH= cd -- "$INPUT_MANAGER_DIR/.." && pwd)
RTC_DIR="$HARDWARE_DIR/Rtc"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-murphy-m4-native.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM
CXX=${CXX:-c++}

"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -DFREEINK_DEVICE_MURPHY_M4=1 -DFREEINK_CAP_RTC=1 -DARDUINO_USB_CDC_ON_BOOT=1 \
  -I"$SCRIPT_DIR/murphy_stubs" -I"$SCRIPT_DIR/picco_stubs" \
  -I"$HARDWARE_DIR/BoardConfig/include" -I"$INPUT_MANAGER_DIR/include" \
  -I"$RTC_DIR/include" -I"$RTC_DIR/test/host/stubs" \
  "$SCRIPT_DIR/test_murphy_m4_native.cpp" "$INPUT_MANAGER_DIR/src/InputManager.cpp" "$RTC_DIR/src/Rtc.cpp" \
  -o "$BUILD_DIR/test_murphy_m4_native"
"$BUILD_DIR/test_murphy_m4_native"