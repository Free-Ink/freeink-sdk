#pragma once

// FreeInk RGB LED manager.
//
// Drives the addressable RGB LEDs described by BoardConfig::ACTIVE.leds. The
// first target is M5Stack PaperColor: two GRB LEDs on GPIO21, powered through
// the M5PM1 RGB LED rail. The public API is deliberately small and independent
// of M5Unified: set one LED, set all LEDs, set brightness, and run simple
// non-blocking flashes from loop().

#include <Arduino.h>
#include <BoardConfig.h>

namespace freeink {

struct LedColor {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  static LedColor rgb(uint8_t red, uint8_t green, uint8_t blue) { return LedColor{red, green, blue}; }
  static LedColor black() { return LedColor{}; }
  static LedColor red() { return LedColor{255, 0, 0}; }
  static LedColor green() { return LedColor{0, 255, 0}; }
  static LedColor blue() { return LedColor{0, 0, 255}; }
  static LedColor white() { return LedColor{255, 255, 255}; }
  static LedColor yellow() { return LedColor{255, 255, 0}; }
  static LedColor cyan() { return LedColor{0, 255, 255}; }
  static LedColor magenta() { return LedColor{255, 0, 255}; }
};

class LedManager {
 public:
  static constexpr uint8_t MAX_LEDS = 8;

  bool begin();
  bool present() const;
  bool begun() const { return begun_; }

  uint8_t count() const;

  // Global brightness scale, 0-255. Color values remain stored unscaled.
  void setBrightness(uint8_t brightness);
  uint8_t brightness() const { return brightness_; }

  void setColor(uint8_t index, LedColor color);
  void setAll(LedColor color);
  LedColor color(uint8_t index) const;

  // Push the current buffer to the physical LEDs.
  void show();
  void clear();

  // Non-blocking flash support. Call update() from loop().
  void flash(LedColor color, uint8_t count = 1, uint16_t onMs = 120, uint16_t offMs = 120);
  void update();
  bool isFlashing() const { return flashing_; }
  void stopFlash(bool restore = true);

 private:
  bool enablePower();
  void writePixels(const LedColor* colors, uint8_t count);
  void writeByte(uint8_t value);
  LedColor scaled(LedColor color) const;

  bool begun_ = false;
  uint8_t brightness_ = 255;
  LedColor pixels_[MAX_LEDS]{};
  LedColor savedPixels_[MAX_LEDS]{};

  bool flashing_ = false;
  bool flashOn_ = false;
  uint8_t flashesRemaining_ = 0;
  uint16_t flashOnMs_ = 120;
  uint16_t flashOffMs_ = 120;
  unsigned long flashNextAt_ = 0;
  LedColor flashColor_{};
};

}  // namespace freeink

using LedColor = freeink::LedColor;
using LedManager = freeink::LedManager;
