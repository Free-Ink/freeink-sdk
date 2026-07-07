/* Compiles the vendored pngle against FreeInkBook's miniz (pngle.c's include
 * is patched to "../miniz/miniz.h" — ESP-IDF ships a ROM miniz.h whose
 * tinfl_decompressor layout differs, and pngle embeds that struct by value).
 * pngle allocates transient scanline buffers with calloc/free at decode time
 * only; that use is bounded by image width and never touches the page-turn
 * path. */
#include "../../third_party/pngle/pngle.c"
