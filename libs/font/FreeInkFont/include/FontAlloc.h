#pragma once

// FreeInkFont — PSRAM-preferring allocator.
//
// Font data (FreeType faces, glyph rasterization, streaming caches, glyph
// bitmap arenas) is large but latency-tolerant, so on a device with external
// PSRAM it belongs there — freeing scarce internal SRAM for the framebuffer,
// task stacks, and DMA buffers. These helpers allocate from PSRAM when it is
// present and fall back to the internal heap otherwise (a no-PSRAM device such
// as an ESP32-S3 N8, or a host build). Detection is at runtime via the ESP-IDF
// heap-caps API, so no build-time PSRAM flag is required; on non-ESP builds
// these are plain malloc/realloc/free.
//
// free() works regardless of which region the block came from.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* fiFontMalloc(size_t size);
void* fiFontRealloc(void* ptr, size_t size);
void fiFontFree(void* ptr);

#ifdef __cplusplus
}
#endif
