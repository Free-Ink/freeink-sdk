#include "FontAlloc.h"

#include <stdlib.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"

// PSRAM first, internal heap as fallback. heap_caps_malloc(MALLOC_CAP_SPIRAM)
// returns NULL when no PSRAM is fitted (or it is exhausted), so the fallback
// also covers no-PSRAM boards transparently.
void* fiFontMalloc(size_t size) {
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (p == NULL) p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

void* fiFontRealloc(void* ptr, size_t size) {
  void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
  if (p == NULL && size != 0) p = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

void fiFontFree(void* ptr) { heap_caps_free(ptr); }

#else  // host / non-ESP build

void* fiFontMalloc(size_t size) { return malloc(size); }
void* fiFontRealloc(void* ptr, size_t size) { return realloc(ptr, size); }
void fiFontFree(void* ptr) { free(ptr); }

#endif
