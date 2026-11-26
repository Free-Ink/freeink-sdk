/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * E0470A01 波形装配：裁剪默认表、8 灰阶、跟随 DU。
 * 自行调整屏幕波形会使设备失去保修。
 *
 * E0470A01 waveform assembly: trimmed default, 8-gray, follow DU.
 * Changing panel waveforms voids the warranty.
 */

#include "e0470_epaper_waveform.h"

#include <assert.h>
#include <string.h>

#include "e0470_waveform_trim.h"
#include "du.h"
#include "gc16.h"
#include "gl16.h"
#include "gray8_gc16.h"
#include "gray8_gl16.h"

// 温度档 0-50°C。在此固件中没有温度分档的演示。
// / One 0–50°C temp range. This firmware has no multi-range demo.
static const EpdWaveformTempInterval e0470_intervals[] = {
    { .min = 0, .max = 50 },
};

// 把 (from, to) 的一个 2bit 动作写进 epdiy 的表：data[frame][to][from/4]，高位是 from0。
// / Write one 2-bit (from, to) action into the epdiy table: data[frame][to][from/4], MSB is from0.
static inline void lut_or(uint8_t (*data)[16][4], int f, int to, int from, int action) {
    data[f][to][from / 4] |= (uint8_t)(action << (6 - 2 * (from % 4)));
}

static inline int lut_get(const uint8_t (*data)[16][4], int f, int to, int from) {
    return (data[f][to][from / 4] >> (6 - 2 * (from % 4))) & 3;
}

void e0470_follow_lut_build(int frames, uint8_t (*dst)[16][4]) {
    memset(dst, 0, (size_t)frames * 16 * 4);
    // 两个方向各有自己的满推次数；短表（连续 DU 单帧、连调 dufr n）按表长封顶。
    // / Each direction has its own full-push count; short tables (1-frame
    // continuous DU, live-tune dufr n) cap at the table length.
    const int black_max = frames < E0470_FOLLOW_BLACK_FRAMES ? frames : E0470_FOLLOW_BLACK_FRAMES;
    const int white_max = frames < E0470_FOLLOW_WHITE_FRAMES ? frames : E0470_FOLLOW_WHITE_FRAMES;
    for (int to = 0; to < 16; to++) {
        for (int from = 0; from < 16; from++) {
            if (to == from) continue;
            const int diff = to > from ? to - from : from - to;
            const int action = to > from ? 2 : 1;  // 往白推 0b10，往黑推 0b01 / 0b10 erase, 0b01 darken
            const int budget = action == 2 ? white_max : black_max;
            // 推动次数 = ceil(diff · 满推 / 15)，至少 1：差得远多推，差得近少推。
            // / Push count = ceil(diff · full / 15), at least 1: far travels more, near travels less.
            const int pushes = (diff * budget + 14) / 15;
            for (int f = 0; f < pushes; f++) lut_or(dst, f, to, from, action);
        }
    }
}

/* ---- 跟随 DU：开机按公式生成 / Follow DU: built at boot ---- */
static uint8_t e0470_follow_data[E0470_FOLLOW_FRAMES][16][4];
static const EpdWaveformPhases e0470_follow_phases = {
    .phases = E0470_FOLLOW_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_follow_data[0],
};
static const EpdWaveformPhases* e0470_follow_ranges[] = { &e0470_follow_phases };
static const EpdWaveformMode e0470_follow_mode = {
    .type = 1,  // MODE_DU / MODE_DU
    .temp_ranges = 1,
    .range_data = &e0470_follow_ranges[0],
};
static const EpdWaveformMode* e0470_follow_modes[] = { &e0470_follow_mode };

const EpdWaveform E0470_FOLLOW_WAVEFORM = {
    .num_modes = 1,
    .num_temp_ranges = 1,
    .mode_data = e0470_follow_modes,
    .temp_intervals = e0470_intervals,
};

/* ---- 阈值 DU / Threshold DU ---- */
// 源表只认目标 0/15。中间灰按 50/50 切开，暗的走整段到黑、亮的走整段到白。
// from 不切片，沿用源表对真实起点的时间序列，上一帧残留的浅墨也会被推到黑或白。
// / Source tables only drive dest 0/15. Mid grays split 50/50: dark runs
// the full path to black, light the full path to white. from is not sliced;
// the source time series for the real start is reused, so leftover ink
// from the last frame is also pushed to black or white.
static uint8_t e0470_complete_du_data[E0470_FULL_DU_FRAMES][16][4];
static const EpdWaveformPhases e0470_complete_du_phases = {
    .phases = E0470_FULL_DU_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_complete_du_data[0],
};
static const EpdWaveformPhases* e0470_complete_du_ranges[] = {
    &e0470_complete_du_phases,
};
static const EpdWaveformMode e0470_complete_du_mode = {
    .type = 1,
    .temp_ranges = 1,
    .range_data = &e0470_complete_du_ranges[0],
};

static void e0470_complete_du_build(void) {
    memset(e0470_complete_du_data, 0, sizeof(e0470_complete_du_data));
    for (int to = 0; to < 16; to++) {
        const int to_bin = to < 8 ? 0 : 15;
        for (int from = 0; from < 16; from++) {
            for (int f = 0; f < E0470_FULL_DU_FRAMES; f++) {
                const int action = lut_get(e0470_full_du_data, f, to_bin, from);
                if (action != 0) lut_or(e0470_complete_du_data, f, to, from, action);
            }
        }
    }
}

// 白底 15→15 源表全保持。挂在已经「往白推」的那一相上再推 1 帧，不增加相数。
// 差分会跳过未变白像素，GL16 必须走全像素这帧才打到白底。
// / Source 15→15 is all-hold. Hang one extra white push on an already-white
// phase without adding phases. Diff skips unchanged white; GL16 must be
// full-pixel for this tick to hit the white background.
static void e0470_gl16_white_tick(uint8_t (*data)[16][4], int frames) {
    int tick = -1;
    for (int f = frames - 1; f >= 0; f--) {
        for (int from = 0; from < 15; from++) {
            if (lut_get(data, f, 15, from) == 2) {
                tick = f;
                break;
            }
        }
        if (tick >= 0) break;
    }
    if (tick < 0) tick = frames > 2 ? frames - 3 : 0;
    lut_or(data, tick, 15, 15, 2);
}

// 把每条 (to, from) 的活跃段搬到从第 0 相开始，即左对齐。e0470_waveform_trim() 交出来的
// 是右对齐的（序列都贴到保持相之前），所以不等长的方向起拍时间不同；左对齐让它们同起拍。
// / Move each (to, from) active run to start at phase 0, i.e. left-align it.
// e0470_waveform_trim() hands back right-aligned sequences pinned to the hold phases, so
// directions of unequal length start on different beats; left-aligning puts them together.
static void e0470_left_align(const uint8_t (*src)[16][4], int frames, uint8_t (*dst)[16][4]) {
    memset(dst, 0, (size_t)frames * 16 * 4);
    for (int to = 0; to < 16; to++) {
        for (int from = 0; from < 16; from++) {
            uint8_t seq[64];
            for (int f = 0; f < frames; f++) seq[f] = (uint8_t)lut_get(src, f, to, from);
            int a = 0;
            while (a < frames && seq[a] == 0) a++;
            int z = frames;
            while (z > a && seq[z - 1] == 0) z--;
            for (int f = a; f < z; f++) lut_or(dst, f - a, to, from, seq[f]);
        }
    }
}

/* ---- 完整表 / Full tables ---- */
// DU 20 相，GC16 48 相；GL16 用 RAM 副本以便白底补 1 帧。
// / DU 20, GC16 48; GL16 uses a RAM copy so the white-bg tick can be added.
static uint8_t e0470_full_gl16_live[E0470_FULL_GL16_FRAMES][16][4];
static const EpdWaveformPhases e0470_full_gl16_live_phases = {
    .phases = E0470_FULL_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_full_gl16_live[0],
};
static const EpdWaveformPhases* e0470_full_gl16_live_ranges[] = {
    &e0470_full_gl16_live_phases,
};
static const EpdWaveformMode e0470_full_gl16_live_mode = {
    .type = 5, .temp_ranges = 1, .range_data = &e0470_full_gl16_live_ranges[0],
};
static const EpdWaveformMode* e0470_full_modes[] = {
    &e0470_full_du_mode,
    &e0470_full_gc16_mode,
    &e0470_full_gl16_live_mode,
};

const EpdWaveform E0470_FULL_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = e0470_full_modes,
    .temp_intervals = e0470_intervals,
};

/* ---- 8 灰阶表 / 8-gray tables ---- */
// GC16 / GL16 各 30 相，拿灰阶档数换速度。
// / GC16 / GL16 30 phases each; trade gray steps for speed.
static const EpdWaveformMode* e0470_gray8_modes[] = {
    &e0470_complete_du_mode,
    &e0470_gray8_gc16_mode,
    &e0470_gray8_gl16_mode,
};

const EpdWaveform E0470_GRAY8_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = e0470_gray8_modes,
    .temp_intervals = e0470_intervals,
};

/* ---- 默认表 / Default tables ---- */
// 完整灰阶表裁掉余量，开机算进 RAM。
// / Trim slack from the full gray tables into RAM at boot.
static uint8_t e0470_gc16_data[E0470_FULL_GC16_FRAMES][16][4];
static uint8_t e0470_gl16_data[E0470_FULL_GL16_FRAMES][16][4];
static const EpdWaveformPhases e0470_gc16_phases = {
    .phases = E0470_GC16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_gc16_data[0],
};
static const EpdWaveformPhases e0470_gl16_phases = {
    .phases = E0470_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_gl16_data[0],
};
static const EpdWaveformPhases* e0470_gc16_ranges[] = { &e0470_gc16_phases };
static const EpdWaveformPhases* e0470_gl16_ranges[] = { &e0470_gl16_phases };
static const EpdWaveformMode e0470_gc16_mode = {
    .type = 2, .temp_ranges = 1, .range_data = &e0470_gc16_ranges[0],
};
static const EpdWaveformMode e0470_gl16_mode = {
    .type = 5, .temp_ranges = 1, .range_data = &e0470_gl16_ranges[0],
};
static const EpdWaveformMode* e0470_modes[] = {
    &e0470_complete_du_mode,
    &e0470_gc16_mode,
    &e0470_gl16_mode,
};

const EpdWaveform E0470_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = e0470_modes,
    .temp_intervals = e0470_intervals,
};

/* ---- 文字转页表 / Text-turn table ---- */
// 取裁剪后的 GL16（37 相，快），但把每条 (to, from) 序列**左对齐**，让所有方向同一起拍。
// e0470_waveform_trim() 是右对齐的：它把不等长的序列都贴到保持相之前，于是「新文字推黑」
// 13 相比「旧文字擦白」18 相晚 5 相开始，中间那几相整页是白的 —— 翻页可见的白闪。
// 左对齐后两者同时开始，是交叉淡化，没有白场；相数仍是 37，不比裁剪表慢。
// 对角线再清零：未变化的像素完全不驱动（原地重推一个黑像素会先擦白再推黑）。
// / Take the trimmed GL16 (37 phases, fast) but **left-align** every (to, from) sequence so
// all directions start on the same beat. e0470_waveform_trim() right-aligns instead, pinning
// unequal sequences to the hold phases, so the 13-phase write of the new text starts 5 phases
// after the 18-phase erase of the old one and the page is blank white in between -- the flash
// a turn shows. Left-aligned they begin together, a cross-fade with no white field, still 37
// phases. The diagonal is then cleared so unchanged pixels are not driven at all.
static uint8_t e0470_textturn_gl16_data[E0470_GL16_FRAMES][16][4];
static const EpdWaveformPhases e0470_textturn_gl16_phases = {
    .phases = E0470_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_textturn_gl16_data[0],
};
static const EpdWaveformPhases* e0470_textturn_gl16_ranges[] = { &e0470_textturn_gl16_phases };
static const EpdWaveformMode e0470_textturn_gl16_mode = {
    .type = 5, .temp_ranges = 1, .range_data = &e0470_textturn_gl16_ranges[0],
};
static const EpdWaveformMode* e0470_textturn_modes[] = {
    &e0470_textturn_gl16_mode,
};

const EpdWaveform E0470_TEXTTURN_WAVEFORM = {
    .num_modes = 1,
    .num_temp_ranges = 1,
    .mode_data = e0470_textturn_modes,
    .temp_intervals = e0470_intervals,
};

const EpdWaveformPhases* e0470_waveform_phases(const EpdWaveform* waveform, int mode) {
    if (waveform == NULL) return NULL;
    const int type = mode & 0x3F;
    for (int i = 0; i < waveform->num_modes; i++) {
        if (waveform->mode_data[i]->type == type) return waveform->mode_data[i]->range_data[0];
    }
    return NULL;
}

int e0470_phase_action(const EpdWaveformPhases* phases, int phase, int to, int from) {
    if (phases == NULL || phases->luts == NULL) return 0;
    if (phase < 0 || phase >= phases->phases) return 0;
    if ((unsigned)to > 15 || (unsigned)from > 15) return 0;
    const uint8_t* cell = phases->luts + ((size_t)phase * 16 + to) * 4 + from / 4;
    return (*cell >> (6 - 2 * (from % 4))) & 3;
}

void e0470_waveform_init(void) {
    e0470_follow_lut_build(E0470_FOLLOW_FRAMES, e0470_follow_data);
    e0470_complete_du_build();

    const e0470_trim_t trim = {
        .erase_max = E0470_TRIM_ERASE_MAX,
        .sat_cut = E0470_TRIM_SAT_CUT,
        .white_sat_cut = E0470_TRIM_WHITE_SAT_CUT,
        .hold = E0470_TRIM_HOLD,
    };
    const int gc = e0470_waveform_trim(&e0470_full_gc16_phases, &trim, e0470_gc16_data);
    const int gl = e0470_waveform_trim(&e0470_full_gl16_phases, &trim, e0470_gl16_data);
    assert(gc == E0470_GC16_FRAMES);
    assert(gl == E0470_GL16_FRAMES);

    memcpy(e0470_full_gl16_live, e0470_full_gl16_data, sizeof(e0470_full_gl16_live));
    e0470_gl16_white_tick(e0470_full_gl16_live, E0470_FULL_GL16_FRAMES);
    e0470_gl16_white_tick(e0470_gl16_data, E0470_GL16_FRAMES);

    // 文字转页表 = 裁剪后的 GL16 左对齐（各方向同起拍，擦白与推黑重叠、无白场）。
    //
    // 这里**不能**再清 to == from 的对角线。GL16 是"整帧重写"波形：每个像素都会被
    // 擦到白再推到目标色，所以一帧结束后屏上的真实状态就是这张 1 bit 图。一旦把
    // to == from 跳过，它就从整帧重写退化成局部更新，而"未变化"只是相对上一帧的
    // 1 bit 图而言——上一帧是抗锯齿帧，屏上真实留着的是灰度，于是这些像素得不到
    // 任何驱动，上一页字形的灰度残影就留了下来，看起来像字被挪了位。残影还会让
    // 屏的真实基线与控制器以为的基线越差越远，后面的差分帧（本页或别的界面）都
    // 按错误的基线算，整屏就散了。左对齐本身已经消掉了白场，不需要靠清对角线。
    // / Text-turn table = the trimmed GL16, left-aligned so every direction starts on the
    // same beat and the erase overlaps the write with no white field, then with every
    // to == from action cleared. An unchanged pixel is therefore not driven at all, which
    // is what removes the erase-to-white step that reads as a white flash on every turn.
    e0470_left_align(e0470_gl16_data, E0470_GL16_FRAMES, e0470_textturn_gl16_data);
    for (int f = 0; f < E0470_GL16_FRAMES; f++) {
        for (int v = 0; v < 16; v++) {
            e0470_textturn_gl16_data[f][v][v / 4] &= (uint8_t)~(3u << (6 - 2 * (v % 4)));
        }
    }

    // 目标为黑的像素不再先擦白，直接把黑压上去；目标为白的像素保留擦白驱动。
    // 新文字进来时是"白 → 黑"，源表给的是先擦白再压黑，那 18 相擦白就是字形先闪一下白
    // 的来源。删掉这段只留压黑，新字直接叠上去。反过来，目标为白的像素必须留着擦白，
    // 那是把旧字真正清掉的那一段，删了会糊成一片。
    // / A pixel whose target is black no longer erases to white first: it is driven
    // straight to black, so a glyph arriving as white-to-black stops flashing white on
    // its way in. A pixel whose target is white keeps its erase, because that erase is
    // what actually clears the old glyph; dropping it would smear the page.
    //
    // to == 15 is white and to != 15 is black (see sat_cut, the black-saturation head cut
    // that applies to to != 15 in e0470_waveform_trim.h). Action 2 is the erase-to-white
    // action, 1 is darken, 0 is hold.
    for (int to = 0; to < 15; to++) {
        for (int from = 0; from < 16; from++) {
            int f = 0;
            while (f < E0470_GL16_FRAMES && lut_get(e0470_textturn_gl16_data, f, to, from) == 2) {
                e0470_textturn_gl16_data[f][to][from / 4] &= (uint8_t)~(3u << (6 - 2 * (from % 4)));
                ++f;
            }
        }
    }
}
