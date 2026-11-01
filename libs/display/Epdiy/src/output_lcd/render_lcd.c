#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../output_common/render_method.h"

#ifdef RENDER_METHOD_LCD

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/cache.h>

#include "../epd_internals.h"
#include "../output_common/line_queue.h"
#include "../output_common/lut.h"
#include "../output_common/spatial_wipe.h"
#include "../output_common/render_context.h"
#include "epd_board.h"
#include "epdiy.h"
#include "lcd_driver.h"
#include "render_lcd.h"

// declare vector optimized line mask application.
void epd_apply_line_mask_VE(uint8_t* line, const uint8_t* mask, int mask_len);

static void IRAM_ATTR record_underflow(RenderContext_t* ctx,int reason,int thread) {
    if(!ctx->fault_reason) {
        ctx->fault_phase=ctx->current_frame+1;
        ctx->fault_line=ctx->lines_consumed;
        ctx->fault_thread=thread;
        ctx->fault_ready=ctx->lines_consumed<ctx->lines_total ?
            __atomic_load_n(&ctx->line_ready[ctx->lines_consumed],__ATOMIC_ACQUIRE) : -1;
        for(int i=0;i<NUM_RENDER_THREADS;++i) {
            ctx->fault_write[i]=atomic_load_explicit(&ctx->line_queues[i].current,memory_order_acquire);
            ctx->fault_read[i]=atomic_load_explicit(&ctx->line_queues[i].last,memory_order_acquire);
        }
        ctx->fault_reason=reason;
    }
    ctx->error|=EPD_DRAW_EMPTY_LINE_QUEUE;
}

__attribute__((optimize("O3"))) static bool IRAM_ATTR
retrieve_line_isr(RenderContext_t* ctx, uint8_t* buf) {
    if (ctx->lines_consumed >= ctx->lines_total) {
        return false;
    }
    int thread = ctx->line_threads[ctx->lines_consumed];
    BaseType_t awoken = pdFALSE;

    // Queue commit is the authoritative release/acquire publication. A producer
    // can be preempted AFTER lq_commit but BEFORE publishing line_ready. That
    // separate flag is useful for prefill, but cannot reject a readable queue.
    if (thread < 0 || thread >= NUM_RENDER_THREADS) {
        record_underflow(ctx,1,thread);
        memset(buf, 0x00, ctx->display_width / 4);
        ctx->lines_consumed += 1;
        return awoken;
    }

    LineQueue_t* lq = &ctx->line_queues[thread];

    if (lq_read(lq, buf) != 0) {
        record_underflow(ctx,2,thread);
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

    // 扫描先于供数结束时置错，禁止残留行进入下一相位
    if (ctx->lines_consumed < ctx->lines_total) {
        record_underflow(ctx,3,-1);
    }

    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &task_awoken);

    portYIELD_FROM_ISR();
}

// 纯扫约 11ms；TB8 相太短 DU/GL 易残影。流式路径保留，仅垫相周期下限。
#ifndef EPD_MIN_PHASE_US
#define EPD_MIN_PHASE_US 12000
#endif
static int s_prefill_lines = 192;
static StaticSemaphore_t s_scan_lock_storage;
static SemaphoreHandle_t s_scan_lock;

// Serialize only task-side start/stop. Never hold an interrupt-disabled lock
// while starting the LCD or waiting for a feeder on the other core.
static void scan_lock(void) {
    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE("epd_lcd", "scan start/stop did not quiesce");
        abort();
    }
}

static void cancel_scan(RenderContext_t* ctx) {
    atomic_store_explicit(&ctx->feed_cancelled, true, memory_order_release);
    scan_lock();
    epd_lcd_line_source_cb(NULL, NULL);
    // abort_frame uses the ISR finish path. Suppress its callback here: the
    // update task must not execute handle_lcd_frame_done/FromISR APIs.
    epd_lcd_frame_done_cb(NULL, NULL);
    epd_lcd_abort_frame();
    xSemaphoreGive(s_scan_lock);
}

static bool start_scan(RenderContext_t* ctx) {
    scan_lock();
    if (atomic_load_explicit(&ctx->feed_cancelled, memory_order_acquire)) {
        xSemaphoreGive(s_scan_lock);
        return false;
    }
    epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
    epd_lcd_start_frame();
    xSemaphoreGive(s_scan_lock);
    return true;
}

static int last_phase_average, last_phase_minimum, last_phase_maximum;
void epd_lcd_get_timing(int* average,int* minimum,int* maximum) {
    *average=__atomic_load_n(&last_phase_average,__ATOMIC_RELAXED);
    *minimum=__atomic_load_n(&last_phase_minimum,__ATOMIC_RELAXED);
    *maximum=__atomic_load_n(&last_phase_maximum,__ATOMIC_RELAXED);
}

static void pace_phase(int64_t phase_start_us) {
    for (;;) {
        if (esp_timer_get_time() - phase_start_us >= EPD_MIN_PHASE_US) {
            break;
        }
        vTaskDelay(0);
    }
}

void lcd_do_update(RenderContext_t* ctx) {
    if (s_scan_lock == NULL) {
        s_scan_lock = xSemaphoreCreateMutexStatic(&s_scan_lock_storage);
        assert(s_scan_lock != NULL);
    }
    const int64_t cycle_start=esp_timer_get_time();
    int64_t previous_phase_start=0, period_total=0;
    int period_min=INT32_MAX, period_max=0, periods=0;
    int completed_frames=0;
    // 复用驱动已有扫描计时，停止扫描后汇总；不在 ISR 内增加日志。
    int64_t scan_total=0;
    int scan_min=INT32_MAX,scan_max=0,eof_gap_max=0,eof_work_max=0;
    ctx->fault_reason=0;
    epd_set_mode(1);
    ESP_LOGD("epd_lcd", "update %d frames, lines=%d (stream pace=%dus)", ctx->cycle_frames,
             ctx->lines_total, EPD_MIN_PHASE_US);

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        const int64_t phase_start=esp_timer_get_time();
        if (previous_phase_start) {
            const int period=(int)(phase_start-previous_phase_start);
            if (period<period_min) period_min=period;
            if (period>period_max) period_max=period;
            period_total+=period; ++periods;
        }
        previous_phase_start=phase_start;
        epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
        prepare_context_for_next_frame(ctx);
        atomic_store_explicit(&ctx->feed_cancelled, false, memory_order_release);

        const int64_t t0 = esp_timer_get_time();

        // start both feeder tasks；开扫由 feeder 预填够行后触发
        xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
        xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);

        if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE("epd_lcd", "LCD frame %d/%d timeout", k + 1, ctx->cycle_frames);
            ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
            cancel_scan(ctx);
            xSemaphoreTake(ctx->frame_done, 0);
        }

        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            if (xSemaphoreTake(ctx->feed_done_smphr[i], pdMS_TO_TICKS(2000)) != pdTRUE) {
                ESP_LOGE("epd_lcd", "feed %d timeout @frame %d", i, k);
                ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
                cancel_scan(ctx);
                // Do not return to render.c, reset queues or free LUTs while
                // this feeder can still touch them. Cancellation normally
                // completes at its next row/queue check; a stuck task is fatal.
                if (xSemaphoreTake(ctx->feed_done_smphr[i], pdMS_TO_TICKS(2000)) != pdTRUE) {
                    ESP_LOGE("epd_lcd", "feed %d failed to stop; retaining live buffers", i);
                    abort();
                }
            }
        }

        pace_phase(phase_start);

        const int64_t period_us = esp_timer_get_time() - t0;
        if ((k & 3) == 0 || k + 1 == ctx->cycle_frames) {
            ESP_LOGD("epd_lcd", "phase %d/%d period=%lldus", k + 1, ctx->cycle_frames,
                     (long long)period_us);
        }

        if (ctx->error) {
            cancel_scan(ctx);
            if(ctx->fault_reason) {
                int eof_gap=0,eof_work=0;
                epd_lcd_get_stream_stats(&eof_gap,&eof_work);
                ESP_LOGE("epd_lcd","stream fault reason=%d phase=%d/%d line=%d/%d owner=%d ready=%d q0=%d/%d q1=%d/%d prefill=%d eof_gap=%dus eof_work=%dus",
                    ctx->fault_reason,ctx->fault_phase,ctx->cycle_frames,ctx->fault_line,ctx->lines_total,
                    ctx->fault_thread,ctx->fault_ready,ctx->fault_write[0],ctx->fault_read[0],
                    ctx->fault_write[1],ctx->fault_read[1],s_prefill_lines,eof_gap,eof_work);
            }
            for (int i = 0; i < NUM_RENDER_THREADS; i++) {
                lq_reset(&ctx->line_queues[i]);
            }
            break;
        }

        ++completed_frames;
        const int scan_us=epd_lcd_get_scan_time_us();
        scan_total+=scan_us;
        if (scan_us<scan_min) scan_min=scan_us;
        if (scan_us>scan_max) scan_max=scan_us;
        int eof_gap=0,eof_work=0;
        epd_lcd_get_stream_stats(&eof_gap,&eof_work);
        if (eof_gap>eof_gap_max) eof_gap_max=eof_gap;
        if (eof_work>eof_work_max) eof_work_max=eof_work;
        ctx->current_frame++;
    }

    epd_lcd_line_source_cb(NULL, NULL);
    epd_lcd_frame_done_cb(NULL, NULL);

    epd_set_mode(0);
    if (periods) {
        __atomic_store_n(&last_phase_average,(int)(period_total/periods),__ATOMIC_RELAXED);
        __atomic_store_n(&last_phase_minimum,period_min,__ATOMIC_RELAXED);
        __atomic_store_n(&last_phase_maximum,period_max,__ATOMIC_RELAXED);
        ESP_LOGI("EpdTiming","phase cadence us: avg=%d min=%d max=%d frames=%d completed=%d cycle=%dms "
        "scan_us=%d/%d/%d eof_gap_max=%d eof_work_max=%d",
        (int)(period_total/periods),period_min,period_max,ctx->cycle_frames,
        completed_frames,(int)((esp_timer_get_time()-cycle_start)/1000),
        completed_frames ? (int)(scan_total/completed_frames) : 0,completed_frames ? scan_min : 0,scan_max,
        eof_gap_max,eof_work_max);
    }
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

    // Compute a line mask based on the drawn area
    uint8_t* dirtyness = malloc(ctx->display_width / 2);
    assert(dirtyness != NULL);

    memset(dirtyness, 0, ctx->display_width / 2);

    for (int i = 0; i < ctx->display_width; i++) {
        if ((i >= ctx->area.x) && (i < ctx->area.x + ctx->area.width)) {
            dirtyness[i / 2] |= i % 2 ? 0xF0 : 0x0F;
        }
    }
    epd_populate_line_mask(ctx->line_mask, dirtyness, ctx->display_width / 4);

    // mask the line pattern with the populated mask
    memset(ctx->static_line_buffer, fill_byte, ctx->display_width / 4);
    epd_apply_line_mask(ctx->static_line_buffer, ctx->line_mask, ctx->display_width / 4);

    free(dirtyness);
}

void epd_push_pixels_lcd(RenderContext_t* ctx, short time, int color) {
    xSemaphoreTake(ctx->frame_done, 0);
    ctx->fault_reason = 0;
    ctx->current_frame = 0;
    ctx->lines_total = ctx->display_height;
    ctx->lines_consumed = 0;
    ctx->static_line_buffer = malloc(ctx->display_width / 4);
    assert(ctx->static_line_buffer != NULL);

    push_pixels_populate_line(ctx, color);
    epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
    epd_lcd_line_source_cb((line_cb_func_t)&push_pixels_isr, ctx);

    epd_set_mode(1);
    epd_lcd_start_frame();
    if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        epd_lcd_line_source_cb(NULL, NULL);
        epd_lcd_frame_done_cb(NULL, NULL);
        epd_lcd_abort_frame();
        ESP_LOGE("epd_lcd", "clear scan timed out");
    }
    epd_set_mode(0);

    if (ctx->error) {
        ESP_LOGE("epd_lcd", "clear scan failed reason=%d line=%d/%d", ctx->fault_reason,
                 ctx->lines_consumed, ctx->lines_total);
    }

    free(ctx->static_line_buffer);
    ctx->static_line_buffer = NULL;
}

#define int_min(a, b) (((a) < (b)) ? (a) : (b))

// 预填够 bounce 再开扫；太少易欠载，太多则短队列在开扫前会堵死
// S31 uses C LUT feeding instead of Pico's Xtensa SIMD. The 64-line profile
// underruns during concurrent audio. Alternate producers to prefill 192 rows
// without exceeding either existing 127-slot queue.
__attribute__((optimize("O3"))) void IRAM_ATTR
lcd_calculate_frame(RenderContext_t* ctx, int thread_id) {
    assert(ctx->lut_lookup_func != NULL);

    LineQueue_t* lq = &ctx->line_queues[thread_id];
    int l = 0;

    // 另一生产者可能已启动并报告欠载；不得再次启动同一帧
    if (ctx->error) {
        return;
    }

    EpdRect area = ctx->area;
    int min_y, max_y, bytes_per_line, _ppB;
    const uint8_t* ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &_ppB);

    assert(area.width == ctx->display_width && area.x == 0 && !ctx->error);

    const int trigger_line = int_min(s_prefill_lines, ctx->lines_total-1);

    // Fixed ownership bounds each producer to half the prefill. With dynamic
    // claims one core can fill its 127-slot queue before a larger prefill starts.
    for (l=thread_id; l<ctx->lines_total; l+=NUM_RENDER_THREADS) {
        if (atomic_load_explicit(&ctx->feed_cancelled, memory_order_acquire)) return;
        ctx->line_threads[l] = (uint8_t)thread_id;

        // 预填够行后开扫，随后边扫边喂
        if (l == trigger_line) {
            // Claimed rows are not necessarily committed: the other core may
            // still own an early row. Start scanning only when the entire
            // prefill prefix is ready, not when a producer claims row 127.
            const int64_t ready_deadline=esp_timer_get_time()+2000000;
            for (int row=0;row<l;++row) {
                while (!__atomic_load_n(&ctx->line_ready[row],__ATOMIC_ACQUIRE)) {
                    if (atomic_load_explicit(&ctx->feed_cancelled, memory_order_acquire)
                        || ctx->error || esp_timer_get_time()>ready_deadline) {
                        ctx->error|=EPD_DRAW_EMPTY_LINE_QUEUE; return;
                    }
                    vTaskDelay(0);
                }
            }
            if (!start_scan(ctx)) return;
        }

        if (l < min_y || l >= max_y
            || (ctx->drawn_lines != NULL && !ctx->drawn_lines[l - area.y])) {
            uint8_t* buf = NULL;
            int wait_iters = 0;
            while (buf == NULL) {
                if (atomic_load_explicit(&ctx->feed_cancelled, memory_order_acquire)
                    || (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE)) {
                    return;
                }
                buf = lq_current(lq);
                if (buf != NULL) {
                    break;
                }
                vTaskDelay(0);
                if (++wait_iters > 30000) {
                    ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
                    return;
                }
            }
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            __atomic_store_n(&ctx->line_ready[l],1,__ATOMIC_RELEASE);
            continue;
        }

        const uint8_t* ptr = ptr_start + bytes_per_line * (l - min_y);
#if !CONFIG_IDF_TARGET_ESP32S31
        Cache_Start_DCache_Preload((uint32_t)ptr, ctx->display_width, 0);
#endif
        uint32_t* lp = (uint32_t*)ptr;

        uint8_t* buf = NULL;
        int wait_iters = 0;
        while (buf == NULL) {
            if (atomic_load_explicit(&ctx->feed_cancelled, memory_order_acquire)
                || (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE)) {
                return;
            }
            buf = lq_current(lq);
            if (buf != NULL) {
                break;
            }
            vTaskDelay(0);
            if (++wait_iters > 30000) {
                ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
                return;
            }
        }

        if(ctx->spatial_luts) epd_spatial_line((const uint8_t*)lp,buf,ctx->display_width,
            ctx->spatial_luts,ctx->spatial_bands,ctx->spatial_by_y,l);
        else ctx->lut_lookup_func(lp, buf, ctx->conversion_lut, ctx->display_width);
        epd_apply_line_mask_VE(buf, ctx->line_mask, ctx->display_width / 4);
        lq_commit(lq);
        __atomic_store_n(&ctx->line_ready[l],1,__ATOMIC_RELEASE);
    }
}

#endif
