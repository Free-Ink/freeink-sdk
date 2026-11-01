/**
 * @brief EPDiy 在 ESP-IDF 6.x 上的最小 RMT legacy 兼容
 * @note 补齐已移除的 rmt_item32_t / rmt_mem_t 等类型，便于沿用现有 LL 路径。
 */
#pragma once

#include <stdint.h>
#include <soc/soc_caps.h>

#if !__has_include(<driver/rmt_types_legacy.h>)
/** RMT 符号项（legacy 布局，与硬件内存字一致） */
typedef union {
    struct {
        uint32_t duration0 : 15; // 第一段时长
        uint32_t level0 : 1;     // 第一段电平
        uint32_t duration1 : 15; // 第二段时长
        uint32_t level1 : 1;     // 第二段电平
    };
    uint32_t val; // 整字访问
} rmt_item32_t;

#ifndef RMT_CHANNEL_1
#define RMT_CHANNEL_1 1
#endif

#ifndef RMT_IDLE_LEVEL_LOW
#define RMT_IDLE_LEVEL_LOW 0
#endif

#ifndef SOC_RMT_CHANNELS_PER_GROUP
#define SOC_RMT_CHANNELS_PER_GROUP 8
#endif

/** RMT 通道内存镜像（对应外设 RMTMEM 链接符号） */
typedef struct {
    struct {
        volatile rmt_item32_t data32[SOC_RMT_MEM_WORDS_PER_CHANNEL];
    } chan[SOC_RMT_CHANNELS_PER_GROUP];
} rmt_mem_t;
#endif
