#include "render.h"

#include "epd_board.h"
#include "epd_internals.h"
#include "epdiy.h"

#include <assert.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
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
#include "output_common/spatial_wipe.h"
#include "output_common/render_method.h"
#ifdef RENDER_METHOD_I2S
#include "output_i2s/render_i2s.h"
#endif
#include "output_lcd/render_lcd.h"

static inline int min(int x, int y) {
    return x < y ? x : y;
}
static inline int max(int x, int y) {
    return x > y ? x : y;
}

const int clear_cycle_time = 15;

#define RTOS_ERROR_CHECK(x)       \
    do {                          \
        esp_err_t __err_rc = (x); \
        if (__err_rc != pdPASS) { \
            abort();              \
        }                         \
    } while (0)

static RenderContext_t render_context;

// ESP-IDF task stack sizes and high-water marks are both measured in bytes.
enum {
    EPD_PREP_STACK_BYTES = 4096,
    EPD_PREP_STACK_RESERVE_PERCENT = 20,
    EPD_PREP_STACK_RESERVE_BYTES =
        (EPD_PREP_STACK_BYTES * EPD_PREP_STACK_RESERVE_PERCENT + 99) / 100,
};
static unsigned prep_stack_minimum[NUM_RENDER_THREADS];
static unsigned prep_stack_reported[NUM_RENDER_THREADS];
static bool prep_stack_warned[NUM_RENDER_THREADS];
static int64_t prep_stack_report_us;

// Only the renderer caller samples/logs, after a waveform has finished. Never
// scan stack fill bytes or print from the high-priority feeder/ISR hot path.
static void report_prep_stack_watermarks(void) {
    const int64_t now = esp_timer_get_time();
    const bool periodic = now - prep_stack_report_us >= 60000000;
    for (int i = 0; i < NUM_RENDER_THREADS; ++i) {
        if (render_context.feed_tasks[i] == NULL) continue;
        const unsigned free_bytes = uxTaskGetStackHighWaterMark(render_context.feed_tasks[i]);
        if (free_bytes < prep_stack_minimum[i]) prep_stack_minimum[i] = free_bytes;
        const unsigned minimum = prep_stack_minimum[i];
        const bool below_reserve = minimum < EPD_PREP_STACK_RESERVE_BYTES;
        const bool first = prep_stack_reported[i] == UINT32_MAX;
        const bool dropped = !first && prep_stack_reported[i] >= minimum + 64;
        if (first || periodic || dropped || (below_reserve && !prep_stack_warned[i])) {
            const unsigned percent = minimum * 100 / EPD_PREP_STACK_BYTES;
            if (below_reserve) {
                ESP_LOGE("EpdStack", "epd_prep core=%d min_free=%u/%uB (%u%%) used_peak=%uB reserve=%uB BELOW-20%%; check stack depth/corruption",
                         i, minimum, EPD_PREP_STACK_BYTES, percent,
                         EPD_PREP_STACK_BYTES - minimum, EPD_PREP_STACK_RESERVE_BYTES);
            } else {
                ESP_LOGI("EpdStack", "epd_prep core=%d min_free=%u/%uB (%u%%) used_peak=%uB reserve=%uB OK",
                         i, minimum, EPD_PREP_STACK_BYTES, percent,
                         EPD_PREP_STACK_BYTES - minimum, EPD_PREP_STACK_RESERVE_BYTES);
            }
            prep_stack_reported[i] = minimum;
        }
        prep_stack_warned[i] = below_reserve;
    }
    if (periodic) prep_stack_report_us = now;
}

void epd_push_pixels(EpdRect area, short time, int color) {
    render_context.area = area;
#ifdef RENDER_METHOD_LCD
    epd_push_pixels_lcd(&render_context, time, color);
#else
    epd_push_pixels_i2s(&render_context, area, time, color);
#endif
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
        ESP_LOGI(
            "epd_path",
            "[upd] waveform_idx=%d type=%u phases=%u temp_range=%d mode=0x%X",
            waveform_index,
            (unsigned)waveform->mode_data[waveform_index]->type,
            (unsigned)frame_count,
            waveform_range,
            (unsigned)mode
        );
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
    render_context.spatial_luts=NULL;
    render_context.spatial_bands=NULL;
    if(mode & MODE_SPATIAL_WIPE_GL) {
#if defined(CONFIG_IDF_TARGET_ESP32S31) && defined(RENDER_METHOD_LCD)
        if(!(mode & MODE_PACKING_1PPB_DIFFERENCE) || !waveform_phases ||
           area.x || area.y || area.width!=epd_width() || area.height!=epd_height() ||
           crop_to.x || crop_to.y || crop_to.width!=area.width || crop_to.height!=area.height)
            return EPD_DRAW_INVALID_CROP;
        const int orient=(mode>>13)&3;
        render_context.spatial_by_y=orient==0 || orient==3;
        const int axis=render_context.spatial_by_y ? epd_height() : epd_width();
        render_context.spatial_luts=heap_caps_malloc(10*256,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        render_context.spatial_bands=heap_caps_malloc(axis,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(!render_context.spatial_luts || !render_context.spatial_bands) {
            free(render_context.spatial_luts);free(render_context.spatial_bands);
            render_context.spatial_luts=render_context.spatial_bands=NULL;
            return EPD_DRAW_FAILED_ALLOC;
        }
        epd_spatial_bands(render_context.spatial_bands,axis,orient==0 || orient==2);
        frame_count+=9;
#else
        return EPD_DRAW_LOOKUP_NOT_IMPLEMENTED;
#endif
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
    render_context.current_frame = 0;
    render_context.cycle_frames = frame_count;
    render_context.phase_times = NULL;
    if (waveform_phases != NULL && waveform_phases->phase_times != NULL) {
        render_context.phase_times = waveform_phases->phase_times;
    }
    if(render_context.spatial_luts) render_context.phase_times=NULL;

    epd_populate_line_mask(
        render_context.line_mask, drawn_columns, render_context.display_width / 4
    );

#ifdef RENDER_METHOD_I2S
    i2s_do_update(&render_context);
#elif defined(RENDER_METHOD_LCD)
    lcd_do_update(&render_context);
#endif

    report_prep_stack_watermarks();

    free(render_context.spatial_luts);free(render_context.spatial_bands);
    render_context.spatial_luts=render_context.spatial_bands=NULL;
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

#ifdef RENDER_METHOD_LCD
        lcd_calculate_frame(&render_context, thread_id);
#elif defined(RENDER_METHOD_I2S)
        if (thread_id == 0) {
            i2s_fetch_frame_data(&render_context, thread_id);
        } else {
            i2s_output_frame(&render_context, thread_id);
        }
#endif

        xSemaphoreGive(render_context.feed_done_smphr[thread_id]);
    }
}

void epd_clear_area(EpdRect area) {
    epd_clear_area_cycles(area, 3, clear_cycle_time);     //  3 --->   1  20251124 test
}

enum EpdDrawError epd_clear_checked(void) {
    render_context.error = EPD_DRAW_SUCCESS;
    epd_clear_area_cycles(epd_full_screen(), 3, clear_cycle_time);
    return render_context.error;
}

void epd_clear_area_cycles(EpdRect area, int cycles, int cycle_time) {
    const short white_time = cycle_time;
    const short dark_time = cycle_time;

    for (int c = 0; c < cycles; c++) {
        for (int i = 0; i < 10; i++) {                       //  10 --->   3  20251124 test
            epd_push_pixels(area, dark_time, 0);
            if (render_context.error) return;
        }
        for (int i = 0; i < 10; i++) {                       //  10 --->   3  20251124 test
            epd_push_pixels(area, white_time, 1);
            if (render_context.error) return;
        }
        for (int i = 0; i < 3; i++) {                       //  3 --->   1  20251124 test
            epd_push_pixels(area, white_time, 2);
            if (render_context.error) return;
        }
    }
}

void epd_renderer_init(enum EpdInitOptions options) {
    // Either the board should be set in menuconfig or the epd_set_board() must
    // be called before epd_init()
    assert((epd_current_board() != NULL));

    epd_current_board()->init(epd_width());
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
        return;
    }

    ESP_LOGI("epd", "Space used for waveform LUT: %dK", lut_size / 1024);
    render_context.conversion_lut
        = (uint8_t*)heap_caps_malloc(lut_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (render_context.conversion_lut == NULL) {
        ESP_LOGE("epd", "could not allocate LUT!");
        abort();
    }
    render_context.conversion_lut_size = lut_size;
    render_context.static_line_buffer = NULL;

    render_context.frame_done = xSemaphoreCreateBinary();

    for (int i = 0; i < NUM_RENDER_THREADS; i++) {
        render_context.feed_done_smphr[i] = xSemaphoreCreateBinary();
    }

    // When using the LCD peripheral, we may need padding lines to
    // satisfy the bounce buffer size requirements
    render_context.line_threads = (uint8_t*)heap_caps_malloc(
        rounded_display_height(), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    assert(render_context.line_threads != NULL);
    render_context.line_ready=heap_caps_calloc(rounded_display_height(),1,MALLOC_CAP_INTERNAL);
    assert(render_context.line_ready!=NULL);

    int queue_len = 32;
    if (options & EPD_FEED_QUEUE_32) {
        queue_len = 32;
    } else if (options & EPD_FEED_QUEUE_8) {
        queue_len = 8;
    }
    const EpdDisplay_t* display = epd_get_display();
    if (display != NULL && display->bus_width == 16) {
        queue_len = max(queue_len, 64);
    }
    // 流式预填约 127 行、两线程均分；128 行缓冲约 2～3ms，吃短毛刺（仍靠 panel busy 挡 WiFi）
    queue_len = max(queue_len, 128);
    ESP_LOGD("epd", "line queue len=%d threads=%d (stream prefill)", queue_len, NUM_RENDER_THREADS);

    if (render_context.conversion_lut == NULL) {
        ESP_LOGE("epd", "could not allocate line mask!");
        abort();
    }

    render_context.line_mask
        = heap_caps_aligned_alloc(16, epd_width() / 4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    assert(render_context.line_mask != NULL);

#ifdef RENDER_METHOD_LCD
    size_t queue_elem_size = render_context.display_width / 4;
#elif defined(RENDER_METHOD_I2S)
    size_t queue_elem_size = render_context.display_width;
#endif

    for (int i = 0; i < NUM_RENDER_THREADS; i++) {
        render_context.line_queues[i] = lq_init(queue_len, queue_elem_size);
        prep_stack_minimum[i] = EPD_PREP_STACK_BYTES;
        prep_stack_reported[i] = UINT32_MAX;
        prep_stack_warned[i] = false;
        RTOS_ERROR_CHECK(xTaskCreatePinnedToCoreWithCaps(
            render_thread,
            "epd_prep",
            EPD_PREP_STACK_BYTES,
            (void*)i,
            configMAX_PRIORITIES - 1, // 流式短队列：feed 必须压过 WiFi 等，否则易欠载
            &render_context.feed_tasks[i],
            i,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        ));
    }
    ESP_LOGI("EpdStack", "epd_prep x%d stack=%uB in SPIRAM",
             NUM_RENDER_THREADS, (unsigned)EPD_PREP_STACK_BYTES);
    prep_stack_report_us = esp_timer_get_time();
    report_prep_stack_watermarks();

    const size_t elem_aligned = ((queue_elem_size + 15) / 16) * 16;
    const size_t line_q_bytes =
        (size_t)NUM_RENDER_THREADS * (size_t)queue_len * elem_aligned;
    const size_t line_threads_bytes = (size_t)rounded_display_height();
    const size_t line_mask_bytes = (size_t)epd_width() / 4;
    ESP_LOGD("epd_dram",
             "render: lut=%u INTERNAL; line_q≈%u B; mask/thr INTERNAL=%u B",
             (unsigned)lut_size, (unsigned)line_q_bytes,
             (unsigned)(line_threads_bytes + line_mask_bytes));
}

void epd_renderer_deinit() {
    const EpdBoardDefinition* epd_board = epd_current_board();

    epd_board->poweroff(epd_ctrl_state());

    for (int i = 0; i < NUM_RENDER_THREADS; i++) {
        if (render_context.feed_tasks[i] != NULL) {
            vTaskDeleteWithCaps(render_context.feed_tasks[i]);
            render_context.feed_tasks[i] = NULL;
        }
        lq_free(&render_context.line_queues[i]);
        vSemaphoreDelete(render_context.feed_done_smphr[i]);
    }

#ifdef RENDER_METHOD_I2S
    i2s_deinit();
#endif

    epd_control_reg_deinit();

    if (epd_board->deinit) {
        epd_board->deinit();
    }

    heap_caps_free(render_context.conversion_lut);
    heap_caps_free(render_context.line_threads);
    heap_caps_free(render_context.line_ready);
    heap_caps_free(render_context.line_mask);
    vSemaphoreDelete(render_context.frame_done);
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
#ifdef RENDER_METHOD_I2S
    return _interlace_line_unaligned(to, from, interlaced, col_dirtyness, fb_width) > 0;
#elif defined(RENDER_METHOD_LCD)
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
#endif
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

    for (int y = crop_to.y; y < y_end; y++) {
        uint32_t offset = y * fb_width / 2;
        int dirty = _epd_interlace_line(
            to + offset, from + offset, interlaced + offset * 2, col_dirtyness, fb_width
        );
        dirty_lines[y] = dirty;
    }

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
