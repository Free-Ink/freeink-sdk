#pragma once

// FreeInk SDK — arena (bump) allocator.
//
// The implementation now lives in the standalone FreeInkFont library
// (freeink::font::Arena) so font code can be used without the book engine.
// This header re-exports it under the historical freeink::book::Arena name so
// existing FreeInkBook code keeps compiling unchanged.

#include <FontArena.h>

namespace freeink {
namespace book {
using freeink::font::Arena;
}  // namespace book
}  // namespace freeink
