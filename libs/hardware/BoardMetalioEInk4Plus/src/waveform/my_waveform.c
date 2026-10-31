/**
 * @brief MY_WAVEFORM：均衡完整波形与快速裁剪波形
 */

#include "my_waveform.h"

#include "my_waveform_du.h"
#include "my_waveform_gc.h"
#include "my_waveform_gl.h"
#include "my_waveform_trim.h"
#include "my_gray8_gc16.h"
#include "my_gray8_gl16.h"

#include <assert.h>
#include <string.h>

#include <esp_log.h>
#include <esp_heap_caps.h>

#define TAG "my_wf"

#define MY_FULL_DU_FRAMES 20
#define MY_FULL_GC16_FRAMES 48
#define MY_FULL_GL16_FRAMES 48
#define MY_TRIM_ERASE_MAX 11
#define MY_TRIM_SAT_CUT 5
#define MY_TRIM_WHITE_SAT_CUT 0
#define MY_TRIM_HOLD 3
#define MY_GC16_FRAMES MY_FULL_GC16_FRAMES
#define MY_GL16_FRAMES MY_FULL_GL16_FRAMES

static const EpdWaveformTempInterval my_waveform_intervals[] = {
    { .min = 0, .max = 50 },
};

static inline void lut_or(uint8_t (*data)[16][4], int f, int to, int from, int action) {
    data[f][to][from / 4] |= (uint8_t)(action << (6 - 2 * (from % 4)));
}

static inline int lut_get(const uint8_t (*data)[16][4], int f, int to, int from) {
    return (data[f][to][from / 4] >> (6 - 2 * (from % 4))) & 3;
}

// 源 DU 只认目标 0/15；中间灰 50/50，暗→黑、亮→白
static uint8_t s_du_data[MY_FULL_DU_FRAMES][16][4];
static const EpdWaveformPhases s_du_phases = {
    .phases = MY_FULL_DU_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_du_data[0],
};
static const EpdWaveformPhases* s_du_ranges[] = { &s_du_phases };
static const EpdWaveformMode s_du_mode = {
    .type = 1,
    .temp_ranges = 1,
    .range_data = &s_du_ranges[0],
};

static void complete_du_build(void) {
    memset(s_du_data, 0, sizeof(s_du_data));
    for (int to = 0; to < 16; to++) {
        const int to_bin = to < 8 ? 0 : 15;
        for (int from = 0; from < 16; from++) {
            for (int f = 0; f < MY_FULL_DU_FRAMES; f++) {
                const int action = lut_get(my_du_25_0_data, f, to_bin, from);
                if (action != 0) {
                    lut_or(s_du_data, f, to, from, action);
                }
            }
        }
    }
    // 在末尾四相为黑→黑补黑，不增加总相数。
    for (int f = MY_FULL_DU_FRAMES - 4; f < MY_FULL_DU_FRAMES; ++f) {
        lut_or(s_du_data, f, 0, 0, 1);
    }
}

// 白底 15→15 源表全保持：挂在已有往白推的一相上再推 1 帧
static void gl16_white_tick(uint8_t (*data)[16][4], int frames) {
    int tick = -1;
    for (int f = frames - 1; f >= 0; f--) {
        for (int from = 0; from < 15; from++) {
            if (lut_get(data, f, 15, from) == 2) {
                tick = f;
                break;
            }
        }
        if (tick >= 0) {
            break;
        }
    }
    if (tick < 0) {
        tick = frames > 2 ? frames - 3 : 0;
    }
    lut_or(data, tick, 15, 15, 2);
}

static uint8_t s_gc16_data[MY_FULL_GC16_FRAMES][16][4];
static uint8_t s_gl16_data[MY_FULL_GL16_FRAMES][16][4];
static const EpdWaveformPhases s_gc16_phases = {
    .phases = MY_GC16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_gc16_data[0],
};
static const EpdWaveformPhases s_gl16_phases = {
    .phases = MY_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_gl16_data[0],
};
static const EpdWaveformPhases* s_gc16_ranges[] = { &s_gc16_phases };
static const EpdWaveformPhases* s_gl16_ranges[] = { &s_gl16_phases };
static const EpdWaveformMode s_gc16_mode = {
    .type = 2,
    .temp_ranges = 1,
    .range_data = &s_gc16_ranges[0],
};
static const EpdWaveformMode s_gl16_mode = {
    .type = 5,
    .temp_ranges = 1,
    .range_data = &s_gl16_ranges[0],
};

static const EpdWaveformMode* s_modes[] = {
    &s_du_mode,
    &s_gc16_mode,
    &s_gl16_mode,
};

const EpdWaveform MY_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = s_modes,
    .temp_intervals = my_waveform_intervals,
};

static EpdWaveformPhases s_fast_gc_phases = { .phases = 36 };
static EpdWaveformPhases s_fast_gl_phases = { .phases = 37 };
static const EpdWaveformPhases* s_fast_gc_ranges[] = { &s_fast_gc_phases };
static const EpdWaveformPhases* s_fast_gl_ranges[] = { &s_fast_gl_phases };
static const EpdWaveformMode s_fast_gc_mode = {
    .type = 2, .temp_ranges = 1, .range_data = s_fast_gc_ranges,
};
static const EpdWaveformMode s_fast_gl_mode = {
    .type = 5, .temp_ranges = 1, .range_data = s_fast_gl_ranges,
};
static const EpdWaveformMode* s_fast_modes[] = {
    &s_du_mode, &s_fast_gc_mode, &s_fast_gl_mode,
};
static const EpdWaveform s_fast_waveform = {
    .num_modes = 3, .num_temp_ranges = 1,
    .mode_data = s_fast_modes, .temp_intervals = my_waveform_intervals,
};

const EpdWaveform* my_waveform_fast(void) {
    return s_fast_gc_phases.luts ? &s_fast_waveform : NULL;
}

static void short_du_build(void);
static void wipe_build(void);

void my_waveform_init(void) {
    static bool initialized;
    if (initialized) return;
    initialized = true;
    complete_du_build();
    short_du_build();
    wipe_build();
    memcpy(s_gc16_data, my_gc_25_0_data, sizeof(s_gc16_data));
    memcpy(s_gl16_data, my_gl_25_0_data, sizeof(s_gl16_data));
    gl16_white_tick(s_gl16_data, MY_GL16_FRAMES);

    // Build once in PSRAM. Switching profiles never modifies an active LUT.
    uint8_t (*data)[16][4] = heap_caps_malloc(
        sizeof(s_gc16_data) + sizeof(s_gl16_data), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        ESP_LOGW(TAG, "fast waveform allocation failed; balanced retained");
        return;
    }
    const my_waveform_trim_t trim = {
        .erase_max = MY_TRIM_ERASE_MAX, .sat_cut = MY_TRIM_SAT_CUT,
        .white_sat_cut = MY_TRIM_WHITE_SAT_CUT, .hold = MY_TRIM_HOLD,
    };
    const int gc = my_waveform_trim(&my_gc_25_0, &trim, data);
    const int gl = my_waveform_trim(&my_gl_25_0, &trim, data + MY_FULL_GC16_FRAMES);
    assert(gc == s_fast_gc_phases.phases);
    assert(gl == s_fast_gl_phases.phases);
    gl16_white_tick(data + MY_FULL_GC16_FRAMES, gl);
    s_fast_gc_phases.luts = (const uint8_t*)data;
    s_fast_gl_phases.luts = (const uint8_t*)(data + MY_FULL_GC16_FRAMES);
}

// Extreme GL uses eight optical grays; GC always uses the 16-gray profiles.
static const EpdWaveformPhases s_gray8_gl_phases = {
    .phases = 30, .phase_times = NULL, .luts = (const uint8_t*)e0470_gray8_gl16_data,
};
static const EpdWaveformPhases* s_gray8_gl_ranges[] = { &s_gray8_gl_phases };
static const EpdWaveformMode s_gray8_gl_mode = { .type=5, .temp_ranges=1, .range_data=s_gray8_gl_ranges };
static const EpdWaveformMode* s_gray8_modes[] = { &s_gray8_gl_mode };
static const EpdWaveform s_gray8_waveform = {
    .num_modes=1, .num_temp_ranges=1, .mode_data=s_gray8_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_gray8(void) { return &s_gray8_waveform; }

// 黑白转换推黑七相、推白八相；黑→黑在末尾四相补黑，其余同色保持。
static uint8_t s_short_du_data[8][16][4];
static const EpdWaveformPhases s_short_du_phases = {
    .phases=8, .phase_times=NULL, .luts=(const uint8_t*)s_short_du_data,
};
static const EpdWaveformPhases* s_short_du_ranges[] = { &s_short_du_phases };
static const EpdWaveformMode s_short_du_mode = { .type=1, .temp_ranges=1, .range_data=s_short_du_ranges };
static const EpdWaveformMode* s_short_du_modes[] = { &s_short_du_mode };
static const EpdWaveform s_short_du_waveform = {
    .num_modes=1, .num_temp_ranges=1, .mode_data=s_short_du_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_short_du(void) { return &s_short_du_waveform; }

static uint8_t s_fast_du_data[14][16][4];
static const EpdWaveformPhases s_fast_du_phases = {
    .phases=14, .phase_times=NULL, .luts=(const uint8_t*)s_fast_du_data,
};
static const EpdWaveformPhases* s_fast_du_ranges[] = { &s_fast_du_phases };
static const EpdWaveformMode s_fast_du_mode = {
    .type=1, .temp_ranges=1, .range_data=s_fast_du_ranges,
};
static const EpdWaveformMode* s_fast_du_modes[] = { &s_fast_du_mode };
static const EpdWaveform s_fast_du_waveform = {
    .num_modes=1, .num_temp_ranges=1,
    .mode_data=s_fast_du_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_fast_du(void) { return &s_fast_du_waveform; }
// Binary page navigation: each band starts one phase later. The target nibble
// holds the band, and the source nibble holds old/new binary flags. Index 15
// keeps existing white pixels and the border undriven.
static uint8_t (*s_reveal_data)[16][4];
static EpdWaveformPhases s_reveal_phases = {
    .phases=2*MY_FULL_DU_FRAMES+9, .phase_times=NULL, .luts=NULL,
};
static const EpdWaveformPhases* s_reveal_ranges[] = { &s_reveal_phases };
static const EpdWaveformMode s_reveal_mode = { .type=1, .temp_ranges=1, .range_data=s_reveal_ranges };
static const EpdWaveformMode* s_reveal_modes[] = { &s_reveal_mode };
static const EpdWaveform s_reveal_waveform = {
    .num_modes=1, .num_temp_ranges=1, .mode_data=s_reveal_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_wipe_reveal(void) { return s_reveal_data ? &s_reveal_waveform : NULL; }
static uint8_t (*s_reveal_short_data)[16][4];
static EpdWaveformPhases s_reveal_short_phases = {
    .phases=25,.phase_times=NULL,.luts=NULL,
};
static const EpdWaveformPhases* s_reveal_short_ranges[] = { &s_reveal_short_phases };
static const EpdWaveformMode s_reveal_short_mode = { .type=1,.temp_ranges=1,.range_data=s_reveal_short_ranges };
static const EpdWaveformMode* s_reveal_short_modes[] = { &s_reveal_short_mode };
static const EpdWaveform s_reveal_short_waveform = {
    .num_modes=1,.num_temp_ranges=1,.mode_data=s_reveal_short_modes,.temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_wipe_reveal_short(void) { return s_reveal_short_data ? &s_reveal_short_waveform : NULL; }
const EpdWaveform* my_waveform_wipe_gl(int profile) {
    static uint8_t (*data[4])[16][4];
    static EpdWaveformPhases phases[4];
    static const EpdWaveformPhases* ranges[4];
    static EpdWaveformMode modes[4];
    static const EpdWaveformMode* mode_ptrs[4];
    static EpdWaveform waves[4];
    const int slot=profile>=0 && profile<4 ? profile : 0;
    if(waves[slot].num_modes) return &waves[slot];
    const EpdWaveform* source=slot==3 ? my_waveform_gray8() :
        (slot>=1 && my_waveform_fast() ? my_waveform_fast() : &MY_WAVEFORM);
    const int erase=slot>=2 ? 8 : MY_FULL_DU_FRAMES;
    const EpdWaveformPhases* gl=NULL;
    for(int m=0;m<source->num_modes;++m)
        if(source->mode_data[m]->type==5) gl=source->mode_data[m]->range_data[0];
    if(!gl || gl->phases>MY_FULL_GL16_FRAMES) return NULL;
    data[slot]=heap_caps_malloc((erase+gl->phases)*16*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!data[slot]) return NULL;
    memset(data[slot],0,(erase+gl->phases)*16*4);
    for(int to=0;to<16;++to) for(int old=0;old<16;++old) {
        for(int f=0;f<erase;++f)
            lut_or(data[slot],f,to,old,lut_get(slot>=2 ? s_short_du_data : my_du_25_0_data,f,15,old));
        for(int f=0;f<gl->phases;++f) {
            const int action=(gl->luts[(f*16+to)*4+3])&3; // source white (15)
            lut_or(data[slot],erase+f,to,old,action);
        }
    }
    phases[slot]=(EpdWaveformPhases){.phases=erase+gl->phases,
        .phase_times=NULL,.luts=(const uint8_t*)data[slot]};
    ranges[slot]=&phases[slot];
    modes[slot]=(EpdWaveformMode){.type=5,.temp_ranges=1,.range_data=&ranges[slot]};
    mode_ptrs[slot]=&modes[slot];
    waves[slot]=(EpdWaveform){.num_modes=1,.num_temp_ranges=1,
        .mode_data=&mode_ptrs[slot],.temp_intervals=my_waveform_intervals};
    return &waves[slot];
}
static void wipe_build(void) {
    // Waveform data is read before each scan into the INTERNAL conversion LUT.
    // Keep the large immutable source tables out of DMA-capable internal RAM.
    s_reveal_data=heap_caps_malloc((2*MY_FULL_DU_FRAMES+9)*16*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_reveal_short_data=heap_caps_malloc(25*16*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(s_reveal_data) memset(s_reveal_data,0,(2*MY_FULL_DU_FRAMES+9)*16*4);
    if(s_reveal_short_data) memset(s_reveal_short_data,0,25*16*4);
    s_reveal_phases.luts=(const uint8_t*)s_reveal_data;
    s_reveal_short_phases.luts=(const uint8_t*)s_reveal_short_data;
    for (int band=0;band<10;++band) for (int code=0;code<4;++code) {
        const int old=(code&1) ? 15 : 0;
        for (int f=0;f<MY_FULL_DU_FRAMES;++f) {
            if(s_reveal_data) lut_or(s_reveal_data,band+f,band,code,lut_get(my_du_25_0_data,f,15,old));
            if (s_reveal_data && (code&2)) lut_or(s_reveal_data,band+MY_FULL_DU_FRAMES+f,band,code,
                lut_get(my_du_25_0_data,f,0,15));
        }
        for(int f=0;f<8;++f) {
            if(s_reveal_short_data) lut_or(s_reveal_short_data,band+f,band,code,lut_get(s_short_du_data,f,15,old));
            if(s_reveal_short_data && (code&2)) lut_or(s_reveal_short_data,band+8+f,band,code,lut_get(s_short_du_data,f,0,15));
        }
    }
}
static void short_du_build(void) {
    for (int to=0;to<16;++to) for (int from=0;from<16;++from) {
        if (to==from) continue;
        const int endpoint=to<8 ? 0 : 15;
        const int action=endpoint==0 ? 1 : 2;
        const int frames=endpoint==0 ? 7 : 8;
        for (int f=0;f<frames;++f) lut_or(s_short_du_data,f,to,from,action);
        const int fast_frames=endpoint==0 ? 13 : 14;
        for (int f=0;f<fast_frames;++f) lut_or(s_fast_du_data,f,to,from,action);
    }
    for (int f=4;f<8;++f) lut_or(s_short_du_data,f,0,0,1);
    for (int f=10;f<14;++f) lut_or(s_fast_du_data,f,0,0,1);
}
