#pragma once

// FreeInk SDK — on-demand memory / cache reclaim.
//
// A small, app-neutral registry of "cache sinks" plus heap reporting, so a
// consumer can free RAM on demand (a control-center "clear caches" action) or
// under memory pressure. Components that hold rebuildable RAM caches — rendered
// pages, decoded images, glyph atlases, parsed-document buffers, PSRAM pools —
// register a sink with an eviction callback; clearCaches() then asks each sink
// (lowest priority first) to release memory until a target is met.
//
// The design mirrors the cache-sink manager pattern common to e-reader
// firmware: a priority-ordered set of evictable caches driven by free-heap
// measurement. Nothing here is board-specific; it is a thin wrapper over the
// ESP-IDF heap-capabilities allocator.

#include <stddef.h>
#include <stdint.h>

#include <functional>

namespace freeink {

// Which heap pool to measure / target.
//   Internal — DMA/task-capable internal SRAM (the scarce pool).
//   Psram    — external SPI RAM (0 on boards without PSRAM).
//   Default  — the allocator's default pool (what ESP.getFreeHeap() reports).
enum class MemPool : uint8_t { Internal, Psram, Default };

// A registrable evictable cache.
struct CacheSink {
  // Stable name (used for logging and as the replace/unregister key). Not copied
  // — pass a string literal or a buffer that outlives the registration.
  const char* name = nullptr;
  // Eviction order: LOWER priority is evicted FIRST. Put cheap-to-rebuild caches
  // (glyphs, decoded images) low; hold expensive/essential state high.
  uint8_t priority = 128;
  // Release memory. `bytesRequested == 0` means "free everything you can".
  // Return the number of bytes actually freed (a best-effort estimate is fine;
  // it only drives when clearCaches() stops early on a byte target).
  std::function<size_t(size_t bytesRequested)> evict;
};

class MemoryManager {
 public:
  static constexpr int kMaxSinks = 12;

  static MemoryManager& instance();

  // Register (or replace, by name) a cache sink. Returns a handle id >= 0, or
  // -1 if the table is full. Safe to call from any component's begin().
  int registerSink(const CacheSink& sink);
  void unregisterSink(int id);
  void unregisterSink(const char* name);

  // --- reporting (bytes) ---
  size_t freeBytes(MemPool pool = MemPool::Default) const;
  size_t largestFreeBlock(MemPool pool = MemPool::Default) const;
  size_t minEverFree(MemPool pool = MemPool::Default) const;  // low-water mark

  // Ask registered sinks (lowest priority first) to release memory until at
  // least `bytesTarget` has been freed; `bytesTarget == 0` purges everything.
  // Returns the total bytes reported freed by the sinks.
  size_t clearCaches(size_t bytesTarget = 0);

  // Control-center "Boost": purge all caches and report the free-heap delta.
  // `freeBefore` / `freeAfter` (either may be null) are filled from `pool`.
  // Returns bytes freed as measured by the heap (freeAfter - freeBefore,
  // clamped at 0), which is the honest number to show the user.
  size_t boost(size_t* freeBefore = nullptr, size_t* freeAfter = nullptr, MemPool pool = MemPool::Default);

 private:
  MemoryManager() = default;

  struct Entry {
    CacheSink sink;
    int id = 0;
    bool used = false;
  };
  Entry _sinks[kMaxSinks];
  int _nextId = 1;
};

}  // namespace freeink
