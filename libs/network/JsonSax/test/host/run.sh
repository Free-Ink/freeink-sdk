#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/include" "$root/src/StreamingJsonParser.cpp" "$root/test/host/test_prefix.cpp" -o "$build/test_prefix"
"$build/test_prefix"
printf 'JSON bounded-prefix and default overflow tests passed\n'
