#!/bin/sh
# Render the FreeInkUI component gallery from the real C++ components.
set -e

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)"
OUT_DIR="${1:-$ROOT_DIR/docs/images}"
BUILD_DIR="${TMPDIR:-/tmp}/freeinkui-gallery"

mkdir -p "$OUT_DIR" "$OUT_DIR/freeinkui-components" "$BUILD_DIR"

c++ -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT_DIR/libs/ui/FreeInkUI/include" \
  "$ROOT_DIR/libs/ui/FreeInkUI/src/FreeInkUI.cpp" \
  "$SCRIPT_DIR/render_gallery.cpp" \
  -o "$BUILD_DIR/render_gallery"

"$BUILD_DIR/render_gallery" "$OUT_DIR"
