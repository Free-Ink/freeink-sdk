/* FreeInkBook only needs miniz's low-level streaming inflate (tinfl). The
 * archive, deflate, stdio, and zlib-compatibility layers are compiled out so
 * the vendored library stays small and never touches the filesystem or clock.
 * Include this header instead of <miniz.h> so every translation unit sees the
 * same configuration. */
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

// Include the vendored miniz by relative path: ESP-IDF ships a ROM miniz.h
// with the SAME include guard but a different (TINFL_LESS_MEMORY) struct
// layout — resolving <miniz.h> through the platform include path would
// silently compile against the wrong structures.
#include "../../third_party/miniz/miniz.h"
