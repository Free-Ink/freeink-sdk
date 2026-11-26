#pragma once

#include <driver/gpio.h>
#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t data[16];
    gpio_num_t clock;
    gpio_num_t ckv;
    gpio_num_t start_pulse;
    gpio_num_t leh;
    gpio_num_t stv;
} lcd_bus_config_t;

/// 一行四段 + CKV。行长 = L_SL + L_BL + L_DL + L_EL，必须在改像素钟之前写好。
typedef struct {
    int le_high_time;      ///< L_SL 钟
    int line_front_porch;  ///< L_BL 钟
    int line_end;          ///< L_EL 钟
    int ckv_high_time;     ///< CKV 高电平，0.1µs
} LcdLineTiming_t;

typedef struct {
    size_t pixel_clock;
    LcdLineTiming_t line;
    int bus_width;
    lcd_bus_config_t bus;
} LcdEpdConfig_t;

esp_err_t epd_lcd_init(const LcdEpdConfig_t* config, int display_width, int display_height);
void epd_lcd_deinit(void);
void epd_lcd_set_pixel_clock_MHz(int frequency);
void epd_lcd_set_line_timing(const LcdLineTiming_t* timing);

#ifdef __cplusplus
}
#endif
