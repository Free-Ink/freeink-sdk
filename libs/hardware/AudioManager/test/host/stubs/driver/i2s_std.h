#pragma once
#include <cassert>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
using gpio_num_t = int;
constexpr int I2S_NUM_0 = 0, I2S_ROLE_SLAVE = 1, I2S_GPIO_UNUSED = -1;
constexpr int I2S_DATA_BIT_WIDTH_32BIT = 32, I2S_SLOT_MODE_STEREO = 2;
struct FakeChannel { bool initialized = false, enabled = false, deleted = true; };
using i2s_chan_handle_t = FakeChannel*;
struct i2s_chan_config_t { int id, role; bool auto_clear = false; };
struct i2s_std_config_t {
  struct { uint32_t sample_rate_hz; int mclk_multiple = 256; } clk_cfg;
  struct { int width, mode; } slot_cfg;
  struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
};
#define I2S_CHANNEL_DEFAULT_CONFIG(id, role) {id, role}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) {rate}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, mode) {width, mode}
inline FakeChannel txFake, rxFake;
inline int allocations = 0, deletions = 0, initCalls = 0, failInitAt = 0;
inline bool failAllocation = false, failEnable = false;
inline int i2s_new_channel(const i2s_chan_config_t* c, i2s_chan_handle_t* tx, i2s_chan_handle_t* rx) {
  assert(c->id == 0 && c->role == I2S_ROLE_SLAVE && c->auto_clear);
  if (failAllocation) return ESP_FAIL;
  assert(txFake.deleted && rxFake.deleted);
  txFake = {}; rxFake = {}; txFake.deleted = rxFake.deleted = false;
  *tx = &txFake; *rx = &rxFake; ++allocations;
  return ESP_OK;
}
inline int i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t* c) {
  assert(c->clk_cfg.sample_rate_hz == 16000 && c->slot_cfg.width == 32 && c->slot_cfg.mode == 2);
  assert(c->gpio_cfg.mclk == -1 && c->gpio_cfg.bclk == 6 && c->gpio_cfg.ws == 43 &&
         c->gpio_cfg.dout == 7 && c->gpio_cfg.din == 17);
  if (++initCalls == failInitAt) return ESP_FAIL;
  h->initialized = true; return ESP_OK;
}
inline int i2s_channel_enable(i2s_chan_handle_t h) {
  assert(h->initialized && !h->enabled && !h->deleted);
  if (failEnable) return ESP_FAIL;
  h->enabled = true; return ESP_OK;
}
inline int i2s_channel_disable(i2s_chan_handle_t h) {
  assert(h->enabled && !h->deleted); h->enabled = false; return ESP_OK;
}
inline int i2s_del_channel(i2s_chan_handle_t h) {
  assert(!h->enabled && !h->deleted); h->deleted = true; ++deletions; return ESP_OK;
}
#include <vector>
#include <cstring>
constexpr int ESP_ERR_TIMEOUT = -2, I2S_NUM_AUTO = -1, I2S_ROLE_MASTER = 0;
constexpr int I2S_MCLK_MULTIPLE_256 = 256, I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_SLOT_MODE_MONO = 1;
using i2s_std_clk_config_t = decltype(i2s_std_config_t::clk_cfg);
inline int i2s_channel_reconfig_std_clock(i2s_chan_handle_t, const i2s_std_clk_config_t*) { return ESP_OK; }
inline std::vector<int32_t> writtenSlots;
inline bool failWrite = false, partialWrite = false;
inline std::vector<int32_t> incomingSlots;
inline bool readTimeout = false;
inline int i2s_channel_write(i2s_chan_handle_t h, const void* data, size_t bytes, size_t* written, uint32_t) {
  assert(h->enabled);
  *written = 0;
  if (failWrite) return ESP_ERR_TIMEOUT;
  if (partialWrite && bytes > 8) bytes = 8;
  const auto* p = static_cast<const int32_t*>(data);
  writtenSlots.insert(writtenSlots.end(), p, p + bytes / 4);
  *written = bytes; return ESP_OK;
}
inline int i2s_channel_read(i2s_chan_handle_t h, void* data, size_t bytes, size_t* read, uint32_t) {
  assert(h->enabled);
  if (bytes > incomingSlots.size() * 4) bytes = incomingSlots.size() * 4;
  memcpy(data, incomingSlots.data(), bytes);
  *read = bytes;
  return readTimeout ? ESP_ERR_TIMEOUT : ESP_OK;
}
