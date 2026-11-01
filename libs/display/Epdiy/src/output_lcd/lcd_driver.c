#include "lcd_driver.h"
#include "epdiy.h"

#ifndef __DECLARE_RCC_ATOMIC_ENV
int __DECLARE_RCC_ATOMIC_ENV __attribute__((unused));
#endif

#include "../output_common/render_method.h"
#include "../output_common/white_border.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "hal/gpio_types.h"

#ifdef RENDER_METHOD_LCD

#include <assert.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#if __has_include(<soc/lcd_periph.h>)
#include <soc/lcd_periph.h>
#else
#include <hal/lcd_periph.h>
#endif
#if __has_include(<soc/rmt_periph.h>)
#include <soc/rmt_periph.h>
#else
#include <hal/rmt_periph.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <driver/rmt_tx.h>
#include <driver/rmt_types.h>
#include "epdiy_idf6_compat.h"
#include <esp_private/esp_clk_tree_common.h>
#include <esp_private/periph_ctrl.h>
#include <esp_clk_tree.h>
#include <hal/rmt_types.h>
#include <soc/clk_tree_defs.h>

#include <driver/gpio.h>
#include <esp_private/gpio.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_private/gdma.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <hal/dma_types.h>
#include <hal/gdma_channel.h>

/* S31/P4：LCD 挂 AXI-GDMA；S3：AHB-GDMA。通道与描述符对齐须一致。 */
#if defined(SOC_GDMA_TRIG_PERIPH_LCD0_BUS) && (SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AXI)
#define LCD_GDMA_NEW_CHANNEL gdma_new_axi_channel
#define LCD_GDMA_DESC_ALIGN 8
typedef dma_descriptor_align8_t lcd_dma_desc_t;
#elif defined(SOC_GDMA_TRIG_PERIPH_LCD0_BUS) && (SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AHB)
#define LCD_GDMA_NEW_CHANNEL gdma_new_ahb_channel
#define LCD_GDMA_DESC_ALIGN 4
typedef dma_descriptor_t lcd_dma_desc_t;
#else
#define LCD_GDMA_NEW_CHANNEL gdma_new_ahb_channel
#define LCD_GDMA_DESC_ALIGN 4
typedef dma_descriptor_t lcd_dma_desc_t;
#endif
#include <hal/gdma_ll.h>
#include <hal/gpio_hal.h>
#include <hal/lcd_hal.h>
#include <hal/lcd_ll.h>
#include <hal/rmt_ll.h>
#include <rom/cache.h>
#if __has_include(<soc/lcd_periph.h>)
#include <soc/lcd_periph.h>
#endif
#include <soc/rmt_struct.h>

#include "hal/gpio_hal.h"

gpio_hal_context_t hal = { .dev = GPIO_HAL_GET_HW(GPIO_PORT_0) };

#define TAG "epdiy"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define LCD_PERIPH_PANEL soc_lcd_rgb_signals[0]
#elif ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 2)
#define LCD_PERIPH_PANEL lcd_periph_signals.panels[0]
#else
#define LCD_PERIPH_PANEL lcd_periph_rgb_signals.panels[0]
#endif

static inline int min(int x, int y) {
    return x < y ? x : y;
}
static inline int max(int x, int y) {
    return x > y ? x : y;
}

#define S3_LCD_PIN_NUM_BK_LIGHT -1
// #define S3_LCD_PIN_NUM_MODE           4

#define LINE_BATCH 1000

// --- 示波器 / 对照实验：一次只改下面宏，其余不动 ---
// 行扫描 16us。bounce=8 → 期望 EOF≈128us；688 可整除。
#ifndef EPD_LCD_PROBE_BOUNCE
#define EPD_LCD_PROBE_BOUNCE 8
#endif
// 与 esp_lcd RGB 一致：always_on=1，数据长度由 DMA / H 时序决定。
#ifndef EPD_LCD_PROBE_ALWAYS_ON
#define EPD_LCD_PROBE_ALWAYS_ON 1
#endif
// 1：CKV 让出 GPIO40，PCLK 改走专用 IOMUX。GPIO16 已确认 20MHz，探针关闭。
#ifndef EPD_LCD_PROBE_PCLK_ON_PAD
#define EPD_LCD_PROBE_PCLK_ON_PAD 0
#endif
// 0：正常画面。1：bounce 填 01 00… 仅用于量 D0 节拍（已确认 1 拍/字节）。
#ifndef EPD_LCD_PROBE_D0_TOGGLE
#define EPD_LCD_PROBE_D0_TOGGLE 0
#endif
#define BOUNCE_BUF_LINES EPD_LCD_PROBE_BOUNCE

#define RMT_CKV_CHAN RMT_CHANNEL_1

// The extern line is declared in esp-idf/components/driver/deprecated/rmt_legacy.c. It has access
// to RMTMEM through the rmt_private.h header which we can't access outside the sdk. Declare our own
// extern here to properly use the RMTMEM smybol defined in
// components/soc/[target]/ld/[target].peripherals.ld Also typedef the new rmt_mem_t struct to the
// old rmt_block_mem_t struct. Same data fields, different names
typedef rmt_mem_t rmt_block_mem_t;
extern rmt_block_mem_t RMTMEM;

// spinlock for protecting the critical section at frame start
static portMUX_TYPE frame_start_spinlock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    lcd_hal_context_t hal;
    intr_handle_t lcd_intr;

    frame_done_func_t frame_done_cb;
    line_cb_func_t line_source_cb;
    void* line_cb_payload;
    void* frame_cb_payload;

    int line_length_us;
    int ckv_period_us; // CKV 一行周期；S31 探测拉伸时可大于 line_length_us
    uint32_t ckv_tick_hz;
    int line_cycles;
    int lcd_res_h;

    LcdEpdConfig_t config;

    uint8_t* bounce_buffer[2];
    // size of a single bounce buffer
    size_t bb_size;
    size_t batches;
    size_t bb_eof_count; // EOF 次序选 bounce，勿用 desc 指针比较（AXI 地址可能不一致）
    size_t lines_fed; // 本帧已喂行数，用于无 VSYNC 时收尾
    bool frame_finishing; // 防止重复 stop/回调
    bool bb_behind_cache; // bounce 在 cacheable 区时需 msync

    // EOF 间隔实测（对示波器：应 ≈ line_length_us * BOUNCE）
    int64_t eof_last_us;
    int64_t eof_dt_min_us;
    int64_t eof_dt_max_us;
    int64_t eof_dt_sum_us;
    uint32_t eof_dt_n;
    int64_t eof_work_min_us; // EOF 回调自身耗时，和间隔分开
    int64_t eof_work_max_us;
    int64_t eof_work_sum_us;
    uint32_t eof_work_n;
    int64_t frame_t0_us; // lcd_ll_start 时刻，收尾算一相墙钟
    int64_t last_frame_us; // 上一相实测时长
    int last_h_hsync;
    int last_h_back;
    int last_h_active;
    int last_h_end;
    int last_h_total;
    // Number of DMA descriptors that used to carry the frame buffer
    size_t num_dma_nodes;
    // DMA channel handle
    gdma_channel_handle_t dma_chan;
    // DMA descriptors pool
    lcd_dma_desc_t* dma_nodes;

    /// The number of bytes in a horizontal display register line.
    int line_bytes;

    // With 8 bit bus width, we need a dummy cycle before the actual data,
    // because the LCD peripheral behaves weirdly.
    // Also see:
    // https://blog.adafruit.com/2022/06/14/esp32uesday-hacking-the-esp32-s3-lcd-peripheral/
    int dummy_bytes;

    /// The number of lines of the display
    int display_lines;
    int native_height;
} s3_lcd_t;

static s3_lcd_t lcd = { 0 };
static bool border_whitening=false;
void epd_lcd_prime_white_border(bool enable) { border_whitening=enable; }
int epd_lcd_white_border_pixels(void) { return lcd.config.white_border_pixels; }

void IRAM_ATTR epd_lcd_line_source_cb(line_cb_func_t line_source, void* payload) {
    lcd.line_source_cb = line_source;
    lcd.line_cb_payload = payload;
}

void IRAM_ATTR epd_lcd_frame_done_cb(frame_done_func_t cb, void* payload) {
    lcd.frame_done_cb = cb;
    lcd.frame_cb_payload = payload;
}

static IRAM_ATTR void lcd_finish_frame_from_isr(void) {
    if (lcd.frame_finishing) {
        return;
    }
    lcd.frame_finishing = true;
    lcd_ll_enable_interrupt(lcd.hal.dev, LCD_LL_EVENT_VSYNC_END | LCD_LL_EVENT_TRANS_DONE, false);
    if (lcd.frame_t0_us > 0) {
        lcd.last_frame_us = esp_timer_get_time() - lcd.frame_t0_us;
    }
    lcd.line_source_cb = NULL;
    lcd_ll_stop(lcd.hal.dev);
    gdma_stop(lcd.dma_chan);
    lcd_ll_clear_interrupt_status(lcd.hal.dev, lcd_ll_get_interrupt_status(lcd.hal.dev));
    if (lcd.frame_done_cb != NULL) {
        lcd.frame_done_cb(lcd.frame_cb_payload);
    }
}

void epd_lcd_abort_frame(void) {
    lcd_finish_frame_from_isr();
}

void epd_lcd_get_stream_stats(int* max_eof_gap_us,int* max_eof_work_us) {
    *max_eof_gap_us=(int)lcd.eof_dt_max_us;
    *max_eof_work_us=(int)lcd.eof_work_max_us;
}

int epd_lcd_get_scan_time_us(void) {
    return (int)lcd.last_frame_us;
}

static IRAM_ATTR bool fill_bounce_buffer(uint8_t* buffer) {
    bool task_awoken = false;

    for (int i = 0; i < BOUNCE_BUF_LINES; i++) {
        // 8bit 时 dummy 占前 1 字节；仍走 line_source 以免队列饿死。
        int buffer_offset = i * (lcd.line_bytes + lcd.dummy_bytes) + (lcd.dummy_bytes % 2);
        if (lcd.lines_fed >= (size_t)lcd.display_lines) {
            memset(&buffer[buffer_offset], 0x00, lcd.line_bytes);
            continue;
        }
        if (lcd.line_source_cb != NULL) {
            task_awoken |= lcd.line_source_cb(lcd.line_cb_payload, &buffer[buffer_offset]);
        } else {
            memset(&buffer[buffer_offset], 0x00, lcd.line_bytes);
        }
#if EPD_LCD_PROBE_D0_TOGGLE
        // 覆盖为 D0 逐字节翻转；dummy 字节不动。
        uint8_t* line = &buffer[buffer_offset];
        for (int b = 0; b < lcd.line_bytes; b++) {
            line[b] = (b & 1) ? 0x00 : 0x01;
        }
#endif
        epd_filter_white_border(&buffer[buffer_offset],(int)lcd.lines_fed,
            lcd.line_bytes,lcd.native_height,lcd.config.white_border_pixels,border_whitening);
        lcd.lines_fed += 1;
    }

#if defined(CONFIG_IDF_TARGET_ESP32S31) || defined(CONFIG_IDF_TARGET_ESP32P4)
    if (lcd.bb_behind_cache) {
        esp_cache_msync(buffer, lcd.bb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
#endif

    // Feeding the last buffer is not completion: DMA still has two buffers
    // in flight. The EOF handler finishes only after the tail is transmitted.
    return task_awoken;
}

static void start_ckv_cycles(int cycles) {
#if EPD_LCD_PROBE_PCLK_ON_PAD && defined(CONFIG_IDF_TARGET_ESP32S31)
    (void)cycles;
    return;
#endif
    if (cycles < 1) {
        return;
    }
    rmt_ll_tx_stop(&RMT, RMT_CKV_CHAN);
    rmt_ll_tx_enable_loop_count(&RMT, RMT_CKV_CHAN, true);
    rmt_ll_tx_enable_loop_autostop(&RMT, RMT_CKV_CHAN, true);
    rmt_ll_tx_set_loop_count(&RMT, RMT_CKV_CHAN, cycles);
    rmt_ll_tx_reset_pointer(&RMT, RMT_CKV_CHAN);
    rmt_ll_tx_start(&RMT, RMT_CKV_CHAN);
}

/**
 * Build the RMT signal according to the timing set in the lcd object.
 * Convert the 0.1us configuration using the measured RMT source clock.
 */
static void ckv_rmt_build_signal() {
    // 门极一行周期必须等于 LCD 一行时间；若偏短会出现「一行数据多次 CKV」→ 格子被拉扁。
    const int period_us = lcd.ckv_period_us > 0 ? lcd.ckv_period_us : 1;
    const int period=(int)(((uint64_t)period_us*lcd.ckv_tick_hz+500000)/1000000);
    const int minimum=(lcd.ckv_tick_hz+999999)/1000000; // E0470A14-AF: high/low >=1us.
    if (period<2*minimum) return; // init precedes calculating the real line length.
    int high=(int)(((uint64_t)lcd.config.ckv_high_time*lcd.ckv_tick_hz+5000000)/10000000);
    high=max(minimum,min(high,period-minimum));
    const int low=period-high;

    volatile rmt_item32_t* rmt_mem_ptr = &(RMTMEM.chan[RMT_CKV_CHAN].data32[0]);
    rmt_mem_ptr->duration0 = high;
    rmt_mem_ptr->level0 = 1;
    rmt_mem_ptr->duration1 = low;
    rmt_mem_ptr->level1 = 0;
    rmt_mem_ptr[1].val = 0;
}

/**
 * Configure the RMT peripheral for use as the CKV clock.
 */
static void init_ckv_rmt() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    // IDF6 / S31：RMT 不在 shared_periph_module 列表，改用 LL 总线时钟
    PERIPH_RCC_ATOMIC() {
        rmt_ll_enable_bus_clock(0, true);
        rmt_ll_reset_register(0);
    }
    rmt_ll_mem_force_power_on(&RMT);
    rmt_ll_enable_mem_access_nonfifo(&RMT, true);
    esp_clk_tree_enable_src((soc_module_clk_t)RMT_CLK_SRC_DEFAULT, true);
    PERIPH_RCC_ATOMIC() {
        // rmt_sclk = src / (1 + (integral-1) + num/den) = src
        rmt_ll_set_group_clock_src(&RMT, RMT_CKV_CHAN, RMT_CLK_SRC_DEFAULT, 1, 1, 0);
        rmt_ll_enable_group_clock(&RMT, true);
    }
#else
    periph_module_reset(PERIPH_RMT_MODULE);
    periph_module_enable(PERIPH_RMT_MODULE);
    rmt_ll_enable_periph_clock(&RMT, true);
    rmt_ll_set_group_clock_src(&RMT, RMT_CKV_CHAN, RMT_CLK_SRC_DEFAULT, 1, 0, 0);
    rmt_ll_enable_mem_access_nonfifo(&RMT, true);
#endif

    uint32_t src_hz = 80000000;
    esp_err_t clk_err =
        esp_clk_tree_src_get_freq_hz((soc_module_clk_t)RMT_CLK_SRC_DEFAULT,
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &src_hz);
    if (clk_err != ESP_OK || src_hz == 0) {
        src_hz = 80000000;
    }
    // 10MHz preserves sub-microsecond gate widths; never assume an APB rate.
    uint32_t chan_div = (src_hz + 5000000) / 10000000;
    if (chan_div < 1) {
        chan_div = 1;
    }
    if (chan_div > 255) {
        chan_div = 255;
    }
    rmt_ll_tx_set_channel_clock_div(&RMT, RMT_CKV_CHAN, chan_div);
    lcd.ckv_tick_hz=src_hz/chan_div;
    // 与厂商一致：2 个 mem block（S31 每通道 48 word，足够）
    rmt_ll_tx_set_mem_blocks(&RMT, RMT_CKV_CHAN, 2);
    rmt_ll_tx_fix_idle_level(&RMT, RMT_CKV_CHAN, RMT_IDLE_LEVEL_LOW, true);
    rmt_ll_tx_enable_carrier_modulation(&RMT, RMT_CKV_CHAN, false);

    rmt_ll_tx_enable_loop(&RMT, RMT_CKV_CHAN, true);

#if EPD_LCD_PROBE_PCLK_ON_PAD && defined(CONFIG_IDF_TARGET_ESP32S31)
    // GPIO40 是 LCD_PCLK 专用脚，也是板级 CKV。诊断时让出 CKV，改走 IOMUX 出像素时钟。
    const soc_lcd_rgb_iomux_desc_t* iomux = &soc_lcd_rgb_iomux_descs[0];
    esp_err_t mux = gpio_iomux_output(iomux->pclk_pin.gpio_num, iomux->pclk_pin.func);
    ESP_LOGI(TAG, "PROBE PCLK IOMUX GPIO%d (CKV off) %s — 蓝线看 20MHz",
             iomux->pclk_pin.gpio_num, esp_err_to_name(mux));
#else
    gpio_hal_func_sel(&hal, lcd.config.bus.ckv, PIN_FUNC_GPIO);
    gpio_set_direction(lcd.config.bus.ckv, GPIO_MODE_OUTPUT);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_rom_gpio_connect_out_signal(
        lcd.config.bus.ckv, soc_rmt_signals[0].channels[RMT_CKV_CHAN].tx_sig, false, 0
    );
#else
    esp_rom_gpio_connect_out_signal(
        lcd.config.bus.ckv, rmt_periph_signals.groups[0].channels[RMT_CKV_CHAN].tx_sig, false, 0
    );
#endif
    ESP_LOGI(TAG, "CKV RMT src=%uHz div=%u → %uHz", (unsigned)src_hz,
             (unsigned)chan_div, (unsigned)(src_hz / chan_div));
#endif

    ckv_rmt_build_signal();
}

/**
 * Reset the CKV RMT configuration.
 */
static void deinit_ckv_rmt() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    PERIPH_RCC_ATOMIC() {
        rmt_ll_reset_register(0);
        rmt_ll_enable_bus_clock(0, false);
    }
#else
    periph_module_reset(PERIPH_RMT_MODULE);
    periph_module_disable(PERIPH_RMT_MODULE);
#endif

    gpio_reset_pin(lcd.config.bus.ckv);
}

__attribute__((optimize("O3"))) IRAM_ATTR static void lcd_isr(void* args) {
    (void)args;

    uint32_t intr_status = lcd_ll_get_interrupt_status(lcd.hal.dev);
    lcd_ll_clear_interrupt_status(lcd.hal.dev, intr_status);
    if (lcd.frame_finishing) {
        return;
    }

    if (intr_status & LCD_LL_EVENT_VSYNC_END) {
        int batches_needed = (lcd.display_lines + LINE_BATCH - 1) / LINE_BATCH;
        if (batches_needed < 1) {
            batches_needed = 1;
        }
        const size_t completed_batches = lcd.batches + 1;
        lcd.batches = completed_batches;

        // DMA EOF 只表示数据进入 LCD 缓冲；末批须等扫描完成再停机。
        if (completed_batches < (size_t)batches_needed) {
            const int lines_done = (int)completed_batches * LINE_BATCH;
            const int lines_left = lcd.display_lines - lines_done;
            const int next_lines = min(LINE_BATCH, lines_left);
            const bool next_is_last = completed_batches + 1 >= (size_t)batches_needed;
            const int tail_lines = next_is_last ? 10 : 1;

            lcd_ll_enable_auto_next_frame(lcd.hal.dev, !next_is_last);
            lcd_ll_set_vertical_timing(lcd.hal.dev, 1, 0, next_lines, tail_lines);
            // apparently, this is needed for the new timing to take effect.
            lcd_ll_start(lcd.hal.dev);

            // skip the LCD front porch line, which is not actual data
            esp_rom_delay_us(lcd.ckv_period_us);
            start_ckv_cycles(next_lines + tail_lines);
        } else {
            lcd_finish_frame_from_isr();
        }
    }

};

// ISR handling bounce buffer refill
static IRAM_ATTR bool lcd_rgb_panel_eof_handler(
    gdma_channel_handle_t dma_chan, gdma_event_data_t* event_data, void* user_data
) {
    (void)dma_chan;
    (void)event_data;
    (void)user_data;
    if (lcd.frame_finishing) {
        return false;
    }
    const int64_t t0 = esp_timer_get_time();
    if (lcd.eof_last_us > 0) {
        const int64_t dt = t0 - lcd.eof_last_us;
        if (lcd.eof_dt_n == 0) {
            lcd.eof_dt_min_us = dt;
            lcd.eof_dt_max_us = dt;
        } else {
            if (dt < lcd.eof_dt_min_us) {
                lcd.eof_dt_min_us = dt;
            }
            if (dt > lcd.eof_dt_max_us) {
                lcd.eof_dt_max_us = dt;
            }
        }
        lcd.eof_dt_sum_us += dt;
        lcd.eof_dt_n += 1;
    }
    lcd.eof_last_us = t0;

    // 与 esp_lcd RGB 一致：用 EOF 计数选 buffer，AXI 下 desc 指针不可靠
    int bb = (int)(lcd.bb_eof_count & 1u);
    lcd.bb_eof_count += 1;
    // 尾部继续补无驱动数据，让 LCD 排空缓冲并完成末行扫描。
    const bool awoken = fill_bounce_buffer(lcd.bounce_buffer[bb]);
    // AXI-GDMA：补数后交还 DMA，否则 check_owner 时描述符停在 CPU
    lcd.dma_nodes[bb].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    const int64_t work = esp_timer_get_time() - t0;
    if (lcd.eof_work_n == 0) {
        lcd.eof_work_min_us = work;
        lcd.eof_work_max_us = work;
    } else {
        if (work < lcd.eof_work_min_us) {
            lcd.eof_work_min_us = work;
        }
        if (work > lcd.eof_work_max_us) {
            lcd.eof_work_max_us = work;
        }
    }
    lcd.eof_work_sum_us += work;
    lcd.eof_work_n += 1;
    return awoken;
}

static esp_err_t init_dma_trans_link() {
    lcd.dma_nodes[0].dw0.suc_eof = 1;
    lcd.dma_nodes[0].dw0.size = lcd.bb_size;
    lcd.dma_nodes[0].dw0.length = lcd.bb_size;
    lcd.dma_nodes[0].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    lcd.dma_nodes[0].buffer = lcd.bounce_buffer[0];

    lcd.dma_nodes[1].dw0.suc_eof = 1;
    lcd.dma_nodes[1].dw0.size = lcd.bb_size;
    lcd.dma_nodes[1].dw0.length = lcd.bb_size;
    lcd.dma_nodes[1].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    lcd.dma_nodes[1].buffer = lcd.bounce_buffer[1];

    // loop end back to start
    lcd.dma_nodes[0].next = &lcd.dma_nodes[1];
    lcd.dma_nodes[1].next = &lcd.dma_nodes[0];

    // alloc DMA channel and connect to LCD peripheral
    gdma_channel_alloc_config_t dma_chan_config = {
        .intr_priority = 3,
#if CONFIG_LCD_RGB_ISR_IRAM_SAFE
        .flags.isr_cache_safe = true,
#endif
    };
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    ESP_RETURN_ON_ERROR(
        LCD_GDMA_NEW_CHANNEL(&dma_chan_config, &lcd.dma_chan, NULL), TAG, "alloc DMA channel failed"
    );
#else
    dma_chan_config.direction = GDMA_CHANNEL_DIRECTION_TX;
    ESP_RETURN_ON_ERROR(
        gdma_new_channel(&dma_chan_config, &lcd.dma_chan), TAG, "alloc DMA channel failed"
    );
#endif
    gdma_trigger_t trigger = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0);
    ESP_RETURN_ON_ERROR(gdma_connect(lcd.dma_chan, trigger), TAG, "dma connect error");

    gdma_strategy_config_t dma_strategy = {
        .eof_till_data_popped = false,
    };
    gdma_apply_strategy(lcd.dma_chan, &dma_strategy);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    /* bounce 在内部 SRAM，勿开 ext_mem，避免 AXI 误触 PSRAM 对齐检查 */
    gdma_transfer_config_t trans_cfg = {
        .max_data_burst_size = 64,
        .access_ext_mem = false,
    };
    ESP_RETURN_ON_ERROR(gdma_config_transfer(lcd.dma_chan, &trans_cfg), TAG, "dma setup error");
#else
    gdma_transfer_ability_t ability = {
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ESP_RETURN_ON_ERROR(gdma_set_transfer_ability(lcd.dma_chan, &ability), TAG, "dma setup error");
#endif

    gdma_tx_event_callbacks_t cbs = {
        .on_trans_eof = lcd_rgb_panel_eof_handler,
    };
    ESP_RETURN_ON_ERROR(
        gdma_register_tx_event_callbacks(lcd.dma_chan, &cbs, NULL), TAG, "dma setup error"
    );

    return ESP_OK;
}

void deinit_dma_trans_link() {
    gdma_reset(lcd.dma_chan);
    gdma_disconnect(lcd.dma_chan);
    gdma_del_channel(lcd.dma_chan);
}

/**
 * Configure LCD peripheral and auxiliary GPIOs
 */
static esp_err_t init_bus_gpio() {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    // 板级 D0..D7 = GPIO8..15，与 S31 专用 LCD_DATA0..7 IOMUX 一致；走 IOMUX 比 GPIO matrix 更稳。
    const soc_lcd_rgb_iomux_desc_t* iomux = &soc_lcd_rgb_iomux_descs[0];
    for (int i = 0; i < lcd.config.bus_width; i++) {
        const int want = lcd.config.bus.data[i];
        const int pad = iomux->data_pins[i].gpio_num;
        if (want != pad) {
            ESP_LOGE(TAG, "D%d gpio %d != IOMUX LCD_DATA%d pad %d", i, want, i, pad);
            return ESP_ERR_INVALID_ARG;
        }
        ESP_RETURN_ON_ERROR(gpio_iomux_output(pad, iomux->data_pins[i].func), TAG, "data iomux");
    }
    ESP_LOGI(TAG, "S31 data D0..D7 via IOMUX LCD_DATA0..7 (GPIO8..15)");
#else
    const int DATA_LINES[16] = {
        lcd.config.bus.data[14], lcd.config.bus.data[15], lcd.config.bus.data[12],
        lcd.config.bus.data[13], lcd.config.bus.data[10], lcd.config.bus.data[11],
        lcd.config.bus.data[8],  lcd.config.bus.data[9],  lcd.config.bus.data[6],
        lcd.config.bus.data[7],  lcd.config.bus.data[4],  lcd.config.bus.data[5],
        lcd.config.bus.data[2],  lcd.config.bus.data[3],  lcd.config.bus.data[0],
        lcd.config.bus.data[1],
    };

    // S3：挂高半字 + 乱序，补偿 LCD 外设 bit 排布
    for (size_t i = (16 - lcd.config.bus_width); i < 16; i++) {
        gpio_hal_func_sel(&hal, DATA_LINES[i], PIN_FUNC_GPIO);
        gpio_set_direction(DATA_LINES[i], GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(
            DATA_LINES[i], LCD_PERIPH_PANEL.data_sigs[i], false, false
        );
    }
#endif
    // XCL/XLE/XSTL 不在专用 LCD 控制脚上，仍走 GPIO matrix
    gpio_hal_func_sel(&hal, lcd.config.bus.leh, PIN_FUNC_GPIO);
    gpio_set_direction(lcd.config.bus.leh, GPIO_MODE_OUTPUT);
    gpio_hal_func_sel(&hal, lcd.config.bus.clock, PIN_FUNC_GPIO);
    gpio_set_direction(lcd.config.bus.clock, GPIO_MODE_OUTPUT);
    gpio_hal_func_sel(&hal, lcd.config.bus.start_pulse, PIN_FUNC_GPIO);
    gpio_set_direction(lcd.config.bus.start_pulse, GPIO_MODE_OUTPUT);

    esp_rom_gpio_connect_out_signal(
        lcd.config.bus.leh, LCD_PERIPH_PANEL.hsync_sig, false, false
    );
    esp_rom_gpio_connect_out_signal(
        lcd.config.bus.clock, LCD_PERIPH_PANEL.pclk_sig, false, false
    );
    esp_rom_gpio_connect_out_signal(
        lcd.config.bus.start_pulse, LCD_PERIPH_PANEL.de_sig, false, false
    );

    gpio_config_t vsync_gpio_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ull << lcd.config.bus.stv,
    };
    gpio_config(&vsync_gpio_conf);
    gpio_set_level(lcd.config.bus.stv, 1);
    return ESP_OK;
}

/**
 * Reset bus GPIO pin functions.
 */
static void deinit_bus_gpio() {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    for (int i = 0; i < lcd.config.bus_width; i++) {
        gpio_reset_pin(lcd.config.bus.data[i]);
    }
#else
    for (size_t i = (16 - lcd.config.bus_width); i < 16; i++) {
        gpio_reset_pin(lcd.config.bus.data[i]);
    }
#endif

    gpio_reset_pin(lcd.config.bus.leh);
    gpio_reset_pin(lcd.config.bus.clock);
    gpio_reset_pin(lcd.config.bus.start_pulse);
    gpio_reset_pin(lcd.config.bus.stv);
}

/**
 * Check if the PSRAM cache is properly configured.
 */
static void check_cache_configuration() {
#if defined(CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE)
    if (CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE < 64) {
        ESP_LOGE(
            "epdiy",
            "cache line size is set to %d (< 64B)! This will degrade performance, please update "
            "this option in menuconfig.",
            CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE
        );
        ESP_LOGE(
            "epdiy",
            "If you are on arduino, you can't set this option yourself, you'll need to use a lower "
            "speed."
        );
        ESP_LOGE(
            "epdiy",
            "Reducing the pixel clock from %d MHz to %d MHz for now!",
            lcd.config.pixel_clock / 1000 / 1000,
            lcd.config.pixel_clock / 1000 / 1000 / 2
        );
        lcd.config.pixel_clock = lcd.config.pixel_clock / 2;

        // fixme: this would be nice, but doesn't work :(
        // uint32_t d_autoload = Cache_Suspend_DCache();
        /// Cache_Set_DCache_Mode(CACHE_SIZE_FULL, CACHE_4WAYS_ASSOC, CACHE_LINE_SIZE_32B);
        // Cache_Invalidate_DCache_All();
        // Cache_Resume_DCache(d_autoload);
    }
#endif
}

/**
 * Assign LCD configuration parameters from a given configuration, without allocating memory or
 * touching the LCD peripheral config.
 */
static void assign_lcd_parameters_from_config(
    const LcdEpdConfig_t* config, int display_width, int display_height
) {
    // copy over the configuraiton object
    memcpy(&lcd.config, config, sizeof(LcdEpdConfig_t));
    assert(config->white_border_pixels>=0 && config->white_border_pixels%4==0);
    assert(config->white_border_pixels*2<display_width && config->white_border_pixels*2<display_height);
    lcd.native_height=display_height;

    // Make sure the bounce buffers divide the display height evenly.
    lcd.display_lines = (((display_height + 7) / 8) * 8);

    lcd.line_bytes = display_width / 4;
    lcd.lcd_res_h = lcd.line_bytes / (lcd.config.bus_width / 8);

    // With 8 bit bus width, we need a dummy cycle before the actual data,
    // because the LCD peripheral behaves weirdly.
    // Also see:
    // https://blog.adafruit.com/2022/06/14/esp32uesday-hacking-the-esp32-s3-lcd-peripheral/
    lcd.dummy_bytes = lcd.config.bus_width / 8;

    // each bounce buffer holds a number of lines with data + dummy bytes each
    lcd.bb_size = BOUNCE_BUF_LINES * (lcd.line_bytes + lcd.dummy_bytes);
    lcd.line_length_us = 1;
    lcd.ckv_period_us = 1;

    check_cache_configuration();

    ESP_LOGI(TAG, "using resolution %dx%d (LCD clocks x lines; panel FB is %dx%d, line_bytes=%d)",
             lcd.lcd_res_h, lcd.display_lines, display_width, display_height, lcd.line_bytes);
}

/**
 * Allocate buffers for LCD driver operation.
 */
static esp_err_t allocate_lcd_buffers() {
    uint32_t dma_flags = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;

    /* AXI-GDMA burst=64 时 buffer 也按 64 对齐；AHB 描述符仍用 LCD_GDMA_DESC_ALIGN */
#if defined(SOC_GDMA_TRIG_PERIPH_LCD0_BUS) && (SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AXI)
    const size_t bb_align = 64;
#else
    const size_t bb_align = LCD_GDMA_DESC_ALIGN;
#endif
    lcd.bb_behind_cache = false;
#if defined(CONFIG_IDF_TARGET_ESP32S31) || defined(CONFIG_IDF_TARGET_ESP32P4)
    lcd.bb_behind_cache = true; // 内部 SRAM 也可能经 cache，与 esp_lcd RGB 一致
#endif
    for (int i = 0; i < 2; i++) {
        lcd.bounce_buffer[i] =
            heap_caps_aligned_calloc(bb_align, 1, lcd.bb_size, dma_flags);
        ESP_RETURN_ON_FALSE(lcd.bounce_buffer[i], ESP_ERR_NO_MEM, TAG, "install interrupt failed");
        if (lcd.bb_behind_cache) {
            esp_cache_msync(
                lcd.bounce_buffer[i], lcd.bb_size,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED
            );
        }
    }

    // So far, I haven't seen any displays with > 4096 pixels per line,
    // so we only need one DMA node for now.
    assert(lcd.bb_size < DMA_DESCRIPTOR_BUFFER_MAX_SIZE);
    lcd.dma_nodes = heap_caps_aligned_calloc(LCD_GDMA_DESC_ALIGN, 2, sizeof(lcd_dma_desc_t),
                                             dma_flags);
    ESP_RETURN_ON_FALSE(lcd.dma_nodes, ESP_ERR_NO_MEM, TAG, "no mem for dma nodes");
    ESP_LOGW("epd_dram",
             "LCD bounce×2=%u B (%u×2) + dma_desc=%u B (align=%u) INTERNAL|DMA",
             (unsigned)(lcd.bb_size * 2), (unsigned)lcd.bb_size,
             (unsigned)(2 * sizeof(lcd_dma_desc_t)), (unsigned)bb_align);
    return ESP_OK;
}

static void free_lcd_buffers() {
    for (int i = 0; i < 2; i++) {
        uint8_t* buf = lcd.bounce_buffer[i];
        if (buf != NULL) {
            heap_caps_free(buf);
            lcd.bounce_buffer[i] = NULL;
        }
    }

    if (lcd.dma_nodes != NULL) {
        heap_caps_free(lcd.dma_nodes);
        lcd.dma_nodes = NULL;
    }
}

/**
 * Initialize the LCD peripheral itself and install interrupts.
 */
static esp_err_t init_lcd_peripheral() {
    esp_err_t ret = ESP_OK;

#if defined(CONFIG_IDF_TARGET_ESP32S31)
    // 与 esp_lcd RGB 一致：RCC 引用计数开总线时钟，并打开 RGB 传输缓冲
    PERIPH_RCC_ACQUIRE_ATOMIC(LCD_PERIPH_PANEL.module, ref_count) {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(0, true);
            lcd_ll_reset_register(0);
        }
    }
    lcd_hal_init(&lcd.hal, 0);
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_clock(lcd.hal.dev, true);
    }
    // 与 esp_lcd RGB 一致：trans buffer 防 underrun（S3 上为空操作）
    lcd_ll_mem_set_low_power_mode(lcd.hal.dev, LCD_LL_MEM_LP_MODE_SHUT_DOWN);
    lcd_ll_mem_power_by_pmu(lcd.hal.dev);
    lcd_ll_enable_trans_buffer(lcd.hal.dev, true);
    esp_clk_tree_enable_src((soc_module_clk_t)LCD_CLK_SRC_DEFAULT, true);
    PERIPH_RCC_ATOMIC() {
        lcd_ll_select_clk_src(lcd.hal.dev, LCD_CLK_SRC_DEFAULT);
    }
#else
    // enable APB to access LCD registers
    periph_module_enable(PERIPH_LCD_CAM_MODULE);
    periph_module_reset(PERIPH_LCD_CAM_MODULE);

    lcd_hal_init(&lcd.hal, 0);
    lcd_ll_enable_clock(lcd.hal.dev, true);
#if defined(LCD_CLK_SRC_PLL240M)
    lcd_ll_select_clk_src(lcd.hal.dev, LCD_CLK_SRC_PLL240M);
#else
    lcd_ll_select_clk_src(lcd.hal.dev, LCD_CLK_SRC_DEFAULT);
#endif
#endif
    ESP_RETURN_ON_ERROR(ret, TAG, "set source clock failed");

    // Install one handler for the shared LCD source.  Registering VSYNC and
    // TRANS_DONE separately let the first handler clear the second event before
    // its shared handler observed it.
    int flags = ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_SHARED
                | ESP_INTR_FLAG_LOWMED;

    int source = LCD_PERIPH_PANEL.irq_id;
    uint32_t status = (uint32_t)lcd_ll_get_interrupt_status_reg(lcd.hal.dev);
    ret = esp_intr_alloc_intrstatus(
        source, flags, status, LCD_LL_EVENT_VSYNC_END | LCD_LL_EVENT_TRANS_DONE,
        lcd_isr, NULL, &lcd.lcd_intr
    );
    ESP_RETURN_ON_ERROR(ret, TAG, "install interrupt failed");

    lcd_ll_fifo_reset(lcd.hal.dev);
    lcd_ll_reset(lcd.hal.dev);
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    // reset 后可能清掉，再开一次
    lcd_ll_enable_trans_buffer(lcd.hal.dev, true);
#endif

    // pixel clock phase and polarity
    lcd_ll_set_clock_idle_level(lcd.hal.dev, false);
    lcd_ll_set_pixel_clock_edge(lcd.hal.dev, false);

    // enable RGB mode and set data width
    lcd_ll_enable_rgb_mode(lcd.hal.dev, true);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    lcd_ll_set_dma_read_stride(lcd.hal.dev, lcd.config.bus_width);
    lcd_ll_set_data_wire_width(lcd.hal.dev, lcd.config.bus_width);
#else
    lcd_ll_set_data_width(lcd.hal.dev, lcd.config.bus_width);
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    // 示波器已确认 1 拍/字节。跟 esp_lcd：phase(0,0,1)+always_on，行长由 H 时序切。
    lcd_ll_set_phase_cycles(lcd.hal.dev, 0, 0, 1);
#else
    lcd_ll_set_phase_cycles(lcd.hal.dev, 0, (lcd.dummy_bytes > 0), 1);
#endif
    lcd_ll_enable_output_hsync_in_porch_region(lcd.hal.dev, false);

#if defined(CONFIG_IDF_TARGET_ESP32S31)
    // 反转会整屏黑白对调；位序问题另查，先保持不反转
    lcd_ll_reverse_wire_bit_order(lcd.hal.dev, false);
#endif

    lcd_ll_enable_output_always_on(lcd.hal.dev, EPD_LCD_PROBE_ALWAYS_ON);
    lcd_ll_set_idle_level(lcd.hal.dev, false, true, true);

    // bk_en=0 时 S31 不再发 VSYNC，帧会停到超时。blank 保持打开。
    lcd_ll_set_blank_cycles(lcd.hal.dev, 1, 1);

    // output hsync even in porch region?
    lcd_ll_enable_output_hsync_in_porch_region(lcd.hal.dev, false);
    // send next frame automatically in stream mode
    lcd_ll_enable_auto_next_frame(lcd.hal.dev, false);

    lcd_ll_enable_interrupt(lcd.hal.dev, LCD_LL_EVENT_VSYNC_END | LCD_LL_EVENT_TRANS_DONE, false);

    // enable intr
    esp_intr_enable(lcd.lcd_intr);
    return ret;
}

static void deinit_lcd_peripheral() {
    // disable and free interrupts
    esp_intr_disable(lcd.lcd_intr);
    esp_intr_free(lcd.lcd_intr);

    lcd_ll_fifo_reset(lcd.hal.dev);
    lcd_ll_reset(lcd.hal.dev);
#if defined(CONFIG_IDF_TARGET_ESP32S31) || defined(CONFIG_IDF_TARGET_ESP32P4)
    lcd_ll_enable_trans_buffer(lcd.hal.dev, false);
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S31)
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_clock(lcd.hal.dev, false);
    }
    PERIPH_RCC_RELEASE_ATOMIC(LCD_PERIPH_PANEL.module, ref_count) {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(0, false);
        }
    }
#else
    periph_module_reset(PERIPH_LCD_CAM_MODULE);
    periph_module_disable(PERIPH_LCD_CAM_MODULE);
#endif
}

/**
 * Configure the LCD driver for epdiy.
 */
void epd_lcd_init(const LcdEpdConfig_t* config, int display_width, int display_height) {
    esp_err_t ret = ESP_OK;
    assign_lcd_parameters_from_config(config, display_width, display_height);

    check_cache_configuration();

    ret = allocate_lcd_buffers();
    ESP_GOTO_ON_ERROR(ret, err, TAG, "lcd buffer allocation failed");

    ret = init_lcd_peripheral();
    ESP_GOTO_ON_ERROR(ret, err, TAG, "lcd peripheral init failed");

    ret = init_dma_trans_link();
    ESP_GOTO_ON_ERROR(ret, err, TAG, "install DMA failed");

    ret = init_bus_gpio();
    ESP_GOTO_ON_ERROR(ret, err, TAG, "configure GPIO failed");

    init_ckv_rmt();

    // setup driver state
    epd_lcd_set_pixel_clock_MHz(lcd.config.pixel_clock / 1000 / 1000);
    epd_lcd_line_source_cb(NULL, NULL);

    ESP_LOGI(TAG, "LCD init done.");
    return;
err:
    ESP_LOGE(TAG, "LCD initialization failed!");
    abort();
}

/**
 * Deinitializue the LCD driver, i.e., free resources and peripherals.
 */
void epd_lcd_deinit() {
    epd_lcd_line_source_cb(NULL, NULL);

    deinit_bus_gpio();
    deinit_lcd_peripheral();
    deinit_dma_trans_link();
    free_lcd_buffers();
    deinit_ckv_rmt();

    ESP_LOGI(TAG, "LCD deinitialized.");
}

void epd_lcd_set_pixel_clock_MHz(int frequency) {
    const int requested_hz = frequency * 1000 * 1000;
    lcd.config.pixel_clock = requested_hz;

#if defined(CONFIG_IDF_TARGET_ESP32S31)
    const uint32_t src_clk_hz = 160000000; // LCD_CLK_SRC_DEFAULT = PLL160M
#else
    const uint32_t src_clk_hz = 240000000;
    int flags = 0;
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    hal_utils_clk_div_t clk_div = {};
/**
 * There was a change in the parameters of this function in this commit:
 * https://github.com/espressif/esp-idf/commit/d39388fe4f4c5bfb0b52df9177307b1688f41016#diff-2df607d77e3f6e350bab8eb31cfd914500ae42744564e1640cec47006cc17a9c
 * There are different builds with the same IDF minor version, some with, some without the commit.
 * So we try to select the correct one by checking if the flag value is defined.
 */
#ifdef LCD_HAL_PCLK_FLAG_ALLOW_EQUAL_SYSCLK
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    uint32_t freq
        = lcd_hal_cal_pclk_freq(&lcd.hal, src_clk_hz, lcd.config.pixel_clock, 0, &clk_div);
#else
    uint32_t freq
        = lcd_hal_cal_pclk_freq(&lcd.hal, src_clk_hz, lcd.config.pixel_clock, flags, &clk_div);
#endif
#else
    uint32_t freq = lcd_hal_cal_pclk_freq(&lcd.hal, src_clk_hz, lcd.config.pixel_clock, &clk_div);
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    PERIPH_RCC_ATOMIC() {
        lcd_ll_set_group_clock_coeff(
            lcd.hal.dev, (int)clk_div.integer, (int)clk_div.denominator, (int)clk_div.numerator
        );
    }
    // HAL 只写了 HP 分频。LCD 寄存器里 clk_en 默认 0（时钟门关闭），
    // lcd_clkm_div_num 默认 4（再除 4 → 约 5MHz，与 GPIO40 上最快 200ns 一致）。
    lcd.hal.dev->lcd_clock.clk_en = 1;
    lcd.hal.dev->lcd_clock.lcd_clk_sel = 2; // 2 = CLK160
    lcd.hal.dev->lcd_clock.lcd_clkm_div_num = 1;
    lcd.hal.dev->lcd_clock.lcd_clkm_div_a = 0;
    lcd.hal.dev->lcd_clock.lcd_clkm_div_b = 0;
    ESP_LOGI(TAG,
             "PCLK gate clk_en=%u sel=%u clkm_div=%u clkcnt_n=%u equ=%u hp_en=%u hp_div=%u",
             (unsigned)lcd.hal.dev->lcd_clock.clk_en,
             (unsigned)lcd.hal.dev->lcd_clock.lcd_clk_sel,
             (unsigned)lcd.hal.dev->lcd_clock.lcd_clkm_div_num,
             (unsigned)lcd.hal.dev->lcd_clock.lcd_clkcnt_n,
             (unsigned)lcd.hal.dev->lcd_clock.lcd_clk_equ_sysclk,
             (unsigned)HP_SYS_CLKRST.lcdcam_lcd_ctrl0.reg_lcd_clk_en,
             (unsigned)HP_SYS_CLKRST.lcdcam_lcd_ctrl0.reg_lcd_clk_div_num);
#else
    lcd_ll_set_group_clock_coeff(
        &LCD_CAM, (int)clk_div.integer, (int)clk_div.denominator, (int)clk_div.numerator
    );
#endif
#else
    uint32_t freq = lcd_hal_cal_pclk_freq(&lcd.hal, src_clk_hz, lcd.config.pixel_clock, flags);
#endif

    ESP_LOGI(TAG, "pclk freq: %u Hz (requested %d)", (unsigned)freq, requested_hz);
    // CKV 必须跟实际 PCLK 对齐；用 requested 会在分频偏差时一行多次 CKV → 门极方向拉伸。
    const uint32_t pclk = freq > 0 ? freq : (uint32_t)requested_hz;
    lcd.config.pixel_clock = (int)pclk;
    // Use 8-bit-equivalent line length for CKV/frame rate; lcd_res_h stays bus-width aware for DMA.
    lcd.line_length_us = (lcd.line_bytes + lcd.config.le_high_time + lcd.config.line_front_porch
                             + lcd.config.dummy_clocks_after_data - 1)
                             * 1000000 / pclk
                         + 1;
    lcd.line_cycles = lcd.line_length_us * pclk / 1000000;
    // EOF 逐脉冲对比度崩；×2/×6 自由跑会撕成多列。先 1:1 自由跑保清晰，拉伸另查。
    lcd.ckv_period_us = lcd.line_length_us;
    if (lcd.config.dummy_clocks_after_data) {
        // Apply Pico's data-window calculation to our 8-bit bus. Keep this
        // panel's >=1us low interval and ten post-data clocks (not Pico's pins
        // or 16-bit bus timings). CKV is expressed in tenths of a microsecond.
        const int data_clocks=lcd.lcd_res_h+lcd.config.le_high_time+lcd.config.line_front_porch;
        const int data_end=(int)(((int64_t)data_clocks*10000000+pclk-1)/pclk);
        lcd.config.ckv_high_time=max(10,min(data_end,lcd.line_length_us*10-10));
    }
    ESP_LOGI(TAG, "line width: %dus, %d cycles", lcd.line_length_us, lcd.line_cycles);

    ckv_rmt_build_signal();
    ESP_LOGI("EpdTiming", "line=%dus CKV high=%d.%dus low=%d.%dus tail=%d clocks RMT=%uHz",
        lcd.ckv_period_us,lcd.config.ckv_high_time/10,lcd.config.ckv_high_time%10,
        (lcd.ckv_period_us*10-lcd.config.ckv_high_time)/10,
        (lcd.ckv_period_us*10-lcd.config.ckv_high_time)%10,
        lcd.line_cycles-lcd.lcd_res_h-lcd.config.le_high_time-lcd.config.line_front_porch,
        (unsigned)lcd.ckv_tick_hz);
    ESP_LOGI(TAG,
             "PROBE bounce=%d always_on=%d d0_toggle=%d | expect EOF≈%dus | scope: D0=GPIO8 "
             "XCL=GPIO%d XLE=GPIO%d CKV=GPIO%d",
             EPD_LCD_PROBE_BOUNCE, EPD_LCD_PROBE_ALWAYS_ON, EPD_LCD_PROBE_D0_TOGGLE,
             lcd.line_length_us * EPD_LCD_PROBE_BOUNCE, (int)lcd.config.bus.clock,
             (int)lcd.config.bus.leh, (int)lcd.config.bus.ckv);
}

#if defined(CONFIG_IDF_TARGET_ESP32S31)
// dout/dummy 已对过，EOF 仍是 384us。一次把时钟、相位、RGB、转换器和 DMA 长度打出来。
static void log_lcd_suspects(void) {
    const lcd_cam_dev_t* d = lcd.hal.dev;
    const hp_sys_clkrst_lcdcam_lcd_ctrl0_reg_t hp = HP_SYS_CLKRST.lcdcam_lcd_ctrl0;
    ESP_LOGI(TAG,
             "suspect clk val=%08lx cnt_n=%u equ=%u idle=%u edge=%u div=%u a=%u b=%u sel=%u en=%u",
             (unsigned long)d->lcd_clock.val, (unsigned)d->lcd_clock.lcd_clkcnt_n,
             (unsigned)d->lcd_clock.lcd_clk_equ_sysclk, (unsigned)d->lcd_clock.lcd_ck_idle_edge,
             (unsigned)d->lcd_clock.lcd_ck_out_edge, (unsigned)d->lcd_clock.lcd_clkm_div_num,
             (unsigned)d->lcd_clock.lcd_clkm_div_a, (unsigned)d->lcd_clock.lcd_clkm_div_b,
             (unsigned)d->lcd_clock.lcd_clk_sel, (unsigned)d->lcd_clock.clk_en);
    ESP_LOGI(TAG, "suspect hp src=%u en=%u div=%u num=%u den=%u", (unsigned)hp.reg_lcd_clk_src_sel,
             (unsigned)hp.reg_lcd_clk_en, (unsigned)hp.reg_lcd_clk_div_num,
             (unsigned)hp.reg_lcd_clk_div_numerator, (unsigned)hp.reg_lcd_clk_div_denominator);
    ESP_LOGI(TAG,
             "suspect user val=%08lx cmd=%u cmd2=%u dummy=%u dmy=%u dout_en=%u dout_len=%u "
             "always=%u byte=%u bit=%u",
             (unsigned long)d->lcd_user.val, (unsigned)d->lcd_user.lcd_cmd,
             (unsigned)d->lcd_user.lcd_cmd_2_cycle_en, (unsigned)d->lcd_user.lcd_dummy,
             (unsigned)d->lcd_user.lcd_dummy_cyclelen, (unsigned)d->lcd_user.lcd_dout,
             (unsigned)d->lcd_user.lcd_dout_cyclelen, (unsigned)d->lcd_user.lcd_always_out_en,
             (unsigned)d->lcd_user.lcd_byte_mode, (unsigned)d->lcd_user.lcd_bit_order);
    ESP_LOGI(TAG,
             "suspect misc val=%08lx rgb=%u wire=%u bk=%u vfk=%u vbk=%u next=%u conv=%u conv8=%u "
             "rgb888=%u",
             (unsigned long)d->lcd_misc.val, (unsigned)d->lcd_misc.lcd_rgb_mode_en,
             (unsigned)d->lcd_misc.lcd_wire_mode, (unsigned)d->lcd_misc.lcd_bk_en,
             (unsigned)d->lcd_misc.lcd_vfk_cyclelen, (unsigned)d->lcd_misc.lcd_vbk_cyclelen,
             (unsigned)d->lcd_misc.lcd_next_frame_en, (unsigned)d->lcd_rgb_yuv.lcd_conv_enable,
             (unsigned)d->lcd_rgb_yuv.lcd_conv_mode_8bits_on,
             (unsigned)d->lcd_rgb_yuv.lcd_conv_rgb_mode);
    ESP_LOGI(TAG, "suspect rgb hsw=%u hb=%u ha=%u ht=%u va=%u vt=%u",
             (unsigned)d->lcd_rgb_ctrl.lcd_hsync_width + 1, (unsigned)d->lcd_rgb_blank.lcd_hb_front,
             (unsigned)d->lcd_rgb_horizontal.lcd_ha_width + 1,
             (unsigned)d->lcd_rgb_horizontal.lcd_ht_width + 1,
             (unsigned)d->lcd_rgb_vertical.lcd_va_height, (unsigned)d->lcd_rgb_vertical.lcd_vt_height);
    ESP_LOGI(TAG, "suspect dma bb=%u desc_size=%u desc_len=%u line_bytes=%d dummy_bytes=%d bounce=%d",
             (unsigned)lcd.bb_size, (unsigned)lcd.dma_nodes[0].dw0.size,
             (unsigned)lcd.dma_nodes[0].dw0.length, lcd.line_bytes, lcd.dummy_bytes,
             BOUNCE_BUF_LINES);
}
#endif

void IRAM_ATTR epd_lcd_start_frame() {
    int initial_lines = min(LINE_BATCH, lcd.display_lines);

    // 用上一相 EOF 间隔反推行周期（bounce 行一组）
    // expect≈line_length_us*BOUNCE；若 avg≈2×expect → 实际行长约 2× 软件标定
    // 每相打 LOGW 会拖慢备帧测时，默认只打首帧
    static int s_timing_logs;
    if (lcd.eof_dt_n > 0 && s_timing_logs < 1) {
        s_timing_logs++;
        const int64_t avg = lcd.eof_dt_sum_us / (int64_t)lcd.eof_dt_n;
        const int expect = lcd.line_length_us * BOUNCE_BUF_LINES;
        const int meas_line = BOUNCE_BUF_LINES > 0 ? (int)(avg / BOUNCE_BUF_LINES) : 0;
        const int theory_frame = lcd.line_length_us * lcd.display_lines;
        const int factor_x100 = expect > 0 ? (int)((avg * 100) / expect) : 0;
        ESP_LOGD(TAG,
                 "timing: frame=%lldus lines=%d | soft line=%dus theory_frame=%dus | "
                 "EOF avg=%lldus expect=%dus → meas_line≈%dus (×%d.%02d) | H hsync=%d back=%d "
                 "active=%d end=%d total=%d clk",
                 (long long)lcd.last_frame_us, lcd.display_lines, lcd.line_length_us, theory_frame,
                 (long long)avg, expect, meas_line, factor_x100 / 100, factor_x100 % 100,
                 lcd.last_h_hsync, lcd.last_h_back, lcd.last_h_active, lcd.last_h_end,
                 lcd.last_h_total);
    }

    // hsync: pulse with, back porch, active width, front porch
    int end_line
        = lcd.line_cycles - lcd.lcd_res_h - lcd.config.le_high_time - lcd.config.line_front_porch;
    const int h_hsync = lcd.config.le_high_time - (lcd.dummy_bytes > 0);
    const int h_back = lcd.config.line_front_porch;
    const int h_active = lcd.lcd_res_h + (lcd.dummy_bytes > 0);
    lcd.last_h_hsync = h_hsync;
    lcd.last_h_back = h_back;
    lcd.last_h_active = h_active;
    lcd.last_h_end = end_line;
    lcd.last_h_total = h_hsync + h_back + h_active + end_line;
    assert(end_line>=lcd.config.dummy_clocks_after_data);
    lcd_ll_set_horizontal_timing(lcd.hal.dev, h_hsync, h_back, h_active, end_line);
    lcd_ll_set_vertical_timing(lcd.hal.dev, 1, 0, initial_lines, 1);

    // generate the hsync at the very beginning of line
    lcd_ll_set_hsync_position(lcd.hal.dev, 1);

    // reset FIFO of DMA and LCD, incase there remains old frame data
    gdma_reset(lcd.dma_chan);
    lcd_ll_stop(lcd.hal.dev);
    lcd_ll_clear_interrupt_status(lcd.hal.dev, lcd_ll_get_interrupt_status(lcd.hal.dev));
    lcd_ll_fifo_reset(lcd.hal.dev);
    lcd_ll_enable_auto_next_frame(lcd.hal.dev, lcd.display_lines > LINE_BATCH);

    lcd.batches = 0;
    lcd.bb_eof_count = 0;
    lcd.lines_fed = 0;
    lcd.frame_finishing = false;
    lcd.eof_last_us = 0;
    lcd.eof_dt_min_us = 0;
    lcd.eof_dt_max_us = 0;
    lcd.eof_dt_sum_us = 0;
    lcd.eof_dt_n = 0;
    lcd.eof_work_min_us = 0;
    lcd.eof_work_max_us = 0;
    lcd.eof_work_sum_us = 0;
    lcd.eof_work_n = 0;
    lcd.frame_t0_us = 0;
    fill_bounce_buffer(lcd.bounce_buffer[0]);
    fill_bounce_buffer(lcd.bounce_buffer[1]);
    lcd.dma_nodes[0].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    lcd.dma_nodes[1].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;

    // the start of DMA should be prior to the start of LCD engine
    gdma_start(lcd.dma_chan, (intptr_t)&lcd.dma_nodes[0]);

#if defined(CONFIG_IDF_TARGET_ESP32S31)
    static int s_suspect_logged = 0;
    if (!s_suspect_logged) {
        s_suspect_logged = 1;
        log_lcd_suspects();
        ESP_LOGD(TAG,
                 "timing setup: pclk=%d line_bytes=%d lcd_res_h=%d le=%d porch=%d "
                 "line_us=%d cycles=%d H=%d+%d+%d+%d=%d → %dus/line",
                 lcd.config.pixel_clock, lcd.line_bytes, lcd.lcd_res_h, lcd.config.le_high_time,
                 lcd.config.line_front_porch, lcd.line_length_us, lcd.line_cycles, h_hsync, h_back,
                 h_active, end_line, lcd.last_h_total,
                 lcd.config.pixel_clock > 0
                     ? (int)((lcd.last_h_total * 1000000LL) / lcd.config.pixel_clock)
                     : 0);
    }
#endif

    // enter a critical section to ensure the frame start timing is correct
    taskENTER_CRITICAL(&frame_start_spinlock);

    // delay 1us is sufficient for DMA to pass data to LCD FIFO
    // in fact, this is only needed when LCD pixel clock is set too high
    gpio_set_level(lcd.config.bus.stv, 0);
    // esp_rom_delay_us(1);
    //  for picture clarity, it seems to be important to start CKV at a "good"
    //  time, seemingly start or towards end of line.
    start_ckv_cycles(initial_lines + 5);
    esp_rom_delay_us(lcd.ckv_period_us);
    gpio_set_level(lcd.config.bus.stv, 1);
    esp_rom_delay_us(lcd.ckv_period_us);
    esp_rom_delay_us(lcd.config.ckv_high_time / 10);

    // start LCD engine
    lcd.frame_t0_us = esp_timer_get_time();
    lcd_ll_clear_interrupt_status(lcd.hal.dev, lcd_ll_get_interrupt_status(lcd.hal.dev));
    lcd_ll_enable_interrupt(
        lcd.hal.dev, LCD_LL_EVENT_VSYNC_END | LCD_LL_EVENT_TRANS_DONE, true
    );
    lcd_ll_start(lcd.hal.dev);

    taskEXIT_CRITICAL(&frame_start_spinlock);
}

#else

/// Dummy implementation to link on the old ESP32
void epd_lcd_init(const LcdEpdConfig_t* config, int display_width, int display_height) {
    assert(false);
}

#endif  // S3 Target
