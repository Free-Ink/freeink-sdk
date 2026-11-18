#!/bin/sh
# Production parser tests over scripted transports; no real TLS or sockets.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeink-securenet-host-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} -Istubs -I../../include test_resumable_fetch.cpp -o "$BUILD_DIR/test_resumable_fetch"
"$BUILD_DIR/test_resumable_fetch"

c++ -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} -DFREEINK_NET_WOLFSSL=1 -fsanitize=address,undefined -Istubs -I../../include test_bulk_request.cpp -o "$BUILD_DIR/test_bulk_request"
"$BUILD_DIR/test_bulk_request"

c++ -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} -Istubs -I../../include test_http_headers.cpp -o "$BUILD_DIR/test_http_headers"
"$BUILD_DIR/test_http_headers"
