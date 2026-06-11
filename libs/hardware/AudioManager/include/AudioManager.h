#pragma once

// FreeInk audio output.
//
// Drives the audio path described by BoardConfig::ACTIVE.audio: bring up the
// control codec over its (possibly shared) I2C bus, master the I2S bus with
// the new ESP-IDF i2s_std driver, and stream 16-bit PCM WAV data from a
// caller-supplied byte source. Two codecs are supported, selected by
// AudioConfig::output: ES8388 (Murphy M3, OEM-recovered register sequence)
// and ES8311 (M5 PaperColor, mirroring M5Unified's speaker bring-up — the
// codec clocks itself from BCLK, plus the AW8737A amp on its ampEnable pin).
//
// Playback runs in a dedicated FreeRTOS task (priority above typical workers,
// like the OEM "musicTask"), so play() returns immediately; with loop=true the
// source is rewound and replayed until stop() is called — the alarm use case.
//
// The WAV source is a pair of callbacks instead of a FILE/Stream so the SDK
// stays storage-agnostic: firmware can serve bytes from LittleFS, SD, or a
// PROGMEM array with the same API.

#include <Arduino.h>

#include <functional>

namespace freeink {

class AudioManager {
 public:
  struct WavSource {
    // Copy up to len bytes to dst, returning the count (0 = EOF, <0 = error).
    std::function<int(uint8_t* dst, size_t len)> read;
    // Absolute seek from the start of the WAV; used for chunk walking and loop
    // rewind. Return false if unsupported (loop and header re-parse then fail).
    std::function<bool(size_t pos)> seek;
  };

  // Initializes the codec + enable pin. Returns false when the active board
  // has no audio path (callers can treat audio as absent).
  bool begin();
  bool present() const;

  // Analog output volume, 0-100 (maps onto the codec's OUT1/OUT2 registers).
  void setVolume(uint8_t percent);

  // Starts WAV playback (16-bit PCM, mono or stereo, 8-48 kHz). Stops any
  // current playback first. loop=true replays until stop().
  bool play(const WavSource& source, bool loop);

  // Convenience: play from a memory buffer (e.g. an embedded default sound).
  bool playBuffer(const uint8_t* data, size_t len, bool loop);

  void stop();
  bool isPlaying() const { return playing_; }

  // Codec power-down (CHIPPOWER off). begin() restores it.
  void powerDown();

 private:
  struct WavInfo {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    size_t dataStart = 0;
    size_t dataLength = 0;
  };

  static void taskEntry(void* self);
  void taskLoop();
  bool parseWavHeader(const WavSource& source, WavInfo& info);
  bool ensureI2s(uint32_t sampleRate);
  void teardownI2s();

  bool codecInit();
  bool codecWrite(uint8_t reg, uint8_t value);
  void codecMute(bool mute);
  void setAmp(bool on);

  bool begun_ = false;
  volatile bool playing_ = false;
  volatile bool stopRequested_ = false;
  TaskHandle_t task_ = nullptr;

  WavSource source_;
  WavInfo wav_;
  bool loop_ = false;

  void* txChan_ = nullptr;  // i2s_chan_handle_t (kept void* to slim the header)
  volatile bool chanEnabled_ = false;
  uint32_t currentRate_ = 0;
};

}  // namespace freeink

using AudioManager = freeink::AudioManager;
