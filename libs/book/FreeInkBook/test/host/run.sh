#!/bin/sh
# Builds and runs the FreeInkBook host tests. No device or PlatformIO needed —
# the library is freestanding C++17. Fixture EPUBs are assembled here with the
# system `zip` (mimetype stored first, per the EPUB OCF spec).
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeinkbook-tests"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/fixtures" "$BUILD_DIR/obj"

# --- fixtures ---------------------------------------------------------------
cp -R ../fixtures/minimal "$BUILD_DIR/fixtures/minimal"
cp -R ../fixtures/ncxonly "$BUILD_DIR/fixtures/ncxonly"
python3 ../fixtures/gen_big_chapter.py "$BUILD_DIR/fixtures/minimal/OEBPS/text/ch2.xhtml"
mkdir -p "$BUILD_DIR/fixtures/minimal/OEBPS/images"
python3 ../fixtures/gen_test_image.py "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.png"
# JPEG variant via macOS sips (or ImageMagick if available).
if command -v sips >/dev/null 2>&1; then
  sips -s format jpeg "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.png" \
    --out "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.jpg" >/dev/null
elif command -v convert >/dev/null 2>&1; then
  convert "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.png" \
    "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.jpg"
else
  # No JPEG converter — reuse the PNG bytes; the JPEG-specific test will skip.
  cp "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.png" \
     "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.jpg"
fi

# make_epub <stage-dir> <out.epub> <compression-flag>
make_epub() {
  (cd "$1" \
    && zip -X -q "$3" "$BUILD_DIR/fixtures/$2" mimetype \
    && zip -X -q -r "$3" "$BUILD_DIR/fixtures/$2" META-INF OEBPS)
}
make_epub "$BUILD_DIR/fixtures/minimal" minimal.epub -9
make_epub "$BUILD_DIR/fixtures/minimal" stored.epub -0
make_epub "$BUILD_DIR/fixtures/ncxonly" ncxonly.epub -9
head -c 4096 /dev/zero > "$BUILD_DIR/fixtures/garbage.bin"

# --- build ------------------------------------------------------------------
INCLUDES="-I../../include -I../../third_party/expat -I../../third_party/miniz -I../../third_party/libunibreak -I../../third_party/pngle -I../../third_party/tjpgd -I../../third_party/stb"
CC_FLAGS="-O1 -std=c99 $INCLUDES"
for src in miniz_impl expat_xmlparse expat_xmlrole expat_xmltok unibreak_impl pngle_impl tjpgd_impl; do
  cc $CC_FLAGS -c "../../src/vendor/$src.c" -o "$BUILD_DIR/obj/$src.o"
done

CORE_SRCS="../../src/FreeInkBook.cpp ../../src/epub/ZipCatalog.cpp ../../src/epub/XmlSax.cpp \
  ../../src/epub/PackageParsers.cpp ../../src/epub/ImageProbe.cpp ../../src/text/EntityFilter.cpp \
  ../../src/text/Hyphenator.cpp ../../src/css/Css.cpp ../../src/layout/ChapterLayout.cpp \
  ../../src/cache/PageCache.cpp ../../src/render/ImageRenderer.cpp ../../src/render/TtfFont.cpp ../../src/render/PageRenderer.cpp"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_freeinkbook.cpp "$BUILD_DIR"/obj/*.o \
  -o "$BUILD_DIR/test_freeinkbook"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_layout.cpp "$BUILD_DIR"/obj/*.o \
  -o "$BUILD_DIR/test_layout"

# The layout suite also builds and runs under the SMALL and LARGE profiles
# (BookProfile.h) so neither tier bit-rots.
c++ -std=c++17 -Wall -Wextra -Werror -DFREEINK_BOOK_SMALL=1 $INCLUDES \
  $CORE_SRCS test_layout.cpp "$BUILD_DIR"/obj/*.o \
  -o "$BUILD_DIR/test_layout_small"

c++ -std=c++17 -Wall -Wextra -Werror -DFREEINK_BOOK_LARGE=1 $INCLUDES \
  $CORE_SRCS test_layout.cpp "$BUILD_DIR"/obj/*.o \
  -o "$BUILD_DIR/test_layout_large"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_cache.cpp "$BUILD_DIR"/obj/*.o \
  -o "$BUILD_DIR/test_cache"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_font.cpp "$BUILD_DIR"/obj/*.o \
  -o "$BUILD_DIR/test_font"

python3 ../../tools/hyphc.py ../../third_party/hyphen-patterns/hyph-en-us.pat.txt \
  "$BUILD_DIR/hyph-en-us.fibh"
python3 ../../tools/hyphc.py ../fixtures/hyph-test-ru.pat.txt \
  "$BUILD_DIR/hyph-test-ru.fibh"

mkdir -p "$BUILD_DIR/cache"
"$BUILD_DIR/test_freeinkbook" "$BUILD_DIR/fixtures"
"$BUILD_DIR/test_layout" "$BUILD_DIR/fixtures" "$BUILD_DIR/hyph-en-us.fibh" "$BUILD_DIR/hyph-test-ru.fibh"
"$BUILD_DIR/test_layout_small" "$BUILD_DIR/fixtures" "$BUILD_DIR/hyph-en-us.fibh" "$BUILD_DIR/hyph-test-ru.fibh"
"$BUILD_DIR/test_layout_large" "$BUILD_DIR/fixtures" "$BUILD_DIR/hyph-en-us.fibh" "$BUILD_DIR/hyph-test-ru.fibh"
"$BUILD_DIR/test_cache" "$BUILD_DIR/fixtures" "$BUILD_DIR/cache"
"$BUILD_DIR/test_font" "$BUILD_DIR/fixtures" ../fixtures/fonts/DejaVuSans.ttf
