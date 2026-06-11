#pragma once

// M5Stack PaperColor — M5PM1 power-management IC (PY32L020), single owner.
//
// Two FreeInk modules drive the same PMIC over the board's internal I2C bus
// (SDA3/SCL2): the ED2208 display driver (EPD rail via GPIO0, boot power policy)
// and LedManager (the 3.3V LDO that feeds the WS2812 RGB rail). They share one
// physical register — PWR_CFG (0x06) — so they must NOT keep private copies of
// its map: an earlier split had LedManager mislabel reg 0x09 as a "watchdog"
// (it's I2C_CFG) and the two callers' notions of the power bits drifted apart.
//
// This header is the single source of truth: one register map, one bus init, one
// place that expresses the board's boot power policy. Header-only (tiny inline
// I2C RMW) so both libs pick it up via their existing BoardConfig dependency with
// no extra lib wiring.

#include <Arduino.h>
#include <Wire.h>

namespace freeink {
namespace m5pm1 {

constexpr uint8_t ADDR = 0x6E;

// Internal I2C bus (PMIC + co-resident peripherals).
constexpr int SDA = 3;
constexpr int SCL = 2;
constexpr uint32_t I2C_HZ = 100000;

// --- registers (official M5PM1 datasheet) ---
constexpr uint8_t REG_PWR_CFG = 0x06;     // [3]BOOST_EN [2]LDO_EN [1]DCDC_EN [0]CHG_EN
                                          // auto-clears to 0 on reset/download/shutdown
constexpr uint8_t REG_I2C_CFG = 0x09;     // [4]SPD(0=100k) [3:0]SLP_TO (0=no idle sleep)
constexpr uint8_t REG_GPIO_MODE = 0x10;   // 1 = output
constexpr uint8_t REG_GPIO_OUT = 0x11;    // output level
constexpr uint8_t REG_GPIO_DRV = 0x13;    // 0 = push-pull
constexpr uint8_t REG_GPIO_FUNC0 = 0x16;  // 0 = plain GPIO (vs alt function)
constexpr uint8_t REG_NEO_CFG = 0x50;     // PM1's own NeoPixel engine: [6]refresh [5:0]count
                                          // 0x00 = M5PM1::disableLeds(): engine off, ESP owns chain

// --- PWR_CFG (0x06) bits ---
constexpr uint8_t CHG_EN = 1 << 0;    // battery charge enable
constexpr uint8_t DCDC_EN = 1 << 1;   // 5V DCDC
constexpr uint8_t LDO_EN = 1 << 2;    // 3.3V LDO = WS2812 RGB rail (+ green indicator LED)
constexpr uint8_t BOOST_EN = 1 << 3;  // 5VINOUT / Grove boost

// EPD_EN is wired to PMIC GPIO0.
constexpr uint8_t GPIO0 = 1 << 0;

// Init the internal I2C bus and pin the PM1 to 100 kHz with idle-sleep disabled
// (SLP_TO=0) so it doesn't drop off the bus between transactions. Idempotent —
// safe to call from each module's begin().
inline void beginBus() {
  Wire.begin(SDA, SCL, I2C_HZ);
  Wire.setTimeOut(4);
  Wire.beginTransmission(ADDR);
  Wire.write(REG_I2C_CFG);
  Wire.write(0x00);
  Wire.endTransmission();
}

inline bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

inline bool readReg(uint8_t reg, uint8_t* value) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(ADDR, static_cast<uint8_t>(1)) != 1) return false;
  *value = Wire.read();
  return true;
}

inline bool updateReg(uint8_t reg, uint8_t clearMask, uint8_t setMask) {
  uint8_t value = 0;
  if (!readReg(reg, &value)) return false;
  return writeReg(reg, static_cast<uint8_t>((value & ~clearMask) | setMask));
}

// Board boot power policy. PWR_CFG auto-clears on reset, so this is where the
// board's standing power state is (re)established each boot:
//   CHG_EN  SET   — the PM1 only charges the 1250 mAh cell when this is on, and
//                   regulates the curve itself (charges only when VIN present,
//                   stops at full). Off by default after reset; without setting
//                   it the battery never tops up over USB.
//   BOOST_EN CLEAR — 5VINOUT/Grove boost, unused on this board.
//   LDO_EN  CLEAR — RGB rail, owned by LedManager (re-enabled lazily while an LED
//                   is lit). Cleared here so the chain stays unpowered from boot.
// Also hands the WS2812 chain to the ESP by switching off the PM1's built-in
// NeoPixel engine — left on, it renders its own status pixel (the stuck green LED
// seen at boot) even while the ESP sleeps. The display is the first PM1 caller,
// so doing both here kills the green LED before LedManager has even run.
inline void applyBootPowerPolicy() {
  updateReg(REG_PWR_CFG, BOOST_EN | LDO_EN, CHG_EN);
  writeReg(REG_NEO_CFG, 0x00);
}

// Silence the PM1's NeoPixel engine so the ESP owns the WS2812 chain.
inline void disableLeds() { writeReg(REG_NEO_CFG, 0x00); }

// The 3.3V LDO RGB rail, owned by LedManager (on only while an LED is lit).
inline void setRgbRail(bool on) { updateReg(REG_PWR_CFG, on ? 0 : LDO_EN, on ? LDO_EN : 0); }

}  // namespace m5pm1
}  // namespace freeink
