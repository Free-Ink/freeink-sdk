#pragma once

#include <stdint.h>

typedef struct {
    int phases;
    const uint8_t* luts;
    const int* phase_times;
} EpdWaveformPhases;

typedef struct {
    uint8_t type;
    uint8_t temp_ranges;
    EpdWaveformPhases const** range_data;
} EpdWaveformMode;

typedef struct {
    int min;
    int max;
} EpdWaveformTempInterval;

typedef struct {
    uint8_t num_modes;
    uint8_t num_temp_ranges;
    EpdWaveformMode const** mode_data;
    EpdWaveformTempInterval const* temp_intervals;
} EpdWaveform;
