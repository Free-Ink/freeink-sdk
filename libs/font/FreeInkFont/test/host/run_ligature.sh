#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FONT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SDK_ROOT=$(CDPATH= cd -- "$FONT_ROOT/../../.." && pwd)
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-font-ligature-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

FONT_FIXTURE="$SDK_ROOT/libs/book/FreeInkBook/test/fixtures/fonts/DejaVuSans.ttf"
INCLUDES="-I$FONT_ROOT/include -I$FONT_ROOT/third_party/freetype/include -I$FONT_ROOT/third_party/stb"
SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer'

for source in "$FONT_ROOT"/src/freetype/*.c; do
  object="$BUILD_DIR/$(basename "$source" .c).o"
  # shellcheck disable=SC2086
  cc -std=c11 $INCLUDES $SANITIZERS -c "$source" -o "$object"
done

# shellcheck disable=SC2086
c++ -std=c++17 $INCLUDES $SANITIZERS \
  "$FONT_ROOT/src/FtFont.cpp" "$FONT_ROOT/src/FontAlloc.c" "$FONT_ROOT/src/Gsub.cpp" "$FONT_ROOT/src/Gpos.cpp" \
  "$SCRIPT_DIR/FtFontLigatureTest.cpp" "$BUILD_DIR"/ft_*.o \
  -o "$BUILD_DIR/ftfont-ligature-test"
"$BUILD_DIR/ftfont-ligature-test" "$FONT_FIXTURE"

echo "FtFont ligature host test: OK"
