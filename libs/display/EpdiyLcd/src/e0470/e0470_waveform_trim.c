/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 从完整灰阶表裁擦除/饱和余量。自行调整屏幕波形会使设备失去保修。
 *
 * Trim erase/sat slack from a full gray table. Changing panel waveforms
 * voids the warranty.
 */

#include "e0470_waveform_trim.h"

#include <string.h>

#define MAX_PHASES 64

// 表的字节布局：data[frame][to][from/4]，每字节高位起 4 个 2bit 动作。
// / Table layout: data[frame][to][from/4], four 2-bit actions MSB-first.
static inline int lut_get(const uint8_t (*data)[16][4], int f, int to, int from) {
    return (data[f][to][from / 4] >> (6 - 2 * (from % 4))) & 3;
}

static inline void lut_set(uint8_t (*data)[16][4], int f, int to, int from, int v) {
    const int shift = 6 - 2 * (from % 4);
    data[f][to][from / 4] = (uint8_t)((data[f][to][from / 4] & ~(3 << shift)) | (v << shift));
}

// One reusable trimmed (from, to) sequence; two passes avoid a 17 KiB static workspace.
typedef struct {
    uint8_t seq[MAX_PHASES];
    int len;
} trimmed_seq_t;

static int run_length(const uint8_t* seq, int start, int end, int value) {
    int n = 0;
    while (start + n < end && seq[start + n] == value) n++;
    return n;
}

// 把一条序列按骨架拆开再拼回去。to=15 的饱和极性是白（0b10），其余是黑（0b01），
// 擦除极性总是饱和的反面。没有擦除段（例如从白压到黑）的序列 e=0，逻辑照样成立。
// / Split a sequence on the skeleton and rebuild. to=15 sat is white (0b10),
// else black (0b01); erase is the opposite of sat. No erase (e.g. white→black)
// means e=0; the same logic still holds.
static void trim_one(
    const uint8_t* seq, int phases, int to, const e0470_trim_t* trim, trimmed_seq_t* out
) {
    const int sat = to == 15 ? 2 : 1;
    const int erase = to == 15 ? 1 : 2;

    int a = 0;
    while (a < phases && seq[a] == 0) a++;
    int z = phases;
    while (z > a && seq[z - 1] == 0) z--;
    out->len = 0;
    if (a >= z) return;  // 全保持（GL16 的白到白） / All-hold (GL16 white-to-white)

    int e = run_length(seq, a, z, erase);
    int s = run_length(seq, a + e, z, sat);
    int keep_e = e < trim->erase_max ? e : trim->erase_max;
    const int want_cut = to == 15 ? trim->white_sat_cut : trim->sat_cut;
    int cut_s = want_cut < s - 1 ? want_cut : s - 1;
    if (cut_s < 0) cut_s = 0;
    int keep_s = s - cut_s;

    int n = 0;
    for (int i = 0; i < keep_e; i++) out->seq[n++] = (uint8_t)erase;
    for (int i = 0; i < keep_s; i++) out->seq[n++] = (uint8_t)sat;
    for (int i = a + e + s; i < z; i++) out->seq[n++] = seq[i];
    out->len = n;
}

int e0470_waveform_trim(
    const EpdWaveformPhases* src, const e0470_trim_t* trim, uint8_t (*dst_data)[16][4]
) {
    if (src == NULL || trim == NULL || dst_data == NULL) return 0;
    if (src->phases <= 0 || src->phases > MAX_PHASES) return 0;
    if (trim->erase_max < 0 || trim->sat_cut < 0 || trim->white_sat_cut < 0 || trim->hold < 1) {
        return 0;
    }

    const uint8_t(*data)[16][4] = (const uint8_t(*)[16][4])src->luts;
    uint8_t seq[MAX_PHASES];
    trimmed_seq_t trimmed;
    int longest = 0;

    for (int to = 0; to < 16; to++) {
        for (int from = 0; from < 16; from++) {
            for (int f = 0; f < src->phases; f++) seq[f] = (uint8_t)lut_get(data, f, to, from);
            trim_one(seq, src->phases, to, trim, &trimmed);
            if (trimmed.len > longest) longest = trimmed.len;
        }
    }

    const int phases = longest + trim->hold;
    if (phases > src->phases) return 0;  // 只允许变短，调用方按 src->phases 给的缓冲 / Shorter only; caller sized dst to src->phases

    memset(dst_data, 0, (size_t)phases * 16 * 4);
    for (int to = 0; to < 16; to++) {
        for (int from = 0; from < 16; from++) {
            for (int f = 0; f < src->phases; f++) seq[f] = (uint8_t)lut_get(data, f, to, from);
            trim_one(seq, src->phases, to, trim, &trimmed);
            const trimmed_seq_t* t = &trimmed;
            const int start = longest - t->len;  // 右对齐到保持相之前 / Right-align before the hold phases
            for (int i = 0; i < t->len; i++) lut_set(dst_data, start + i, to, from, t->seq[i]);
        }
    }
    return phases;
}
