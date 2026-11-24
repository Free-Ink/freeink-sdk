#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-usb-storage.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -pthread ${CXXFLAGS:-} -Istubs -I../../include \
  test_usb_storage.cpp ../../src/UsbMassStorage.cpp -o "$BUILD_DIR/test_usb_storage"
"$BUILD_DIR/test_usb_storage"
