#pragma once

// FreeInk SDK — arena (bump) allocator for FreeInkBook.
//
// All engine memory comes from arenas the application sizes up front (on
// ESP32-class targets typically placed in PSRAM). Allocation is a pointer
// bump; there is no per-object free, so fragmentation cannot accumulate.
// Arenas reset wholesale at book/chapter boundaries and report a high-water
// mark so host tests can assert peak-memory ceilings.
//
// Freestanding C++17 — no Arduino or ESP-IDF dependency.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace freeink {
namespace book {

class Arena {
 public:
  Arena() = default;
  Arena(void* buffer, size_t capacity) { init(buffer, capacity); }

  void init(void* buffer, size_t capacity) {
    base_ = static_cast<uint8_t*>(buffer);
    capacity_ = capacity;
    used_ = 0;
    highWater_ = 0;
    failedAllocSize_ = 0;
  }

  // Discards everything allocated from this arena.
  void reset() { used_ = 0; }

  // Saves/restores the allocation cursor so short-lived scratch (parse
  // buffers, inflate windows) can be reclaimed without a full reset. Restore
  // order must nest: release() only what was allocated after the mark.
  size_t mark() const { return used_; }
  void release(size_t marked) {
    if (marked <= used_) used_ = marked;
  }

  // Returns nullptr when the arena is exhausted; never aborts.
  void* alloc(size_t size, size_t align = alignof(max_align_t)) {
    if (base_ == nullptr || align == 0 || (align & (align - 1)) != 0) return nullptr;
    const size_t aligned = (used_ + (align - 1)) & ~(align - 1);
    if (aligned < used_ || size > capacity_ || aligned > capacity_ - size) {
      if (size > failedAllocSize_) failedAllocSize_ = size;
      return nullptr;
    }
    used_ = aligned + size;
    if (used_ > highWater_) highWater_ = used_;
    return base_ + aligned;
  }

  template <typename T>
  T* allocArray(size_t count) {
    if (count != 0 && count > SIZE_MAX / sizeof(T)) return nullptr;
    return static_cast<T*>(alloc(count * sizeof(T), alignof(T)));
  }

  // Copies a NUL-terminated string into the arena.
  const char* strdup(const char* s) {
    if (s == nullptr) return nullptr;
    return strdup(s, strlen(s));
  }

  const char* strdup(const char* s, size_t len) {
    if (s == nullptr) return nullptr;
    char* copy = static_cast<char*>(alloc(len + 1, 1));
    if (copy == nullptr) return nullptr;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
  }

  size_t used() const { return used_; }
  size_t capacity() const { return capacity_; }
  size_t highWater() const { return highWater_; }
  // Diagnostics: the largest allocation this arena has REFUSED since init()
  // (0 = none). On an OutOfMemory build this names the request that missed,
  // which highWater() alone cannot — it records the last success, so an
  // arena can fail with headroom still showing.
  size_t failedAllocSize() const { return failedAllocSize_; }

 private:
  uint8_t* base_ = nullptr;
  size_t capacity_ = 0;
  size_t used_ = 0;
  size_t highWater_ = 0;
  size_t failedAllocSize_ = 0;
};

}  // namespace book
}  // namespace freeink
