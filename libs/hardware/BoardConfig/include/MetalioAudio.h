#pragma once

// Shared by AudioManager and Microphone. Only include in Metalio audio paths.
// Lifecycle calls belong to one hardware/control task. One reader and one
// writer may stream concurrently; end/powerDown must not race their own I/O.
#include <BoardConfig.h>
#include <HardwareSerial.h>
#include <driver/i2s_std.h>
#include <detail/MetalioPcm.h>

namespace freeink { namespace metalio {
constexpr uint32_t AUDIO_RATE = 16000;

struct AudioBus {
  i2s_chan_handle_t tx = nullptr;
  i2s_chan_handle_t rx = nullptr;
  const void* speaker = nullptr;
  const void* microphone = nullptr;
  bool txEnabled = false;
  bool rxEnabled = false;
  HardwareSerial uart{2};
};

inline AudioBus& audioBus() {
  static AudioBus bus;
  return bus;
}

inline void destroyAudioBus() {
  auto& b = audioBus();
  if (b.txEnabled) i2s_channel_disable(b.tx);
  if (b.rxEnabled) i2s_channel_disable(b.rx);
  b.txEnabled = b.rxEnabled = false;
  if (b.tx) i2s_del_channel(b.tx);
  if (b.rx) i2s_del_channel(b.rx);
  b.tx = b.rx = nullptr;
  b.uart.end();
}

inline bool acquireAudio(const void* owner, bool input) {
  auto& b = audioBus();
  const void*& slot = input ? b.microphone : b.speaker;
  if (slot) return slot == owner;
  if (!b.tx) {
    if (!ensureBooted() || !setAmplifier(false)) return false;
    // Vendor InitializeBTAudio/BtDefaultModeTask: UART2, mode 1 supplies clocks.
    b.uart.begin(115200, SERIAL_8N1, 47, 48);
    if (b.uart.print("AT+RX=2\r\n") != 9) { b.uart.end(); return false; }
    b.uart.flush();
    delay(700);
    if (b.uart.print("AT+MODE=1\r\n") != 11) { b.uart.end(); return false; }
    b.uart.flush();

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, &b.tx, &b.rx) != ESP_OK) {
      destroyAudioBus();
      return false;
    }
    i2s_std_config_t cfg = {};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE);
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.bclk);
    cfg.gpio_cfg.ws = static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.lrclk);
    cfg.gpio_cfg.dout = static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.dout);
    cfg.gpio_cfg.din = static_cast<gpio_num_t>(BoardConfig::ACTIVE.mic.data);
    if (i2s_channel_init_std_mode(b.tx, &cfg) != ESP_OK ||
        i2s_channel_init_std_mode(b.rx, &cfg) != ESP_OK) {
      destroyAudioBus();
      return false;
    }
  }
  slot = owner;
  return true;
}

inline bool startAudio(const void* owner, bool input) {
  auto& b = audioBus();
  if ((input ? b.microphone : b.speaker) != owner) return false;
  bool& enabled = input ? b.rxEnabled : b.txEnabled;
  if (enabled) return true;
  if (i2s_channel_enable(input ? b.rx : b.tx) != ESP_OK) return false;
  enabled = true;
  return true;
}

inline void stopAudio(const void* owner, bool input) {
  auto& b = audioBus();
  if ((input ? b.microphone : b.speaker) != owner) return;
  bool& enabled = input ? b.rxEnabled : b.txEnabled;
  if (enabled) i2s_channel_disable(input ? b.rx : b.tx);
  enabled = false;
}

inline void releaseAudio(const void* owner, bool input) {
  auto& b = audioBus();
  const void*& slot = input ? b.microphone : b.speaker;
  if (slot != owner) return;
  stopAudio(owner, input);
  slot = nullptr;
  if (!b.speaker && !b.microphone) destroyAudioBus();
}
} }  // namespace freeink::metalio
