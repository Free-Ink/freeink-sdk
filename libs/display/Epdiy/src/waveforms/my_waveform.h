#pragma once
#include "epd_internals.h"
#ifdef __cplusplus
extern "C" {
#endif
// Clear: complete GC/GL (48 phases) and DU (20 phases).
extern const EpdWaveform MY_WAVEFORM;
// Initialize both immutable profiles before epd_hl_init().
void my_waveform_init(void);
// Balanced: trimmed GC/GL (36/37 phases), unchanged DU; NULL if unavailable.
const EpdWaveform* my_waveform_fast(void);
// Extreme: 30-phase, 8-gray GL only; GC uses the 16-gray profiles.
const EpdWaveform* my_waveform_gray8(void);
const EpdWaveform* my_waveform_short_du(void);
/**
 * @brief 获取快速档普通局刷使用的十四相 DU 波形
 */
const EpdWaveform* my_waveform_fast_du(void);
// Binary wipe + reveal: band in target index, old/new binary flags in source.
const EpdWaveform* my_waveform_wipe_reveal(void);
const EpdWaveform* my_waveform_wipe_reveal_short(void);
// Profile DU white conditioning (20/8 phases) + profile GL from white.
// The renderer adds spatial band delays without consuming gray indices.
const EpdWaveform* my_waveform_wipe_gl(int profile);
#ifdef __cplusplus
}
#endif
