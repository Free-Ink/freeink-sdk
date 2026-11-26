/* FreeInk local change (2026-09-30): recover partial initialization; reuse clear buffers. */
#include "render.h"

#include "epd_board.h"
#include "epd_internals.h"
#include "epdiy.h"

#include <assert.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "output_common/line_queue.h"
#include "output_common/lut.h"
#include "output_common/render_context.h"
#include "output_common/render_method.h"
#include "output_lcd/render_lcd.h"

static inline int min(int x, int y) {
    return x < y ? x : y;
}
static inline int max(int x, int y) {
    return x > y ? x : y;
}

const int clear_cycle_time = 15;

static RenderContext_t render_context;
static bool board_init_attempted;
static bool renderer_ready;

void epd_push_pixels(EpdRect area, short time, int color) {
    render_context.area = area;
    epd_push_pixels_lcd(&render_context, time, color);
}

///////////////////////////// Coordination ///////////////////////////////

/**
 * Find the waveform temperature range index for a given temperature in °C.
 * If no range in the waveform data fits the given temperature, return the
 * closest one.
 * Returns -1 if the waveform does not contain any temperature range.
 */
int waveform_temp_range_index(const EpdWaveform* waveform, int temperature) {
    int idx = 0;
    if (waveform->num_temp_ranges == 0) {
        return -1;
    }
    while (idx < waveform->num_temp_ranges - 1 && waveform->temp_intervals[idx].min < temperature) {
        idx++;
    }
    return idx;
}

static int get_waveform_index(const EpdWaveform* waveform, enum EpdDrawMode mode) {
    for (int i = 0; i < waveform->num_modes; i++) {
        if (waveform->mode_data[i]->type == (mode & 0x3F)) {
            return i;
        }
    }
    return -1;
}

/////////////////////////////  API Procedures //////////////////////////////////

/// Rounded up display height for even division into multi-line buffers.
// ---- 前导保持相跳过 ----

static bool s_leading_skip_enabled;
static int s_last_leading_skip;
static uint8_t s_present[256];
static const uint8_t* s_present_for;

// 按波形表缓存每个 (to<<4|from) 最早动作的相号，同一张表反复刷不用重算。
static const EpdWaveformPhases* s_first_active_phases;
static uint8_t s_first_active[256];

void epd_set_leading_skip(bool enable) {
    s_leading_skip_enabled = enable;
}

void epd_leading_skip_discard(void) {
    s_present_for = NULL;
}

int epd_last_leading_skip(void) {
    return s_last_leading_skip;
}

static void build_first_active(const EpdWaveformPhases* phases) {
    const uint8_t(*luts)[16][4] = (const uint8_t(*)[16][4])phases->luts;
    for (int to = 0; to < 16; to++) {
        for (int from = 0; from < 16; from++) {
            int first = phases->phases;
            for (int f = 0; f < phases->phases; f++) {
                if ((luts[f][to][from / 4] >> (6 - 2 * (from % 4))) & 3) {
                    first = f;
                    break;
                }
            }
            s_first_active[(to << 4) | from] = (uint8_t)first;
        }
    }
    s_first_active_phases = phases;
}

// 差分图里出现过哪些 (to<<4)|from 字节。差分图在 PSRAM 里，事后再扫一遍要 15ms 左右，
// 所以尽量在算差分时顺手统计：那时这一行刚写完还在 cache 里。s_present_for 记的是
// 统计对应的差分缓冲，epd_draw_base 拿到同一块缓冲才用它，否则退回整块扫。

// 从 4bpp 的 to / from 源行统计：差分图是写出去的，PSRAM 写不进 cache，回头读一遍
// 要 13ms；源行刚被差分读过，还在 cache 里。每个 32bit 字装 8 个像素，高低半字节
// 各拼出 4 个 (to<<4)|from。相邻字都相同就跳过——白底整行都是 0xFF。
__attribute__((optimize("O3"))) static void mark_present_line(
    const uint8_t* to, const uint8_t* from, int fb_width
) {
    const uint32_t* tw = (const uint32_t*)to;
    const uint32_t* fw = (const uint32_t*)from;
    uint32_t prev_t = ~tw[0];
    uint32_t prev_f = 0;
    for (int i = 0; i < fb_width / 8; i++) {
        const uint32_t t = tw[i];
        const uint32_t f = fw[i];
        if (t == prev_t && f == prev_f) continue;
        prev_t = t;
        prev_f = f;
        const uint32_t hi = (t & 0xF0F0F0F0u) | ((f & 0xF0F0F0F0u) >> 4);
        const uint32_t lo = ((t & 0x0F0F0F0Fu) << 4) | (f & 0x0F0F0F0Fu);
        s_present[hi & 0xFF] = 1;
        s_present[(hi >> 8) & 0xFF] = 1;
        s_present[(hi >> 16) & 0xFF] = 1;
        s_present[hi >> 24] = 1;
        s_present[lo & 0xFF] = 1;
        s_present[(lo >> 8) & 0xFF] = 1;
        s_present[(lo >> 16) & 0xFF] = 1;
        s_present[lo >> 24] = 1;
    }
}

// 取出现过的对里最早动作的相号。data 不是刚统计过的那块时整块扫（含 crop 外的部分，
// 只会让结果更保守）。
__attribute__((optimize("O3"))) static int leading_skip_frames(
    const EpdWaveformPhases* phases, const uint8_t* data, EpdRect area
) {
    if (phases != s_first_active_phases) build_first_active(phases);

    uint8_t* present = s_present;
    if (s_present_for != data) {
        memset(s_present, 0, sizeof(s_present));
        const size_t n = (size_t)area.width * (size_t)area.height;
        for (size_t i = 0; i < n; i++) s_present[data[i]] = 1;
    }

    int skip = phases->phases;
    for (int i = 0; i < 256; i++) {
        if (present[i] && s_first_active[i] < skip) skip = s_first_active[i];
    }
    // 全是保持也至少扫一相，把上一相残留在像素上的电压写成 0。
    if (skip >= phases->phases) skip = phases->phases - 1;
    return skip < 0 ? 0 : skip;
}

static inline int rounded_display_height() {
    return (((epd_height() + 7) / 8) * 8);
}

// FIXME: fix misleading naming:
//  area -> buffer dimensions
//  crop -> area taken out of buffer
enum EpdDrawError IRAM_ATTR epd_draw_base(
    EpdRect area,
    const uint8_t* data,
    EpdRect crop_to,
    enum EpdDrawMode mode,
    int temperature,
    const bool* drawn_lines,
    const uint8_t* drawn_columns,
    const EpdWaveform* waveform
) {
    if (waveform == NULL) {
        return EPD_DRAW_NO_PHASES_AVAILABLE;
    }
    int waveform_range = waveform_temp_range_index(waveform, temperature);
    if (waveform_range < 0) {
        return EPD_DRAW_NO_PHASES_AVAILABLE;
    }
    int waveform_index = 0;
    uint8_t frame_count = 0;
    const EpdWaveformPhases* waveform_phases = NULL;

    // no waveform required for monochrome mode
    if (!(mode & MODE_EPDIY_MONOCHROME)) {
        waveform_index = get_waveform_index(waveform, mode);
        if (waveform_index < 0) {
            return EPD_DRAW_MODE_NOT_FOUND;
        }

        waveform_phases = waveform->mode_data[waveform_index]->range_data[waveform_range];
        // FIXME: error if not present
        frame_count = waveform_phases->phases;
    } else {
        frame_count = 1;
    }

    if (crop_to.width < 0 || crop_to.height < 0) {
        return EPD_DRAW_INVALID_CROP;
    }

    const bool crop = (crop_to.width > 0 && crop_to.height > 0);
    if (crop
        && (crop_to.width > area.width || crop_to.height > area.height || crop_to.x > area.width
            || crop_to.y > area.height)) {
        return EPD_DRAW_INVALID_CROP;
    }

#ifdef RENDER_METHOD_LCD
    if (mode & MODE_PACKING_1PPB_DIFFERENCE && render_context.conversion_lut_size > 1 << 10) {
        ESP_LOGI(
            "epdiy",
            "Using optimized vector implementation on the ESP32-S3, only 1k of %d LUT in use!",
            render_context.conversion_lut_size
        );
    }
#endif

    LutFunctionPair lut_functions = find_lut_functions(mode, render_context.conversion_lut_size);
    if (lut_functions.build_func == NULL || lut_functions.lookup_func == NULL) {
        ESP_LOGE("epdiy", "no output lookup method found for your mode and LUT size!");
        return EPD_DRAW_LOOKUP_NOT_IMPLEMENTED;
    }

    render_context.area = area;
    render_context.crop_to = crop_to;
    render_context.waveform_range = waveform_range;
    render_context.waveform_index = waveform_index;
    render_context.mode = mode;
    render_context.waveform = waveform;
    render_context.error = EPD_DRAW_SUCCESS;
    render_context.drawn_lines = drawn_lines;
    render_context.data_ptr = data;
    render_context.lut_build_func = lut_functions.build_func;
    render_context.lut_lookup_func = lut_functions.lookup_func;

    render_context.lines_prepared = 0;
    render_context.lines_consumed = 0;
    render_context.lines_total = rounded_display_height();

    // 试过按最后一个脏行截短帧长来省扫描时间（局部刷新时尾部大片行没有变化，
    // 却照样在按 line length 逐行扫）。结论是不行：连续帧之间面板 gate 的复位
    // 依赖走完完整的一屏，start_frame 里那个 STV 脉冲拽不回第 0 行，于是第二、
    // 三帧的数据接着上一帧停下的行位置继续贴，画面会出现几个沿行方向并列、铺满
    // 全屏的副本。要再动这里得先用示波器确认 STV/CKV 的 gate 复位时序。

    // 差分模式下先跳过整屏都还在保持的前导相位，见 epd_set_leading_skip()。单相位的
    // 波形（连续 DU 一次只发一相）没有可跳的，别为它扫一遍差分图。
    int skip = 0;
    if (s_leading_skip_enabled && waveform_phases != NULL && waveform_phases->phases > 1
        && (mode & MODE_PACKING_1PPB_DIFFERENCE)) {
        skip = leading_skip_frames(waveform_phases, data, area);
    }
    s_last_leading_skip = skip;
    s_present_for = NULL;

    render_context.current_frame = skip;
    render_context.cycle_frames = frame_count - skip;
    render_context.phase_times = NULL;
    if (waveform_phases != NULL && waveform_phases->phase_times != NULL) {
        render_context.phase_times = waveform_phases->phase_times;
    }

    epd_populate_line_mask(
        render_context.line_mask, drawn_columns, render_context.display_width / 4
    );

    lcd_do_update(&render_context);

    if (render_context.error & EPD_DRAW_EMPTY_LINE_QUEUE) {
        ESP_LOGE("epdiy", "line buffer underrun occurred!");
    }

    if (render_context.error != EPD_DRAW_SUCCESS) {
        return render_context.error;
    }
    return EPD_DRAW_SUCCESS;
}

static void IRAM_ATTR render_thread(void* arg) {
    int thread_id = (int)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        lcd_calculate_frame(&render_context, thread_id);

        xSemaphoreGive(render_context.feed_done_smphr[thread_id]);
    }
}

void epd_clear_area(EpdRect area) {
    epd_clear_area_cycles(area, 3, clear_cycle_time);     //  3 --->   1  20251124 test
}

void epd_clear_area_cycles(EpdRect area, int cycles, int cycle_time) {
    const short white_time = cycle_time;
    const short dark_time = cycle_time;

    for (int c = 0; c < cycles; c++) {
        for (int i = 0; i < 10; i++) {                       //  10 --->   3  20251124 test
            epd_push_pixels(area, dark_time, 0);                  
        }
        // 此面板擦白需要比压黑更多相位，否则容易停在浅灰。
        for (int i = 0; i < 13; i++) {
            epd_push_pixels(area, white_time, 1);
        }
        for (int i = 0; i < 3; i++) {                       //  3 --->   1  20251124 test
            epd_push_pixels(area, white_time, 2);            
        }
    }
}

bool epd_renderer_init(enum EpdInitOptions options) {
    if (renderer_ready) return true;
    const EpdBoardDefinition* board = epd_current_board();
    if (board == NULL || board->init == NULL) return false;
    board_init_attempted = true;
    if (!board->init(epd_width())) goto fail;
    epd_control_reg_init();

    render_context.display_width = epd_width();
    render_context.display_height = epd_height();
    size_t lut_size = 0;
    if (options & EPD_LUT_1K) {
        lut_size = 1 << 10;
    } else if (options & EPD_LUT_64K) {
        lut_size = 1 << 16;
    } else if (options == EPD_OPTIONS_DEFAULT) {
#ifdef RENDER_METHOD_LCD
        lut_size = 1 << 10;
#else
        lut_size = 1 << 16;
#endif
    } else {
        ESP_LOGE("epd", "invalid init options: %d", options);
        goto fail;
    }
    // Owned by render_context; DMA/feed tasks need these buffers beyond this call.
    render_context.conversion_lut = heap_caps_malloc(lut_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (render_context.conversion_lut == NULL) goto fail;
    render_context.conversion_lut_size = lut_size;
    render_context.frame_done = xSemaphoreCreateBinary();
    if (render_context.frame_done == NULL) goto fail;
    for (int i = 0; i < NUM_RENDER_THREADS; i++) {
        render_context.feed_done_smphr[i] = xSemaphoreCreateBinary();
        if (render_context.feed_done_smphr[i] == NULL) goto fail;
    }
    render_context.line_threads = heap_caps_malloc(rounded_display_height(), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (render_context.line_threads == NULL) goto fail;
    render_context.line_mask = heap_caps_aligned_alloc(16, epd_width() / 4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (render_context.line_mask == NULL) goto fail;

    int queue_len = (options & EPD_FEED_QUEUE_8) ? 8 : 32;
    if (epd_get_display()->bus_width == 16) queue_len = 64;
    for (int i = 0; i < NUM_RENDER_THREADS; i++) {
        render_context.line_queues[i] = lq_init(queue_len, render_context.display_width / 4);
        if (render_context.line_queues[i].bufs == NULL) goto fail;
        render_context.feed_line_buffers[i] = heap_caps_malloc(render_context.display_width, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (render_context.feed_line_buffers[i] == NULL) goto fail;
    }
    // Start workers only after all buffers and synchronization objects exist.
    for (int i = 0; i < NUM_RENDER_THREADS; i++) {
        if (xTaskCreatePinnedToCore(render_thread, "epd_prep", 1 << 12, (void*)i,
                                  configMAX_PRIORITIES - 1, &render_context.feed_tasks[i], i) != pdPASS) goto fail;
    }
    renderer_ready = true;
    return true;
fail:
    ESP_LOGE("epd", "renderer initialization failed; releasing partial resources");
    epd_renderer_deinit();
    return false;
}

void epd_renderer_deinit() {
    const EpdBoardDefinition* board = epd_current_board();
    if (board_init_attempted && board != NULL) board->poweroff(epd_ctrl_state());
    for (int i = NUM_RENDER_THREADS - 1; i >= 0; i--) {
        // FreeRTOS interprets a null task handle as the calling task.
        if (render_context.feed_tasks[i] != NULL) vTaskDelete(render_context.feed_tasks[i]);
    }
    for (int i = NUM_RENDER_THREADS - 1; i >= 0; i--) {
        heap_caps_free(render_context.feed_line_buffers[i]);
        lq_free(&render_context.line_queues[i]);
    }
    heap_caps_free(render_context.line_mask);
    heap_caps_free(render_context.line_threads);
    for (int i = NUM_RENDER_THREADS - 1; i >= 0; i--) {
        if (render_context.feed_done_smphr[i] != NULL) vSemaphoreDelete(render_context.feed_done_smphr[i]);
    }
    if (render_context.frame_done != NULL) vSemaphoreDelete(render_context.frame_done);
    heap_caps_free(render_context.conversion_lut);
    if (board_init_attempted && board != NULL) {
        epd_control_reg_deinit();
        if (board->deinit != NULL) board->deinit();
    }
    memset(&render_context, 0, sizeof(render_context));
    board_init_attempted = false;
    renderer_ready = false;
}

#ifdef RENDER_METHOD_LCD
uint32_t epd_interlace_4bpp_line_VE(
    const uint8_t* to,
    const uint8_t* from,
    uint8_t* interlaced,
    uint8_t* col_dirtyness,
    int fb_width
);
#endif

/**
 * Interlaces `len` nibbles from the buffers `to` and `from` into `interlaced`.
 * In the process, tracks which nibbles differ in `col_dirtyness`.
 * Returns `1` if there are differences, `0` otherwise.
 * Does not require special alignment of the buffers beyond 32 bit alignment.
 */
__attribute__((optimize("O3"))) static inline int _interlace_line_unaligned(
    const uint8_t* to, const uint8_t* from, uint8_t* interlaced, uint8_t* col_dirtyness, int len
) {
    int dirty = 0;
    for (int x = 0; x < len; x++) {
        uint8_t t = *(to + x / 2);
        uint8_t f = *(from + x / 2);
        t = (x % 2) ? (t >> 4) : (t & 0x0f);
        f = (x % 2) ? (f >> 4) : (f & 0x0f);
        col_dirtyness[x / 2] |= (t ^ f) << (4 * (x % 2));
        dirty |= (t ^ f);
        interlaced[x] = (t << 4) | f;
    }
    return dirty;
}

/**
 * Interlaces the lines at `to`, `from` into `interlaced`.
 * returns `1` if there are differences, `0` otherwise.
 */
__attribute__((optimize("O3"))) bool _epd_interlace_line(
    const uint8_t* to,
    const uint8_t* from,
    uint8_t* interlaced,
    uint8_t* col_dirtyness,
    int fb_width
) {
    // Use Vector Extensions with the ESP32-S3.
    // Both input buffers should have the same alignment w.r.t. 16 bytes,
    // as asserted in epd_difference_image_base.
    uint32_t dirty = 0;

    // alignment boundaries in pixels
    int unaligned_len_front_px = ((16 - (uint32_t)to % 16) * 2) % 32;
    int unaligned_len_back_px = (((uint32_t)to + fb_width / 2) % 16) * 2;
    int unaligned_back_start_px = fb_width - unaligned_len_back_px;
    int aligned_len_px = fb_width - unaligned_len_front_px - unaligned_len_back_px;

    dirty |= _interlace_line_unaligned(to, from, interlaced, col_dirtyness, unaligned_len_front_px);
    dirty |= epd_interlace_4bpp_line_VE(
        to + unaligned_len_front_px / 2,
        from + unaligned_len_front_px / 2,
        interlaced + unaligned_len_front_px,
        col_dirtyness + unaligned_len_front_px / 2,
        aligned_len_px
    );
    dirty |= _interlace_line_unaligned(
        to + unaligned_back_start_px / 2,
        from + unaligned_back_start_px / 2,
        interlaced + unaligned_back_start_px,
        col_dirtyness + unaligned_back_start_px / 2,
        unaligned_len_back_px
    );
    return dirty;
}

void epd_difference_column_range(EpdRect crop_to, int* x_start, int* x_end) {
    const int fb_width = epd_width();
    int start = crop_to.x & ~31;
    int end = (crop_to.x + crop_to.width + 31) & ~31;
    if (start < 0) start = 0;
    if (end > fb_width) end = fb_width;
    if (end < start) end = start;
    *x_start = start;
    *x_end = end;
}

EpdRect epd_difference_image_base(
    const uint8_t* to,
    const uint8_t* from,
    EpdRect crop_to,
    int fb_width,
    int fb_height,
    uint8_t* interlaced,
    bool* dirty_lines,
    uint8_t* col_dirtyness
) {
    assert(fb_width % 8 == 0);
    assert(col_dirtyness != NULL);

    // these buffers should be allocated 16 byte aligned
    assert((uint32_t)to % 16 == 0);
    assert((uint32_t)from % 16 == 0);
    assert((uint32_t)col_dirtyness % 16 == 0);
    assert((uint32_t)interlaced % 16 == 0);

    memset(col_dirtyness, 0, fb_width / 2);
    memset(dirty_lines, 0, sizeof(bool) * fb_height);

    int x_end = min(fb_width, crop_to.x + crop_to.width);
    int y_end = min(fb_height, crop_to.y + crop_to.height);

    // 只算裁剪矩形覆盖的那段列（对齐到 32 像素 = 16 字节，向量实现要求两边缓冲同
    // 对齐）。局部刷新的区域旋转后往往覆盖几乎所有行，按整行算差分白花一倍多时间。
    // 段外的列不进 col_dirtyness，扫描时被行掩码归零，不会被驱动；highlevel 回写
    // back_fb 也只回写这一段，两边一致。
    int x_start, x_stop;
    epd_difference_column_range(crop_to, &x_start, &x_stop);
    const int seg_px = x_stop - x_start;

    if (s_leading_skip_enabled) memset(s_present, 0, sizeof(s_present));
    for (int y = crop_to.y; y < y_end; y++) {
        uint32_t offset = y * fb_width / 2 + x_start / 2;
        int dirty = _epd_interlace_line(
            to + offset, from + offset, interlaced + offset * 2, col_dirtyness + x_start / 2, seg_px
        );
        dirty_lines[y] = dirty;
        if (s_leading_skip_enabled) mark_present_line(to + offset, from + offset, seg_px);
    }
    s_present_for = s_leading_skip_enabled ? interlaced : NULL;

    int min_x, min_y, max_x, max_y;
    for (min_x = crop_to.x; min_x < x_end; min_x++) {
        uint8_t mask = min_x % 2 ? 0xF0 : 0x0F;
        if ((col_dirtyness[min_x / 2] & mask) != 0)
            break;
    }
    for (max_x = x_end - 1; max_x >= crop_to.x; max_x--) {
        uint8_t mask = min_x % 2 ? 0xF0 : 0x0F;
        if ((col_dirtyness[max_x / 2] & mask) != 0)
            break;
    }
    for (min_y = crop_to.y; min_y < y_end; min_y++) {
        if (dirty_lines[min_y] != 0)
            break;
    }
    for (max_y = y_end - 1; max_y >= crop_to.y; max_y--) {
        if (dirty_lines[max_y] != 0)
            break;
    }

    EpdRect crop_rect = {
        .x = min_x,
        .y = min_y,
        .width = max(max_x - min_x + 1, 0),
        .height = max(max_y - min_y + 1, 0),
    };

    return crop_rect;
}

EpdRect epd_difference_image(
    const uint8_t* to,
    const uint8_t* from,
    uint8_t* interlaced,
    bool* dirty_lines,
    uint8_t* col_dirtyness
) {
    return epd_difference_image_base(
        to,
        from,
        epd_full_screen(),
        epd_width(),
        epd_height(),
        interlaced,
        dirty_lines,
        col_dirtyness
    );
}

EpdRect epd_difference_image_cropped(
    const uint8_t* to,
    const uint8_t* from,
    EpdRect crop_to,
    uint8_t* interlaced,
    bool* dirty_lines,
    uint8_t* col_dirtyness
) {
    EpdRect result = epd_difference_image_base(
        to, from, crop_to, epd_width(), epd_height(), interlaced, dirty_lines, col_dirtyness
    );
    return result;
}
