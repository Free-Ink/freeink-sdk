#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeink-opds-host-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra ${CXXFLAGS:-} -fsanitize=address,undefined \
  -Istubs -I../../include -I../../../JsonSax/include \
  ../../src/*.cpp ../../../JsonSax/src/StreamingJsonParser.cpp \
  test_client.cpp -lexpat -o "$BUILD_DIR/test_client"
"$BUILD_DIR/test_client"
