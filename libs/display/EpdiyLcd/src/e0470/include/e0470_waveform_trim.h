/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 从完整灰阶表派生更短的表。自行调整屏幕波形会使设备失去保修。
 *
 * 完整表每对 (from, to) 的序列都是同一个骨架：
 *
 *     [保持补齐][擦除：往目标的反向轨道推 0~19 相][饱和：压到目标侧轨道 17 相][灰阶尾：0~10 相][保持 3 相]
 *
 * 灰阶的档数和位置全在「灰阶尾」里，擦除和饱和只是为了让所有像素在进尾巴之前站
 * 在同一个起点上，室温下 19 / 17 相里有相当大的余量。这里保留尾巴一相不动，只裁擦除
 * 的上限和饱和的开头，再把所有序列重新右对齐，就得到一张相数更少、灰阶梯子不变的表。
 * 代价是擦除余量变小，旧内容的残影会先出来，所以参数要上机看着残影调。
 *
 * Derive a shorter table from a full gray table. Changing panel waveforms
 * voids the warranty.
 *
 * Every (from, to) sequence in the full table shares one skeleton:
 *
 *     [hold pad][erase: opposite rail 0–19][sat: dest rail 17][gray tail 0–10][hold 3]
 *
 * Gray count and placement live in the tail. Erase and sat only park every
 * pixel at the same start before the tail; room-temp 19 / 17 has a lot of
 * slack. This keeps the tail intact, trims erase max and sat head, then
 * right-aligns every sequence so phase count drops and the ladder stays.
 * The cost is less erase margin, so old-content ghosting shows first;
 * tune on hardware against that ghosting.
 */

#pragma once

#include <stdint.h>

#include "epd_waveform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int erase_max;  ///< 擦除段最多保留几相（源表最长 19） / Max erase phases kept (source max 19)
    int sat_cut;    ///< 黑饱和段（to≠15）开头砍掉几相（源表 17，最短的一段是 14，所以 ≤13） / Black-sat head cut (source 17; shortest run 14, so ≤13)
    int white_sat_cut;  ///< 白饱和段（to=15）开头砍掉几相；白轨压不满就发灰，单独给 / White-sat head cut (to=15); under-driven white goes gray, so separate
    int hold;       ///< 结尾保持相数（源表 3，至少 1：要把最后一相的电压从像素上写掉） / Trailing holds (source 3, at least 1 to write off the last voltage)
} e0470_trim_t;

/// 按参数从 src 派生一张表写进 dst_data，容量至少要 src->phases 相。返回新相数，
/// 参数不合法返回 0。src 可以是 GC16 也可以是 GL16，骨架一样。
/// / Derive a table from src into dst_data (at least src->phases). Returns
/// the new phase count, or 0 on bad args. src may be GC16 or GL16; same skeleton.
int e0470_waveform_trim(
    const EpdWaveformPhases* src, const e0470_trim_t* trim, uint8_t (*dst_data)[16][4]
);

#ifdef __cplusplus
}
#endif
