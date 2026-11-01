#include "epd_display.h"
#include "waveforms/my_waveform.h"

const EpdDisplay_t ED060SCT = {
    .width = 1448,
    .height = 1072,
    .bus_width = 8,
    .bus_speed = 33,
    .default_waveform = &epdiy_ED047TC2,
    .display_type = DISPLAY_TYPE_GENERIC,
};

const EpdDisplay_t ED060XC3 = {
    .width = 1024,
    .height = 768,
    .bus_width = 8,
    .bus_speed = 20,
    .default_waveform = &epdiy_ED047TC2,
    .display_type = DISPLAY_TYPE_GENERIC,
};

const EpdDisplay_t ED097OC4 = {
    .width = 1200,
    .height = 825,
    .bus_width = 8,
    .bus_speed = 15,
    .default_waveform = &epdiy_ED097OC4,
    .display_type = DISPLAY_TYPE_GENERIC,
};

const EpdDisplay_t ED097TC2 = {
    .width = 1200,
    .height = 825,
    .bus_width = 8,
    .bus_speed = 22,
    .default_waveform = &epdiy_ED097TC2,
    .display_type = DISPLAY_TYPE_ED097TC2,
};

const EpdDisplay_t ED133UT2 = {
    .width = 1600,
    .height = 1200,
    .bus_width = 8,
    .bus_speed = 20,
    .default_waveform = &epdiy_ED097TC2,
    .display_type = DISPLAY_TYPE_ED097TC2,
};

const EpdDisplay_t ED047TC1 = {
    .width = 960,
    .height = 540,
    .bus_width = 8,
    .bus_speed = 20,
    .default_waveform = &epdiy_ED047TC1,
    .display_type = DISPLAY_TYPE_GENERIC,
};

const EpdDisplay_t ED047TC2 = {
    .width = 960,
    .height = 540,
    .bus_width = 8,
    .bus_speed = 20,
    .default_waveform = &epdiy_ED047TC2,
    .display_type = DISPLAY_TYPE_GENERIC,
};

const EpdDisplay_t ED047TC2_1216 = {
    .width = 1216,
    .height = 684,
    .bus_width = 8,
    .bus_speed = 20,
    .default_waveform = &epdiy_ED047TC2,
    .display_type = DISPLAY_TYPE_GENERIC,
};

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

const EpdDisplay_t ED078KC1 = {
    .width = 1872,
    .height = 1404,
    .bus_width = 16,
    .bus_speed = 28,
    .default_waveform = &epdiy_ED047TC2,
    .display_type = DISPLAY_TYPE_GENERIC,
};

// Attention is by default horizontal rows mirrored
const EpdDisplay_t ED052TC4 = {
    .width = 1280,
    .height = 720,
    .bus_width = 8,
    .bus_speed = 22,
    .default_waveform = &epdiy_ED097TC2,
    .display_type = DISPLAY_TYPE_ED097TC2,
};
