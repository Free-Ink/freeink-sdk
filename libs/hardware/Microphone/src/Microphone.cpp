#include "Microphone.h"

#include <BoardConfig.h>

// Capability-gated like AudioManager/FrontlightManager: boards without
// FREEINK_CAP_MIC compile the stub bodies at the bottom and link no I2S code.
#if FREEINK_CAP_MIC

#include <driver/gpio.h>
#include <driver/i2s_pdm.h>

namespace freeink {

namespace {
// Assert/deassert the mic power-enable rail per the board profile's polarity.
void setMicPower(const BoardConfig::MicConfig& mic, bool on) {
  if (mic.enable == BoardConfig::PIN_UNASSIGNED) return;
  pinMode(mic.enable, OUTPUT);
  digitalWrite(mic.enable, (on == mic.enableActiveHigh) ? HIGH : LOW);
}
}  // namespace

bool Microphone::begin(uint32_t sampleRate) {
  if (begun_) return true;
  const BoardConfig::MicConfig& mic = BoardConfig::ACTIVE.mic;
  if (mic.input != BoardConfig::MicInput::Pdm || mic.clk == BoardConfig::PIN_UNASSIGNED ||
      mic.data == BoardConfig::PIN_UNASSIGNED) {
    return false;
  }

  setMicPower(mic, true);
  // PDM mics need a brief settle after the rail comes up before the clock runs.
  delay(10);

  i2s_chan_handle_t rx = nullptr;
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chanCfg, nullptr, &rx) != ESP_OK) {
    setMicPower(mic, false);
    return false;
  }

  i2s_pdm_rx_config_t pdmCfg = {
      .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sampleRate),
      .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .clk = static_cast<gpio_num_t>(mic.clk),
              .din = static_cast<gpio_num_t>(mic.data),
              .invert_flags = {.clk_inv = false},
          },
  };
  if (i2s_channel_init_pdm_rx_mode(rx, &pdmCfg) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
    i2s_del_channel(rx);
    setMicPower(mic, false);
    return false;
  }

  rxChan_ = rx;
  sampleRate_ = sampleRate;
  begun_ = true;
  return true;
}

int Microphone::read(int16_t* dst, size_t maxSamples, uint32_t timeoutMs) {
  if (!begun_ || !rxChan_ || !dst || maxSamples == 0) return -1;
  size_t bytesRead = 0;
  const esp_err_t err = i2s_channel_read(static_cast<i2s_chan_handle_t>(rxChan_), dst,
                                         maxSamples * sizeof(int16_t), &bytesRead, pdMS_TO_TICKS(timeoutMs));
  if (err == ESP_ERR_TIMEOUT) return 0;
  if (err != ESP_OK) return -1;
  return static_cast<int>(bytesRead / sizeof(int16_t));
}

void Microphone::end() {
  if (rxChan_) {
    i2s_channel_disable(static_cast<i2s_chan_handle_t>(rxChan_));
    i2s_del_channel(static_cast<i2s_chan_handle_t>(rxChan_));
    rxChan_ = nullptr;
  }
  setMicPower(BoardConfig::ACTIVE.mic, false);
  begun_ = false;
  sampleRate_ = 0;
}

}  // namespace freeink

#else  // FREEINK_CAP_MIC — no mic on this board: stub bodies, no I2S linkage.

namespace freeink {
bool Microphone::begin(uint32_t) { return false; }
int Microphone::read(int16_t*, size_t, uint32_t) { return -1; }
void Microphone::end() {}
}  // namespace freeink

#endif  // FREEINK_CAP_MIC
