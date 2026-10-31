/**
 * @brief 从完整灰阶表派生更短表
 *
 * 骨架：[保持][擦除][饱和][灰阶尾][保持]
 * 保留尾巴，裁擦除上限与饱和开头，再右对齐。
 */

#pragma once

#include <stdint.h>

#include "epd_internals.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int erase_max; // 擦除段最多保留几相（源表最长 19）
    int sat_cut; // 黑饱和段（to≠15）开头砍掉几相
    int white_sat_cut; // 白饱和段（to=15）开头砍掉几相
    int hold; // 结尾保持相数（至少 1）
} my_waveform_trim_t;

/**
 * @brief 按参数从 src 派生表写进 dst_data（容量 ≥ src->phases）
 * @return 新相数；参数不合法返回 0
 */
int my_waveform_trim(const EpdWaveformPhases* src, const my_waveform_trim_t* trim,
                     uint8_t (*dst_data)[16][4]);

#ifdef __cplusplus
}
#endif
