#include "epd_display.h"
#include "waveforms/my_waveform.h"

// FreeInk: trimmed to the one panel in use. The other upstream entries pulled
// the built-in epdiy_* waveform tables (about 600 KB of flash) into the image.

// E0470A14-AF rev A: 34-pin, 8-bit source bus; use landscape buffer order.
// MY_WAVEFORM remains an experimental table until the factory LUT is supplied.
const EpdDisplay_t E0470A14_AF = {
    .width = 1216,
    .height = 684,
    .bus_width = 8,
    .bus_speed = 20,
    .default_waveform = &MY_WAVEFORM,
    .display_type = DISPLAY_TYPE_GENERIC,
};
