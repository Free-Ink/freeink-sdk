#pragma once

/* This module needs miniz's streaming inflate for on-read content access. Include this
 * header instead of <miniz.h> so every translation unit sees the same config.
 *
 * Two modes, selected by the embedding project:
 *  - CONTENT_EXTERNAL_MINIZ (device build): bind to the host application's
 *    already-linked miniz instead of compiling a second copy (two copies in one
 *    image corrupt inflate). CrossPoint's lib/miniz/src/MinizConfig.h prefixes
 *    only tinfl_decompress* and mz_crc32/adler32/free with crosspoint_ and
 *    exports the mz_inflate* zlib API under plain names; we mirror that rename
 *    set EXACTLY so the linker binds to its definitions.
 *  - standalone (host tests): compile our own vendored copy under a content_
 *    prefix so the tinfl symbols never bind to a ROM/host miniz copy.
 */

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

#if defined(CONTENT_EXTERNAL_MINIZ)

// Match the host application's MinizConfig.h rename set exactly.
#define tinfl_decompress crosspoint_tinfl_decompress
#define tinfl_decompress_mem_to_heap crosspoint_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem crosspoint_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback crosspoint_tinfl_decompress_mem_to_callback
#define mz_crc32 crosspoint_mz_crc32
#define mz_adler32 crosspoint_mz_adler32
#define mz_free crosspoint_mz_free
// mz_inflate*, tinfl_decompressor_alloc/free, mz_uncompress*, mz_error,
// mz_version, miniz_def_*: the host exports these under plain names — leave
// them unprefixed so we bind to its definitions.

#else  // standalone: our own copy, content_-prefixed.

#define tinfl_decompress content_tinfl_decompress
#define tinfl_decompress_mem_to_heap content_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem content_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback content_tinfl_decompress_mem_to_callback
#define tinfl_decompressor_alloc content_tinfl_decompressor_alloc
#define tinfl_decompressor_free content_tinfl_decompressor_free
#define mz_crc32 content_mz_crc32
#define mz_adler32 content_mz_adler32
#define mz_free content_mz_free
#define mz_error content_mz_error
#define mz_version content_mz_version
#define mz_inflate content_mz_inflate
#define mz_inflateInit content_mz_inflateInit
#define mz_inflateInit2 content_mz_inflateInit2
#define mz_inflateReset content_mz_inflateReset
#define mz_inflateEnd content_mz_inflateEnd
#define mz_uncompress content_mz_uncompress
#define mz_uncompress2 content_mz_uncompress2
#define miniz_def_alloc_func content_miniz_def_alloc_func
#define miniz_def_realloc_func content_miniz_def_realloc_func
#define miniz_def_free_func content_miniz_def_free_func

#endif

#include "../third_party/miniz/miniz.h"
