#pragma once
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <unordered_map>
constexpr uint32_t MALLOC_CAP_INTERNAL=1, MALLOC_CAP_SPIRAM=2, MALLOC_CAP_8BIT=4;
struct FakeAllocation { size_t size; uint32_t caps; };
inline std::unordered_map<void*,FakeAllocation> fakeHeapAllocations;
inline uint32_t failAllocationCaps=0;
inline void* heap_caps_malloc(size_t size,uint32_t caps) {
  if (caps & failAllocationCaps) return nullptr;
  void* p=std::malloc(size);
  if(p) fakeHeapAllocations.emplace(p,FakeAllocation{size,caps});
  return p;
}
inline void heap_caps_free(void* p) {
  if(!p) return;
  assert(fakeHeapAllocations.erase(p)==1); // caller must own it, exactly once
  std::free(p);
}
