#include "MemoryManager.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(ARDUINO)
#include <Arduino.h>  // Serial (optional logging)
#endif

namespace freeink {
namespace {

uint32_t capsFor(MemPool pool) {
  switch (pool) {
    case MemPool::Internal: return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    case MemPool::Psram: return MALLOC_CAP_SPIRAM;
    case MemPool::Default:
    default: return MALLOC_CAP_DEFAULT;
  }
}

#if defined(ARDUINO) && defined(ENABLE_SERIAL_LOG)
#define MEM_LOGF(fmt, ...)                                          \
  do {                                                              \
    if (Serial) Serial.printf("[%lu] [MEM] " fmt "\n", millis(), ##__VA_ARGS__); \
  } while (0)
#else
#define MEM_LOGF(fmt, ...) \
  do {                     \
  } while (0)
#endif

}  // namespace

MemoryManager& MemoryManager::instance() {
  static MemoryManager mgr;
  return mgr;
}

int MemoryManager::registerSink(const CacheSink& sink) {
  if (!sink.evict) return -1;
  // Replace an existing sink with the same name.
  if (sink.name) {
    for (auto& e : _sinks) {
      if (e.used && e.sink.name && strcmp(e.sink.name, sink.name) == 0) {
        const int id = e.id;
        e.sink = sink;
        return id;
      }
    }
  }
  for (auto& e : _sinks) {
    if (!e.used) {
      e.sink = sink;
      e.id = _nextId++;
      e.used = true;
      return e.id;
    }
  }
  return -1;  // table full
}

void MemoryManager::unregisterSink(int id) {
  for (auto& e : _sinks) {
    if (e.used && e.id == id) {
      e = Entry{};
      return;
    }
  }
}

void MemoryManager::unregisterSink(const char* name) {
  if (!name) return;
  for (auto& e : _sinks) {
    if (e.used && e.sink.name && strcmp(e.sink.name, name) == 0) {
      e = Entry{};
      return;
    }
  }
}

size_t MemoryManager::freeBytes(MemPool pool) const { return heap_caps_get_free_size(capsFor(pool)); }

size_t MemoryManager::largestFreeBlock(MemPool pool) const { return heap_caps_get_largest_free_block(capsFor(pool)); }

size_t MemoryManager::minEverFree(MemPool pool) const { return heap_caps_get_minimum_free_size(capsFor(pool)); }

size_t MemoryManager::clearCaches(size_t bytesTarget) {
  // Evict lowest-priority sinks first. Selection-scan the table (kMaxSinks is
  // tiny) so we don't need it kept sorted on insert.
  size_t freedTotal = 0;
  bool done[kMaxSinks] = {};
  for (;;) {
    // Find the used, not-yet-visited sink with the lowest priority.
    int pick = -1;
    for (int i = 0; i < kMaxSinks; i++) {
      if (!_sinks[i].used || done[i]) continue;
      if (pick < 0 || _sinks[i].sink.priority < _sinks[pick].sink.priority) pick = i;
    }
    if (pick < 0) break;
    done[pick] = true;

    const size_t remaining = (bytesTarget == 0) ? 0 : (bytesTarget > freedTotal ? bytesTarget - freedTotal : 0);
    if (bytesTarget != 0 && remaining == 0) break;  // target already met

    if (_sinks[pick].sink.evict) freedTotal += _sinks[pick].sink.evict(remaining);
  }
  return freedTotal;
}

size_t MemoryManager::boost(size_t* freeBefore, size_t* freeAfter, MemPool pool) {
  const size_t before = freeBytes(pool);
  if (freeBefore) *freeBefore = before;

  clearCaches(0);  // purge everything

  const size_t after = freeBytes(pool);
  if (freeAfter) *freeAfter = after;

#if defined(ARDUINO) && defined(ENABLE_SERIAL_LOG)
  if (Serial) {
    Serial.printf("[%lu] [MEM] Boost: freed %u bytes (%u -> %u free)\n", millis(),
                  static_cast<unsigned>(after > before ? after - before : 0), static_cast<unsigned>(before),
                  static_cast<unsigned>(after));
  }
#endif

  return after > before ? after - before : 0;
}

void MemoryManager::rebootBoost() {
  clearCaches(0);  // let sinks run teardown side effects (flush-to-disk etc.)
  MEM_LOGF("rebootBoost: restarting");
#if defined(ARDUINO) && defined(ENABLE_SERIAL_LOG)
  if (Serial) Serial.flush();
#endif
  esp_restart();
}

void MemoryManager::setWatermarks(uint8_t softPct, uint8_t hardPct) {
  const uint32_t caps = capsFor(MemPool::Internal);
  _internalTotal = heap_caps_get_total_size(caps);
  _softUsed = _internalTotal * softPct / 100;
  _hardUsed = _internalTotal * hardPct / 100;
  const size_t reserved = _internalTotal - heap_caps_get_free_size(caps);
  (void)reserved;
  MEM_LOGF("init: internal_total=%u soft=%u(%u%%) hard=%u(%u%%) reserved=%u",
           static_cast<unsigned>(_internalTotal), static_cast<unsigned>(_softUsed), softPct,
           static_cast<unsigned>(_hardUsed), hardPct, static_cast<unsigned>(reserved));
}

MemPressure MemoryManager::pressure() const {
  if (_internalTotal == 0) return MemPressure::None;
  const size_t used = _internalTotal - heap_caps_get_free_size(capsFor(MemPool::Internal));
  if (used >= _hardUsed) return MemPressure::Hard;
  if (used >= _softUsed) return MemPressure::Soft;
  return MemPressure::None;
}

size_t MemoryManager::relievePressure() {
  const MemPressure p = pressure();
  if (p == MemPressure::None) return 0;
  if (p == MemPressure::Hard) {
    const size_t freed = clearCaches(0);
    MEM_LOGF("relievePressure: hard, purged all sinks, freed=%u", static_cast<unsigned>(freed));
    return freed;
  }
  const size_t used = _internalTotal - heap_caps_get_free_size(capsFor(MemPool::Internal));
  const size_t freed = clearCaches(used > _softUsed ? used - _softUsed : 1);
  MEM_LOGF("relievePressure: soft, freed=%u", static_cast<unsigned>(freed));
  return freed;
}

bool MemoryManager::ensureFree(size_t bytes, MemPool pool) {
  const size_t have = freeBytes(pool);
  if (have >= bytes) return true;
  clearCaches(bytes - have);
  return freeBytes(pool) >= bytes;
}

TaskStack MemoryManager::acquireTaskStack(const char* slot, const char* owner, size_t stackBytes) {
  (void)owner;  // logging only
  if (!slot || stackBytes == 0) return {};
  StackSlot* s = nullptr;
  for (auto& e : _stacks) {
    if (e.name && strcmp(e.name, slot) == 0) {
      s = &e;
      break;
    }
  }
  if (!s) {
    for (auto& e : _stacks) {
      if (!e.name) {
        e = StackSlot{};
        e.name = slot;
        s = &e;
        break;
      }
    }
  }
  if (!s) {
    MEM_LOGF("acquireStaticTaskStack: slot table full (slot=%s)", slot);
    return {};
  }
  if (s->owned) {
    MEM_LOGF("acquireStaticTaskStack: slot=%s already owned", slot);
    return {};
  }
  const uint32_t caps = capsFor(MemPool::Internal);
  if (s->stack && s->stackBytes < stackBytes) {
    heap_caps_free(s->stack);
    s->stack = nullptr;
    s->stackBytes = 0;
  }
  if (!s->stack) {
    s->stack = heap_caps_malloc(stackBytes, caps);
    if (!s->stack) {
      MEM_LOGF("acquireStaticTaskStack: slot=%s alloc %u B failed", slot, static_cast<unsigned>(stackBytes));
      return {};
    }
    s->stackBytes = stackBytes;
  }
  if (!s->tcb) {
    s->tcb = heap_caps_malloc(sizeof(StaticTask_t), caps);
    if (!s->tcb) {
      heap_caps_free(s->stack);
      *s = StackSlot{};
      return {};
    }
  }
  s->owned = true;
  MEM_LOGF("acquireStaticTaskStack: slot=%s owner=%s stack=%u B", slot, owner ? owner : "?",
           static_cast<unsigned>(s->stackBytes));
  return {s->stack, s->tcb, s->stackBytes};
}

void MemoryManager::releaseTaskStack(const char* slot) {
  if (!slot) return;
  for (auto& e : _stacks) {
    if (e.name && strcmp(e.name, slot) == 0 && e.owned) {
      e.owned = false;
      MEM_LOGF("releaseStaticTaskStack: slot=%s", slot);
      return;
    }
  }
}

int MemoryManager::arenaCreate(size_t bytes, MemPool pool) {
  if (bytes == 0) return -1;
  for (int i = 0; i < kMaxArenas; i++) {
    if (_arenas[i].live) continue;
    uint8_t* base = static_cast<uint8_t*>(heap_caps_malloc(bytes, capsFor(pool)));
    if (!base) return -1;
    _arenas[i] = Arena{base, bytes, 0, true};
    MEM_LOGF("arenaInit ok id=%d bytes=%u", i, static_cast<unsigned>(bytes));
    return i;
  }
  return -1;
}

void* MemoryManager::arenaAlloc(int id, size_t bytes, size_t align) {
  if (id < 0 || id >= kMaxArenas || !_arenas[id].live || align == 0 || (align & (align - 1))) return nullptr;
  Arena& a = _arenas[id];
  const size_t start = (a.used + align - 1) & ~(align - 1);
  if (start > a.size || bytes > a.size - start) return nullptr;
  a.used = start + bytes;
  return a.base + start;
}

void MemoryManager::arenaReset(int id) {
  if (id < 0 || id >= kMaxArenas || !_arenas[id].live) return;
  _arenas[id].used = 0;
}

void MemoryManager::arenaDestroy(int id) {
  if (id < 0 || id >= kMaxArenas || !_arenas[id].live) return;
  heap_caps_free(_arenas[id].base);
  _arenas[id] = Arena{};
}

size_t MemoryManager::arenaRemaining(int id) const {
  if (id < 0 || id >= kMaxArenas || !_arenas[id].live) return 0;
  return _arenas[id].size - _arenas[id].used;
}

}  // namespace freeink
