#!/bin/sh
# Builds and runs the fetchResumable host test against a scripted fake
# WiFiClient (stubs/); https is never exercised.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeink-securenet-host-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra -Werror -Istubs -I../../include test_resumable_fetch.cpp -o "$BUILD_DIR/test_resumable_fetch"
"$BUILD_DIR/test_resumable_fetch"

c++ -std=c++17 -Wall -Wextra -Werror -DFREEINK_NET_WOLFSSL=1 -fsanitize=address,undefined -Istubs -I../../include test_bulk_request.cpp -o "$BUILD_DIR/test_bulk_request"
"$BUILD_DIR/test_bulk_request"
