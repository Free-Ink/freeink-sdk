#pragma once

// FreeInkBook build profile — one knob that sizes every fixed working set
// for the target's RAM class. Select with a build flag:
//
//   -DFREEINK_BOOK_SMALL=1   PSRAM-less MCUs (ESP32-C3 class, ~200 KB free
//                            heap): fixed buffers shrink; very long
//                            paragraphs flush in more segments, dense pages
//                            split earlier, fewer links/images per page.
//   (no flag)                Standard — PSRAM parts (ESP32-S3 class).
//   -DFREEINK_BOOK_LARGE=1   Generous RAM (large PSRAM budgets, host
//                            tools, Linux targets): bigger paragraph/page
//                            capacities and much larger font caches — fewer
//                            glyph re-rasterizations means faster page
//                            renders. Costs RAM only; output is identical
//                            except where Standard would have split a
//                            pathological page early.
//
// Capacities themselves live next to their use sites (ChapterLayout.cpp,
// TtfFont.h), switched on FREEINK_BOOK_PROFILE. Runtime features — TTF vs
// bitmap fonts, Gray8 vs 1-bit rendering, arena budgets — are orthogonal:
// the caller picks those per device at runtime; this knob only sets the
// compile-time ceilings around them.

#ifndef FREEINK_BOOK_SMALL
#define FREEINK_BOOK_SMALL 0
#endif
#ifndef FREEINK_BOOK_LARGE
#define FREEINK_BOOK_LARGE 0
#endif

#if FREEINK_BOOK_SMALL && FREEINK_BOOK_LARGE
#error "FREEINK_BOOK_SMALL and FREEINK_BOOK_LARGE are mutually exclusive"
#endif

#define FREEINK_BOOK_PROFILE_SMALL 0
#define FREEINK_BOOK_PROFILE_STANDARD 1
#define FREEINK_BOOK_PROFILE_LARGE 2

#if FREEINK_BOOK_SMALL
#define FREEINK_BOOK_PROFILE FREEINK_BOOK_PROFILE_SMALL
#elif FREEINK_BOOK_LARGE
#define FREEINK_BOOK_PROFILE FREEINK_BOOK_PROFILE_LARGE
#else
#define FREEINK_BOOK_PROFILE FREEINK_BOOK_PROFILE_STANDARD
#endif
