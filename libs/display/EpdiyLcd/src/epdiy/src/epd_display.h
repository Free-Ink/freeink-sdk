#pragma once

#include <stdint.h>
#include "epd_internals.h"

typedef struct {
    /// Width of the display in pixels.
    int width;
    /// Height of the display in pixels.
    int height;

    /// Width of the data bus in bits.
    uint8_t bus_width;
    /// Speed of the data bus in MHz, if configurable.
    /// (Only used by the LCD based renderer in V7+)
    int bus_speed;

    /// Default waveform to use.
    const EpdWaveform* default_waveform;
} EpdDisplay_t;

extern const EpdDisplay_t E0470_DISPLAY;