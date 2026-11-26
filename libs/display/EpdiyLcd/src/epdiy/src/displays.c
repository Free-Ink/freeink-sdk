#include <stddef.h>

#include "epd_display.h"

// E0470A01 / 684×1216 40pin。总线按本板 16bit，不是原厂 demo 板的 8bit/20MHz。
// 波形在应用侧 epd_hl_init(&E0470_WAVEFORM)，这里不挂社区 LUT。
const EpdDisplay_t E0470_DISPLAY = {
    .width = 1216,
    .height = 684,
    .bus_width = 16,
    .bus_speed = 24,
    .default_waveform = NULL,
};
