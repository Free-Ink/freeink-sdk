#pragma once

// Public shim over FreeInkBook's vendored expat.
//
// The engine vendors and compiles expat itself (src/vendor/expat_*.c,
// configured by third_party/expat/expat_config.h). Applications that need an
// XML parser of their own — OPDS feeds, sync protocols — should include this
// header instead of carrying a second expat copy: one configuration, one set
// of symbols, no duplicate-definition links.

#include "../../third_party/expat/expat.h"
