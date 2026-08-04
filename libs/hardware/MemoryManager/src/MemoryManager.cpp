#include "MemoryManager.h"

#include <string.h>

#include "esp_heap_caps.h"

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

}  // namespace freeink
