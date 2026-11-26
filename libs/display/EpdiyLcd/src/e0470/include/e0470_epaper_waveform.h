/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * E0470A01（684×1216，40pin）的 epdiy 波形。
 * 自行调整屏幕波形会使设备失去保修。
 *   E0470_WAVEFORM         默认：裁剪 GC16 36 相 / GL16 37 相 + 50/50 阈值 DU
 *   E0470_FULL_WAVEFORM    GC16 / GL16 各 48 相，一个温度档 0-50°C
 *   E0470_GRAY8_WAVEFORM   8 灰阶：GC16 / GL16 各 30 相，灰阶少一半换来整屏约 360ms
 *   E0470_FOLLOW_WAVEFORM  跟随 DU：8 帧（黑 7 / 白 8），用于触摸笔迹
 * 默认与 8 灰阶波形挂阈值 DU（目标 0-7→黑、8-15→白）；波形保留只驱动 0/15 的源表。
 * GL16 给 15→15 补 1 帧白推；刷新走全像素，否则差分跳过白底。
 * 开机先调一次 e0470_waveform_init()。
 *
 * epdiy waveforms for the E0470A01 (684×1216, 40-pin).
 * Changing panel waveforms voids the warranty.
 *   E0470_WAVEFORM         Default: trimmed GC16 36 / GL16 37 + 50/50 threshold DU
 *   E0470_FULL_WAVEFORM    GC16 / GL16 48 phases each, one 0–50°C temp range
 *   E0470_GRAY8_WAVEFORM   8-gray: GC16 / GL16 30 phases; half the steps, ~360 ms full
 *   E0470_FOLLOW_WAVEFORM  Follow DU: 8 frames (black 7 / white 8) for touch ink
 * Default and 8-gray hang a threshold DU (dest 0–7→black, 8–15→white); the
 * source tables still drive only 0/15. GL16 adds one white push on 15→15;
 * refresh is full-pixel or the diff skips already-white.
 * Call e0470_waveform_init() once at boot.
 */

#pragma once

#include <stdint.h>

#include "epd_waveform.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 完整灰阶表：GC16 / GL16 各 48 相，白推动梯子 17 档、去重后 11 级可分辨灰阶。
/// DU 是 20 相，只驱动目标 0/15；E0470_FULL_WAVEFORM 原样保留对照。
/// / Full gray tables: GC16 / GL16 48 phases, 17-step white-push ladder,
/// 11 distinguishable after collapse. DU is 20 phases, dest 0/15 only;
/// E0470_FULL_WAVEFORM is kept as a reference.
#define E0470_FULL_DU_FRAMES 20
#define E0470_FULL_GC16_FRAMES 48
#define E0470_FULL_GL16_FRAMES 48
extern const EpdWaveform E0470_FULL_WAVEFORM;

/// 另一份表：只有 8 级可分辨灰阶（净推动梯子 8 档），30 相，整屏约 360ms（默认表约 430ms）。
/// 灰阶档数 = 白推动梯子的档数，在这块屏上 16 级与 30 相不可兼得，所以它是备选不是默认。
/// / Alternate table: 8 distinguishable grays (8-step net push), 30 phases,
/// ~360 ms full (default ~430 ms). Gray count is the white-push ladder; 16
/// levels and 30 phases cannot both fit this panel, so this is optional.
#define E0470_GRAY8_GC16_FRAMES 30
#define E0470_GRAY8_GL16_FRAMES 30
extern const EpdWaveform E0470_GRAY8_WAVEFORM;

/// 裁剪版保留新屏所需的 3 个尾部保持相。完整48相实测未明显改善边缘残影，因此恢复
/// 已验证的主动段裁剪，避免无收益地增加约150ms；无损前导保持跳过仍由渲染器执行。
/// / Trim keeps the 3 trailing hold phases the new panel needs. The full 48
/// did not clearly help edge ghosting, so the proven active-segment trim is
/// restored instead of adding ~150 ms for no gain. Lossless leading-hold
/// skip is still done by the renderer.
#define E0470_TRIM_ERASE_MAX 11
#define E0470_TRIM_SAT_CUT 5
#define E0470_TRIM_WHITE_SAT_CUT 0
#define E0470_TRIM_HOLD 3
#define E0470_GC16_FRAMES 36
#define E0470_GL16_FRAMES 37

/// 帧周期取算法文档第 8 节的设计点 ν≈90Hz。
/// / Frame period is the §8 design point ν≈90 Hz.
#define E0470_WAVEFORM_FRAME_US 11090
extern const EpdWaveform E0470_WAVEFORM;

/// 文字转页表：裁剪后的 GL16（37 相）**左对齐**，对角线（to == from）整条置为保持。
/// 两件事各自对应一种可见的闪：
///   1. 原地重推一个黑像素会先擦白再推黑 —— 对角线清零后不再发生。
///   2. e0470_waveform_trim() 把序列右对齐到保持相之前，于是「新文字推黑」13 相比
///      「旧文字擦白」18 相晚 5 相开始，中间整页是白的；左对齐后两者同起拍，是交叉
///      淡化，没有白场，相数仍是 37（比未裁剪的 48 相快约 120ms）。
/// 静止内容的刷新交给周期性 GC16，与原厂 APP_GC16_EVERY 同一分工。
/// / Text-turn table: the trimmed GL16 (37 phases) **left-aligned**, with the whole
/// `to == from` diagonal held. Two things each account for a visible flash:
///   1. Re-driving a black pixel in place erases it white first; clearing the diagonal
///      stops that.
///   2. e0470_waveform_trim() right-aligns sequences against the hold phases, so the
///      13-phase write of the new text starts 5 phases after the 18-phase erase of the old
///      one and the page is blank white in between; left-aligned they start together, a
///      cross-fade with no white field, still 37 phases (~120 ms faster than untrimmed).
/// Static content is refreshed by the periodic GC16 instead, the same division of labour as
/// the vendor's APP_GC16_EVERY.
extern const EpdWaveform E0470_TEXTTURN_WAVEFORM;

/// 触摸笔迹跟手用的 8 帧短 DU，走 FAST 扫描（帧周期约 7ms）。
/// 推动次数按 |to-from| 比例分配，满幅迁移往黑推 7 相、往白推 8 相。
/// / 8-frame short DU for touch ink, FAST scan (~7 ms/frame). Push count
/// scales with |to-from|; full travel is 7 black / 8 white.
#define E0470_FOLLOW_BLACK_FRAMES 7
#define E0470_FOLLOW_WHITE_FRAMES 8
#define E0470_FOLLOW_FRAMES 8
extern const EpdWaveform E0470_FOLLOW_WAVEFORM;

/// 按上面的公式生成 frames 帧的跟随表写进 dst（容量 frames×16×4 字节）。
/// / Build a follow table of `frames` into dst (frames×16×4 bytes).
void e0470_follow_lut_build(int frames, uint8_t (*dst)[16][4]);

/// 在一条 EpdWaveform 里按模式（MODE_GC16 / MODE_GL16 / MODE_DU）找唯一温度档的相位表。
/// / Look up the single temp-range phase table for MODE_GC16 / MODE_GL16 / MODE_DU.
const EpdWaveformPhases* e0470_waveform_phases(const EpdWaveform* waveform, int mode);

/// 查 (from→to) 在第 phase 相的 2bit 动作：0 保持、1 压黑、2 擦白。
/// / 2-bit action for (from→to) at `phase`: 0 hold, 1 darken, 2 erase.
int e0470_phase_action(const EpdWaveformPhases* phases, int phase, int to, int from);

/// 生成 E0470_WAVEFORM 与 E0470_FOLLOW_WAVEFORM 的表。必须在第一次刷新之前调用一次。
/// / Build E0470_WAVEFORM and E0470_FOLLOW_WAVEFORM. Call once before the first refresh.
void e0470_waveform_init(void);

#ifdef __cplusplus
}
#endif
