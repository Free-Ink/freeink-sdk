#pragma once
#include <cstddef>
struct HostEsp {
  size_t getMaxAllocHeap() const { return 380000; }
};
inline HostEsp ESP;
