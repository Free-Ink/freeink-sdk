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
// The design mirrors the memory-manager pattern common to e-reader firmware:
// a priority-ordered set of evictable caches driven by free-heap measurement,
// soft/hard used-bytes watermarks on the scarce internal pool, named static
// task-stack slots that big-stack transient tasks (radio bring-up, sync)
// borrow and return without fragmenting the heap, small fixed bump arenas for
// phase-scoped scratch, and a reboot-flavored "boost" for when only a clean
// software restart can undo fragmentation. Nothing here is board-specific; it
// is a thin wrapper over the ESP-IDF heap-capabilities allocator and FreeRTOS
// static-task primitives.

#include <stddef.h>
#include <stdint.h>

#include <functional>

namespace freeink {

// Which heap pool to measure / target.
//   Internal — DMA/task-capable internal SRAM (the scarce pool).
//   Psram    — external SPI RAM (0 on boards without PSRAM).
//   Default  — the allocator's default pool (what ESP.getFreeHeap() reports).
enum class MemPool : uint8_t { Internal, Psram, Default };

// Heap-pressure level against the watermarks set by setWatermarks().
//   None — used bytes below the soft watermark.
//   Soft — above soft: rebuildable caches should start giving memory back.
//   Hard — above hard: evict aggressively before allocations start failing.
enum class MemPressure : uint8_t { None, Soft, Hard };

// A borrowed static task stack (see acquireTaskStack). `stack`/`tcb` are owned
// by the manager and stay allocated across release for fragmentation-free
// reuse; pass both to xTaskCreateStatic().
struct TaskStack {
  void* stack = nullptr;  // stackBytes of internal (task-capable) RAM
  void* tcb = nullptr;    // a StaticTask_t, opaque here to keep the header light
  size_t stackBytes = 0;
};

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
  static constexpr int kMaxStackSlots = 6;
  static constexpr int kMaxArenas = 4;

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

  // Control-center "Boost" (reboot flavor): purge caches, then perform a clean
  // software CPU restart — the only way to fully undo internal-heap
  // fragmentation. Shut down anything with external state first (stop wifi/BLE,
  // unmount SD, put the display driver to sleep); this call does not know about
  // those subsystems. Does not return.
  [[noreturn]] void rebootBoost();

  // --- heap-pressure watermarks ---
  // Arm soft/hard used-bytes watermarks as percentages of the internal pool's
  // total size, and record how much was already in use ("reserved") at the time
  // of the call — so call it once at end of app init. Defaults mirror common
  // e-reader firmware (soft 60%, hard 75% of internal RAM).
  void setWatermarks(uint8_t softPct = 60, uint8_t hardPct = 75);

  // Current pressure of the internal pool against the armed watermarks.
  // Always None before setWatermarks().
  MemPressure pressure() const;

  // If pressure is Soft or Hard, evict sinks (lowest priority first) until used
  // bytes drop back under the soft watermark; Hard additionally purges every
  // sink outright. Returns bytes reported freed. Cheap no-op at None — safe to
  // call periodically or before a large allocation.
  size_t relievePressure();

  // Evict sinks until at least `bytes` are free in `pool` (or all sinks are
  // spent). Returns true if the pool ended with >= `bytes` free.
  bool ensureFree(size_t bytes, MemPool pool = MemPool::Default);

  // --- static task-stack slots ---
  // Borrow a named, preallocated internal-RAM stack (+ TCB) for
  // xTaskCreateStatic(), so short-lived big-stack tasks (wifi/radio bring-up,
  // OTA, sync) don't repeatedly carve 4–8 KB holes in the internal heap. The
  // slot's buffer is allocated on first acquire and kept across release for
  // reuse; a later acquire with a larger size reallocates it (only possible
  // while released). Returns {nullptr,...} if the slot is currently owned, the
  // table is full, or allocation fails. `owner` is for logging only.
  TaskStack acquireTaskStack(const char* slot, const char* owner, size_t stackBytes);

  // Return a slot after the borrowing task has been deleted. The buffer stays
  // allocated for the next acquire.
  void releaseTaskStack(const char* slot);

  // --- fixed arenas ---
  // Create a bump arena of `bytes` in `pool` for phase-scoped allocations that
  // are freed all at once (layout scratch, decode scratch). Returns an arena id
  // >= 0, or -1 on failure.
  int arenaCreate(size_t bytes, MemPool pool = MemPool::Internal);
  // Bump-allocate from an arena (align must be a power of two). Returns nullptr
  // when the arena is exhausted — there is no per-allocation free.
  void* arenaAlloc(int id, size_t bytes, size_t align = 4);
  // Reset the arena to empty (invalidates everything allocated from it).
  void arenaReset(int id);
  // Free the arena's backing memory and recycle the id.
  void arenaDestroy(int id);
  size_t arenaRemaining(int id) const;

 private:
  MemoryManager() = default;

  struct Entry {
    CacheSink sink;
    int id = 0;
    bool used = false;
  };
  Entry _sinks[kMaxSinks];
  int _nextId = 1;

  struct StackSlot {
    const char* name = nullptr;  // slot key (string literal expected, not copied)
    void* stack = nullptr;
    void* tcb = nullptr;
    size_t stackBytes = 0;
    bool owned = false;
  };
  StackSlot _stacks[kMaxStackSlots];

  struct Arena {
    uint8_t* base = nullptr;
    size_t size = 0;
    size_t used = 0;
    bool live = false;
  };
  Arena _arenas[kMaxArenas];

  // Watermarks, as used-bytes thresholds on the internal pool. 0 = not armed.
  size_t _internalTotal = 0;
  size_t _softUsed = 0;
  size_t _hardUsed = 0;
};

}  // namespace freeink
