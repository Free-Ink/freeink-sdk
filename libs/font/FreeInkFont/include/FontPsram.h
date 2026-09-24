#pragma once

// FreeInkFont — C++ helpers that place font containers in PSRAM (when present),
// backed by the FontAlloc PSRAM-preferring allocator. See FontAlloc.h.
//
//   PsramVector<T>            — std::vector routed to PSRAM.
//   psramNewArray<T>(n)       — new[]-style resident buffer (nullptr on OOM,
//                               like `new (std::nothrow) T[n]`); T must be a
//                               trivial type (no constructor is run).
//   psramDeleteArray(p)       — free a psramNewArray buffer.

#include <cstddef>
#include <cstdlib>
#include <vector>

#include "FontAlloc.h"

namespace freeink {
namespace font {

template <typename T>
struct PsramAlloc {
  using value_type = T;
  PsramAlloc() noexcept = default;
  template <typename U>
  PsramAlloc(const PsramAlloc<U>&) noexcept {}

  T* allocate(std::size_t n) {
    void* p = fiFontMalloc(n * sizeof(T));
    // std::vector requires a non-null block; matching the default allocator's
    // throw-or-die contract (and this project's -fno-exceptions builds), abort
    // rather than hand back nullptr. The callers cap these buffers small and
    // PSRAM is plentiful, so this is not a practical failure path.
    if (p == nullptr) abort();
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) noexcept { fiFontFree(p); }
};

template <typename A, typename B>
bool operator==(const PsramAlloc<A>&, const PsramAlloc<B>&) noexcept {
  return true;
}
template <typename A, typename B>
bool operator!=(const PsramAlloc<A>&, const PsramAlloc<B>&) noexcept {
  return false;
}

template <typename T>
using PsramVector = std::vector<T, PsramAlloc<T>>;

template <typename T>
T* psramNewArray(std::size_t n) {
  return static_cast<T*>(fiFontMalloc((n ? n : 1) * sizeof(T)));
}

template <typename T>
void psramDeleteArray(T* p) {
  fiFontFree(p);
}

}  // namespace font
}  // namespace freeink
