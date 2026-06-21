#pragma once

// FreeInk microphone capture (PDM input).
//
// Captures 16-bit mono PCM from the PDM microphone described by
// BoardConfig::ACTIVE.mic, using the ESP-IDF i2s_pdm RX driver. This is the
// input counterpart to AudioManager (which is output-only): a board can have a
// mic without an output codec, so the two are independent capabilities
// (FREEINK_CAP_MIC vs FREEINK_CAP_AUDIO).
//
// Pull-style API: begin() once, then read() raw PCM blocks on demand (a VAD /
// wake-word / recorder loop pumps it). The mic's power/enable rail is asserted
// in begin() and released in end(). Boards without a mic (FREEINK_CAP_MIC off,
// or MicInput::None) link stub bodies and present() returns false.

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

namespace freeink {

class Microphone {
 public:
  // Default PDM sample rate. 16 kHz suits voice/wake-word; PDM mics also support
  // 8 kHz and higher — pass a rate to begin() to override.
  static constexpr uint32_t kDefaultSampleRate = 16000;

  // Powers the mic rail and starts the i2s_pdm RX channel at sampleRate.
  // Returns false if the active board has no mic or bring-up fails.
  bool begin(uint32_t sampleRate = kDefaultSampleRate);

  // True when the active board declares a PDM mic and begin() succeeded.
  bool present() const { return begun_; }

  // Read up to maxSamples 16-bit mono samples into dst. Blocks up to timeoutMs
  // for data. Returns samples read (0 = timeout/no data, <0 = error/not begun).
  int read(int16_t* dst, size_t maxSamples, uint32_t timeoutMs = 100);

  // Stops the RX channel and powers the mic rail down. begin() restarts it.
  void end();

  uint32_t sampleRate() const { return sampleRate_; }

 private:
  bool begun_ = false;
  void* rxChan_ = nullptr;  // i2s_chan_handle_t (void* to keep the header light)
  uint32_t sampleRate_ = 0;
};

}  // namespace freeink

using Microphone = freeink::Microphone;
