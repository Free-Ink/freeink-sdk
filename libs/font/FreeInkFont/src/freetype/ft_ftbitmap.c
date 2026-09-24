/* FreeInkFont build wrapper: compile the vendored FreeType amalgam with the
 * library build macro set, so PlatformIO compiles it as part of this lib while
 * FtFont.cpp stays a plain public-API consumer. Generated; do not edit. */
#define FT2_BUILD_LIBRARY
#include "../../third_party/freetype/src/base/ftbitmap.c"
