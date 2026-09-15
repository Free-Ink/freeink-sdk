#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-haptic.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
for mode in enabled disabled absent; do
  case "$mode" in
    enabled) FLAGS="-DFREEINK_DEVICE_METALIO_EINK4=1" ;;
    disabled) FLAGS="-DFREEINK_DEVICE_METALIO_EINK4=1 -DFREEINK_CAP_HAPTIC=0" ;;
    absent) FLAGS="-DFREEINK_DEVICE_X4=1" ;;
  esac
  c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter $FLAGS \
    -Istubs -I../../../InputManager/test/host/metalio_stubs \
    -I../../include -I../../../BoardConfig/include -I../../../InputManager/include \
    test_haptic.cpp ../../src/HapticManager.cpp ../../../InputManager/src/InputManager.cpp -o "$BUILD_DIR/$mode"
  "$BUILD_DIR/$mode"
  if [ "$mode" = enabled ]; then "$BUILD_DIR/$mode" mutex-fail; fi
done
