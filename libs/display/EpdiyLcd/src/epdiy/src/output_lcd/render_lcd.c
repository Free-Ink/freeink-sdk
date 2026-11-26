/* FreeInk local change (2026-09-30): recover partial initialization; reuse clear buffers. */
#include <stdint.h>
#include <string.h>

#include "../output_common/render_method.h"

#ifdef RENDER_METHOD_LCD

#include <esp_idf_version.h>
#include <esp_log.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#if __has_include(<rom/cache.h>)
#include <rom/cache.h>
#elif __has_include(<esp32s3/rom/cache.h>)
#include <esp32s3/rom/cache.h>
#endif
#else
#include <rom/cache.h>
#endif

#include "../epd_internals.h"
#include "../output_common/line_queue.h"
#include "../output_common/lut.h"
#include "../output_common/render_context.h"
#include "epd_board.h"
#include "epdiy.h"
#include "lcd_driver.h"
#include "render_lcd.h"

// declare vector optimized line mask application.
void epd_apply_line_mask_VE(uint8_t* line, const uint8_t* mask, int mask_len);

__attribute__((optimize("O3"))) static bool IRAM_ATTR
retrieve_line_isr(RenderContext_t* ctx, uint8_t* buf) {
    if (ctx->lines_consumed >= ctx->lines_total) {
        return false;
    }
    int thread = ctx->line_threads[ctx->lines_consumed];
    if (thread >= NUM_RENDER_THREADS) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        memset(buf, 0x00, ctx->display_width / 4);
        ctx->lines_consumed += 1;
        return pdFALSE;
    }

    LineQueue_t* lq = &ctx->line_queues[thread];

    BaseType_t awoken = pdFALSE;

    if (lq_read(lq, buf) != 0) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        memset(buf, 0x00, ctx->display_width / 4);
    }

    if (ctx->lines_consumed >= ctx->display_height) {
        memset(buf, 0x00, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return awoken;
}

/// start the next frame in the current update cycle
static void IRAM_ATTR handle_lcd_frame_done(RenderContext_t* ctx) {
    epd_lcd_frame_done_cb(NULL, NULL);
    epd_lcd_line_source_cb(NULL, NULL);

    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &task_awoken);

    portYIELD_FROM_ISR();
}

void lcd_do_update(RenderContext_t* ctx) {
    epd_set_mode(1);

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
        prepare_context_for_next_frame(ctx);

        // start both feeder tasks
        xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
        xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);

        // transmission is started in renderer threads, now wait util it's done
        xSemaphoreTake(ctx->frame_done, portMAX_DELAY);

        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            xSemaphoreTake(ctx->feed_done_smphr[i], portMAX_DELAY);
        }

        if (ctx->error) {
            for (int i = 0; i < NUM_RENDER_THREADS; i++) {
                lq_reset(&ctx->line_queues[i]);
            }
            break;
        }

        ctx->current_frame++;

        // make the watchdog happy.
        vTaskDelay(0);
    }

    epd_lcd_line_source_cb(NULL, NULL);
    epd_lcd_frame_done_cb(NULL, NULL);

    epd_set_mode(0);
}

__attribute__((optimize("O3"))) static bool IRAM_ATTR
push_pixels_isr(RenderContext_t* ctx, uint8_t* buf) {
    // Output no-op outside of drawn area
    if (ctx->lines_consumed < ctx->area.y) {
        memset(buf, 0, ctx->display_width / 4);
    } else if (ctx->lines_consumed >= ctx->area.y + ctx->area.height) {
        memset(buf, 0, ctx->display_width / 4);
    } else {
        memcpy(buf, ctx->static_line_buffer, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return pdFALSE;
}

/**
 * Populate the line mask for use in epd_push_pixels.
 */
static void push_pixels_populate_line(RenderContext_t* ctx, int color) {
    // Select fill pattern by draw color
    int fill_byte = 0;
    switch (color) {
        case 0:
            fill_byte = DARK_BYTE;
            break;
        case 1:
            fill_byte = CLEAR_BYTE;
            break;
        default:
            fill_byte = 0x00;
    }

    // Same two-bit enable mask as epd_populate_line_mask, without a scratch allocation.
    memset(ctx->line_mask, 0, ctx->display_width / 4);
    for (int x = 0; x < ctx->display_width; x++) {
        if (x >= ctx->area.x && x < ctx->area.x + ctx->area.width) {
            ctx->line_mask[x / 4] |= 3U << (2 * (x & 3));
        }
    }
    memset(ctx->static_line_buffer, fill_byte, ctx->display_width / 4);
    epd_apply_line_mask(ctx->static_line_buffer, ctx->line_mask, ctx->display_width / 4);

}

void epd_push_pixels_lcd(RenderContext_t* ctx, short time, int color) {
    ctx->current_frame = 0;
    ctx->lines_total = ctx->display_height;
    ctx->lines_consumed = 0;
    // Synchronous clear runs with both feed tasks idle. Borrow their existing
    // buffer until frame_done; only the renderer owns/frees it.
    ctx->static_line_buffer = ctx->feed_line_buffers[0];

    push_pixels_populate_line(ctx, color);
    epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
    epd_lcd_line_source_cb((line_cb_func_t)&push_pixels_isr, ctx);

    epd_set_mode(1);
    epd_lcd_start_frame();
    xSemaphoreTake(ctx->frame_done, portMAX_DELAY);
    epd_set_mode(0);

    ctx->static_line_buffer = NULL;
}

#define int_min(a, b) (((a) < (b)) ? (a) : (b))

// 每帧开扫之前先算好多少行。这段预填是帧与帧之间的空档：行队列只要跑在 DMA 前面
// 就行，一行 16us 才消费一条，喂线程几微秒就算完一条，所以预填不需要很多；太少了
// 会在 ISR 延迟抖动时欠载（EPD_DRAW_EMPTY_LINE_QUEUE）。
static int s_prefill_lines = 127;

void epd_lcd_set_prefill_lines(int lines) {
    if (lines < 4) lines = 4;
    s_prefill_lines = lines;
}

int epd_lcd_prefill_lines(void) {
    return s_prefill_lines;
}
__attribute__((optimize("O3"))) void IRAM_ATTR
lcd_calculate_frame(RenderContext_t* ctx, int thread_id) {
    assert(ctx->lut_lookup_func != NULL);
    uint8_t* input_line = ctx->feed_line_buffers[thread_id];

    LineQueue_t* lq = &ctx->line_queues[thread_id];
    int l = 0;

    // line must be able to hold 2-pixel-per-byte or 1-pixel-per-byte data
    memset(input_line, 0x00, ctx->display_width);

    EpdRect area = ctx->area;
    int min_y, max_y, bytes_per_line, _ppB;
    const uint8_t* ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &_ppB);

    assert(area.width == ctx->display_width && area.x == 0 && !ctx->error);

    // index of the line that triggers the frame output when processed
    int trigger_line = int_min(s_prefill_lines, max_y - min_y);

    while (l = atomic_fetch_add(&ctx->lines_prepared, 1), l < ctx->lines_total) {
        ctx->line_threads[l] = thread_id;

        // queue is sufficiently filled to fill both bounce buffers, frame
        // can begin
        if (l - min_y == trigger_line) {
            epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
            epd_lcd_start_frame();
        }

        if (l < min_y || l >= max_y
            || (ctx->drawn_lines != NULL && !ctx->drawn_lines[l - area.y])) {
            uint8_t* buf = NULL;
            while (buf == NULL) {
                // break in case of errors
                if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                    lq_reset(lq);
                    return;
                };

                buf = lq_current(lq);
            }
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            continue;
        }

        uint32_t* lp = (uint32_t*)input_line;
        const uint8_t* ptr = ptr_start + bytes_per_line * (l - min_y);

        Cache_Start_DCache_Preload((uint32_t)ptr, ctx->display_width, 0);

        lp = (uint32_t*)ptr;

        uint8_t* buf = NULL;
        while (buf == NULL) {
            // break in case of errors
                if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                    lq_reset(lq);
                    return;
                };

            buf = lq_current(lq);
        }

        ctx->lut_lookup_func(lp, buf, ctx->conversion_lut, ctx->display_width);

        // apply the line mask
        epd_apply_line_mask_VE(buf, ctx->line_mask, ctx->display_width / 4);

        lq_commit(lq);
    }
}

#endif
