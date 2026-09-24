#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FONT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SDK_ROOT=$(CDPATH= cd -- "$FONT_ROOT/../../.." && pwd)
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-font-render-options-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

FONT_FIXTURE="$SDK_ROOT/libs/book/FreeInkBook/test/fixtures/fonts/DejaVuSans.ttf"
INCLUDES="-I$FONT_ROOT/include -I$FONT_ROOT/third_party/freetype/include -I$FONT_ROOT/third_party/stb"
DEFINES='-DFREEINK_FONT_ENABLE_AUTOHINT=1 -DFREEINK_FONT_ENABLE_NATIVE_HINTING=1 -DFREEINK_FONT_ENABLE_MONOCHROME=1'
SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer'

# The GSUB feature branch adds an optional companion translation unit to
# FtFont.cpp. Keep this runner useful on the standalone hinting branch while
# making the same command link when both branches are combined.
GSUB_SOURCE=
if [ -f "$FONT_ROOT/src/Gsub.cpp" ]; then
  GSUB_SOURCE="$FONT_ROOT/src/Gsub.cpp"
fi
if [ -f "$FONT_ROOT/src/Gpos.cpp" ]; then
  GSUB_SOURCE="$GSUB_SOURCE $FONT_ROOT/src/Gpos.cpp"
fi

if cc --version 2>&1 | grep -qi clang; then
  FRAME_CHECK='-Wframe-larger-than=2048 -Werror=frame-larger-than'
else
  FRAME_CHECK='-Werror=frame-larger-than=2048'
fi

# Host toolchain, not the ESP32 cross-compiler, so this doesn't reproduce
# target-specific codegen. Scoped to the two modules this change vendors
# (autofit, raster) — NOT ft_smooth.c, which independently trips this same
# 2048 limit (a ~16.7 KB frame in gray_convert_glyph, present already in
# main with none of these flags set) and is a pre-existing, unrelated finding
# tracked outside this change, not something introduced here.
# shellcheck disable=SC2086
cc -std=c99 -O2 $INCLUDES $DEFINES $FRAME_CHECK -c "$FONT_ROOT/src/freetype/ft_autofit.c" \
  -o "$BUILD_DIR/autofit-stack-check.o"
# shellcheck disable=SC2086
cc -std=c99 -O2 $INCLUDES $DEFINES $FRAME_CHECK -c "$FONT_ROOT/src/freetype/ft_raster.c" \
  -o "$BUILD_DIR/raster-stack-check.o"

for source in "$FONT_ROOT"/src/freetype/*.c; do
  object="$BUILD_DIR/$(basename "$source" .c).o"
  # shellcheck disable=SC2086
  cc -std=c11 $INCLUDES $DEFINES $SANITIZERS -c "$source" -o "$object"
done

# shellcheck disable=SC2086
c++ -std=c++17 $INCLUDES $DEFINES $SANITIZERS \
  "$FONT_ROOT/src/FtFont.cpp" "$FONT_ROOT/src/FontAlloc.c" $GSUB_SOURCE \
  "$SCRIPT_DIR/FtFontRenderOptionsTest.cpp" "$BUILD_DIR"/ft_*.o \
  -o "$BUILD_DIR/ftfont-render-options-test"
"$BUILD_DIR/ftfont-render-options-test" "$FONT_FIXTURE"

# A consumer that defines none of the three flags must still build AND RUN
# clean: Default rendering still works, and setRenderOptions() reports
# (without crashing on) a request this build can't honor.
for source in "$FONT_ROOT"/src/freetype/*.c; do
  object="$BUILD_DIR/minimal-$(basename "$source" .c).o"
  # shellcheck disable=SC2086
  cc -std=c11 $INCLUDES $SANITIZERS -c "$source" -o "$object"
done
# shellcheck disable=SC2086
c++ -std=c++17 $INCLUDES $SANITIZERS \
  "$FONT_ROOT/src/FtFont.cpp" "$FONT_ROOT/src/FontAlloc.c" $GSUB_SOURCE \
  "$SCRIPT_DIR/FtFontMinimalTest.cpp" "$BUILD_DIR"/minimal-ft_*.o \
  -o "$BUILD_DIR/ftfont-minimal-test"
"$BUILD_DIR/ftfont-minimal-test" "$FONT_FIXTURE"

# The PlatformIO idiom "-D FLAG=0" (explicitly off, distinct from never
# mentioning the flag) must behave identically to the never-mentioned case
# above — confirmed broken once already by using #ifdef instead of #if for
# the compiled-capability constants in FtFont.cpp (kAutohintCompiled etc.),
# which meant a "=0" build still reported monochrome as supported while
# every glyph silently rasterized to nullptr.
EXPLICIT_OFF_DEFINES='-DFREEINK_FONT_ENABLE_AUTOHINT=0 -DFREEINK_FONT_ENABLE_NATIVE_HINTING=0 -DFREEINK_FONT_ENABLE_MONOCHROME=0'
for source in "$FONT_ROOT"/src/freetype/*.c; do
  object="$BUILD_DIR/explicitOff-$(basename "$source" .c).o"
  # shellcheck disable=SC2086
  cc -std=c11 $INCLUDES $EXPLICIT_OFF_DEFINES $SANITIZERS -c "$source" -o "$object"
done
# shellcheck disable=SC2086
c++ -std=c++17 $INCLUDES $EXPLICIT_OFF_DEFINES $SANITIZERS \
  "$FONT_ROOT/src/FtFont.cpp" "$FONT_ROOT/src/FontAlloc.c" $GSUB_SOURCE \
  "$SCRIPT_DIR/FtFontMinimalTest.cpp" "$BUILD_DIR"/explicitOff-ft_*.o \
  -o "$BUILD_DIR/ftfont-explicit-off-test"
"$BUILD_DIR/ftfont-explicit-off-test" "$FONT_FIXTURE"

# The actual regression check for "compiling AUTOHINT in never changes a
# Default caller's output": a per-instance assertion inside ONE binary can't
# catch a difference that only shows up BETWEEN two build configurations, so
# this hashes Default's rasterized output across many sizes/codepoints and
# compares an AUTOHINT+MONOCHROME build (no native hinting) against the
# minimal one. These two MUST match: nothing about Default should ever prefer
# the auto-hinter just because the module became available (FT_LOAD_NO_AUTOHINT
# blocks exactly that fallback) — this is the original, real bug (advances
# measurably changed, 559 -> 563 on a real font, before that flag was added).
#
# NATIVE_HINTING is deliberately excluded from this comparison: once a native
# TrueType hinter is compiled in AND registered, FreeType uses it automatically
# for any hinted (non-NO_HINTING) load on a font that has real hint
# instructions — there is no FT_LOAD_* flag that means "hint, but only with a
# hinter I didn't just make available". A project that defines
# FREEINK_FONT_ENABLE_NATIVE_HINTING is making a build-time choice to enable
# real bytecode hinting, and Default rendering may legitimately look different
# afterward as a direct, expected consequence — that is not the same claim as
# "compiling in a module silently changes output for a build that never asked
# for it", which is what this check (and the original bug) is actually about.
# shellcheck disable=SC2086
NO_NATIVE_DEFINES='-DFREEINK_FONT_ENABLE_AUTOHINT=1 -DFREEINK_FONT_ENABLE_MONOCHROME=1'
for source in "$FONT_ROOT"/src/freetype/*.c; do
  object="$BUILD_DIR/noNative-$(basename "$source" .c).o"
  # shellcheck disable=SC2086
  cc -std=c11 $INCLUDES $NO_NATIVE_DEFINES $SANITIZERS -c "$source" -o "$object"
done
# shellcheck disable=SC2086
c++ -std=c++17 $INCLUDES $NO_NATIVE_DEFINES $SANITIZERS \
  "$FONT_ROOT/src/FtFont.cpp" "$FONT_ROOT/src/FontAlloc.c" $GSUB_SOURCE \
  "$SCRIPT_DIR/FtFontDefaultParityHash.cpp" "$BUILD_DIR"/noNative-ft_*.o \
  -o "$BUILD_DIR/parity-no-native"
# shellcheck disable=SC2086
c++ -std=c++17 $INCLUDES $SANITIZERS \
  "$FONT_ROOT/src/FtFont.cpp" "$FONT_ROOT/src/FontAlloc.c" $GSUB_SOURCE \
  "$SCRIPT_DIR/FtFontDefaultParityHash.cpp" "$BUILD_DIR"/minimal-ft_*.o \
  -o "$BUILD_DIR/parity-minimal"
HASH_NO_NATIVE=$("$BUILD_DIR/parity-no-native" "$FONT_FIXTURE")
HASH_MINIMAL=$("$BUILD_DIR/parity-minimal" "$FONT_FIXTURE")
echo "Default output hash — AUTOHINT+MONOCHROME (no native): $HASH_NO_NATIVE, minimal: $HASH_MINIMAL"
if [ "$HASH_NO_NATIVE" != "$HASH_MINIMAL" ]; then
  echo "FAIL: compiling in AUTOHINT/MONOCHROME (without NATIVE_HINTING) changed HintingMode::Default's output" >&2
  exit 1
fi

echo "FtFont RenderOptions host test: OK"
