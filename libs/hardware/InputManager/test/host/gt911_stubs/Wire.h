#pragma once
// Model GT911 behind a fake Wire, for the INT-wake host test. It holds a
// register file (config block 0x8047..0x80FF, Config_Fresh 0x8100, status
// 0x814E, points 0x8150), consumes a config table on Config_Fresh, latches a
// report when it does, drives the INT pin LOW while a report is pending in
// low-level mode, reloads its stored table on a RST rising edge, and counts every transaction so the test can check
// exactly what the driver wrote. Fault knobs make the bus or individual registers NACK.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Arduino.h"

struct WireStub {
  static constexpr uint16_t CFG = 0x8047, CHK = 0x80FF, FRESH = 0x8100, SWITCH1 = 0x804D, STATUS = 0x814E;
  static constexpr int INT_PIN = 10;  // X4 Pro touch irq
  static constexpr int RST_PIN = 4;   // X4 Pro touch reset

  // --- model state ----------------------------------------------------------
  uint8_t address = 0x5D;
  std::vector<uint8_t> mem = std::vector<uint8_t>(0x10000, 0);
  std::vector<uint8_t> factory;  // 0x8047..0x80FF as the controller loads it on reset
  bool persistent = false;       // controller keeps an applied table across reset
  bool acceptConfig = true;      // clears Config_Fresh (consumes the table)
  bool honorIntMode = true;      // false: consumes the table but keeps its old INT mode
  uint8_t activeMode = 0;        // INT mode the controller is running, not just the register

  // --- fault knobs ----------------------------------------------------------
  bool busDead = false;           // every transfer NACKs
  unsigned failStatusClears = 0;  // next N writes to 0x814E NACK

  // --- counters -------------------------------------------------------------
  unsigned configWriteTxns = 0;  // write transactions touching 0x8047..0x8100
  unsigned applies = 0;          // Config_Fresh writes seen
  unsigned configReads = 0;      // read transactions inside the config block
  unsigned statusReadAttempts = 0;
  unsigned statusClears = 0;  // successful writes of 0 to 0x814E
  unsigned statusClearAttempts = 0;

  // --- Wire surface ---------------------------------------------------------
  uint8_t txAddr = 0;
  std::vector<uint8_t> tx, rx;
  size_t rxPos = 0;
  uint16_t ptr = 0;

  bool begin(int, int, uint32_t) { return true; }
  void setTimeOut(int) {}
  void beginTransmission(uint8_t a) {
    txAddr = a;
    tx.clear();
  }
  size_t write(uint8_t v) {
    tx.push_back(v);
    return 1;
  }
  size_t write(const uint8_t* v, size_t n) {
    tx.insert(tx.end(), v, v + n);
    return n;
  }
  int endTransmission(bool = true) {
    if (tx.size() >= 2) {
      const uint16_t reg = static_cast<uint16_t>(tx[0] << 8 | tx[1]);
      if (tx.size() == 2) {
        if (reg == STATUS) ++statusReadAttempts;
      } else if (reg == STATUS) {
        ++statusClearAttempts;
      }
    }
    if (busDead || txAddr != address) return 2;
    if (tx.empty()) return 0;  // address probe
    const uint16_t reg = static_cast<uint16_t>(tx[0] << 8 | tx[1]);
    ptr = reg;
    if (tx.size() == 2) return 0;  // register pointer for a following read
    if (reg == STATUS && failStatusClears > 0) {
      --failStatusClears;
      return 2;
    }
    const size_t n = tx.size() - 2;
    if (reg <= FRESH && reg + n > CFG) ++configWriteTxns;
    for (size_t i = 0; i < n; ++i) mem[reg + i] = tx[2 + i];
    if (reg == STATUS && n == 1 && tx[2] == 0) ++statusClears;
    if (reg <= FRESH && reg + n > FRESH && mem[FRESH] == 1) onFresh();
    syncInt();
    return 0;
  }
  uint8_t requestFrom(uint8_t a, uint8_t n, uint8_t) {
    rx.clear();
    rxPos = 0;
    if (busDead || a != address) return 0;
    if (ptr >= CFG && ptr <= CHK) ++configReads;
    rx.assign(mem.begin() + ptr, mem.begin() + ptr + n);
    ptr = static_cast<uint16_t>(ptr + n);
    return n;
  }
  int available() { return static_cast<int>(rx.size() - rxPos); }
  uint8_t read() { return rx.at(rxPos++); }

  // --- model helpers --------------------------------------------------------
  static uint8_t checksum(const uint8_t* table) {
    uint8_t sum = 0;
    for (uint16_t i = 0; i < CHK - CFG; ++i) sum = static_cast<uint8_t>(sum + table[i]);
    return static_cast<uint8_t>(-sum);
  }
  // Factory table with the given Module_Switch1 INT mode and a valid checksum.
  void loadFactory(uint8_t intMode, uint8_t version = 0x41) {
    factory.assign(CHK - CFG + 1, 0);
    for (size_t i = 0; i < factory.size(); ++i) factory[i] = static_cast<uint8_t>(i * 7 + 3);
    factory[0] = version;
    factory[SWITCH1 - CFG] = static_cast<uint8_t>(0x30 | intMode);
    factory[CHK - CFG] = checksum(factory.data());
    powerCycle();
  }
  // Controller reset: reloads the stored table, nothing pending.
  void powerCycle() {
    std::copy(factory.begin(), factory.end(), mem.begin() + CFG);
    activeMode = mem[SWITCH1] & 0x03;
    mem[FRESH] = 0;
    mem[STATUS] = 0;
    syncInt();
  }
  void onFresh() {
    ++applies;
    if (!acceptConfig) return;  // Config_Fresh stays set
    if (checksum(&mem[CFG]) != mem[CHK]) return;
    if (!honorIntMode) {
      mem[SWITCH1] = factory[SWITCH1 - CFG];
      mem[CHK] = checksum(&mem[CFG]);
    }
    mem[FRESH] = 0;
    activeMode = mem[SWITCH1] & 0x03;
    if (persistent) factory.assign(mem.begin() + CFG, mem.begin() + CHK + 1);
    mem[STATUS] = 0x80;  // re-reading the table latches a report
  }
  // A refused table leaves the register patched but the running mode as it was.
  uint8_t activeIntMode() const { return activeMode; }
  void report(uint8_t status) {
    mem[STATUS] = status;
    syncInt();
  }
  // Low-level mode holds INT LOW while 0x814E has anything pending.
  void syncInt() { gpioLevels[INT_PIN] = (activeMode == 2 && mem[STATUS] != 0) ? LOW : HIGH; }
  void resetCounters() {
    configWriteTxns = applies = configReads = statusReadAttempts = statusClears = statusClearAttempts = 0;
  }
};

inline WireStub Wire;

// The begin() reset dance raises RST: the controller reloads its stored table.
inline const bool gt911ResetModelInstalled = (gpioRisingHook = [](int pin) {
                                                if (pin == WireStub::RST_PIN) Wire.powerCycle();
                                              },
                                              true);
