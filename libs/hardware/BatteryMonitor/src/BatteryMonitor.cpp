#include "BatteryMonitor.h"

#include <Arduino.h>
#include <initializer_list>  // BoardConfig.h range-for over the latch pins
#include <BoardConfig.h>
#include <I2cBusLock.h>
#include <M5Pm1.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR < 5
#include <esp_adc_cal.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

#if FREEINK_BATTERY_I2C_GAUGE
#include <Wire.h>
#if FREEINK_DEVICE_WS397
#include <Axp2101.h>
#endif

// Minimal, dependency-free I2C fuel-gauge read for boards that carry one (e.g.
// LilyGo T5 S3: BQ27220 gauge + BQ25896 charger). Standard TI command registers;
// the gauge reports true battery state, so no ADC pin or divider is involved.
// Addresses/pins come from BoardConfig::ACTIVE.batteryGauge.
namespace {
constexpr uint8_t BQ27220_VOLTAGE = 0x08;          // battery voltage, mV (u16 LE)
constexpr uint8_t BQ27220_CURRENT = 0x0C;          // average current, signed mA (i16 LE)
constexpr uint8_t BQ27220_STATE_OF_CHARGE = 0x2C;  // SoC, percent (u16 LE)
constexpr uint8_t BQ25896_REG_STATUS = 0x0B;       // CHRG_STAT in bits [4:3]
constexpr uint8_t SGM41562_REG_STATUS = 0x08;      // CHRG_STAT [4:3]; bit 1 = input present (stock)
constexpr uint8_t SGM41562_REG_BATFET = 0x06;      // bit 5 = BATFET off (ship mode)

// The gauge's I2C controller (Wire or Wire1) per BoardConfig. On single-bus SoCs
// (ESP32-C3, SOC_I2C_NUM == 1) Wire1 doesn't exist, so always use Wire there.
TwoWire& gaugeWire() {
#if SOC_I2C_NUM > 1
  if (BoardConfig::ACTIVE.batteryGauge.i2cBus == 1) return Wire1;
#endif
  return Wire;
}

bool g_wireReady = false;
void ensureWire() {
  if (g_wireReady) return;
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  gaugeWire().begin(g.i2cSda, g.i2cScl, g.i2cHz);
  g_wireReady = true;
}

bool readReg16(uint8_t addr, uint8_t reg, uint16_t& out) {
  const freeink::I2cBusLock busLock;
  if (addr == 0) return false;
  ensureWire();
  TwoWire& w = gaugeWire();
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) return false;
  const uint8_t lo = w.read();
  const uint8_t hi = w.read();
  out = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
  return true;
}

bool readReg8(uint8_t addr, uint8_t reg, uint8_t& out) {
  const freeink::I2cBusLock busLock;
  if (addr == 0) return false;
  ensureWire();
  TwoWire& w = gaugeWire();
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  out = w.read();
  return true;
}

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* out, uint8_t length) {
  const freeink::I2cBusLock busLock;
  if (addr == 0) return false;
  ensureWire();
  TwoWire& w = gaugeWire();
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, length, static_cast<uint8_t>(true)) < length) return false;
  for (uint8_t i = 0; i < length; ++i) out[i] = static_cast<uint8_t>(w.read());
  return true;
}

// True when a device acknowledges its address (an empty write).
bool i2cAcks(uint8_t addr) {
  const freeink::I2cBusLock busLock;
  ensureWire();
  TwoWire& w = gaugeWire();
  w.beginTransmission(addr);
  return w.endTransmission(true) == 0;
}

bool writeReg8(uint8_t addr, uint8_t reg, uint8_t val) {
  const freeink::I2cBusLock busLock;
  if (addr == 0) return false;
  ensureWire();
  TwoWire& w = gaugeWire();
  w.beginTransmission(addr);
  w.write(reg);
  w.write(val);
  return w.endTransmission(true) == 0;
}

// One I2C write per word: the X3's BQ27220 ignores a Control() subcommand split in two.
bool writeReg16(uint8_t addr, uint8_t reg, uint16_t val) {
  const freeink::I2cBusLock busLock;
  ensureWire();
  TwoWire& w = gaugeWire();
  delayMicroseconds(66);  // BQ27220 bus-free time between transactions
  w.beginTransmission(addr);
  w.write(reg);
  w.write(static_cast<uint8_t>(val & 0xFF));
  w.write(static_cast<uint8_t>(val >> 8));
  return w.endTransmission(true) == 0;
}

// --- BQ27220 Design Capacity -------------------------------------------------
// TRM SLUUBD4A 6.1: unseal, full access, CONFIG UPDATE, checksummed Data Memory
// writes, exit with reinit, seal. The X3's gauge ignores keys sent back to back,
// so they go 1.5 s apart.
constexpr uint8_t BQ27220_CONTROL = 0x00;
constexpr uint8_t BQ27220_FULL_CHARGE_CAPACITY = 0x12;
constexpr uint8_t BQ27220_OPERATION_STATUS = 0x3A;  // CFGUPDATE bit 10, SEC[1:0] bits 2:1
constexpr uint8_t BQ27220_DESIGN_CAPACITY = 0x3C;
constexpr uint8_t BQ27220_MAC_CONTROL = 0x3E;
constexpr uint8_t BQ27220_MAC_DATA = 0x40;
constexpr uint8_t BQ27220_MAC_DATA_SUM = 0x60;  // MACDataLen() in the high byte
constexpr uint16_t BQ27220_DM_LEARNED_FCC = 0x929D;
constexpr uint16_t BQ27220_DM_DESIGN_CAPACITY = 0x929F;
// Unseal, full access, ENTER_CFG_UPDATE.
constexpr uint16_t BQ27220_UNLOCK[] = {0x0414, 0x3672, 0xFFFF, 0xFFFF, 0x0090};

struct Bq27220Load {
  uint8_t step = 0;  // 0 check, 1-5 BQ27220_UNLOCK, 6 Learned FCC, 7 Design Capacity, 8 exit, 9 done
  bool cfg = false;
  bool wrote = false;
  unsigned long at = 0;
  unsigned long since = 0;
};
Bq27220Load bq27220Load;

// A learning cycle moves Learned Full Charge Capacity at most 256 mAh down (TRM
// 1.1.3), so one learned against TI's 3000 mAh default stays far above a small cell.
bool bq27220TooHigh(const uint16_t learned, const uint16_t mah) { return learned > mah + mah / 4; }

// Replaces TI's default Design Capacity, or a Learned FCC that is too high, in one
// big-endian word, adjusting the block checksum by the bytes that change.
bool bq27220WriteParam(const uint8_t addr, const uint16_t address, const uint16_t mah) {
  uint16_t old = 0;
  uint16_t sum = 0;
  if (!writeReg16(addr, BQ27220_MAC_CONTROL, address)) return false;
  delay(10);
  if (!readReg16(addr, BQ27220_MAC_DATA, old) || !readReg16(addr, BQ27220_MAC_DATA_SUM, sum)) return false;
  const uint16_t value = __builtin_bswap16(old);
  if (address == BQ27220_DM_LEARNED_FCC ? !bq27220TooHigh(value, mah) : value != 3000) return true;
  bq27220Load.wrote = true;
  sum = (sum & 0xFF00) | static_cast<uint8_t>(sum + (old & 0xFF) + (old >> 8) - (mah & 0xFF) - (mah >> 8));
  // Reading MACDataSum() moves the X3's gauge to the next block: select this one again.
  if (!writeReg16(addr, BQ27220_MAC_CONTROL, address)) return false;
  delay(10);
  return writeReg16(addr, BQ27220_MAC_DATA, __builtin_bswap16(mah)) && writeReg16(addr, BQ27220_MAC_DATA_SUM, sum);
}

bool bq27220LoadStep(const uint8_t addr, const uint16_t mah, const unsigned long now) {
  Bq27220Load& s = bq27220Load;
  if (s.step == 9 || static_cast<long>(now - s.at) < 0) return s.step != 9;
  const auto next = [&](const uint8_t step, const unsigned long gap) {
    s.step = step;
    s.at = now + gap;
    return true;
  };
  // Leaves CONFIG UPDATE (with reinit once a block was written), then seals.
  const auto close = [&] {
    if (s.cfg) {
      s.cfg = false;
      s.since = now;
      if (writeReg16(addr, BQ27220_CONTROL, s.wrote ? 0x0091 : 0x0092)) return next(8, 500);
    }
    writeReg16(addr, BQ27220_CONTROL, 0x0030);
    s.step = 9;
    return false;
  };
  uint16_t status = 0;
  if (s.step == 0) {
    uint16_t dc = 0;
    uint16_t fcc = 0;
    s.step = 9;
    if (!readReg16(addr, BQ27220_DESIGN_CAPACITY, dc) || !readReg16(addr, BQ27220_OPERATION_STATUS, status)) {
      return false;
    }
    s.cfg = status & 0x0400;
    const uint8_t security = (status >> 1) & 3;
    if (dc == mah && (s.cfg || security != 3)) {
      s.wrote = true;  // an earlier load was cut short
      return close();
    }
    if (dc == mah ? !readReg16(addr, BQ27220_FULL_CHARGE_CAPACITY, fcc) || !bq27220TooHigh(fcc, mah) : dc != 3000) {
      return false;
    }
    s.step = security == 3 ? 1 : security == 1 ? 5 : 3;
  }
  if (s.step <= 5) {
    s.cfg |= s.step == 5;
    s.since = now;
    if (!writeReg16(addr, BQ27220_CONTROL, BQ27220_UNLOCK[s.step - 1])) return close();
    return next(s.step + 1, s.step == 5 ? 500 : 1500);
  }
  if (s.step == 6 || s.step == 8) {
    const bool reached =
        readReg16(addr, BQ27220_OPERATION_STATUS, status) && ((status & 0x0400) != 0) == (s.step == 6);
    if (!reached && now - s.since < 5000) return next(s.step, 500);
    // Learned FCC first, Design Capacity half a second later: the X3's gauge can miss
    // a block select sent right after a block write. If anything fails, Design
    // Capacity still reads 3000 or FullChargeCapacity() too high, and the next start retries.
    if (s.step == 6 && reached && bq27220WriteParam(addr, BQ27220_DM_LEARNED_FCC, mah)) return next(7, 500);
  } else {
    bq27220WriteParam(addr, BQ27220_DM_DESIGN_CAPACITY, mah);
  }
  return close();
}

// --- CW2017 (CellWise) fuel gauge -------------------------------------------
// Register map + init recovered from the Xteink X4 Pro OEM firmware (the
// XTEink::Cw2017PowerHal class in app1) via Ghidra. Unlike the BQ27220, the CW2017
// reports 0% until a matching 80-byte BATINFO battery profile is resident, so init
// must verify/upload one before SoC reads mean anything.
constexpr uint8_t CW2017_REG_VERSION = 0x00;    // 0xA0 while starting; running versions match 0x0D/0x0F
constexpr uint8_t CW2017_REG_VCELL_H = 0x02;    // 14-bit VCELL, big-endian over 0x02/0x03
constexpr uint8_t CW2017_REG_SOC = 0x04;        // integer percent (0x05 = fraction, unused)
constexpr uint8_t CW2017_REG_MODE = 0x08;       // soft-reset / sleep control
constexpr uint8_t CW2017_REG_SOC_ALERT = 0x0B;  // bit7 = profile-loaded / update-enable
constexpr uint8_t CW2017_REG_BATINFO = 0x10;    // 80-byte profile spans 0x10..0x5F

constexpr uint8_t CW2017_MODE_NORMAL = 0x00;
constexpr uint8_t CW2017_MODE_RESTART = 0x30;
constexpr uint8_t CW2017_MODE_DEFAULT = 0xF0;
constexpr uint8_t CW2017_UPDATE_FLAG = 0x80;
constexpr unsigned long CW2017_INIT_RETRY_MS = 1000;

// The exact BATINFO profile the OEM uploads (app1 table @ DROM 0x3c5d8d00). This is
// battery-model-specific; it is the profile for the X4 Pro's cell.
constexpr uint8_t CW2017_BATINFO[80] = {
    0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0xbf, 0xb5, 0xb4, 0xa4, 0x9c, 0xeb, 0xe2,
    0xdf, 0xe5, 0xca, 0xa0, 0x8a, 0x62, 0x53, 0x48, 0x40, 0x3a, 0x32, 0xb1, 0xae, 0xda, 0xb5, 0xff,
    0xff, 0xff, 0xe8, 0xdb, 0xd9, 0xd6, 0xd4, 0xd2, 0xd0, 0xcb, 0xc3, 0xbc, 0x9e, 0x87, 0x7b, 0x71,
    0x72, 0x7c, 0x8c, 0xa3, 0xb7, 0xc8, 0xa5, 0x4f, 0x00, 0x00, 0xab, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23};

bool cw2017VersionIsRunning(const uint8_t version) { return (version & 0xFD) == 0x0D; }

// Soft-reset: MODE 0xF0 -> 0x30 -> 0x00, 20 ms apart (OEM FUN_4215042c).
bool cw2017Reset(const uint8_t addr) {
  if (!writeReg8(addr, CW2017_REG_MODE, CW2017_MODE_DEFAULT)) return false;
  delay(20);
  if (!writeReg8(addr, CW2017_REG_MODE, CW2017_MODE_RESTART)) return false;
  delay(20);
  if (!writeReg8(addr, CW2017_REG_MODE, CW2017_MODE_NORMAL)) return false;
  delay(20);
  return true;
}

// Wait for the calculation engine to leave its 0xA0 startup state, then for a
// valid SoC. The bounds mirror the vendor drivers: roughly 1 s for VERSION and
// 3 s for SoC, but this path only runs after a reset/profile update.
bool cw2017WaitUntilReady(const uint8_t addr) {
  bool versionReady = false;
  for (int i = 0; i < 50; ++i) {
    uint8_t version = 0;
    if (readReg8(addr, CW2017_REG_VERSION, version) && cw2017VersionIsRunning(version)) {
      versionReady = true;
      break;
    }
    delay(20);
  }
  if (!versionReady) return false;

  for (int i = 0; i < 30; ++i) {
    uint8_t soc = 0;
    if (readReg8(addr, CW2017_REG_SOC, soc) && soc <= 100) return true;
    delay(100);
  }
  return false;
}

// Check the update flag and compare every resident BATINFO byte. An I2C error is
// kept distinct from a real mismatch so a transient bus failure never triggers
// a partial profile rewrite.
bool cw2017ProfileMatches(const uint8_t addr, bool& matches) {
  uint8_t config = 0;
  if (!readReg8(addr, CW2017_REG_SOC_ALERT, config)) return false;
  if ((config & CW2017_UPDATE_FLAG) == 0) {
    matches = false;
    return true;
  }

  for (uint8_t i = 0; i < sizeof(CW2017_BATINFO); ++i) {
    uint8_t stored = 0;
    if (!readReg8(addr, static_cast<uint8_t>(CW2017_REG_BATINFO + i), stored)) return false;
    if (stored != CW2017_BATINFO[i]) {
      matches = false;
      return true;
    }
  }

  matches = true;
  return true;
}

// Make sure the gauge is running with the correct profile. Every failure is
// propagated so callers can retry instead of permanently accepting a failed
// first attempt. This matters on the X4 Pro because the gauge shares Wire with
// the GT911 and an early transient I2C error is otherwise easy to cache forever.
bool cw2017EnsureProfile(const uint8_t addr) {
  uint8_t mode = 0;
  uint8_t version = 0;
  if (!readReg8(addr, CW2017_REG_MODE, mode)) return false;
  if (!readReg8(addr, CW2017_REG_VERSION, version)) return false;

  bool profileMatches = false;
  if (!cw2017ProfileMatches(addr, profileMatches)) return false;

  bool restartRequired = mode != CW2017_MODE_NORMAL || !cw2017VersionIsRunning(version);
  if (!profileMatches) {
    for (uint8_t i = 0; i < sizeof(CW2017_BATINFO); ++i) {
      if (!writeReg8(addr, static_cast<uint8_t>(CW2017_REG_BATINFO + i), CW2017_BATINFO[i])) return false;
    }
    if (!writeReg8(addr, CW2017_REG_SOC_ALERT, CW2017_UPDATE_FLAG)) return false;
    delay(20);
    restartRequired = true;
  }

  if (restartRequired) {
    if (!cw2017Reset(addr)) return false;
    return cw2017WaitUntilReady(addr);
  }

  uint8_t soc = 0;
  if (!readReg8(addr, CW2017_REG_SOC, soc)) return false;
  if (soc <= 100) return true;

  // A running gauge should never expose an invalid integer SoC. Give it one
  // controlled restart rather than marking initialization successful forever.
  if (!cw2017Reset(addr)) return false;
  return cw2017WaitUntilReady(addr);
}

// --- CW2015 (CellWise) fuel gauge -------------------------------------------
// Sequence and conversions recovered from the EEGO A4 stock firmware (V1.0.3,
// functions FUN_420a461c / FUN_420a620c / FUN_420a4908 / FUN_420a4a58) via Ghidra.
// Stock probes the gauge once at boot and uses its battery ADC pin when the gauge
// does not answer; BatteryMonitor does the same (see gaugeActive()).
constexpr uint8_t CW2015_REG_VCELL_H = 0x02;  // 14-bit VCELL, 305 uV/LSB, big-endian over 0x02/0x03
constexpr uint8_t CW2015_REG_CONFIG = 0x08;   // bit 1 = UFG, the profile update flag
constexpr uint8_t CW2015_REG_MODE = 0x0A;     // [7:6] = 11 sleep, [3:0] = 1111 power-on reset
constexpr uint8_t CW2015_REG_BATINFO = 0x10;  // 64-byte profile spans 0x10..0x4F
constexpr uint8_t CW2015_CONFIG_UFG = 0x02;
constexpr uint8_t CW2015_MODE_SLEEP = 0xC0;
constexpr uint8_t CW2015_MODE_POR = 0x0F;

// The profile stock uploads ("3.7V 2000mAh target profile", DROM 0x3c697280).
constexpr uint8_t CW2015_BATINFO[64] = {
    0x15, 0x7e, 0x7c, 0x5c, 0x64, 0x6a, 0x65, 0x5c, 0x55, 0x53, 0x56, 0x61, 0x6f, 0x66, 0x50, 0x48,
    0x43, 0x42, 0x40, 0x43, 0x4b, 0x5f, 0x75, 0x7d, 0x52, 0x44, 0x07, 0xae, 0x11, 0x22, 0x40, 0x56,
    0x6c, 0x7c, 0x85, 0x86, 0x3d, 0x19, 0x8d, 0x1b, 0x06, 0x34, 0x46, 0x79, 0x8d, 0x90, 0x90, 0x46,
    0x67, 0x80, 0x97, 0xaf, 0x80, 0x9f, 0xae, 0xcb, 0x2f, 0x00, 0x64, 0xa5, 0xb5, 0x11, 0xd0, 0x11};

// Stock reads the profile in 8-byte bursts.
bool cw2015ReadBatinfo(const uint8_t addr, uint8_t* out) {
  for (uint8_t i = 0; i < sizeof(CW2015_BATINFO); i += 8) {
    if (!readRegs(addr, static_cast<uint8_t>(CW2015_REG_BATINFO + i), out + i, 8)) return false;
  }
  return true;
}

// Stock upload: profile bytes one register at a time, CONFIG |= UFG, then MODE
// power-on reset (|= 0x0F) and back (&= 0xF0) two RTOS ticks later, then read the
// profile and CONFIG back. true when the profile and UFG read back.
bool cw2015UploadProfile(const uint8_t addr) {
  uint8_t config = 0;
  uint8_t mode = 0;
  if (!readReg8(addr, CW2015_REG_CONFIG, config) || !readReg8(addr, CW2015_REG_MODE, mode)) return false;
  for (uint8_t i = 0; i < sizeof(CW2015_BATINFO); ++i) {
    if (!writeReg8(addr, static_cast<uint8_t>(CW2015_REG_BATINFO + i), CW2015_BATINFO[i])) return false;
  }
  if (!writeReg8(addr, CW2015_REG_CONFIG, config | CW2015_CONFIG_UFG)) return false;
  if (!writeReg8(addr, CW2015_REG_MODE, mode | CW2015_MODE_POR)) return false;
  delay(20);  // stock: 2 RTOS ticks (2 ms at 1 kHz, 20 ms at 100 Hz); the longer is used
  if (!writeReg8(addr, CW2015_REG_MODE, mode & 0xF0)) return false;
  uint8_t stored[sizeof(CW2015_BATINFO)] = {};
  return cw2015ReadBatinfo(addr, stored) && readReg8(addr, CW2015_REG_CONFIG, config) &&
         std::memcmp(stored, CW2015_BATINFO, sizeof(stored)) == 0 && (config & CW2015_CONFIG_UFG) != 0;
}

// One-time setup, as stock runs it after the boot probe: wake a gauge left in
// sleep mode, then upload the profile when the resident one is empty or differs.
// A matching profile is kept even with UFG clear, and a failed profile read
// uploads nothing, both as stock does.
void cw2015Start(const uint8_t addr) {
  uint8_t mode = 0;
  if (readReg8(addr, CW2015_REG_MODE, mode) && (mode & CW2015_MODE_SLEEP) == CW2015_MODE_SLEEP) {
    writeReg8(addr, CW2015_REG_MODE, 0x00);
  }
  uint8_t resident[sizeof(CW2015_BATINFO)] = {};
  if (!cw2015ReadBatinfo(addr, resident)) return;
  if (std::memcmp(resident, CW2015_BATINFO, sizeof(resident)) != 0) cw2015UploadProfile(addr);
}

struct Cw2015State {
  bool probed = false;
  bool present = false;
};
Cw2015State cw2015State;

// Voltage and the stock battery-icon percentage in one 7-byte read (0x02..0x08).
// Stock rejects the sample while the gauge sleeps, outside 2500..4500 mV, or with
// SOC above 100.00%. Percent is SOC rounded to an integer plus one, capped at 100.
bool cw2015Read(uint16_t& millivolts, uint16_t& percent) {
  const uint8_t addr = BoardConfig::ACTIVE.batteryGauge.gaugeAddr;
  uint8_t r[7] = {};
  uint8_t mode = 0;
  if (!readRegs(addr, CW2015_REG_VCELL_H, r, sizeof(r)) || !readReg8(addr, CW2015_REG_MODE, mode)) return false;
  if ((mode & CW2015_MODE_SLEEP) == CW2015_MODE_SLEEP) return false;
  const uint32_t mv = ((static_cast<uint32_t>(r[0] & 0x3F) << 8 | r[1]) * 305 + 500) / 1000;
  const uint16_t soc = static_cast<uint16_t>(r[2] << 8 | r[3]);  // 1/256 %
  if (mv < 2500 || mv > 4500 || soc > 0x6400) return false;
  millivolts = static_cast<uint16_t>(mv);
  percent = static_cast<uint16_t>(std::min((soc + 0x80) / 256 + 1, 100));
  return true;
}

// Whether the profile's gauge is the battery source. A CW2015 is probed once and,
// when it answers, set up; when it does not, the board's ADC pin is used instead.
bool gaugeActive() {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeAddr == 0) return false;
  if (g.gaugeType != BoardConfig::GaugeType::Cw2015) return true;
  if (!cw2015State.probed) {
    cw2015State.probed = true;
    cw2015State.present = i2cAcks(g.gaugeAddr);
    if (cw2015State.present) cw2015Start(g.gaugeAddr);
  }
  return cw2015State.present;
}

// SoC (0..100) from the active gauge, dispatched by type. false on I2C failure.
bool readGaugeSoc(uint16_t& out) {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
#if FREEINK_DEVICE_WS397
  if (g.gaugeType == BoardConfig::GaugeType::Axp2101) {
    uint8_t soc = 0;
    if (!freeink::axp2101::batteryPercent(soc)) return false;
    out = soc;
    return true;
  }
#endif
  if (g.gaugeType == BoardConfig::GaugeType::Cw2015) {
    uint16_t mv = 0;
    return cw2015Read(mv, out);
  }
  if (g.gaugeType == BoardConfig::GaugeType::Cw2017) {
    static bool initialized = false;
    static unsigned long lastInitAttemptMs = 0;

    const unsigned long now = millis();
    if (!initialized) {
      if (lastInitAttemptMs != 0 && (now - lastInitAttemptMs) < CW2017_INIT_RETRY_MS) return false;
      lastInitAttemptMs = now;
      initialized = cw2017EnsureProfile(g.gaugeAddr);
      if (!initialized) return false;
    }

    // Do not treat an acknowledged but sleeping/not-ready gauge as a fresh SoC
    // source. Clearing initialized makes the next call run the bounded recovery.
    uint8_t mode = 0;
    uint8_t version = 0;
    uint8_t soc = 0;
    if (!readReg8(g.gaugeAddr, CW2017_REG_MODE, mode) || mode != CW2017_MODE_NORMAL ||
        !readReg8(g.gaugeAddr, CW2017_REG_VERSION, version) || !cw2017VersionIsRunning(version) ||
        !readReg8(g.gaugeAddr, CW2017_REG_SOC, soc) || soc > 100) {
      initialized = false;
      return false;
    }

    out = soc;
    return true;
  }
  uint16_t soc = 0;
  if (!readReg16(g.gaugeAddr, BQ27220_STATE_OF_CHARGE, soc)) return false;
  out = soc > 100 ? 100 : soc;
  return true;
}

// Battery voltage (mV) from the active gauge, dispatched by type. false on failure.
bool readGaugeMillivolts(uint16_t& out) {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
#if FREEINK_DEVICE_WS397
  if (g.gaugeType == BoardConfig::GaugeType::Axp2101) return freeink::axp2101::batteryMillivolts(out);
#endif
  if (g.gaugeType == BoardConfig::GaugeType::Cw2015) {
    uint16_t percent = 0;
    return cw2015Read(out, percent);
  }
  if (g.gaugeType == BoardConfig::GaugeType::Cw2017) {
    uint8_t hi = 0, lo = 0;
    if (!readReg8(g.gaugeAddr, CW2017_REG_VCELL_H, hi)) return false;
    if (!readReg8(g.gaugeAddr, static_cast<uint8_t>(CW2017_REG_VCELL_H + 1), lo)) return false;
    const uint16_t raw14 = static_cast<uint16_t>((hi & 0x3F) << 8) | lo;
    out = static_cast<uint16_t>((raw14 * 5 + 8) >> 4);  // OEM formula (~0.3125 mV/LSB, rounded)
    return true;
  }
  uint16_t mv = 0;
  if (!readReg16(g.gaugeAddr, BQ27220_VOLTAGE, mv)) return false;
  out = mv;
  return true;
}

uint8_t chargerStatusReg() {
  return BoardConfig::ACTIVE.batteryGauge.chargerType == BoardConfig::ChargerType::Sgm41562 ? SGM41562_REG_STATUS
                                                                                          : BQ25896_REG_STATUS;
}

// CHRG_STAT [4:3] from the charger IC: 01 pre-charge or 10 fast-charge means
// charging (11 = charge done). `known` is false with no charger or a failed read.
bool readChargerCharging(bool& known) {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  uint8_t status = 0;
  known = g.chargerAddr != 0 && readReg8(g.chargerAddr, chargerStatusReg(), status);
  if (!known) return false;
  const uint8_t chrg = (status >> 3) & 0x03;
  return chrg == 0x01 || chrg == 0x02;
}

// Charging state for an I2C-gauge board, from the active board's gauge config.
// Two sources, in order of preference:
//   1. A dedicated charger IC (BQ25896): CHRG_STAT in REG0B[4:3] — 01 pre-charge
//      or 10 fast-charge means charging. Used by LilyGo T5 S3.
//   2. Gauge-native fallback (BQ27220 Current(), signed mA): current flowing INTO
//      the battery (> 0) means charging. Lets boards with a gauge but NO charger
//      IC — e.g. Xteink X3 — still report charge status. Current() is used rather
//      than the BatteryStatus DSG bit because DSG also clears during rest, so it
//      can't tell "charging" from "idle"; the current sign can.
// `known` is set false only when neither source responds (transient I2C failure or
// a board with neither gaugeAddr nor chargerAddr).
bool readGaugeCharging(bool& known) {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
#if FREEINK_DEVICE_WS397
  if (g.gaugeType == BoardConfig::GaugeType::Axp2101) return freeink::axp2101::isCharging(known);
#endif
  // CW2017/CW2015 have no current register and their boards have no charger IC on
  // this bus, so charging state is not observable from the gauge (the OEM firmware
  // reads a charger STAT pin).
  if (g.gaugeType == BoardConfig::GaugeType::Cw2017 || g.gaugeType == BoardConfig::GaugeType::Cw2015) {
    known = false;
    return false;
  }
  const bool charging = readChargerCharging(known);
  if (known) return charging;
  uint16_t raw = 0;
  if (readReg16(g.gaugeAddr, BQ27220_CURRENT, raw)) {
    known = true;
    return static_cast<int16_t>(raw) > 0;
  }
  known = false;
  return false;
}

// External-power presence from the charger IC, out of the SAME REG0B read that
// readGaugeCharging() uses for CHRG_STAT:
//   VBUS_STAT [7:5] — 000 no input, 001 USB host SDP, 010 USB CDP, 011 adapter,
//                     111 OTG (we are SOURCING power, not receiving it)
//   PG_STAT   [2]   — power good
// Anything but "no input" and "OTG" means a cable is supplying us. Unlike
// charge state this stays true at 100%, which is exactly the case that makes
// charge-based USB inference wrong.
//
// Only the BQ25896 answers this. The BQ27220 gauge measures the BATTERY, not
// the input rail, so there is deliberately no gauge fallback here — reporting
// "no external power" from a gauge that cannot see the input would be a lie.
bool readChargerExternalPower(bool& known) {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.chargerAddr == 0) {
    known = false;
    return false;
  }
  uint8_t status = 0;
  if (!readReg8(g.chargerAddr, chargerStatusReg(), status)) {
    known = false;
    return false;
  }
  known = true;
  // Stock (FUN_4200a124 -> FUN_42011620) treats REG08 bit 1 as "USB present".
  if (g.chargerType == BoardConfig::ChargerType::Sgm41562) return (status & 0x02) != 0;
  const uint8_t vbus = (status >> 5) & 0x07;
  const bool powerGood = (status & 0x04) != 0;
  return (vbus != 0x00 && vbus != 0x07) || powerGood;
}
}  // namespace
#endif  // FREEINK_BATTERY_I2C_GAUGE

namespace {
#if FREEINK_DEVICE_PICCO && FREEINK_BATTERY_I2C_GAUGE
// Onyx Picco: a high-voltage cell (stock programs the charger to 4.35 V), so the
// generic Li-ion curve reads 100% for the first stretch of discharge. These are
// the stock firmware's tables (build 2912, DROM 0x3c823ce8 / 0x3c823d3c): battery
// mV at 100%, 95%, ... 0%, one for charging (terminal voltage runs high under
// charge current) and one for discharging. Interpolated like stock.
constexpr uint16_t PICCO_CHARGING_MV[21] = {4305, 4305, 4300, 4290, 4235, 4187, 4124, 4078, 4045, 4013, 3984,
                                            3961, 3941, 3923, 3906, 3894, 3876, 3840, 3805, 3760, 3500};
constexpr uint16_t PICCO_DISCHARGING_MV[21] = {4305, 4229, 4171, 4118, 4065, 4019, 3955, 3915, 3876, 3841, 3812,
                                               3786, 3764, 3745, 3730, 3717, 3697, 3667, 3636, 3565, 3300};

uint16_t piccoPercent(uint16_t mv, bool charging) {
  const uint16_t* t = charging ? PICCO_CHARGING_MV : PICCO_DISCHARGING_MV;
  if (mv == 0 || mv <= t[20]) return 0;  // 0 mV = failed read, never "full"
  if (mv >= t[0]) return 100;
  uint16_t pct = 0;
  for (uint8_t i = 1; i <= 20; ++i) {
    if (mv < t[i]) continue;
    const uint16_t hi = t[i - 1], lo = t[i];
    pct = static_cast<uint16_t>(100 - 5 * i + (hi > lo ? 5 * (mv - lo) / (hi - lo) : 5));
    break;
  }
  return pct;
}
#endif

// Percentage for an ADC-measured battery voltage on the active board.
uint16_t adcPercentFromMillivolts(uint16_t millivolts) {
#if FREEINK_DEVICE_PICCO && FREEINK_BATTERY_I2C_GAUGE
  if (BoardConfig::isPicco()) {
    bool known = false;
    const bool charging = readChargerCharging(known);
    return piccoPercent(millivolts, known && charging);
  }
#endif
  return BatteryMonitor::percentageFromMillivolts(millivolts);
}
}  // namespace

namespace {
constexpr uint8_t M5PM1_REG_PWR_SRC = 0x04;
constexpr uint8_t M5PM1_REG_VREF_L = 0x20;
constexpr uint8_t M5PM1_PWR_SRC_5VIN = 1u << 0;
constexpr uint8_t M5PM1_PWR_SRC_5VINOUT = 1u << 1;

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
}
}  // namespace

BatteryMonitor::BatteryMonitor()
    : BatteryMonitor(BoardConfig::ACTIVE.batteryAdc, BoardConfig::ACTIVE.batteryDividerMultiplier,
                     BoardConfig::ACTIVE.batteryChargeStatus) {}

namespace {
// Level meaning "charging" on the charge-status pin, per the active board's
// polarity. Active-low /STAT lines are open-drain and need the internal
// pull-up; an active-high STAT (X4 Pro GPIO21) is push-pull driven with no
// pull — stock reads it bare, and a pull-up would fake "charging" if the
// driver ever tri-states.
int chargeActiveLevel() {
  return BoardConfig::ACTIVE.batteryChargeStatusActiveHigh ? HIGH : LOW;
}
}  // namespace

BatteryMonitor::BatteryMonitor(int8_t adcPin, float dividerMultiplier, int8_t chargeStatusPin)
    : _adcPin(adcPin), _dividerMultiplier(dividerMultiplier), _chargeStatusPin(chargeStatusPin) {
  if (_chargeStatusPin >= 0) {
    pinMode(_chargeStatusPin, BoardConfig::ACTIVE.batteryChargeStatusActiveHigh ? INPUT : INPUT_PULLUP);
  }
}

bool BatteryMonitor::hasAdcBackend() const {
  return _adcPin >= 0;
}

bool BatteryMonitor::hasGaugeBackend() const {
#if FREEINK_BATTERY_I2C_GAUGE
  return gaugeActive();
#else
  return false;
#endif
}

bool BatteryMonitor::hasM5Pm1Backend() const {
  return BoardConfig::isM5StackPaperColor() || BoardConfig::isPaperMono();
}

uint16_t BatteryMonitor::readPercentage() const {
#if FREEINK_BATTERY_I2C_GAUGE
  // Runtime, per active profile: gauge boards (X3, LilyGo, X4 Pro) read SoC over I2C;
  // ADC boards (X4) in the same binary fall through to the divider path below.
  if (hasGaugeBackend()) {
    uint16_t soc = 0;
    if (!readGaugeSoc(soc)) return 0;
    return soc;
  }
#endif
  if (hasM5Pm1Backend()) {
    Status status;
    if (readM5Pm1Status(status) && status.percentageKnown) return status.percentage;
    return 0;
  }
  if (!hasAdcBackend()) return 0;
  return adcPercentFromMillivolts(readMillivolts());
}

bool BatteryMonitor::readPercentageChecked(uint16_t& out) const {
#if FREEINK_BATTERY_I2C_GAUGE
  if (hasGaugeBackend()) {
    uint16_t soc = 0;
    if (!readGaugeSoc(soc)) return false;
    out = soc;
    return true;
  }
#endif
  if (hasM5Pm1Backend()) {
    Status status;
    if (!readM5Pm1Status(status) || !status.percentageKnown) return false;
    out = status.percentage;
    return true;
  }
  if (!hasAdcBackend()) return false;
  out = adcPercentFromMillivolts(readMillivolts());
  return true;
}

bool BatteryMonitor::enterShipMode() {
#if FREEINK_BATTERY_I2C_GAUGE
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.chargerAddr == 0 || g.chargerType != BoardConfig::ChargerType::Sgm41562) return false;
  uint8_t v = 0;
  if (!readReg8(g.chargerAddr, SGM41562_REG_BATFET, v)) return false;
  return writeReg8(g.chargerAddr, SGM41562_REG_BATFET, v | 0x20);
#else
  return false;
#endif
}

bool BatteryMonitor::configureCharger() {
#if FREEINK_BATTERY_I2C_GAUGE
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.chargerAddr == 0 || g.chargerType != BoardConfig::ChargerType::Sgm41562) return false;
  uint8_t gate = 0xFF;
  if (!readReg8(g.chargerAddr, 0x0B, gate) || gate != 0) return false;
  // {register, mask, value}, in stock order (FUN_4200a2d0, build 2912).
  static const uint8_t steps[][3] = {
      {0x05, 0x60, 0x00},  // FUN_4200a1dc(0)
      {0x02, 0x7F, 0x3E},
      {0x06, 0x80, 0x80},  // FUN_4200a1c0(1)
      {0x01, 0x08, 0x00},  // FUN_4200a17c(1)
      {0x04, 0xFE, 0xAA},  // FUN_4200a260(4350): charge voltage code 85 = 4.35 V
      {0x0C, 0x02, 0x00},  // FUN_4200a1a0(2800)
      {0x0D, 0xF8, 0x48},  // FUN_4200a214(50)
      {0x01, 0xE0, 0x00},  // FUN_4200a294(8, 2)
  };
  for (const auto& s : steps) {
    uint8_t old = 0;
    if (!readReg8(g.chargerAddr, s[0], old)) return false;
    const uint8_t next = static_cast<uint8_t>((old & ~s[1]) | (s[2] & s[1]));
    if (next != old && !writeReg8(g.chargerAddr, s[0], next)) return false;
  }
  return true;
#else
  return false;
#endif
}

bool BatteryMonitor::loadDesignCapacity() {
#if FREEINK_BATTERY_I2C_GAUGE
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeType == BoardConfig::GaugeType::Bq27220 && g.designCapacityMah != 0) {
    return bq27220LoadStep(g.gaugeAddr, g.designCapacityMah, millis());
  }
#endif
  return false;
}

BatteryMonitor::Status BatteryMonitor::readStatus() const {
  Status status;

#if FREEINK_BATTERY_I2C_GAUGE
  if (hasGaugeBackend()) {
    status.supported = true;
    uint16_t soc = 0;
    if (readGaugeSoc(soc)) {
      status.percentageKnown = true;
      status.percentage = soc;
    }
    uint16_t mv = 0;
    if (readGaugeMillivolts(mv)) {
      status.millivoltsKnown = true;
      status.millivolts = mv;
    }
    // Charging: from a dedicated charger IC when present, else the gauge's own
    // Current() sign — so gauge-only boards (X3) report it too. A gauge that
    // cannot observe charging at all (CW2017) leaves chargingKnown false; fall
    // back to the charger's STAT pin when the board has one (X4 Pro GPIO21).
    bool chargingKnown = false;
    bool charging = readGaugeCharging(chargingKnown);
    if (!chargingKnown && _chargeStatusPin >= 0) {
      chargingKnown = true;
      charging = digitalRead(_chargeStatusPin) == chargeActiveLevel();
    }
    status.chargingKnown = chargingKnown;
    status.charging = charging;
    return status;
  }
#endif

  if (hasM5Pm1Backend()) {
    readM5Pm1Status(status);
    return status;
  }

  if (hasAdcBackend()) {
    status.supported = true;
    status.millivolts = readMillivolts();
    status.millivoltsKnown = status.millivolts > 0;
    if (status.millivoltsKnown) {
      status.percentage = adcPercentFromMillivolts(status.millivolts);
      status.percentageKnown = true;
    }
    if (_chargeStatusPin >= 0) {
      status.chargingKnown = true;
      status.charging = digitalRead(_chargeStatusPin) == chargeActiveLevel();
    }
#if FREEINK_BATTERY_I2C_GAUGE
    if (!status.chargingKnown) status.charging = readChargerCharging(status.chargingKnown);
    status.externalPower = readChargerExternalPower(status.externalPowerKnown);
#endif
  }
  return status;
}

uint16_t BatteryMonitor::readMillivolts() const {
#if FREEINK_BATTERY_I2C_GAUGE
  if (hasGaugeBackend()) {
    uint16_t gaugeMv = 0;
    readGaugeMillivolts(gaugeMv);
    return gaugeMv;  // gauge reports true battery mV (no divider)
  }
#endif
  if (hasM5Pm1Backend()) {
    Status status;
    if (readM5Pm1Status(status) && status.millivoltsKnown) return status.millivolts;
    return 0;
  }
  if (!hasAdcBackend()) return 0;
#if ESP_IDF_VERSION_MAJOR < 5
  // ESP-IDF 4.x doesn't have analogReadMilliVolts, so calibrate manually.
  const uint16_t raw = analogRead(_adcPin);
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  const uint16_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
#else
  // ESP-IDF 5.x has analogReadMilliVolts
  // OnePage only: pause charging around the read. Other boards with a
  // chargeEnable pin (e.g. Sticky) must NOT have it glitched during battery reads.
  uint16_t mv = 0;
  if (BoardConfig::isOnePage() && BoardConfig::ACTIVE.power.chargeEnable >= 0) {
    const int8_t ce = BoardConfig::ACTIVE.power.chargeEnable;
    const bool activeHigh = BoardConfig::ACTIVE.power.chargeEnableActiveHigh;
    pinMode(ce, OUTPUT);
    digitalWrite(ce, activeHigh ? LOW : HIGH);  // pause charging
    delay(5);
    mv = analogReadMilliVolts(_adcPin);
    digitalWrite(ce, activeHigh ? HIGH : LOW);  // resume charging
  } else {
    mv = analogReadMilliVolts(_adcPin);
  }
#endif

  return static_cast<uint16_t>(mv * _dividerMultiplier);
}

double BatteryMonitor::readVolts() const {
  return static_cast<double>(readMillivolts()) / 1000.0;
}

bool BatteryMonitor::isCharging() const {
#if FREEINK_BATTERY_I2C_GAUGE
  // Gauge boards: prefer a charger IC's status (BQ25896), else fall back to the
  // gauge's own Current() sign, so a board with a gauge but no charger IC (e.g.
  // X3) still reports charging. A gauge that cannot observe charging at all
  // (CW2017) reports unknown — fall through to the STAT pin below (X4 Pro
  // GPIO21). Failed reads report false.
  if (hasGaugeBackend()) {
    bool known = false;
    const bool charging = readGaugeCharging(known);
    if (known) {
      return charging;
    }
  }
#endif
  if (hasM5Pm1Backend()) {
    Status status;
    return readM5Pm1Status(status) && status.chargingKnown && status.charging;
  }
  if (_chargeStatusPin < 0) {
#if FREEINK_BATTERY_I2C_GAUGE
    bool known = false;
    return readChargerCharging(known);
#else
    return false;
#endif
  }
  // STAT at its board-declared active level (default: MCP73832-style /STAT,
  // LOW while charging).
  return digitalRead(_chargeStatusPin) == chargeActiveLevel();
}

bool BatteryMonitor::isExternalPowerPresent(bool* known) const {
  bool observed = false;
#if FREEINK_BATTERY_I2C_GAUGE
  const bool present = readChargerExternalPower(observed);
  if (observed) {
    if (known) *known = true;
    return present;
  }
#endif
  if (hasM5Pm1Backend()) {
    // The M5 PMIC reports its supply source directly (PWR_SRC 5VIN/5VINOUT),
    // which readM5Pm1Status() already decodes into externalPower — the right
    // field here, rather than the `charging` one it derives from it.
    Status status;
    if (readM5Pm1Status(status) && status.externalPowerKnown) {
      if (known) *known = true;
      return status.externalPower;
    }
  }
  // Boards with a dedicated detect pin but no charger IC can still answer: the
  // pin is the input rail's presence signal. Read it with the pull that puts a
  // disconnected pin in the "absent" state, so a floating line cannot report a
  // cable that is not there.
  const int8_t pin = BoardConfig::ACTIVE.usbDetect;
  if (pin >= 0) {
    const bool activeHigh = BoardConfig::ACTIVE.usbDetectActiveHigh;
    pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
    const bool level = digitalRead(pin) == HIGH;
    if (known) *known = true;
    return activeHigh ? level : !level;
  }

  if (known) *known = false;
  return false;
}

bool BatteryMonitor::readM5Pm1Status(Status& status) const {
  status.supported = true;
  if (!hasM5Pm1Backend()) return false;

  freeink::m5pm1::beginBus();

  // VREF/VBAT/VIN/5VOUT are four complete 16-bit little-endian millivolt
  // registers (0x20..0x27). Only ADC_RES at 0x28 is 12-bit. Read the whole
  // block atomically so paired bytes and the three rails share one sample.
  uint8_t rails[8] = {};
  const bool railsKnown = freeink::m5pm1::readBytes(M5PM1_REG_VREF_L, rails, sizeof(rails));
  if (railsKnown) {
    const uint16_t batMv = readLe16(rails + 2);
    const uint16_t vinMv = readLe16(rails + 4);
    const uint16_t vinOutMv = readLe16(rails + 6);
    status.millivoltsKnown = true;
    status.millivolts = batMv;
    status.percentageKnown = true;
    status.percentage = percentageFromMillivolts(batMv);
    status.pm1VinMv = vinMv;
    status.pm1VinOutMv = vinOutMv;
  }

  uint8_t powerSource = 0;
  const bool pwrSrcKnown = freeink::m5pm1::readReg(M5PM1_REG_PWR_SRC, &powerSource);
  if (pwrSrcKnown) {
    const uint8_t sources = powerSource & 0x07;
    status.pm1PowerSource = sources;
    status.externalPowerKnown = true;
    // PM1 manual: PWR_SRC is a bitmap, not the enum used by older M5PM1
    // wrappers. Multiple bits may be set at once (the connected Paper Mono
    // reports 0x05 = BAT | 5VIN). Unlike the ADC rail samples, these validity
    // bits drop when the cable is removed, so they are the authoritative
    // source for the charging badge.
    status.externalPower = (sources & (M5PM1_PWR_SRC_5VIN | M5PM1_PWR_SRC_5VINOUT)) != 0;
  }

  // Product semantics for Paper Mono: external supply present means charging.
  status.chargingKnown = status.externalPowerKnown;
  status.charging = status.externalPower;
  return status.percentageKnown || status.millivoltsKnown || status.externalPowerKnown;
}

// Standard 1S Li-ion / LiPo (4.20 V) rest-voltage discharge curve, one entry per
// 10% notch. The curve is deliberately not resampled any finer: between 20% and
// 60% the whole span is ~130 mV, so a tenth of a volt-step is already below the
// noise of any of the three backends. A caller that needs real resolution should
// read millivolts and show those instead.
//
// The 0% anchor is 3.45 V rather than the cell's protection cut-off. Below that
// the pack falls off a cliff and the remaining runtime is minutes, so reporting
// it as empty is honest; it also leaves headroom for the sag under an e-ink
// refresh, which is the heaviest load this device draws.
constexpr uint16_t LIION_NOTCH_MV[11] = {
    3450,  //   0%
    3680,  //  10%
    3740,  //  20%
    3770,  //  30%
    3790,  //  40%
    3820,  //  50%
    3870,  //  60%
    3920,  //  70%
    3980,  //  80%
    4060,  //  90%
    4200,  // 100%
};

// A notch change has to clear the boundary by this much before it is accepted.
// The 20-40% band is only 50 mV wide, so without a deadband a few millivolts of
// sampler noise would swap the icon back and forth between page turns. Kept
// well under the narrowest half-segment (10 mV) so no notch can become a trap.
constexpr uint16_t NOTCH_HYSTERESIS_MV = 8;

uint16_t BatteryMonitor::percentageFromMillivolts(uint16_t millivolts) {
  // A failed read reports 0 mV, which lands on 0% here. That is deliberate: the
  // cubic this table replaced evaluated to +7501 at 0 V and clamped to a
  // confident 100%, so an I2C or ADC failure showed a full battery.
  if (millivolts >= LIION_NOTCH_MV[10]) return 100;
  // Round at the midpoint of each segment instead of flooring, so a cell resting
  // just below 4.20 V straight off the charger still reads 100%.
  for (uint8_t i = 10; i > 0; --i) {
    const uint16_t boundary = static_cast<uint16_t>((LIION_NOTCH_MV[i - 1] + LIION_NOTCH_MV[i]) / 2);
    if (millivolts >= boundary) return static_cast<uint16_t>(i * 10);
  }
  return 0;
}

uint16_t BatteryMonitor::percentageFromMillivolts(uint16_t millivolts, uint16_t previousPercent) {
  const uint16_t notch = percentageFromMillivolts(millivolts);
  if (previousPercent > 100) return notch;  // no usable history
  const uint16_t previousNotch = static_cast<uint16_t>((previousPercent / 10) * 10);
  if (notch == previousNotch) return notch;

  // Re-run the lookup with the sample pushed back toward the notch we are
  // leaving. If it still crosses, the move is real; if not, hold.
  const int32_t bias = notch > previousNotch ? -NOTCH_HYSTERESIS_MV : NOTCH_HYSTERESIS_MV;
  const int32_t biased = std::clamp<int32_t>(static_cast<int32_t>(millivolts) + bias, 0, UINT16_MAX);
  return percentageFromMillivolts(static_cast<uint16_t>(biased)) == notch ? notch : previousNotch;
}
