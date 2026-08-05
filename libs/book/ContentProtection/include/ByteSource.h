#pragma once

// FreeInk — random-access byte source.
//
// Mirrors FreeInkBook's BookSource shape so adapting an SD file (CrossPoint
// HalFile, FreeInkBook BookSource, host stdio) is a few lines. The content
// module never touches a filesystem directly.
//
// Freestanding C++17.

#include <stdint.h>

namespace freeink {
namespace content {

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  // Returns bytes actually read (may be short at EOF), or negative on error.
  virtual int32_t readAt(uint64_t offset, void* dst, uint32_t len) = 0;
  virtual uint64_t size() const = 0;
};

}  // namespace content
}  // namespace freeink
