#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-picco-light.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -DFREEINK_DEVICE_PICCO=1 \
  -Istubs -I../../include -I../../../BoardConfig/include \
  test_picco.cpp ../../src/FrontlightManager.cpp -o "$BUILD_DIR/test_picco"
"$BUILD_DIR/test_picco"
