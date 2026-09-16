#pragma once
#include "i2s_std.h"
struct i2s_pdm_rx_config_t {
  struct { uint32_t rate; } clk_cfg;
  struct { int width, mode; } slot_cfg;
  struct { int clk, din; struct { bool clk_inv; } invert_flags; } gpio_cfg;
};
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate) {rate}
#define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(width, mode) {width, mode}
inline int i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t, const i2s_pdm_rx_config_t*) { return ESP_FAIL; }
