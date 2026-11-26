#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
static int calls, fail_at, live;
static bool resource_step(void) { return ++calls != fail_at; }
void* __real_malloc(size_t);
void __real_free(void*);
void* resource_allocate(size_t size, size_t alignment) {
  assert(alignment <= 16);
  if (!resource_step()) return NULL;
  void* p = __real_malloc(size);
  assert(p);
  ++live;
  return p;
}
void resource_release(void* p) {
  if (p) {
    --live;
    __real_free(p);
  }
}
void* __wrap_malloc(size_t size) { return resource_allocate(size, 1); }
void* __wrap_calloc(size_t count, size_t size) {
  void* p = resource_allocate(count * size, 1);
  if (p) memset(p, 0, count * size);
  return p;
}
void __wrap_free(void* p) { resource_release(p); }
