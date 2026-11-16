#!/usr/bin/env python3
"""Compile the real UC8253 X3 driver with a recording bus and host Arduino shims."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / "src"
with tempfile.TemporaryDirectory(prefix="uc8253_power-test-") as directory:
    root = Path(directory)
    for folder in ("src/driver", "src/bus", "src/lut", "include"):
        (root / folder).mkdir(parents=True, exist_ok=True)
    for name in ("Uc8253X3Driver.cpp", "Uc8253X3Driver.h", "PanelDriver.h"):
        shutil.copy2(SOURCE / "driver" / name, root / "src/driver" / name)
    shutil.copy2(SOURCE / "FreeInkDisplay.cpp", root / "src/FreeInkDisplay.cpp")
    shutil.copy2(SOURCE / "lut/Uc8253X3Luts.h", root / "src/lut/Uc8253X3Luts.h")
    shutil.copy2(SOURCE / "lut/Uc8253X3Lut16.h", root / "src/lut/Uc8253X3Lut16.h")
    shutil.copy2(SOURCE / "lut/UltraChipDirectGrayLuts.h", root / "src/lut/UltraChipDirectGrayLuts.h")
    for inc_name in ("FreeInkDisplay.h", "GrayscaleCapabilities.h", "Uc8253X3Config.h"):
        shutil.copy2(SOURCE.parent / "include" / inc_name, root / "include" / inc_name)
    (root / "SPI.h").write_text("#pragma once\n")
    (root / "sdkconfig.h").write_text("#pragma once\n")
    (root / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#define HIGH 1
#define LOW 0
#define PROGMEM
#define pgm_read_byte(p) (*reinterpret_cast<const uint8_t*>(p))
#define log_e(...) std::fprintf(stderr, __VA_ARGS__)
inline unsigned long& hostMillisState() { static unsigned long t = 0; return t; }
inline unsigned long millis() { return ++hostMillisState(); }
inline void advanceHostMillis(unsigned long delta) { hostMillisState() += delta; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}
inline int& hostBusyPinLevel() { static int lvl = 0; return lvl; }
inline int digitalRead(int) { return hostBusyPinLevel(); }
''')
    (root / "BoardConfig.h").write_text('''#pragma once
#include <cstdint>
namespace BoardConfig {
enum class Board { XteinkX3, XteinkX3Uc8279 };
enum class DisplayController { UC8253, UC8279 };
inline struct {
  uint16_t displayWidth = 792, displayHeight = 528;
  uint32_t displaySpiHz = 0;
  struct { int8_t sclk = 0, mosi = 0, cs = 0, dc = 0, rst = 0, busy = 0, powerEnable = -1; } display;
  Board board = Board::XteinkX3;
  DisplayController displayController = DisplayController::UC8253;
} ACTIVE;
constexpr uint32_t MAX_FRAMEBUFFER_BYTES = (792 / 8) * 528;
inline void selectDevice(Board b) { ACTIVE.board = b; }
}
''')
    (root / "src/bus/EpdBus.h").write_text('''#pragma once
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <cstring>
namespace freeink {
enum class BusyPolarity { X3TwoPhase, ActiveLowReady };
struct EpdPins { int8_t sclk = 0, mosi = 0, cs = 0, dc = 0, rst = 0, busy = 0, powerEnable = -1; };
class EpdBus {
  uint8_t command=0;
  bool inPartial=false;
  uint16_t ptlXByteStart=0, ptlWinWb=99;
  uint16_t ptlGateStart=0, ptlGateEnd=0;
  uint16_t ptlCurGate=0, ptlCurCol=0;
  std::vector<uint8_t> curParam;
public:
  std::vector<uint8_t> oldPlane, newPlane, lastBank, lastWw, lastBw, lastWb, lastBb, rawRegisters;
  struct RefreshRecord {
    std::array<uint8_t, 2> cdi;
    uint8_t pll;
    uint8_t vcom;
    std::array<uint8_t, 5> pwr;
    uint8_t ww0, bw0, wb0, bb0;
    uint8_t ww6, bw6, wb6, bb6;
    std::vector<uint8_t> oldPlaneDuring;
    std::vector<uint8_t> newPlaneDuring;
  };
  std::vector<RefreshRecord> refreshHistory;
  std::vector<uint8_t> pllWrites;
  std::vector<uint8_t> vcomWrites;
  std::vector<std::array<uint8_t, 5>> pwrWrites;
  std::vector<std::array<uint8_t, 2>> cdiWrites;
  std::vector<uint8_t> pllAtRefresh;
  std::vector<uint8_t> vcomAtRefresh;
  std::vector<std::array<uint8_t, 5>> pwrAtRefresh;
  std::vector<uint8_t> pllAtPowerOff;
  std::vector<uint8_t> vcomAtPowerOff;
  std::vector<std::array<uint8_t, 5>> pwrAtPowerOff;
  uint8_t currentPll=0;
  uint8_t currentVcom=0;
  std::array<uint8_t, 5> currentPwr{};
  std::array<uint8_t, 2> currentCdi{};
  unsigned powerOns=0, powerOffs=0, refreshes=0, pollingRefreshWaits=0, activeLowReadyDrfWaits=0, interruptWaits=0, dataStops=0, partialIns=0, partialOuts=0;
  unsigned ponWaits=0, activeLowReadyPonWaits=0, pofWaits=0, idlePofWaits=0;
  BusyPolarity lastWaitPolarity = BusyPolarity::X3TwoPhase;
  unsigned dtm1FullWrites=0, dtm2FullWrites=0;
  unsigned streamOpens=0, streamChunks=0;
  bool drfActive=false, ponActive=false, streamActive=false;
  uint8_t streamCmd=0;
  size_t streamCursorByte=0;
  void commitParam() {
    if (command == 0x30 && curParam.size() == 1) {
      currentPll = curParam[0];
      pllWrites.push_back(currentPll);
    } else if (command == 0x82 && curParam.size() == 1) {
      currentVcom = curParam[0];
      vcomWrites.push_back(currentVcom);
    } else if (command == 0x01 && curParam.size() == 5) {
      for (size_t i = 0; i < 5; ++i) currentPwr[i] = curParam[i];
      pwrWrites.push_back(currentPwr);
    } else if (command == 0x50 && curParam.size() == 2) {
      currentCdi = {curParam[0], curParam[1]};
      cdiWrites.push_back(currentCdi);
    } else if (command == 0x90 && curParam.size() == 9) {
      const uint16_t xs = static_cast<uint16_t>((curParam[0] << 8) | curParam[1]);
      const uint16_t xe = static_cast<uint16_t>((curParam[2] << 8) | curParam[3]);
      ptlXByteStart = static_cast<uint16_t>(xs >> 3);
      ptlWinWb = static_cast<uint16_t>(((xe - xs) >> 3) + 1u);
      ptlGateStart = static_cast<uint16_t>((curParam[4] << 8) | curParam[5]);
      ptlGateEnd = static_cast<uint16_t>((curParam[6] << 8) | curParam[7]);
      ptlCurGate = ptlGateStart;
      ptlCurCol = 0;
    }
  }
  void cmd(uint8_t c) {
    assert(!drfActive && !ponActive && !streamActive);
    commitParam();
    curParam.clear();
    command = c;
    if (c == 0x02) {
      ++powerOffs;
      pllAtPowerOff.push_back(currentPll);
      vcomAtPowerOff.push_back(currentVcom);
      pwrAtPowerOff.push_back(currentPwr);
    }
    if (c == 0x04) { assert(!inPartial); ++powerOns; ponActive = true; }
    if (c == 0x11) { assert(!inPartial); ++dataStops; }
    if (c == 0x12) {
      drfActive = true;
      ++refreshes;
      pllAtRefresh.push_back(currentPll);
      vcomAtRefresh.push_back(currentVcom);
      pwrAtRefresh.push_back(currentPwr);
      refreshHistory.push_back({
          currentCdi,
          currentPll,
          currentVcom,
          currentPwr,
          lastWw.empty() ? uint8_t(0) : lastWw[0],
          lastBw.empty() ? uint8_t(0) : lastBw[0],
          lastWb.empty() ? uint8_t(0) : lastWb[0],
          lastBb.empty() ? uint8_t(0) : lastBb[0],
          lastWw.size() > 6 ? lastWw[6] : uint8_t(0),
          lastBw.size() > 6 ? lastBw[6] : uint8_t(0),
          lastWb.size() > 6 ? lastWb[6] : uint8_t(0),
          lastBb.size() > 6 ? lastBb[6] : uint8_t(0),
          oldPlane,
          newPlane,
      });
      // UC8253 hardware N2OCP=1 (CDI bit 3 = 0x08): full-frame DRF copies DTM2 -> DTM1 on completion;
      // inside PTIN partial mode, uncovered DTM1 outside PTL is retained.
      if ((currentCdi[0] & 0x08u) != 0 && !inPartial && !newPlane.empty()) {
        oldPlane = newPlane;
      }
    }
    if (c == 0x91) { inPartial = true; ++partialIns; }
    if (c == 0x92) { inPartial = false; ++partialOuts; }
    if ((c == 0x10 || c == 0x13) && inPartial) { ptlCurGate = ptlGateStart; ptlCurCol = 0; }
  }
  void cmdData2(uint8_t c, uint8_t a, uint8_t b) { cmd(c); data(a); data(b); commitParam(); curParam.clear(); }
  void data(uint8_t v) { assert(!drfActive && !ponActive); curParam.push_back(v); }
  void data(const uint8_t* p, size_t n) {
    assert(!drfActive && !ponActive);
    curParam.assign(p, p + n);
    commitParam();
    curParam.clear();
    if(n == 42 || n == 49) {
      if(command == 0x20) rawRegisters.clear();
      rawRegisters.push_back(command);
      if(command == 0x20) lastBank.assign(p,p+n);
      if(command == 0x21) lastWw.assign(p,p+n);
      if(command == 0x22) lastBw.assign(p,p+n);
      if(command == 0x23) lastWb.assign(p,p+n);
      if(command == 0x24) lastBb.assign(p,p+n);
    }
  }
  void cmdData(uint8_t c, const uint8_t* p, size_t n) { cmd(c); data(p,n); }
  void sendPlaneFlipped(uint8_t c, const uint8_t* p, uint16_t h, uint16_t wb) {
    assert(!drfActive && !ponActive);
    if (c == 0x10) ++dtm1FullWrites; else if (c == 0x13) ++dtm2FullWrites;
    auto& dst = (c == 0x10 ? oldPlane : newPlane);
    dst.resize(size_t(h) * wb);
    for (uint16_t row = 0; row < h; ++row) {
      const uint16_t gate = static_cast<uint16_t>(h - 1 - row);
      std::memcpy(dst.data() + size_t(gate) * wb, p + size_t(row) * wb, wb);
    }
  }
  void fillPlane(uint8_t c, uint8_t v, uint16_t h, uint16_t wb) {
    assert(!drfActive && !ponActive && !streamActive);
    (c == 0x10 ? oldPlane : newPlane).assign(size_t(h)*wb,v);
  }
  using StreamChunkCallback = size_t (*)(void* ctx, const uint8_t** outChunk);
  size_t sendStream(uint8_t c, StreamChunkCallback nextChunk, void* ctx) {
    assert(!drfActive && !ponActive && !inPartial && !streamActive);
    if (!nextChunk) return 0;
    cmd(c);
    streamActive = true;
    streamCmd = c;
    streamCursorByte = 0;
    ++streamOpens;
    if (c == 0x10) ++dtm1FullWrites; else if (c == 0x13) ++dtm2FullWrites;
    auto& dst = (c == 0x10 ? oldPlane : newPlane);
    if (dst.size() != 99u * 528u) dst.assign(99u * 528u, 0);
    for (;;) {
      const uint8_t* p = nullptr;
      const size_t n = nextChunk(ctx, &p);
      if (n == 0 || !p) break;
      assert(streamActive && !drfActive && !ponActive && !inPartial);
      if (streamCursorByte < dst.size()) {
        const size_t copyLen = (n <= dst.size() - streamCursorByte) ? n : (dst.size() - streamCursorByte);
        std::memcpy(dst.data() + streamCursorByte, p, copyLen);
      }
      streamCursorByte += n;
      ++streamChunks;
    }
    streamActive = false;
    streamCmd = 0;
    return streamCursorByte;
  }
  void reset(int) { drfActive = false; ponActive = false; streamActive = false; }
  void begin(const EpdPins&, uint32_t, BusyPolarity, int8_t = -1, int8_t = -1) {}
  bool isBusy() const { return false; }
  void setBusyWaitHooks(void (*)(), void (*)()) {}
  void setBusyWaitSliceHook(bool (*)(int8_t, uint8_t)) {}
  void waitBusy(const char* tag) { waitBusy(BusyPolarity::X3TwoPhase, tag); }
  void waitBusy(BusyPolarity pol, const char* tag) {
    lastWaitPolarity = pol;
    if (tag && std::strstr(tag, "DRF")) drfActive = false;
    if (tag && !std::strcmp(tag, " X3_PON")) {
      ponActive = false;
      ++ponWaits;
      if (pol == BusyPolarity::ActiveLowReady) ++activeLowReadyPonWaits;
    }
    if (tag && !std::strcmp(tag, " X3_POF")) ++pofWaits;
    if (tag && !std::strcmp(tag, " X3_IDLE_POF")) ++idlePofWaits;
    if (tag && !std::strcmp(tag, " X3_DRF")) {
      ++pollingRefreshWaits;
      if (pol == BusyPolarity::ActiveLowReady) ++activeLowReadyDrfWaits;
    }
  }
  void waitRefreshComplete(const char*) { drfActive = false; ++interruptWaits; }
  EpdPins pins() const { return {}; }
  struct Transaction {
    EpdBus* owner;
    void writeBytes(const uint8_t* p, size_t n) {
      assert(owner && !owner->drfActive && !owner->ponActive);
      if (!owner || !owner->inPartial) return;
      auto& dst = (owner->command == 0x10 ? owner->oldPlane : owner->newPlane);
      for (size_t i = 0; i < n; ++i) {
        if (owner->ptlCurGate <= owner->ptlGateEnd && owner->ptlWinWb > 0) {
          const size_t idx = static_cast<size_t>(owner->ptlCurGate) * 99u + owner->ptlXByteStart + owner->ptlCurCol;
          if (idx < dst.size()) dst[idx] = p[i];
          if (++owner->ptlCurCol >= owner->ptlWinWb) {
            owner->ptlCurCol = 0;
            ++owner->ptlCurGate;
          }
        }
      }
    }
    void end() {}
  };
  Transaction beginTxn() { assert(!drfActive && !ponActive); return {this}; }
};
}
''')
    (root / "test.cpp").write_text("""
#include <cassert>
#include <vector>
#include <iostream>
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Uc8253X3Driver.h"
#undef private
#include "src/lut/Uc8253X3Lut16.h"
static std::vector<uint8_t> gateFlipped(const std::vector<uint8_t>& logical, size_t h = 528, size_t wb = 99) {
  std::vector<uint8_t> out(h * wb);
  for (size_t r = 0; r < h; ++r) {
    std::memcpy(out.data() + (h - 1 - r) * wb, logical.data() + r * wb, wb);
  }
  return out;
}
int main() {
 freeink::EpdBus bus; freeink::Uc8253X3Driver d;
 static_assert(freeink::Uc8253X3Driver::kIdlePowerOffMs == 1200u);
 static_assert(freeink::x3_lut16::kVcomDcStandard == 0x24u);
 static_assert(freeink::x3_lut16::kVcomDcIdle == 0x00u);
 assert(d.supportsGray16());
 assert(d.spiHz() == 16000000u);
 const auto& defCfg = freeink::uc8253X3DefaultConfig();
 assert(defCfg.gc.bw[0] == 0x80 && defCfg.gc.bw[1] == 0x03 && defCfg.gc.bw[2] == 0x02 && defCfg.gc.bw[3] == 0x00);
 assert(defCfg.lut16.pllBase == 0x09 && defCfg.lut16.pllPass1 == 0x0B && defCfg.lut16.pllPass2 == 0x0E &&
        defCfg.lut16.vdlPass2 == 0x1E && !defCfg.lut16.step0Enable &&
        freeink::x3_lut16::kPllStandard == 0x09);
 assert(defCfg.lut16.step0Erase.vcom == freeink::x3_lut16::kLut16Step0Erase_vcom &&
        defCfg.lut16.step0Erase.ww == freeink::x3_lut16::kLut16Step0Erase_ww &&
        defCfg.lut16.step0Erase.wb == freeink::x3_lut16::kLut16Step0Erase_wb &&
        defCfg.lut16.step0Erase.bw == freeink::x3_lut16::kLut16Step0Erase_bw &&
        defCfg.lut16.step0Erase.bb == freeink::x3_lut16::kLut16Step0Erase_bb);
 assert(defCfg.lut16.preBwMid.ww == freeink::x3_lut16::kLut16CrossDissolve_ww &&
        +freeink::x3_lut16::kLut16PreBwMid_ww == +freeink::x3_lut16::kLut16CrossDissolve_ww);
 // Verify 42-byte tables for Step 0 erase (24f + 4f = 28f) and cross-dissolve (17f + 19f = 36f):
 constexpr std::array<uint8_t, 42> kExpStep0Vcom = {0x00, 0x02, 0x06, 0x08, 0x08, 0x01, 0x00, 0x03, 0x01, 0x00, 0x00, 0x01};
 constexpr std::array<uint8_t, 42> kExpStep0Ww   = {0x00, 0x02, 0x06, 0x08, 0x08, 0x01, 0x00, 0x03, 0x01, 0x00, 0x00, 0x01};
 constexpr std::array<uint8_t, 42> kExpStep0Wb   = {0x2A, 0x02, 0x06, 0x08, 0x08, 0x01, 0x80, 0x03, 0x01, 0x00, 0x00, 0x01};
 constexpr std::array<uint8_t, 42> kExpStep0Bw   = {0xAA, 0x02, 0x06, 0x08, 0x08, 0x01, 0x80, 0x03, 0x01, 0x00, 0x00, 0x01};
 constexpr std::array<uint8_t, 42> kExpCrossVcom = {0x00, 0x01, 0x03, 0x06, 0x07, 0x01, 0x00, 0x0E, 0x01, 0x03, 0x01, 0x01};
 constexpr std::array<uint8_t, 42> kExpCrossWw   = {0x00, 0x01, 0x03, 0x06, 0x07, 0x01, 0x20, 0x0E, 0x01, 0x03, 0x01, 0x01};
 constexpr std::array<uint8_t, 42> kExpCrossBw   = {0x08, 0x01, 0x03, 0x06, 0x07, 0x01, 0x80, 0x0E, 0x01, 0x03, 0x01, 0x01};
 constexpr std::array<uint8_t, 42> kExpCrossWb   = {0x54, 0x01, 0x03, 0x06, 0x07, 0x01, 0x54, 0x0E, 0x01, 0x03, 0x01, 0x01};
 constexpr std::array<uint8_t, 42> kExpCrossBb   = {0x80, 0x01, 0x03, 0x06, 0x07, 0x01, 0x54, 0x0E, 0x01, 0x03, 0x01, 0x01};
 assert(std::memcmp(freeink::x3_lut16::kLut16Step0Erase_vcom, kExpStep0Vcom.data(), 42) == 0);
 assert(std::memcmp(freeink::x3_lut16::kLut16Step0Erase_ww, kExpStep0Ww.data(), 42) == 0);
 assert(std::memcmp(freeink::x3_lut16::kLut16Step0Erase_wb, kExpStep0Wb.data(), 42) == 0);
 assert(std::memcmp(freeink::x3_lut16::kLut16Step0Erase_bw, kExpStep0Bw.data(), 42) == 0);
 assert(std::memcmp(freeink::x3_lut16::kLut16Step0Erase_bb, kExpStep0Bw.data(), 42) == 0);
 assert(freeink::x3_lut16::kLut16Step0Erase_ww[1] + freeink::x3_lut16::kLut16Step0Erase_ww[2] +
            freeink::x3_lut16::kLut16Step0Erase_ww[3] + freeink::x3_lut16::kLut16Step0Erase_ww[4] == 24);
 assert(freeink::x3_lut16::kLut16Step0Erase_ww[7] + freeink::x3_lut16::kLut16Step0Erase_ww[8] +
            freeink::x3_lut16::kLut16Step0Erase_ww[9] + freeink::x3_lut16::kLut16Step0Erase_ww[10] == 4);
 assert(std::memcmp(defCfg.lut16.preBwMid.vcom, kExpCrossVcom.data(), 42) == 0);
 assert(std::memcmp(defCfg.lut16.preBwMid.ww, kExpCrossWw.data(), 42) == 0);
 assert(std::memcmp(defCfg.lut16.preBwMid.bw, kExpCrossBw.data(), 42) == 0);
 assert(std::memcmp(defCfg.lut16.preBwMid.wb, kExpCrossWb.data(), 42) == 0);
 assert(std::memcmp(defCfg.lut16.preBwMid.bb, kExpCrossBb.data(), 42) == 0);
 assert(defCfg.lut16.preBwMid.ww[1] + defCfg.lut16.preBwMid.ww[2] +
            defCfg.lut16.preBwMid.ww[3] + defCfg.lut16.preBwMid.ww[4] == 17);
 assert(defCfg.lut16.preBwMid.ww[7] + defCfg.lut16.preBwMid.ww[8] +
            defCfg.lut16.preBwMid.ww[9] + defCfg.lut16.preBwMid.ww[10] == 19);
 const auto caps = d.grayscaleCapabilities();
 assert(caps.supported() && caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
 assert(d.grayscaleCapabilities(freeink::GrayscaleMode::Direct).base == freeink::GrayscaleBase::Combined);
 std::vector<uint8_t> fb(792/8*528, 0xAA);
 for (size_t r = 0; r < 528; ++r) fb[r * 99] = static_cast<uint8_t>(r & 0xFF);
 const auto fbGates = gateFlipped(fb);
 d.begin(bus);
 // Rollback: both splash and first real screen clean; only the third paint is fast.
 assert(!d.supportsAsyncDisplay());
 assert(!d.displayStart(bus,fb.data(),nullptr,freeink::RefreshMode::Fast,false));
 assert(bus.refreshes == 2 && bus.pollingRefreshWaits == 2);
 assert(bus.oldPlane == fbGates && bus.newPlane == fbGates);
 d.displayFinish(bus,fb.data());
 assert(bus.refreshes == 2);
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Fast,false);
 assert(bus.refreshes == 5 && bus.pollingRefreshWaits == 5);
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Fast,false);
 assert(bus.refreshes == 6 && bus.pollingRefreshWaits == 6 && bus.interruptWaits == 0);
 assert(bus.powerOns == 1);
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Full,false);
 assert(bus.powerOns == 1);
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Half,true);
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Fast,false);
 assert(bus.powerOns == 2);
 for (auto fallback : {freeink::RefreshMode::Half, freeink::RefreshMode::Fast}) {
   const auto before = bus.refreshes;
   d.beginGrayscale(bus, fb.data(), freeink::GrayscaleMode::Direct, fallback, false);
   d.copyGrayscaleLsb(bus, fb.data());
   d.copyGrayscaleMsb(bus, fb.data());
   assert(bus.refreshes == before);
   d.displayGray(bus, fb.data(), false, nullptr, true);
   assert(bus.refreshes == before + 1);
   const auto vcom = freeink::uc8253X3DefaultConfig().directGray->vcom;
   assert(bus.lastBank == std::vector<uint8_t>(vcom, vcom + 42));
   d.cleanupGrayscaleBuffers(bus, fb.data());
   d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Full,false);
   assert(bus.newPlane == fbGates && bus.oldPlane == fbGates);
 }
 // Rollback: restored B/W RAM permits OEM 4-gray lut_x3_*_aa_pre_bw_mid base (NOT 16-gray CrossDissolve/BipolarScrub!)
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Fast,false);
 d.copyGrayscaleLsb(bus, fb.data());
 d.copyGrayscaleMsb(bus, fb.data());
 d.displayGray(bus, fb.data(), false, nullptr, false);
 d.cleanupGrayscaleBuffers(bus, fb.data());
 const auto before = bus.refreshes;
 d.beginGrayscale(bus, fb.data(), freeink::GrayscaleMode::Absolute, freeink::RefreshMode::Half, false);
 assert(bus.refreshes == before + 1 && bus.interruptWaits == 0);
 assert(bus.currentPll == freeink::x3_lut16::kPllStandard);
 assert(bus.lastWw[0] == 0x20 && bus.lastBb[0] == 0x10);
 d.cleanupGrayscaleBuffers(bus, fb.data());

 // Verify 4-gray beginGrayscale(Overlay, Fast) and (Overlay, Half) both load OEM lut_x3_*_aa_pre_bw_mid
 // (ww[0] == 0x20, bb[0] == 0x10), never kLut16CrossDissolve (ww[0]==0x00) or kLut16BipolarScrub (ww[0]==0x48):
 {
   const unsigned before4gFast = bus.refreshes;
   d.beginGrayscale(bus, fb.data(), freeink::GrayscaleMode::Overlay, freeink::RefreshMode::Fast, false);
   assert(bus.refreshes == before4gFast + 1);
   assert((bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
   assert(bus.refreshHistory.back().ww0 == 0x20 && bus.refreshHistory.back().bb0 == 0x10);
   d.cleanupGrayscaleBuffers(bus, fb.data());

   const unsigned before4gHalf = bus.refreshes;
   d.beginGrayscale(bus, fb.data(), freeink::GrayscaleMode::Overlay, freeink::RefreshMode::Half, false);
   assert(bus.refreshes == before4gHalf + 1);
   assert((bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
   assert(bus.refreshHistory.back().ww0 == 0x20 && bus.refreshHistory.back().bb0 == 0x10);
   d.cleanupGrayscaleBuffers(bus, fb.data());

   // Also verify HalDisplay's 4-gray HALF_REFRESH requestResync(1) + beginGrayscale(Overlay, Half) runs full sync + preBwMid:
   d.requestResync(1);
   const unsigned before4gResyncHalf = bus.refreshes;
   d.beginGrayscale(bus, fb.data(), freeink::GrayscaleMode::Overlay, freeink::RefreshMode::Half, false);
   assert(bus.refreshes == before4gResyncHalf + 4);  // _full + _normal(1) + _fast + _preBwMid
   assert(bus.refreshHistory.back().ww0 == 0x20 && bus.refreshHistory.back().bb0 == 0x10);
   d.cleanupGrayscaleBuffers(bus, fb.data());
 }

 // =========================================================================
 // Two-Pass 16-Gray and Single-Pass Cross-Dissolve Base
 // =========================================================================
 constexpr std::array<uint8_t, 5> kStdPwr = {0x07, 0x17, 0x3F, 0x3F, 0x17};
 constexpr std::array<uint8_t, 5> kFinePwr = {0x07, 0x17, 0x3F, 0x1E, 0x17};
 std::vector<uint8_t> fb2(792 / 8 * 528, 0x55);
 for (size_t r = 0; r < 528; ++r) fb2[r * 99] = static_cast<uint8_t>((r * 3 + 7) & 0xFF);
 const auto fb2Gates = gateFlipped(fb2);
 std::vector<uint8_t> cLsb(99 * 528, 0x11), cMsb(99 * 528, 0x22);
 std::vector<uint8_t> fLsb(99 * 528, 0x33), fMsb(99 * 528, 0x44);
 std::vector<uint8_t> cleanupDtm1(99 * 528, 0xFF);
 for (size_t r = 0; r < 528; ++r) {
   cLsb[r * 99 + 1] = static_cast<uint8_t>(r);
   fMsb[r * 99 + 2] = static_cast<uint8_t>(r ^ 0x55);
   cleanupDtm1[r * 99 + 3] = static_cast<uint8_t>(~(r & 0xFF));  // ink 1..15 + status bar 0 bits
 }
 auto streamStrips = [&](freeink::GrayPlane plane, const std::vector<uint8_t>& planeImg) {
   for (uint16_t y = 0; y < 528; y += 80) {
     const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
     d.writeGrayscalePlaneStrip(bus, plane, planeImg.data() + size_t(y) * 99, y, rows);
   }
 };

 // Page 1: entering 2-pass 16-gray from synced 1-bit B/W state (_gray16CleanupActive == false)
 // executes 1-Pass Cross-Dissolve (@ PLL = 0x09) and restores idle PLL == 0x09:
 assert(!d.isGray16CleanupActive());
 const unsigned refBeforeP1 = bus.refreshes;
 const unsigned dtm1BeforeP1 = bus.dtm1FullWrites;
 const unsigned dtm2BeforeP1 = bus.dtm2FullWrites;
 d.displayGray16Base(bus, fb.data(), freeink::RefreshMode::Fast, false);
 assert(bus.refreshes == refBeforeP1 + 1);
 assert(bus.dtm1FullWrites == dtm1BeforeP1);      // 0 software DTM1 uploads (hardware N2OCP=1!)
 assert(bus.dtm2FullWrites == dtm2BeforeP1 + 1);  // 1 DTM2 upload
 const auto& p1Cross = bus.refreshHistory.back();
 assert((p1Cross.cdi == std::array<uint8_t, 2>{0xA9, 0x07}) && p1Cross.pll == freeink::x3_lut16::kPllBase &&
        p1Cross.pwr == kStdPwr);
 assert(p1Cross.ww0 == 0x00 && p1Cross.bw0 == 0x08 && p1Cross.wb0 == 0x54 && p1Cross.bb0 == 0x80);
 assert(p1Cross.ww6 == 0x20 && p1Cross.bw6 == 0x80 && p1Cross.wb6 == 0x54 && p1Cross.bb6 == 0x54);
 assert(bus.currentPll == freeink::x3_lut16::kPllStandard);  // 0x09 restored right after cross-dissolve!
 assert(bus.oldPlane == fbGates && bus.newPlane == fbGates);

 // Step 2: Pass 1 Coarse (PLL=0x0B, CDI 0xA9/0x07 BDZ=1 Hi-Z border, kLut16Pass1Coarse -> restore 0x09 + 0x24)
 streamStrips(freeink::GrayPlane::Lsb, cLsb);
 streamStrips(freeink::GrayPlane::Msb, cMsb);
 assert(bus.oldPlane == gateFlipped(cLsb) && bus.newPlane == gateFlipped(cMsb));
 d.displayGray16Pass1(bus);
 assert(bus.refreshes == refBeforeP1 + 2);
 assert(bus.pllAtRefresh.back() == freeink::x3_lut16::kPllPass1Coarse);
 assert(bus.vcomAtRefresh.back() == freeink::x3_lut16::kVcomDcStandard);
 assert(bus.pwrAtRefresh.back() == kStdPwr);
 assert((bus.cdiWrites.back() == std::array<uint8_t, 2>{0xA9, 0x07}));
 assert(bus.lastWw == std::vector<uint8_t>(freeink::x3_lut16::kLut16Pass1Coarse_ww,
                                           freeink::x3_lut16::kLut16Pass1Coarse_ww + 42));
 assert(bus.currentPll == freeink::x3_lut16::kPllStandard);
 assert(bus.currentVcom == freeink::x3_lut16::kVcomDcStandard);

 // Early-abort of Pass 2 (before LSB strips are written) must leave PLL == 0x09 (kPllStandard):
 d.displayGray16Pass2(bus, false);
 assert(bus.refreshes == refBeforeP1 + 2);
 assert(bus.currentPll == freeink::x3_lut16::kPllStandard);

 // Step 3: Pass 2 Fine (PLL=0x0E, VDL=0x1E, VCOM=0x22, CDI 0xA9/0x07 -> restore 0x09 + kPwrStandard + idle VCOM 0x00)
 streamStrips(freeink::GrayPlane::Lsb, fLsb);
 streamStrips(freeink::GrayPlane::Msb, fMsb);
 assert(bus.oldPlane == gateFlipped(fLsb) && bus.newPlane == gateFlipped(fMsb));
 d.displayGray16Pass2(bus, false);
 assert(bus.refreshes == refBeforeP1 + 3);
 assert(bus.pllAtRefresh.back() == freeink::x3_lut16::kPllPass2Fine);
 assert(bus.vcomAtRefresh.back() == freeink::x3_lut16::kVcomPass2Fine);
 assert(bus.pwrAtRefresh.back() == kFinePwr);
 assert((bus.cdiWrites.back() == std::array<uint8_t, 2>{0xA9, 0x07}));
 assert(bus.lastWw == std::vector<uint8_t>(freeink::x3_lut16::kLut16Pass2Fine_ww,
                                           freeink::x3_lut16::kLut16Pass2Fine_ww + 42));
 assert(bus.currentPll == freeink::x3_lut16::kPllStandard);
 assert(bus.currentPwr == kStdPwr);
 assert(bus.currentVcom == freeink::x3_lut16::kVcomDcIdle);  // Parked at 0x00 during warm PON hold

 // Step 4: Post-pass DTM1+DTM2 cleanup (restores DTM2 from Pass 2 fine MSB to BW while HV rails stay ON)
 const unsigned refBeforeCleanup = bus.refreshes;
 const unsigned dtm2BeforeCleanup = bus.dtm2FullWrites;
 d.beginGray16Cleanup(bus, fb.data());
 assert(bus.dtm2FullWrites == dtm2BeforeCleanup + 1);
 assert(bus.newPlane == fbGates);
 streamStrips(freeink::GrayPlane::Lsb, cleanupDtm1);
 d.finishGray16Cleanup(bus);
 assert(bus.refreshes == refBeforeCleanup);  // ZERO BUSY refreshes during cleanup!
 assert(bus.oldPlane == gateFlipped(cleanupDtm1));
 assert(d.isGray16CleanupActive());

 // Guard: calling preconditionGrayscale directly while _gray16CleanupActive == true (DTM1 != DTM2)
 // MUST be a no-op:
 d.preconditionGrayscale(bus, 0, 0, 792, 528);
 assert(bus.refreshes == refBeforeCleanup);
 assert(d.isGray16CleanupActive());

 // Page 2: displayGray16Base(Fast) with _gray16CleanupActive == true & step0Enable == false MUST:
 //   1) Upload fb2 to CMD_DTM2 + CMD_DATA_STOP while retaining Page 1's cleanupDtm1 in DTM1
 //   2) Run 1-Pass kLut16CrossDissolve (CDI 0xA9, PLL = 0x09) in a single DRF
 //   3) Restore idle PLL == 0x09 and copy DTM2 -> DTM1 in hardware via N2OCP=1
 const unsigned dtm1BeforeP2 = bus.dtm1FullWrites;
 const unsigned dtm2BeforeP2 = bus.dtm2FullWrites;
 const unsigned dataStopsBeforeP2 = bus.dataStops;
 d.displayGray16Base(bus, fb2.data(), freeink::RefreshMode::Fast, false);
 assert(!d.isGray16CleanupActive());
 assert(bus.refreshes == refBeforeCleanup + 1);
 assert(bus.dtm1FullWrites == dtm1BeforeP2);
 assert(bus.dtm2FullWrites == dtm2BeforeP2 + 1);
 assert(bus.dataStops == dataStopsBeforeP2 + 1);
 const auto& p2Cross = bus.refreshHistory.back();
 assert(p2Cross.oldPlaneDuring == gateFlipped(cleanupDtm1));  // Page 1 CleanupDTM1 intact during Cross-Dissolve!
 assert(p2Cross.newPlaneDuring == fb2Gates);                  // Page 2 Gray16BW uploaded before Cross-Dissolve!
 assert((p2Cross.cdi == std::array<uint8_t, 2>{0xA9, 0x07}) && p2Cross.pll == freeink::x3_lut16::kPllBase &&
        p2Cross.pwr == kStdPwr);
 assert(p2Cross.ww0 == 0x00 && p2Cross.bw0 == 0x08 && p2Cross.wb0 == 0x54 && p2Cross.bb0 == 0x80);
 assert(p2Cross.ww6 == 0x20 && p2Cross.bw6 == 0x80 && p2Cross.wb6 == 0x54 && p2Cross.bb6 == 0x54);
 assert(bus.currentPll == freeink::x3_lut16::kPllStandard);
 assert(bus.oldPlane == fb2Gates && bus.newPlane == fb2Gates);

 // Re-arm cleanup and verify Reader Toolbar open (display Fast @ 0x09) + close (displayGray16Base Fast @ 0x09)
 streamStrips(freeink::GrayPlane::Lsb, cLsb);
 streamStrips(freeink::GrayPlane::Msb, cMsb);
 d.displayGray16Pass1(bus);
 streamStrips(freeink::GrayPlane::Lsb, fLsb);
 streamStrips(freeink::GrayPlane::Msb, fMsb);
 d.displayGray16Pass2(bus, false);
 d.beginGray16Cleanup(bus, fb2.data());
 d.copyGrayscaleLsb(bus, cleanupDtm1.data());
 d.finishGray16Cleanup(bus);
 assert(d.isGray16CleanupActive());

 const unsigned refBeforePopup = bus.refreshes;
 d.display(bus, fb.data(), nullptr, freeink::RefreshMode::Fast, false);
 assert(bus.refreshes == refBeforePopup + 1);  // Single fast CrossDissolve @ 0x09 (CDI 0xA9), NO grayscaleRevert!
 assert((bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
 assert(bus.refreshHistory.back().ww0 == 0x00 && bus.refreshHistory.back().bw0 == 0x08 &&
        bus.refreshHistory.back().wb0 == 0x54 && bus.refreshHistory.back().bb0 == 0x80);
 assert(bus.refreshHistory.back().pll == freeink::x3_lut16::kPllStandard);
 assert(!d.isGray16CleanupActive());
 // Closing Toolbar back to 16-gray page runs 1-pass cross-dissolve (diffing popup fb -> fb2):
 const unsigned refBeforeToolbarClose = bus.refreshes;
 d.displayGray16Base(bus, fb2.data(), freeink::RefreshMode::Fast, /*turnOff=*/true);
 assert(bus.refreshes == refBeforeToolbarClose + 1);
 assert(bus.refreshHistory.back().oldPlaneDuring == fbGates);
 assert(bus.refreshHistory.back().newPlaneDuring == fb2Gates);
 assert(bus.pllAtPowerOff.back() == freeink::x3_lut16::kPllStandard);  // 0x09 restored BEFORE POF!
 d.display(bus, fb2.data(), nullptr, freeink::RefreshMode::Fast, false);

 // Verify grayscaleRevert (_inGrayscaleMode == true without cleanup) scrubs to 0xFF white @ 0x09
 // and runs Cross-Dissolve @ kPllBase (2 refreshes total):
 streamStrips(freeink::GrayPlane::Lsb, cLsb);
 streamStrips(freeink::GrayPlane::Msb, cMsb);
 d.displayGray16Pass1(bus);
 const unsigned refBeforeRevertBase = bus.refreshes;
 d.displayGray16Base(bus, fb.data(), freeink::RefreshMode::Fast, false);
 assert(bus.refreshes == refBeforeRevertBase + 2);
 assert(bus.refreshHistory[bus.refreshHistory.size() - 2].pll == freeink::x3_lut16::kPllStandard);
 assert((bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
 assert(bus.refreshHistory.back().pll == freeink::x3_lut16::kPllBase);
 assert(bus.refreshHistory.back().ww0 == 0x00 && bus.refreshHistory.back().wb0 == 0x54);
 assert(bus.currentPll == freeink::x3_lut16::kPllStandard);

 // Verify step0Enable == true with null step0Erase safely skips Step 0 (1 refresh, 0 crashes),
 // while default populated step0Erase runs Step 0 @ 0x09 + Step 1 @ pllBase (0x0B) and restores idle 0x09:
 freeink::Uc8253X3Config withStep0Cfg = freeink::uc8253X3DefaultConfig();
 withStep0Cfg.lut16.step0Enable = true;
 withStep0Cfg.lut16.pllBase = 0x0B;
 {
   freeink::Uc8253X3Config nullStep0Cfg = withStep0Cfg;
   nullStep0Cfg.lut16.step0Erase = {};
   freeink::Uc8253X3Driver nullStep0Driver(nullStep0Cfg);
   nullStep0Driver.begin(bus);
   nullStep0Driver.skipInitialResync();
   const unsigned refBeforeNullStep0 = bus.refreshes;
   nullStep0Driver.displayGray16Base(bus, fb2.data(), freeink::RefreshMode::Fast, false);
   assert(bus.refreshes == refBeforeNullStep0 + 1);
 }
 freeink::Uc8253X3Driver withStep0Driver(withStep0Cfg);
 withStep0Driver.begin(bus);
 withStep0Driver.skipInitialResync();
 const unsigned dtm2BeforeStep0Cleanup = bus.dtm2FullWrites;
 withStep0Driver.beginGray16Cleanup(bus, fb.data());
 assert(bus.dtm2FullWrites == dtm2BeforeStep0Cleanup + 1);
 withStep0Driver.copyGrayscaleLsb(bus, cleanupDtm1.data());
 withStep0Driver.finishGray16Cleanup(bus);
 const unsigned refBeforeWithStep0 = bus.refreshes;
 withStep0Driver.displayGray16Base(bus, fb2.data(), freeink::RefreshMode::Fast, /*turnOff=*/true);
 assert(bus.refreshes == refBeforeWithStep0 + 2);
 assert((bus.refreshHistory[bus.refreshHistory.size() - 2].cdi == std::array<uint8_t, 2>{0x29, 0x07}));
 assert(bus.refreshHistory[bus.refreshHistory.size() - 2].pll == freeink::x3_lut16::kPllStandard);
 assert((bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
 assert(bus.refreshHistory.back().pll == 0x0B);
 assert(bus.pllAtPowerOff.back() == freeink::x3_lut16::kPllStandard);
 assert(bus.currentPll == freeink::x3_lut16::kPllStandard);

 // Re-arm cleanup and verify HALF_REFRESH runs 1-DRF bipolarScrub (CDI 0xA9, ww0=0x48) + preconditionGrayscale:
 d.beginGray16Cleanup(bus, fb.data());
 streamStrips(freeink::GrayPlane::Lsb, cleanupDtm1);
 d.finishGray16Cleanup(bus);
 assert(d.isGray16CleanupActive());
 d.display(bus, fb.data(), nullptr, freeink::RefreshMode::Half, false);
 assert((bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
 assert(bus.refreshHistory.back().ww0 == 0x48 && bus.refreshHistory.back().bw0 == 0x48);
 d.preconditionGrayscale(bus, 0, 0, 792, 528);
 assert(bus.refreshHistory.back().pll == freeink::x3_lut16::kPllStandard);
 assert(!d.isGray16CleanupActive());

 // Verify displayGray16Pass2(turnOff=true) restores PLL (0x09) + kPwrStandard BEFORE CMD_POWER_OFF
 // and forces CMD_POWER_ON on the next display refresh:
 streamStrips(freeink::GrayPlane::Lsb, cLsb);
 streamStrips(freeink::GrayPlane::Msb, cMsb);
 d.displayGray16Pass1(bus);
 streamStrips(freeink::GrayPlane::Lsb, fLsb);
 streamStrips(freeink::GrayPlane::Msb, fMsb);
 const unsigned pwrOnsBeforeTurnOff = bus.powerOns;
 const unsigned pwrOffsBeforeTurnOff = bus.powerOffs;
 d.displayGray16Pass2(bus, /*turnOff=*/true);
 assert(bus.powerOffs == pwrOffsBeforeTurnOff + 1);
 assert(bus.pllAtPowerOff.back() == freeink::x3_lut16::kPllStandard);
 assert(bus.pwrAtPowerOff.back() == kStdPwr);
 assert(bus.currentPll == freeink::x3_lut16::kPllStandard);
 assert(bus.currentPwr == kStdPwr);
 d.beginGray16Cleanup(bus, fb.data());
 streamStrips(freeink::GrayPlane::Lsb, cleanupDtm1);
 d.finishGray16Cleanup(bus);
 d.displayGray16Base(bus, fb.data(), freeink::RefreshMode::Fast, false);
 assert(bus.powerOns == pwrOnsBeforeTurnOff + 1);

 // Verify register bitmasks clamp high bits in Uc8253X3Config::lut16:
 freeink::Uc8253X3Config maskedCfg = freeink::uc8253X3DefaultConfig();
 maskedCfg.lut16.pllPass1 = 0xFA;
 maskedCfg.lut16.pllPass2 = 0xFF;
 maskedCfg.lut16.vdlPass2 = 0xDE;  // high bits masked -> 0x1E
 maskedCfg.lut16.vcomPass1 = 0xAC;  // bit 7 masked -> 0x2C
 maskedCfg.lut16.vcomPass2 = 0xC8;  // bit 7 masked -> 0x48 (bit 6 preserved!)
 freeink::Uc8253X3Driver maskedDriver(maskedCfg);
 maskedDriver.begin(bus);
 assert(bus.currentVcom == freeink::x3_lut16::kVcomDcStandard);
 maskedDriver.skipInitialResync();
 maskedDriver.displayGray16Base(bus, fb.data(), freeink::RefreshMode::Fast, false);
 assert(bus.vcomAtRefresh.back() == freeink::x3_lut16::kVcomDcStandard);
 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   maskedDriver.writeGrayscalePlaneStrip(bus, freeink::GrayPlane::Lsb, cLsb.data() + size_t(y) * 99, y, rows);
   maskedDriver.writeGrayscalePlaneStrip(bus, freeink::GrayPlane::Msb, cMsb.data() + size_t(y) * 99, y, rows);
 }
 assert(maskedDriver.displayGray16Pass1Start(bus));
 assert(bus.drfActive && bus.vcomAtRefresh.back() == 0x2C && bus.currentVcom == 0x2C);
 maskedDriver.displayFinish(bus, nullptr);
 assert(!bus.drfActive && bus.currentVcom == freeink::x3_lut16::kVcomDcStandard);
 assert(bus.pllAtRefresh.back() == 0x0A);
 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   maskedDriver.writeGrayscalePlaneStrip(bus, freeink::GrayPlane::Lsb, fLsb.data() + size_t(y) * 99, y, rows);
   maskedDriver.writeGrayscalePlaneStrip(bus, freeink::GrayPlane::Msb, fMsb.data() + size_t(y) * 99, y, rows);
 }
 assert(maskedDriver.displayGray16Pass2Start(bus, /*turnOff=*/true));
 assert(bus.drfActive && bus.pllAtRefresh.back() == 0x0F && bus.vcomAtRefresh.back() == 0x48);
 assert(bus.pwrAtRefresh.back() == kFinePwr);
 maskedDriver.displayFinish(bus, nullptr);
 assert(!bus.drfActive && bus.vcomAtPowerOff.back() == freeink::x3_lut16::kVcomDcStandard);
 assert(bus.currentVcom == freeink::x3_lut16::kVcomDcStandard && bus.currentPwr == kStdPwr);

 // Verify Uc8253LutBank / Uc8253Lut16Config defaults and null-bank guard:
 const freeink::Uc8253LutBank emptyBank{};
 assert(emptyBank.vcom == nullptr && emptyBank.ww == nullptr && emptyBank.bw == nullptr &&
        emptyBank.wb == nullptr && emptyBank.bb == nullptr);
 const freeink::Uc8253Lut16Config defaultLut16Cfg{};
 assert(defaultLut16Cfg.pllBase == 0x09 && defaultLut16Cfg.pllPass1 == 0x0B &&
        defaultLut16Cfg.pllPass2 == 0x0E && defaultLut16Cfg.vdlPass2 == 0x1E && !defaultLut16Cfg.step0Enable);
 assert(defaultLut16Cfg.step0Erase.vcom == nullptr);

 // Verify cleanBaseNeeded when _grayState.lsbValid == true promotes Fast fallback to Half (_cfg.half)
 // so it never diffs _cfg.fast against an aborted grayscale LSB plane:
 d.display(bus, fb.data(), nullptr, freeink::RefreshMode::Fast, false);
 d.writeGrayscalePlaneStrip(bus, freeink::GrayPlane::Lsb, cLsb.data(), 0, 80);
 const unsigned refBeforeDirtyLsb = bus.refreshes;
 d.displayGrayscaleBase(bus, fb2.data(), freeink::RefreshMode::Fast, false);
 assert(bus.refreshes == refBeforeDirtyLsb + 2);  // 1 Half scrub (CDI 0xA9, WW==BW=0xAA) + 1 preBwMid settle
 assert((bus.refreshHistory[bus.refreshHistory.size() - 2].cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
 assert(bus.refreshHistory[bus.refreshHistory.size() - 2].ww0 == 0xAA &&
        bus.refreshHistory[bus.refreshHistory.size() - 2].bw0 == 0xAA);
 assert(bus.refreshHistory[bus.refreshHistory.size() - 2].pll == freeink::x3_lut16::kPllStandard &&
        bus.refreshHistory.back().pll == freeink::x3_lut16::kPllStandard);
 assert(freeink::gray16::kMask4BitBw == 0xFFFCu &&
        freeink::gray16::kMaskBwClear == 0xFF00u &&
        freeink::gray16::kMaskPass1Dtm1 == 0x24EEu &&
        freeink::gray16::kMaskPass1Dtm2 == 0x3756u &&
        freeink::gray16::kMaskPass2Dtm1 == 0x71DAu &&
        freeink::gray16::kMaskPass2Dtm2 == 0x6D0Au &&
        freeink::gray16::kMaskCleanupBlack == 0xFFFEu);
 for (int ink = 0; ink < 16; ++ink) {
   const uint8_t bw = ((freeink::gray16::kMaskBwClear >> ink) & 1u) ? 0u : 1u;
   const uint8_t c1 = (freeink::gray16::kMaskPass1Dtm1 >> ink) & 1u;
   const uint8_t c2 = (freeink::gray16::kMaskPass1Dtm2 >> ink) & 1u;
   const uint8_t f1 = (freeink::gray16::kMaskPass2Dtm1 >> ink) & 1u;
   const uint8_t f2 = (freeink::gray16::kMaskPass2Dtm2 >> ink) & 1u;
   const uint8_t code5 = static_cast<uint8_t>((bw << 4) | (c1 << 3) | (c2 << 2) | (f1 << 1) | f2);
   assert(freeink::gray16::kDecodeGray16[code5] == static_cast<uint8_t>(15 - ink));
 }
 freeink::Uc8253X3Config noLut16Cfg = freeink::uc8253X3DefaultConfig();
 noLut16Cfg.lut16 = {};
 freeink::Uc8253X3Driver noLut16Driver(noLut16Cfg);
 assert(!noLut16Driver.supportsGray16());
 noLut16Driver.begin(bus);
 noLut16Driver.skipInitialResync();
 noLut16Driver.displayGrayscaleBase(bus, fb.data(), freeink::RefreshMode::Fast, false);
 noLut16Driver.writeGrayscalePlaneStrip(bus, freeink::GrayPlane::Lsb, cLsb.data(), 0, 80);
 noLut16Driver.writeGrayscalePlaneStrip(bus, freeink::GrayPlane::Msb, cMsb.data(), 0, 80);
 const unsigned refBeforeNoLut16 = bus.refreshes;
 noLut16Driver.displayGray16Pass1(bus);
 noLut16Driver.displayGray16Pass2(bus, false);
 assert(bus.refreshes == refBeforeNoLut16);
 noLut16Driver.beginGray16Cleanup(bus, fb.data());
 noLut16Driver.finishGray16Cleanup(bus);
 assert(!noLut16Driver.isGray16CleanupActive());

 // Verify powerOffBetweenFrames powers off HV rails cleanly when _isScreenOn is true:
 const unsigned pwrOffBeforeIdle = bus.powerOffs;
 noLut16Driver.powerOffBetweenFrames(bus);
 assert(bus.powerOffs == pwrOffBeforeIdle + 1);
 noLut16Driver.powerOffBetweenFrames(bus);
 assert(bus.powerOffs == pwrOffBeforeIdle + 1);  // idempotent when already off

 // Verify FreeInkDisplay Dark -> Light displayGrayscaleBase runs preconditionGrayscale
 // without prematurely cycling HV rails when turnOffScreen == false:
 freeink::FreeInkDisplay facade(0, 0, 0, 0, 0, 0);
 facade.setDisplayX3();
 facade.begin();
 facade.skipInitialResync();
 std::memcpy(facade.getFrameBuffer(), fb.data(), fb.size());
 facade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);

 // Enter Dark Mode: displayGrayscaleBase runs HALF_REFRESH scrub without preconditionGrayscale.
 facade.setInverted(true);
 const unsigned ptlBeforeDark = facade._bus.partialIns;
 facade.displayGrayscaleBase(freeink::FreeInkDisplay::FAST_REFRESH, false);
 assert(facade._bus.partialIns == ptlBeforeDark);

 // Exit Dark Mode (turnOffScreen == false): HALF_REFRESH scrub + preconditionGrayscale, 0 powerOffs!
 facade.setInverted(false);
 const unsigned refBeforeExitDark = facade._bus.refreshes;
 const unsigned pofBeforeExitDark = facade._bus.powerOffs;
 const unsigned ptlBeforeExitDark = facade._bus.partialIns;
 facade.displayGrayscaleBase(freeink::FreeInkDisplay::FAST_REFRESH, /*turnOffScreen=*/false);
 assert(facade._bus.powerOffs == pofBeforeExitDark);
 assert(facade._bus.refreshes == refBeforeExitDark + 2);  // 1 HALF scrub + 1 preconditionGrayscale settle
 assert(facade._bus.partialIns == ptlBeforeExitDark + 1 && facade._bus.partialOuts == ptlBeforeExitDark + 1);
 assert((facade._bus.cdiWrites.back() == std::array<uint8_t, 2>{0xA9, 0x07}));
 assert(facade._bus.lastWw[0] == 0x20 && facade._bus.lastBb[0] == 0x10);

 // Exit Dark Mode with turnOffScreen == true: rails stay ON across HALF + precondition, then power off ONCE:
 facade.setInverted(true);
 facade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
 facade.setInverted(false);
 const unsigned pofBeforeTurnOffExit = facade._bus.powerOffs;
 const unsigned ponBeforeTurnOffExit = facade._bus.powerOns;
 facade.displayGrayscaleBase(freeink::FreeInkDisplay::FAST_REFRESH, /*turnOffScreen=*/true);
 assert(facade._bus.powerOns == ponBeforeTurnOffExit);        // no mid-sequence POF+PON bounce
 assert(facade._bus.powerOffs == pofBeforeTurnOffExit + 1);   // powered off cleanly at end

 // =========================================================================
 // Async Deferred Gray16 Base & Pass 1 Parity + SPI Safety Interlock Tests
 // =========================================================================
 assert(!noLut16Driver.supportsDeferredGrayscaleBase());
 assert(!noLut16Driver.supportsDeferredGray16Base());
 assert(!d.supportsDeferredGrayscaleBase());
 assert(d.supportsDeferredGray16Base());
 assert(!facade.supportsDeferredGrayscaleBase());
 assert(facade.supportsDeferredGray16Base());

 // Compare full 2-pass Gray16 turn (Base -> Pass 1 -> Pass 2 -> Cleanup) between
 // synchronous FreeInkDisplay and async FreeInkDisplay (`displayGray16BaseAsync` + `displayGray16Pass1Async`):
 freeink::FreeInkDisplay syncFacade(0, 0, 0, 0, 0, 0);
 freeink::FreeInkDisplay asyncFacade(0, 0, 0, 0, 0, 0);
 freeink::Uc8253X3Driver syncDrv;
 freeink::Uc8253X3Driver asyncDrv;
 for (const auto& pair : {std::pair{&syncFacade, &syncDrv}, std::pair{&asyncFacade, &asyncDrv}}) {
   pair.first->setDisplayX3();
   pair.first->begin();
   pair.first->_driver = pair.second;
   pair.second->begin(pair.first->_bus);
   pair.first->skipInitialResync();
   std::memcpy(pair.first->getFrameBuffer(), fb.data(), fb.size());
   pair.first->displayGray16Base(freeink::FreeInkDisplay::FAST_REFRESH, false);
   for (uint16_t y = 0; y < 528; y += 80) {
     const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
     pair.first->writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, cLsb.data() + size_t(y) * 99, y, rows);
     pair.first->writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, cMsb.data() + size_t(y) * 99, y, rows);
   }
   pair.first->displayGray16Pass1();
   for (uint16_t y = 0; y < 528; y += 80) {
     const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
     pair.first->writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, fLsb.data() + size_t(y) * 99, y, rows);
     pair.first->writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, fMsb.data() + size_t(y) * 99, y, rows);
   }
   pair.first->displayGray16Pass2(false);
   pair.first->beginGray16Cleanup(fb.data());
   pair.first->copyGrayscaleLsbBuffers(cleanupDtm1.data());
   pair.first->finishGray16Cleanup();
 }

 // Now execute Page 2 synchronously on syncFacade and asynchronously on asyncFacade:
 const size_t syncRefStart = syncFacade._bus.refreshHistory.size();
 const size_t syncPllStart = syncFacade._bus.pllWrites.size();
 const size_t syncVcomStart = syncFacade._bus.vcomWrites.size();
 std::memcpy(syncFacade.getFrameBuffer(), fb2.data(), fb2.size());
 syncFacade.displayGray16Base(freeink::FreeInkDisplay::FAST_REFRESH, false);
 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   syncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, cLsb.data() + size_t(y) * 99, y, rows);
   syncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, cMsb.data() + size_t(y) * 99, y, rows);
 }
 syncFacade.displayGray16Pass1();
 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   syncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, fLsb.data() + size_t(y) * 99, y, rows);
   syncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, fMsb.data() + size_t(y) * 99, y, rows);
 }
 syncFacade.displayGray16Pass2(false);
 syncFacade.beginGray16Cleanup(fb2.data());
 syncFacade.copyGrayscaleLsbBuffers(cleanupDtm1.data());
 syncFacade.finishGray16Cleanup();

 const size_t asyncRefStart = asyncFacade._bus.refreshHistory.size();
 const size_t asyncPllStart = asyncFacade._bus.pllWrites.size();
 const size_t asyncVcomStart = asyncFacade._bus.vcomWrites.size();
 const unsigned asyncIrqStart = asyncFacade._bus.interruptWaits;
 std::memcpy(asyncFacade.getFrameBuffer(), fb2.data(), fb2.size());
 assert(asyncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
 assert(asyncFacade.isRefreshPending());
 assert(asyncFacade._bus.drfActive);  // Base X3_DRF is actively in flight!
 assert(asyncFacade._bus.interruptWaits == asyncIrqStart);
 asyncFacade.waitRefreshComplete();
 assert(!asyncFacade.isRefreshPending());
 assert(!asyncFacade._bus.drfActive);
 assert(asyncFacade._bus.interruptWaits == asyncIrqStart + 1);
 assert(asyncFacade._bus.currentPll == freeink::x3_lut16::kPllStandard);

 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, cLsb.data() + size_t(y) * 99, y, rows);
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, cMsb.data() + size_t(y) * 99, y, rows);
 }
 assert(asyncFacade.displayGray16Pass1Async());
 assert(asyncFacade.isRefreshPending());
 assert(asyncFacade._bus.drfActive);  // Pass 1 X3_DRF is actively in flight!
 assert(asyncFacade._bus.currentPll == freeink::x3_lut16::kPllPass1Coarse);  // 0x09 NOT yet sent mid-DRF!
 asyncFacade.waitRefreshComplete();
 assert(!asyncFacade.isRefreshPending());
 assert(!asyncFacade._bus.drfActive);
 assert(asyncFacade._bus.interruptWaits == asyncIrqStart + 2);
 assert(asyncFacade._bus.currentPll == freeink::x3_lut16::kPllStandard);  // 0x09 restored on finish!

 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, fLsb.data() + size_t(y) * 99, y, rows);
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, fMsb.data() + size_t(y) * 99, y, rows);
 }
 assert(asyncFacade.displayGray16Pass2Async(false));
 assert(asyncFacade.isRefreshPending());
 assert(asyncFacade._bus.drfActive);  // Pass 2 X3_DRF is actively in flight!
 assert(asyncFacade._bus.currentPll == freeink::x3_lut16::kPllPass2Fine);
 asyncFacade.waitRefreshComplete();
 assert(!asyncFacade.isRefreshPending());
 assert(!asyncFacade._bus.drfActive);
 assert(asyncFacade._bus.interruptWaits == asyncIrqStart + 3);
 assert(asyncFacade._bus.currentPll == freeink::x3_lut16::kPllStandard);
 assert(asyncFacade._bus.currentPwr == kStdPwr);
 assert(asyncFacade._bus.currentVcom == freeink::x3_lut16::kVcomDcIdle);  // Parked at 0x00 in drainPendingRefresh!
 asyncFacade.beginGray16Cleanup(fb2.data());
 asyncFacade.copyGrayscaleLsbBuffers(cleanupDtm1.data());
 asyncFacade.finishGray16Cleanup();

 // Verify 100.000% bit-for-bit identical refreshes, planes, PLL, and VCOM sequences:
 assert(syncFacade._bus.refreshHistory.size() - syncRefStart == 3);
 assert(asyncFacade._bus.refreshHistory.size() - asyncRefStart == 3);
 for (size_t i = 0; i < 3; ++i) {
   const auto& sr = syncFacade._bus.refreshHistory[syncRefStart + i];
   const auto& ar = asyncFacade._bus.refreshHistory[asyncRefStart + i];
   assert((sr.cdi == std::array<uint8_t, 2>{0xA9, 0x07}));  // Base, Pass 1, and Pass 2 all keep BDZ=1 Hi-Z border!
   assert(sr.vcom == (i == 2 ? freeink::x3_lut16::kVcomPass2Fine : freeink::x3_lut16::kVcomDcStandard));
   assert(sr.cdi == ar.cdi && sr.pll == ar.pll && sr.vcom == ar.vcom && sr.pwr == ar.pwr);
   assert(sr.ww0 == ar.ww0 && sr.bw0 == ar.bw0 && sr.wb0 == ar.wb0 && sr.bb0 == ar.bb0);
   assert(sr.ww6 == ar.ww6 && sr.bw6 == ar.bw6 && sr.wb6 == ar.wb6 && sr.bb6 == ar.bb6);
   assert(sr.oldPlaneDuring == ar.oldPlaneDuring && sr.newPlaneDuring == ar.newPlaneDuring);
 }
 assert(syncFacade._bus.oldPlane == asyncFacade._bus.oldPlane);
 assert(syncFacade._bus.newPlane == asyncFacade._bus.newPlane);
 assert(std::vector<uint8_t>(syncFacade._bus.pllWrites.begin() + syncPllStart, syncFacade._bus.pllWrites.end()) ==
        std::vector<uint8_t>(asyncFacade._bus.pllWrites.begin() + asyncPllStart, asyncFacade._bus.pllWrites.end()));
 assert(std::vector<uint8_t>(syncFacade._bus.vcomWrites.begin() + syncVcomStart, syncFacade._bus.vcomWrites.end()) ==
        std::vector<uint8_t>(asyncFacade._bus.vcomWrites.begin() + asyncVcomStart, asyncFacade._bus.vcomWrites.end()));
 assert(syncFacade._bus.currentVcom == freeink::x3_lut16::kVcomDcIdle);
 assert(asyncFacade._bus.currentVcom == freeink::x3_lut16::kVcomDcIdle);
 assert(asyncFacade.isGray16CleanupActive());

 // Safety Interlock Test: calling writeGrayscalePlaneStrip, copyGrayscaleLsb, display,
 // beginGray16Cleanup, or powerOffBetweenFrames WITHOUT calling waitRefreshComplete()
 // MUST auto-drain waitRefreshComplete() + restore PLL 0x09 BEFORE touching SPI (`assert(!drfActive)` in EpdBus!):
 assert(asyncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
 assert(asyncFacade._bus.drfActive);
 asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, cLsb.data(), 0, 80);
 assert(!asyncFacade._bus.drfActive && !asyncFacade.isRefreshPending());
 asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, cMsb.data(), 0, 80);
 assert(asyncFacade.displayGray16Pass1Async());
 assert(asyncFacade._bus.drfActive && asyncFacade._bus.currentPll == freeink::x3_lut16::kPllPass1Coarse);
 // Interrupt mid-Pass 1 directly with displayBuffer(FAST_REFRESH) (e.g. popup open) — must auto-drain & revert cleanly!
 asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
 assert(!asyncFacade._bus.drfActive && !asyncFacade.isRefreshPending());
 assert(asyncFacade._bus.currentPll == freeink::x3_lut16::kPllStandard);

 // Direct driver-level safety guard (without facade):
 assert(asyncDrv.displayGray16BaseStart(asyncFacade._bus, fb.data(), freeink::RefreshMode::Fast, false));
 assert(asyncFacade._bus.drfActive);
 asyncDrv.copyGrayscaleLsb(asyncFacade._bus, cLsb.data());
 assert(!asyncFacade._bus.drfActive && asyncFacade._bus.currentPll == freeink::x3_lut16::kPllStandard);
 asyncDrv.copyGrayscaleMsb(asyncFacade._bus, cMsb.data());
 assert(asyncDrv.displayGray16Pass1Start(asyncFacade._bus));
 assert(asyncFacade._bus.drfActive && asyncFacade._bus.currentPll == freeink::x3_lut16::kPllPass1Coarse);
 asyncDrv.powerOffBetweenFrames(asyncFacade._bus);
 assert(!asyncFacade._bus.drfActive && asyncFacade._bus.currentPll == freeink::x3_lut16::kPllStandard);
 assert(asyncFacade._bus.pllAtPowerOff.back() == freeink::x3_lut16::kPllStandard);

 // =========================================================================
 // Single-DRF Bipolar Scrub, Idle HV Power-Off, Pre-Warm PON, and N2OCP=1
 // =========================================================================

 // (a) Verify 42-byte kLut16BipolarScrub tables (33f G0 + 13f G1 + 16f G2 = 62f):
 constexpr std::array<uint8_t, 42> kExpScrubVcom = {
     0x00, 0x14, 0x01, 0x0B, 0x01, 0x01, 0x00, 0x0B, 0x02, 0x00, 0x00, 0x01, 0x00, 0x0F, 0x01, 0x00, 0x00, 0x01};
 constexpr std::array<uint8_t, 42> kExpScrubWwBw = {
     0x48, 0x14, 0x01, 0x0B, 0x01, 0x01, 0x80, 0x0B, 0x02, 0x00, 0x00, 0x01, 0x00, 0x0F, 0x01, 0x00, 0x00, 0x01};
 constexpr std::array<uint8_t, 42> kExpScrubWbBb = {
     0x48, 0x14, 0x01, 0x0B, 0x01, 0x01, 0x40, 0x0B, 0x02, 0x00, 0x00, 0x01, 0x40, 0x0F, 0x01, 0x00, 0x00, 0x01};
 assert(std::memcmp(defCfg.lut16.bipolarScrub.vcom, kExpScrubVcom.data(), 42) == 0);
 assert(std::memcmp(defCfg.lut16.bipolarScrub.ww, kExpScrubWwBw.data(), 42) == 0);
 assert(std::memcmp(defCfg.lut16.bipolarScrub.bw, kExpScrubWwBw.data(), 42) == 0);
 assert(std::memcmp(defCfg.lut16.bipolarScrub.wb, kExpScrubWbBb.data(), 42) == 0);
 assert(std::memcmp(defCfg.lut16.bipolarScrub.bb, kExpScrubWbBb.data(), 42) == 0);
 assert((defCfg.lut16.bipolarScrub.ww[1] + defCfg.lut16.bipolarScrub.ww[2] +
         defCfg.lut16.bipolarScrub.ww[3] + defCfg.lut16.bipolarScrub.ww[4]) * defCfg.lut16.bipolarScrub.ww[5] == 33);
 assert((defCfg.lut16.bipolarScrub.ww[7] + defCfg.lut16.bipolarScrub.ww[8] +
         defCfg.lut16.bipolarScrub.ww[9] + defCfg.lut16.bipolarScrub.ww[10]) * defCfg.lut16.bipolarScrub.ww[11] == 13);
 assert((defCfg.lut16.bipolarScrub.ww[13] + defCfg.lut16.bipolarScrub.ww[14] +
         defCfg.lut16.bipolarScrub.ww[15] + defCfg.lut16.bipolarScrub.ww[16]) * defCfg.lut16.bipolarScrub.ww[17] == 16);

 // Sync vs Async full 16-gray HALF_REFRESH scrub turn (1 DRF base + Pass 1 + Pass 2 + Cleanup):
 const size_t scrubSyncRefStart = syncFacade._bus.refreshHistory.size();
 const size_t scrubSyncPllStart = syncFacade._bus.pllWrites.size();
 const unsigned scrubSyncDtm1Before = syncFacade._bus.dtm1FullWrites;
 std::memcpy(syncFacade.getFrameBuffer(), fb.data(), fb.size());
 syncFacade.displayGray16Base(freeink::FreeInkDisplay::HALF_REFRESH, false);
 assert(syncFacade._bus.refreshHistory.size() == scrubSyncRefStart + 1);  // Single-DRF scrub
 assert(syncFacade._bus.dtm1FullWrites == scrubSyncDtm1Before);           // 0 software DTM1 writes (N2OCP=1)
 {
   const auto& srBase = syncFacade._bus.refreshHistory.back();
   assert((srBase.cdi == std::array<uint8_t, 2>{0xA9, 0x07}) && srBase.pll == freeink::x3_lut16::kPllStandard &&
          srBase.pwr == kStdPwr);
   assert(srBase.ww0 == 0x48 && srBase.bw0 == 0x48 && srBase.wb0 == 0x48 && srBase.bb0 == 0x48);
   assert(srBase.ww6 == 0x80 && srBase.bw6 == 0x80 && srBase.wb6 == 0x40 && srBase.bb6 == 0x40);
   assert(syncFacade._bus.oldPlane == fbGates && syncFacade._bus.newPlane == fbGates);
 }
 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   syncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, cLsb.data() + size_t(y) * 99, y, rows);
   syncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, cMsb.data() + size_t(y) * 99, y, rows);
 }
 syncFacade.displayGray16Pass1();
 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   syncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, fLsb.data() + size_t(y) * 99, y, rows);
   syncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, fMsb.data() + size_t(y) * 99, y, rows);
 }
 syncFacade.displayGray16Pass2(false);
 syncFacade.beginGray16Cleanup(fb.data());
 syncFacade.copyGrayscaleLsbBuffers(cleanupDtm1.data());
 syncFacade.finishGray16Cleanup();

 // Bring asyncFacade to identical powered-on + cleanupDtm1 state first, then run async HALF_REFRESH turn:
 std::memcpy(asyncFacade.getFrameBuffer(), fb2.data(), fb2.size());
 assert(asyncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
 asyncFacade.waitRefreshComplete();
 asyncFacade.beginGray16Cleanup(fb2.data());
 asyncFacade.copyGrayscaleLsbBuffers(cleanupDtm1.data());
 asyncFacade.finishGray16Cleanup();

 const size_t scrubAsyncRefStart = asyncFacade._bus.refreshHistory.size();
 const size_t scrubAsyncPllStart = asyncFacade._bus.pllWrites.size();
 const unsigned scrubAsyncIrqStart = asyncFacade._bus.interruptWaits;
 std::memcpy(asyncFacade.getFrameBuffer(), fb.data(), fb.size());
 assert(asyncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::HALF_REFRESH));
 assert(asyncFacade.isRefreshPending() && asyncFacade._bus.drfActive);  // Async overlap active during 62f scrub
 // Polling serviceIdlePower() while async scrub DRF is in flight MUST be a non-blocking no-op:
 advanceHostMillis(5000);
 assert(!asyncFacade.serviceIdlePower());
 assert(asyncFacade.isRefreshPending() && asyncFacade._bus.drfActive);
 asyncFacade.waitRefreshComplete();
 assert(!asyncFacade.isRefreshPending() && !asyncFacade._bus.drfActive);
 assert(asyncFacade._bus.interruptWaits == scrubAsyncIrqStart + 1);
 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, cLsb.data() + size_t(y) * 99, y, rows);
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, cMsb.data() + size_t(y) * 99, y, rows);
 }
 assert(asyncFacade.displayGray16Pass1Async());
 asyncFacade.waitRefreshComplete();
 for (uint16_t y = 0; y < 528; y += 80) {
   const uint16_t rows = (528 - y < 80) ? (528 - y) : 80;
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, fLsb.data() + size_t(y) * 99, y, rows);
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, fMsb.data() + size_t(y) * 99, y, rows);
 }
 assert(asyncFacade.displayGray16Pass2Async(false));
 asyncFacade.waitRefreshComplete();
 asyncFacade.beginGray16Cleanup(fb.data());
 asyncFacade.copyGrayscaleLsbBuffers(cleanupDtm1.data());
 asyncFacade.finishGray16Cleanup();

 assert(syncFacade._bus.refreshHistory.size() - scrubSyncRefStart == 3);
 assert(asyncFacade._bus.refreshHistory.size() - scrubAsyncRefStart == 3);
 for (size_t i = 0; i < 3; ++i) {
   const auto& sr = syncFacade._bus.refreshHistory[scrubSyncRefStart + i];
   const auto& ar = asyncFacade._bus.refreshHistory[scrubAsyncRefStart + i];
   assert(sr.cdi == ar.cdi && sr.pll == ar.pll && sr.vcom == ar.vcom && sr.pwr == ar.pwr);
   assert(sr.ww0 == ar.ww0 && sr.bw0 == ar.bw0 && sr.wb0 == ar.wb0 && sr.bb0 == ar.bb0);
   assert(sr.ww6 == ar.ww6 && sr.bw6 == ar.bw6 && sr.wb6 == ar.wb6 && sr.bb6 == ar.bb6);
   assert(sr.newPlaneDuring == ar.newPlaneDuring);
 }
 assert(std::vector<uint8_t>(syncFacade._bus.pllWrites.begin() + scrubSyncPllStart, syncFacade._bus.pllWrites.end()) ==
        std::vector<uint8_t>(asyncFacade._bus.pllWrites.begin() + scrubAsyncPllStart, asyncFacade._bus.pllWrites.end()));

 // Verify HALF_REFRESH with turnOffScreen == true powers off cleanly with standard PLL/PWR/VCOM:
 {
   const unsigned pofBeforeHalfTurnOff = syncFacade._bus.powerOffs;
   syncFacade.displayGray16Base(freeink::FreeInkDisplay::HALF_REFRESH, /*turnOffScreen=*/true);
   assert(syncFacade._bus.powerOffs == pofBeforeHalfTurnOff + 1);
   assert(syncFacade._bus.pllAtPowerOff.back() == freeink::x3_lut16::kPllStandard);
   assert(syncFacade._bus.pwrAtPowerOff.back() == kStdPwr);
 }

 // Verify FULL_REFRESH still forces full factory sync (synchronous, returns false on Async):
 const size_t beforeFullBase = syncFacade._bus.refreshHistory.size();
 assert(!syncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::FULL_REFRESH));
 assert(!syncFacade.isRefreshPending());
 assert(syncFacade._bus.refreshHistory.size() >= beforeFullBase + 3);  // _full + _fast + _preBwMid

 // (b) & (c): Verify 1,200 ms post-settle idle POF + _redRamSynced/_gray16CleanupActive preservation
 // + non-blocking prewarmPowerOn() + X3_PON drain before DRF:
 assert(asyncDrv._isScreenOn && asyncDrv._redRamSynced && asyncDrv.isGray16CleanupActive());
 assert(asyncFacade._bus.currentVcom == freeink::x3_lut16::kVcomDcIdle);
 const unsigned pofBefore1200 = asyncFacade._bus.powerOffs;
 const unsigned idlePofBefore1200 = asyncFacade._bus.idlePofWaits;
 // At +1,100 ms (< 1,200 ms), HV rails MUST remain warm with VCOM parked at 0x00:
 advanceHostMillis(1100);
 assert(!asyncFacade.serviceIdlePower());
 assert(asyncDrv._isScreenOn && asyncFacade._bus.powerOffs == pofBefore1200);
 assert(asyncFacade._bus.currentVcom == freeink::x3_lut16::kVcomDcIdle);
 assert(!asyncFacade.prewarmPowerOn());  // Already powered on -> 0-cost no-op

 // At >= 1,200 ms, serviceIdlePower() sends CMD_POWER_OFF (0x02) + waitBusy(" X3_IDLE_POF") once:
 advanceHostMillis(100);
 assert(asyncFacade.serviceIdlePower());
 assert(!asyncDrv._isScreenOn);
 assert(asyncFacade._bus.powerOffs == pofBefore1200 + 1);
 assert(asyncFacade._bus.idlePofWaits == idlePofBefore1200 + 1);
 // UC8253 POF preserves DTM1/DTM2 SRAM, _redRamSynced, and _gray16CleanupActive!
 assert(asyncDrv._redRamSynced && asyncDrv.isGray16CleanupActive());
 assert(!asyncFacade.serviceIdlePower());  // Idempotent once off

 // Input-edge non-blocking prewarmPowerOn(): sends CMD_POWER_ON (0x04) WITHOUT blocking!
 const unsigned ponBeforePrewarm = asyncFacade._bus.powerOns;
 const unsigned ponWaitsBeforePrewarm = asyncFacade._bus.ponWaits;
 assert(asyncFacade.prewarmPowerOn());
 assert(asyncDrv._powerOnPending && !asyncDrv._isScreenOn);
 assert(asyncFacade._bus.powerOns == ponBeforePrewarm + 1);
 assert(asyncFacade._bus.ponActive);                        // CMD 0x04 in flight, NOT yet waited!
 assert(asyncFacade._bus.ponWaits == ponWaitsBeforePrewarm);
 assert(!asyncFacade.prewarmPowerOn());                     // Duplicate button bounce -> no second 0x04!
 assert(asyncFacade._bus.powerOns == ponBeforePrewarm + 1);
 // serviceIdlePower() during prewarm (< 1,200 ms) must NOT cancel the pending PON:
 advanceHostMillis(65);
 assert(!asyncFacade.serviceIdlePower());
 assert(asyncDrv._powerOnPending && asyncFacade._bus.ponActive);

 // Next 16-gray fast turn drains " X3_PON" before touching controller registers/RAM/DRF, restores VCOM 0x24,
 // issues 0 extra PONs, and runs differential kLut16CrossDissolve (NOT cleanBaseNeeded!):
 const size_t refBeforeWarmFast16 = asyncFacade._bus.refreshHistory.size();
 std::memcpy(asyncFacade.getFrameBuffer(), fb2.data(), fb2.size());
 assert(asyncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
 assert(!asyncFacade._bus.ponActive && !asyncDrv._powerOnPending && asyncDrv._isScreenOn);
 assert(asyncFacade._bus.ponWaits == ponWaitsBeforePrewarm + 1);
 assert(asyncFacade._bus.powerOns == ponBeforePrewarm + 1);  // Zero extra PON!
 assert(asyncFacade._bus.vcomAtRefresh.back() == freeink::x3_lut16::kVcomDcStandard);
 asyncFacade.waitRefreshComplete();
 assert(asyncFacade._bus.refreshHistory.size() == refBeforeWarmFast16 + 1);
 assert(asyncFacade._bus.refreshHistory.back().ww0 == 0x00 &&
        asyncFacade._bus.refreshHistory.back().bw0 == 0x08);  // kLut16CrossDissolve!

 // Verify X3TwoPhase fast-completion guard: if >=20 ms elapsed during CPU work and BUSY_N already returned HIGH,
 // finishPendingPowerOn() marks _isScreenOn = true without stalling on a phantom falling edge:
 advanceHostMillis(1300);
 assert(asyncFacade.serviceIdlePower());
 assert(asyncFacade.prewarmPowerOn());
 advanceHostMillis(120);
 hostBusyPinLevel() = HIGH;
 asyncFacade._bus.ponActive = false;  // Simulate hardware PON ramp already completed before display call
 const unsigned ponWaitsBeforeFastDone = asyncFacade._bus.ponWaits;
 asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
 hostBusyPinLevel() = 0;
 assert(asyncFacade._bus.ponWaits == ponWaitsBeforeFastDone);  // Skipped redundant X3TwoPhase falling-edge wait!
 assert(asyncDrv._isScreenOn);
 assert(asyncFacade._bus.vcomAtRefresh.back() == freeink::x3_lut16::kVcomDcStandard);
 assert(asyncFacade._bus.currentVcom == freeink::x3_lut16::kVcomDcIdle);

 // Verify abandoned prewarmPowerOn() (e.g., Next Page pressed at EOF with no subsequent render for >=1,200 ms)
 // is cleanly drained and powered off by serviceIdlePower():
 advanceHostMillis(1300);
 assert(asyncFacade.serviceIdlePower());
 assert(!asyncDrv._isScreenOn);
 assert(asyncFacade.prewarmPowerOn());
 assert(asyncDrv._powerOnPending && asyncFacade._bus.ponActive);
 advanceHostMillis(1200);
 const unsigned idlePofBeforeEof = asyncFacade._bus.idlePofWaits;
 assert(asyncFacade.serviceIdlePower());
 assert(!asyncDrv._powerOnPending && !asyncDrv._isScreenOn && !asyncFacade._bus.ponActive);
 assert(asyncFacade._bus.idlePofWaits == idlePofBeforeEof + 1);

 // (b) & (d): Verify Warm-POF 1-bpp `Fast` preservation AND `N2OCP=1` redundant `DTM1` skip:
 // After idle POF, displayBuffer(FAST_REFRESH) MUST stay Fast (_cfg.fast, CDI 0x29) and emit 0 software DTM1 writes!
 const unsigned dtm1BeforeFast1bpp = asyncFacade._bus.dtm1FullWrites;
 const unsigned dtm2BeforeFast1bpp = asyncFacade._bus.dtm2FullWrites;
 std::memcpy(asyncFacade.getFrameBuffer(), fb.data(), fb.size());
 asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
 assert(asyncFacade._bus.dtm1FullWrites == dtm1BeforeFast1bpp);      // 0 software DTM1 writes!
 assert(asyncFacade._bus.dtm2FullWrites == dtm2BeforeFast1bpp + 1);  // 1 DTM2 write
 assert((asyncFacade._bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0x29, 0x07}));  // _cfg.fast, NOT Half!
 assert(asyncFacade._bus.oldPlane == fbGates && asyncFacade._bus.newPlane == fbGates);

 // Likewise steady-state 1-bpp Half (CDI 0xA9, N2OCP=1) emits 0 software DTM1 writes:
 std::memcpy(asyncFacade.getFrameBuffer(), fb2.data(), fb2.size());
 asyncFacade.displayBuffer(freeink::FreeInkDisplay::HALF_REFRESH, false);
 assert(asyncFacade._bus.dtm1FullWrites == dtm1BeforeFast1bpp);      // Still 0 software DTM1 writes!
 assert((asyncFacade._bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
 assert(asyncFacade._bus.oldPlane == fb2Gates && asyncFacade._bus.newPlane == fb2Gates);

 // Whereas cold wake right after begin() + skipInitialResync() (_hasPresentedSinceInit == false)
 // STILL promotes the very first Fast display() to Half (CDI 0xA9):
 freeink::Uc8253X3Driver seamlessWakeDrv;
 seamlessWakeDrv.begin(bus);
 seamlessWakeDrv.skipInitialResync();
 seamlessWakeDrv.display(bus, fb.data(), nullptr, freeink::RefreshMode::Fast, false);
 assert((bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
 seamlessWakeDrv.display(bus, fb2.data(), nullptr, freeink::RefreshMode::Fast, false);
 assert((bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0x29, 0x07}));

 // =========================================================================
 // Scoped Full-Plane Gate-Order Burst Stream (no PTIN/PTOUT)
 // =========================================================================
 struct HostGateStreamCtx {
   const std::vector<uint8_t>* logicalPlane;
   std::vector<uint8_t> scratch;
   int nextStrip;
 };
 auto hostNextGateChunk = [](void* raw, const uint8_t** outChunk) -> size_t {
   auto* ctx = static_cast<HostGateStreamCtx*>(raw);
   if (ctx->nextStrip < 0) return 0;
   const size_t yStart = static_cast<size_t>(ctx->nextStrip--) * 80u;
   const size_t rows = (528u - yStart < 80u) ? (528u - yStart) : 80u;
   for (size_t r = 0; r < rows; ++r) {
     std::memcpy(ctx->scratch.data() + (rows - 1u - r) * 99u, ctx->logicalPlane->data() + (yStart + r) * 99u, 99u);
   }
   *outChunk = ctx->scratch.data();
   return rows * 99u;
 };
 auto streamPlaneGateOrder = [&](freeink::FreeInkDisplay& fac, freeink::FreeInkDisplay::GrayPlane plane,
                                 const std::vector<uint8_t>& logicalPlane) {
   HostGateStreamCtx ctx{&logicalPlane, std::vector<uint8_t>(80u * 99u, 0), 6};
   assert(fac.writeGrayscalePlaneGateStream(plane, &ctx, +hostNextGateChunk));
   assert(!fac._bus.streamActive);  // Scoped RAII stream MUST close before returning!
 };

 // Idle-POF + prewarmPowerOn() + async HALF_REFRESH bipolar scrub + 4 streamed planes:
 advanceHostMillis(2600);
 assert(asyncFacade.serviceIdlePower());
 assert(!asyncDrv._isScreenOn);
 assert(asyncFacade.prewarmPowerOn());
 assert(asyncFacade._bus.ponActive);

 const unsigned ptlInBeforeBurst = asyncFacade._bus.partialIns;
 const unsigned ptlOutBeforeBurst = asyncFacade._bus.partialOuts;
 const unsigned stopsBeforeBurst = asyncFacade._bus.dataStops;
 const unsigned opensBeforeBurst = asyncFacade._bus.streamOpens;
 const unsigned chunksBeforeBurst = asyncFacade._bus.streamChunks;
 const size_t refBeforeBurst = asyncFacade._bus.refreshHistory.size();

 std::memcpy(asyncFacade.getFrameBuffer(), fb.data(), fb.size());
 assert(asyncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::HALF_REFRESH));
 assert(asyncFacade.isRefreshPending() && asyncFacade._bus.drfActive && !asyncFacade._bus.ponActive);

 // Calling writeGrayscalePlaneGateStream directly auto-drains pending Base DRF and sets _grayState.lsbValid:
 streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_LSB, cLsb);
 assert(!asyncFacade.isRefreshPending() && !asyncFacade._bus.drfActive);
 assert(asyncDrv._grayState.lsbValid);
 streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_MSB, cMsb);
 assert(asyncFacade.displayGray16Pass1Async());
 assert(asyncFacade._bus.drfActive);

 // Auto-drain Pass 1 DRF on Pass 2 LSB stream open:
 streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_LSB, fLsb);
 assert(!asyncFacade._bus.drfActive && asyncDrv._grayState.lsbValid);
 streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_MSB, fMsb);
 assert(asyncFacade.displayGray16Pass2Async(false));
 asyncFacade.waitRefreshComplete();
 asyncFacade.beginGray16Cleanup(fb.data());
 asyncFacade.copyGrayscaleLsbBuffers(cleanupDtm1.data());
 asyncFacade.finishGray16Cleanup();

 // Null callback to writeGrayscalePlaneGateStream must be a safe no-op (no extra CMD_DATA_STOP):
 const unsigned stopsAfterCleanup = asyncFacade._bus.dataStops;
 assert(!asyncFacade.writeGrayscalePlaneGateStream(freeink::FreeInkDisplay::GRAY_PLANE_LSB, nullptr, nullptr));
 assert(asyncFacade._bus.dataStops == stopsAfterCleanup);

 // Zero PTIN/PTOUT across the entire 1-DRF scrub + 2-pass gate-stream turn!
 assert(asyncFacade._bus.partialIns == ptlInBeforeBurst);
 assert(asyncFacade._bus.partialOuts == ptlOutBeforeBurst);
 assert(asyncFacade._bus.streamOpens == opensBeforeBurst + 4);
 assert(asyncFacade._bus.streamChunks == chunksBeforeBurst + 28);  // 4 planes * 7 chunks
 // 1 base DTM2 + 4 streamed planes + 2 cleanup planes (DTM2 + DTM1) + 1 finishGray16Cleanup DATA_STOP = 8
 assert(stopsAfterCleanup == stopsBeforeBurst + 8);
 assert(asyncFacade._bus.refreshHistory.size() - refBeforeBurst == 3);
 assert(asyncFacade._bus.refreshHistory[refBeforeBurst + 1].oldPlaneDuring == gateFlipped(cLsb));
 assert(asyncFacade._bus.refreshHistory[refBeforeBurst + 1].newPlaneDuring == gateFlipped(cMsb));
 assert(asyncFacade._bus.refreshHistory[refBeforeBurst + 2].oldPlaneDuring == gateFlipped(fLsb));
 assert(asyncFacade._bus.refreshHistory[refBeforeBurst + 2].newPlaneDuring == gateFlipped(fMsb));

 // =========================================================================
 // Leaving 16-Gray Cleanup on Fast/Half and Dark-Mode Exit Scrub
 // =========================================================================
 // 1. Leaving 16-gray Cleanup (_gray16CleanupActive == true, even after warm idle POF):
 //    displayBuffer(FAST_REFRESH) applies _cfg.lut16.preBwMid (CrossDissolve, CDI 0xA9, 0 software DTM1 writes),
 //    clearing cleanup so full G00 erase and 18f VDH black UI strokes over ink 1..7 AA halos run in 1 fast pass:
 advanceHostMillis(2600);
 assert(asyncFacade.serviceIdlePower());
 assert(!asyncDrv._isScreenOn && asyncDrv.isGray16CleanupActive());
 {
   const unsigned dtm1BeforeQuickSleep = asyncFacade._bus.dtm1FullWrites;
   const size_t refBeforeQuickSleep = asyncFacade._bus.refreshHistory.size();
   std::memcpy(asyncFacade.getFrameBuffer(), fb2.data(), fb2.size());
   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   assert(asyncFacade._bus.refreshHistory.size() == refBeforeQuickSleep + 1);
   assert((asyncFacade._bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
   assert(asyncFacade._bus.refreshHistory.back().ww0 == 0x00 &&
          asyncFacade._bus.refreshHistory.back().bw0 == 0x08 &&
          asyncFacade._bus.refreshHistory.back().wb0 == 0x54 &&
          asyncFacade._bus.refreshHistory.back().bb0 == 0x80);
   assert(asyncFacade._bus.dtm1FullWrites == dtm1BeforeQuickSleep);
   assert(!asyncDrv.isGray16CleanupActive());
   // Next 1-bit UI Fast refresh runs normal _cfg.fast (CDI 0x29):
   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   assert((asyncFacade._bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0x29, 0x07}));
 }

 // 1b. Dark Mode ON -> OFF -> 16-gray (both sync displayGray16Base & async displayGray16BaseAsync):
 //     MUST promote FAST_REFRESH -> HALF_REFRESH (kLut16BipolarScrub, 1 single DRF, CDI 0xA9, N2OCP=1)
 //     WITHOUT firing a redundant displayBuffer(HALF_REFRESH) first, and preserving async Pass 1 overlap!
 {
   // Sync Dark -> Light 16-gray:
   syncFacade.setInverted(true);
   std::memcpy(syncFacade.getFrameBuffer(), fb.data(), fb.size());
   syncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   syncFacade.setInverted(false);
   const size_t refBeforeSyncDarkExit = syncFacade._bus.refreshHistory.size();
   std::memcpy(syncFacade.getFrameBuffer(), fb2.data(), fb2.size());
   syncFacade.displayGray16Base(freeink::FreeInkDisplay::FAST_REFRESH, false);
   assert(syncFacade._bus.refreshHistory.size() == refBeforeSyncDarkExit + 1);  // Strictly 1 single DRF!
   assert((syncFacade._bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
   assert(syncFacade._bus.refreshHistory.back().ww0 == 0x48 &&
          syncFacade._bus.refreshHistory.back().bw0 == 0x48);  // Promoted to bipolarScrub!
   assert(syncFacade._bus.oldPlane == fb2Gates && syncFacade._bus.newPlane == fb2Gates);

   // Async Dark -> Light 16-gray (supportsDeferredGray16Base() stays true when _inversionDirty):
   asyncFacade.setInverted(true);
   std::memcpy(asyncFacade.getFrameBuffer(), fb.data(), fb.size());
   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   asyncFacade.setInverted(false);
   assert(asyncFacade.supportsDeferredGray16Base());
   const size_t refBeforeAsyncDarkExit = asyncFacade._bus.refreshHistory.size();
   std::memcpy(asyncFacade.getFrameBuffer(), fb2.data(), fb2.size());
   assert(asyncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
   assert(asyncFacade.isRefreshPending() && asyncFacade._bus.drfActive);  // Async overlap preserved!
   asyncFacade.waitRefreshComplete();
   assert(asyncFacade._bus.refreshHistory.size() == refBeforeAsyncDarkExit + 1);  // Strictly 1 single DRF!
   assert((asyncFacade._bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
   assert(asyncFacade._bus.refreshHistory.back().ww0 == 0x48 &&
          asyncFacade._bus.refreshHistory.back().bw0 == 0x48);
   assert(asyncFacade._bus.oldPlane == fb2Gates && asyncFacade._bus.newPlane == fb2Gates);

   // beginGray16Cleanup(nullptr) + dual GateStream (DTM2 + DTM1) leaves host fb untouched:
   streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_LSB, cLsb);
   streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_MSB, cMsb);
   assert(asyncFacade.displayGray16Pass1Async());
   streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_LSB, fLsb);
   streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_MSB, fMsb);
   assert(asyncFacade.displayGray16Pass2Async(false));
   asyncFacade.waitRefreshComplete();
   const unsigned dtm2BeforeNullCleanup = asyncFacade._bus.dtm2FullWrites;
   asyncFacade.beginGray16Cleanup(nullptr);
   assert(asyncFacade._bus.dtm2FullWrites == dtm2BeforeNullCleanup);  // 0 redundant full-buffer writes!
   streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_MSB, cleanupDtm1);
   streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_LSB, cleanupDtm1);
   asyncFacade.finishGray16Cleanup();
   assert(asyncFacade.isGray16CleanupActive());
   assert(asyncFacade._bus.newPlane == gateFlipped(cleanupDtm1));
   assert(asyncFacade._bus.oldPlane == gateFlipped(cleanupDtm1));
   assert(std::memcmp(asyncFacade.getFrameBuffer(), fb2.data(), fb2.size()) == 0);  // Host fb untouched!
 }

 // 2. finishPendingPowerOn X3TwoPhase BUSY_N boundary cases:
 //    (a) LOW observed in prewarmPowerOn, still LOW at <20 ms -> waits with ActiveLowReady (never X3TwoPhase!)
 advanceHostMillis(2600);
 assert(asyncFacade.serviceIdlePower());
 hostBusyPinLevel() = LOW;
 assert(asyncFacade.prewarmPowerOn());
 assert(asyncDrv._powerOnSawLow);
 advanceHostMillis(5);
 {
   const unsigned alrBefore = asyncFacade._bus.activeLowReadyPonWaits;
   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   assert(asyncFacade._bus.activeLowReadyPonWaits == alrBefore + 1);
 }
 //    (b) LOW observed in prewarmPowerOn, already returned HIGH before 20 ms -> skips waitBusy completely (0 stall!)
 advanceHostMillis(2600);
 assert(asyncFacade.serviceIdlePower());
 hostBusyPinLevel() = LOW;
 assert(asyncFacade.prewarmPowerOn());
 advanceHostMillis(12);  // < 20 ms!
 hostBusyPinLevel() = HIGH;
 asyncFacade._bus.ponActive = false;
 {
   const unsigned ponWaitsBeforeEarlyDone = asyncFacade._bus.ponWaits;
   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   assert(asyncFacade._bus.ponWaits == ponWaitsBeforeEarlyDone);
   assert(asyncDrv._isScreenOn);
 }
 //    (c) HIGH during prewarmPowerOn (!_powerOnSawLow) and still HIGH at <20 ms -> uses X3TwoPhase!
 advanceHostMillis(2600);
 assert(asyncFacade.serviceIdlePower());
 hostBusyPinLevel() = HIGH;
 assert(asyncFacade.prewarmPowerOn());
 assert(!asyncDrv._powerOnSawLow);
 advanceHostMillis(2);  // < 20 ms and never saw LOW yet
 {
   const unsigned ponWaitsBeforeTwoPhase = asyncFacade._bus.ponWaits;
   const unsigned alrBeforeTwoPhase = asyncFacade._bus.activeLowReadyPonWaits;
   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   assert(asyncFacade._bus.ponWaits == ponWaitsBeforeTwoPhase + 1);
   assert(asyncFacade._bus.activeLowReadyPonWaits == alrBeforeTwoPhase);
 }
 hostBusyPinLevel() = LOW;

 // 3. startAsyncRefresh / drainPendingRefresh _pendingDrfSawLow boundary cases:
 //    (a) BUSY_N observed LOW in startAsyncRefresh, then returns HIGH during CPU overlap ->
 //        skips waitRefreshComplete(" X3_DRF") across Base, Pass 1, and Pass 2 while still restoring PLL/PWR/VCOM!
 std::memcpy(asyncFacade.getFrameBuffer(), fb.data(), fb.size());
 hostBusyPinLevel() = LOW;
 assert(asyncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
 assert(asyncDrv._pendingDrfSawLow);
 // Hardware finishes Base DRF while CPU encodes RLE:
 hostBusyPinLevel() = HIGH;
 asyncFacade._bus.drfActive = false;
 {
   const unsigned irqBeforeSkipBase = asyncFacade._bus.interruptWaits;
   asyncFacade.waitRefreshComplete();
   assert(asyncFacade._bus.interruptWaits == irqBeforeSkipBase);  // Skipped 20 ms / 1000 ms waitRefreshComplete!
   assert(asyncFacade._bus.currentPll == freeink::x3_lut16::kPllStandard);
 }
 streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_LSB, cLsb);
 streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_MSB, cMsb);
 hostBusyPinLevel() = LOW;
 assert(asyncFacade.displayGray16Pass1Async());
 assert(asyncDrv._pendingDrfSawLow && asyncFacade._bus.currentPll == freeink::x3_lut16::kPllPass1Coarse);
 hostBusyPinLevel() = HIGH;
 asyncFacade._bus.drfActive = false;
 {
   const unsigned irqBeforeSkipP1 = asyncFacade._bus.interruptWaits;
   asyncFacade.waitRefreshComplete();
   assert(asyncFacade._bus.interruptWaits == irqBeforeSkipP1);
   assert(asyncFacade._bus.currentPll == freeink::x3_lut16::kPllStandard);
   assert(asyncFacade._bus.currentVcom == freeink::x3_lut16::kVcomDcStandard);
 }
 streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_LSB, fLsb);
 streamPlaneGateOrder(asyncFacade, freeink::FreeInkDisplay::GRAY_PLANE_MSB, fMsb);
 hostBusyPinLevel() = LOW;
 assert(asyncFacade.displayGray16Pass2Async(/*turnOffScreen=*/true));
 assert(asyncDrv._pendingDrfSawLow && asyncFacade._bus.currentPll == freeink::x3_lut16::kPllPass2Fine);
 hostBusyPinLevel() = HIGH;
 asyncFacade._bus.drfActive = false;
 {
   const unsigned irqBeforeSkipP2 = asyncFacade._bus.interruptWaits;
   const unsigned pofBeforeSkipP2 = asyncFacade._bus.powerOffs;
   asyncFacade.waitRefreshComplete();
   assert(asyncFacade._bus.interruptWaits == irqBeforeSkipP2);
   assert(asyncFacade._bus.powerOffs == pofBeforeSkipP2 + 1);
   assert(!asyncDrv._isScreenOn);
   assert(asyncFacade._bus.currentPll == freeink::x3_lut16::kPllStandard);
   assert(asyncFacade._bus.currentPwr == kStdPwr);
   assert(asyncFacade._bus.currentVcom == freeink::x3_lut16::kVcomDcStandard);
 }
 //    (b) If BUSY_N was never observed LOW in startAsyncRefresh (!_pendingDrfSawLow), drainPendingRefresh
 //        still calls waitRefreshComplete(" X3_DRF"):
 hostBusyPinLevel() = HIGH;
 assert(asyncFacade.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
 assert(!asyncDrv._pendingDrfSawLow);
 {
   const unsigned irqBeforeFallback = asyncFacade._bus.interruptWaits;
   asyncFacade.waitRefreshComplete();
   assert(asyncFacade._bus.interruptWaits == irqBeforeFallback + 1);
 }
 hostBusyPinLevel() = LOW;

 // 4. Windowed partial update over active Gray16 cleanup (displayWindow):
 //    Consecutive sub-rectangle updates use CDI 0xA9, run ensurePowerOn outside PTIN,
 //    frame DTM2 write, DRF, and DTM1 sync in separate PTIN..PTOUT windows (zero CMD_DATA_STOP inside PTIN),
 //    keep oldPlane == newPlane == fb inside win, and preserve 0xFE (CleanupDTM1) outside win:
 {
   asyncFacade.beginGray16Cleanup(/*bwBuffer=*/nullptr);
   std::vector<uint8_t> cleanMask(99 * 528, 0xFE);
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_MSB, cleanMask.data(), 0, 528);
   asyncFacade.writeGrayscalePlaneStrip(freeink::FreeInkDisplay::GRAY_PLANE_LSB, cleanMask.data(), 0, 528);
   asyncFacade.finishGray16Cleanup();
   assert(asyncDrv.isGray16CleanupActive());

   // Also verify displayWindow() from idle-POF powers on HV rails OUTSIDE PTIN (!inPartial):
   advanceHostMillis(1300);
   assert(asyncFacade.serviceIdlePower());
   assert(!asyncDrv._isScreenOn && asyncDrv.isGray16CleanupActive());

   // First sub-window update: x=560..719 (cols 70..89, 20 bytes/row), y=40..159 (gates 368..487).
   // Put 0x11 outside the window in host fb to prove outside bytes are NEVER uploaded to DTM1/DTM2!
   std::vector<uint8_t> winFb1(99 * 528, 0x11);
   for (size_t r = 40; r < 160; ++r) {
     for (size_t c = 70; c < 90; ++c) {
       winFb1[r * 99 + c] = static_cast<uint8_t>((r * 7u + c * 3u + 0x21u) & 0xFFu);
     }
   }
   std::memcpy(asyncFacade.getFrameBuffer(), winFb1.data(), winFb1.size());
   const unsigned ptlInBeforeWin = asyncFacade._bus.partialIns;
   const unsigned ptlOutBeforeWin = asyncFacade._bus.partialOuts;
   const unsigned stopsBeforeWin = asyncFacade._bus.dataStops;
   const unsigned ponBeforeWin = asyncFacade._bus.powerOns;
   const unsigned refBeforeWin = asyncFacade._bus.refreshes;
   const unsigned dtm1FullBeforeWin = asyncFacade._bus.dtm1FullWrites;
   const unsigned dtm2FullBeforeWin = asyncFacade._bus.dtm2FullWrites;
   asyncFacade.displayWindow(560, 40, 160, 120, /*turnOffScreen=*/false);
   assert(asyncFacade._bus.powerOns == ponBeforeWin + 1 && asyncDrv._isScreenOn);
   assert(asyncFacade._bus.partialIns == ptlInBeforeWin + 3);
   assert(asyncFacade._bus.partialOuts == ptlOutBeforeWin + 3);
   assert(asyncFacade._bus.dataStops == stopsBeforeWin);  // Zero CMD_DATA_STOP inside PTIN!
   assert(asyncFacade._bus.refreshes == refBeforeWin + 1);
   assert(asyncFacade._bus.dtm1FullWrites == dtm1FullBeforeWin);
   assert(asyncFacade._bus.dtm2FullWrites == dtm2FullBeforeWin);
   assert(asyncFacade._bus.refreshHistory.back().bw0 == 0x08);  // kLut16CrossDissolve inside window
   assert((asyncFacade._bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
   assert((asyncFacade._bus.currentCdi == std::array<uint8_t, 2>{0xA9, 0x07}));
   assert(asyncDrv.isGray16CleanupActive());

   const auto winFb1Gates = gateFlipped(winFb1);
   for (size_t g = 0; g < 528; ++g) {
     const bool inGate = (g >= 527u - 159u && g <= 527u - 40u);
     for (size_t c = 0; c < 99; ++c) {
       const size_t idx = g * 99u + c;
       const uint8_t exp = (inGate && c >= 70u && c < 90u) ? winFb1Gates[idx] : 0xFEu;
       assert(asyncFacade._bus.newPlane[idx] == exp);
       assert(asyncFacade._bus.oldPlane[idx] == exp);
     }
   }

   // Second consecutive sub-window update on the same rectangle: must diff against Call #1's
   // synced DTM1 inside win while STILL preserving 0xFE everywhere outside win!
   std::vector<uint8_t> winFb2(99 * 528, 0x22);
   for (size_t r = 40; r < 160; ++r) {
     for (size_t c = 70; c < 90; ++c) {
       winFb2[r * 99 + c] = static_cast<uint8_t>((r * 13u + c * 5u + 0x9Au) & 0xFFu);
     }
   }
   const auto prevOldSnapshot = asyncFacade._bus.oldPlane;
   std::memcpy(asyncFacade.getFrameBuffer(), winFb2.data(), winFb2.size());
   asyncFacade.displayWindow(560, 40, 160, 120, /*turnOffScreen=*/false);
   assert(asyncFacade._bus.refreshes == refBeforeWin + 2);
   assert(asyncFacade._bus.partialIns == ptlInBeforeWin + 6);
   assert(asyncFacade._bus.partialOuts == ptlOutBeforeWin + 6);
   assert(asyncFacade._bus.dataStops == stopsBeforeWin);
   assert((asyncFacade._bus.refreshHistory.back().cdi == std::array<uint8_t, 2>{0xA9, 0x07}));
   assert(asyncFacade._bus.refreshHistory.back().oldPlaneDuring == prevOldSnapshot);
   assert(asyncDrv.isGray16CleanupActive());

   const auto winFb2Gates = gateFlipped(winFb2);
   for (size_t g = 0; g < 528; ++g) {
     const bool inGate = (g >= 527u - 159u && g <= 527u - 40u);
     for (size_t c = 0; c < 99; ++c) {
       const size_t idx = g * 99u + c;
       const uint8_t exp = (inGate && c >= 70u && c < 90u) ? winFb2Gates[idx] : 0xFEu;
       assert(asyncFacade._bus.newPlane[idx] == exp);
       assert(asyncFacade._bus.oldPlane[idx] == exp);
     }
   }

   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH);
   assert(!asyncDrv.isGray16CleanupActive());
 }

 // 5. Truncated / nullptr-chunk sendStream abort & Direct-mode gate-stream rejection:
 {
   // (a) Direct mode rejects writeGrayscalePlaneGateStream before touching SPI:
   const unsigned opensBeforeDirect = asyncFacade._bus.streamOpens;
   assert(asyncFacade.displayGrayscaleBase(freeink::GrayscaleMode::Direct, freeink::FreeInkDisplay::FAST_REFRESH, false));
   HostGateStreamCtx directCtx{&cLsb, std::vector<uint8_t>(80u * 99u, 0), 6};
   assert(!asyncFacade.writeGrayscalePlaneGateStream(freeink::FreeInkDisplay::GRAY_PLANE_LSB, &directCtx, +hostNextGateChunk));
   assert(asyncFacade._bus.streamOpens == opensBeforeDirect);
   asyncFacade.cleanupGrayscaleBuffers(fb.data());
   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FULL_REFRESH, false);

   // (b) Truncated callback (< _bufferSize) returns false and clears lsbValid, _redRamSynced, and _gray16CleanupActive:
   std::memcpy(asyncFacade.getFrameBuffer(), fb.data(), fb.size());
   asyncFacade.displayGray16Base(freeink::FreeInkDisplay::FAST_REFRESH, false);
   assert(asyncDrv._redRamSynced);
   HostGateStreamCtx shortCtx{&cLsb, std::vector<uint8_t>(80u * 99u, 0), 2};  // Only 3 strips (< 528 rows)
   assert(!asyncFacade.writeGrayscalePlaneGateStream(freeink::FreeInkDisplay::GRAY_PLANE_LSB, &shortCtx, +hostNextGateChunk));
   assert(!asyncFacade._bus.streamActive);
   assert(!asyncDrv._grayState.lsbValid && !asyncDrv._redRamSynced && !asyncDrv.isGray16CleanupActive());
   assert(asyncFacade._grayPassFailed);
   // Next B/W display recovers cleanly from !_redRamSynced:
   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   assert(asyncDrv._redRamSynced && !asyncFacade._grayPassFailed);

   // (c) Non-zero length with nullptr chunk pointer aborts cleanly with !lsbValid and !_redRamSynced:
   asyncFacade.displayGray16Base(freeink::FreeInkDisplay::FAST_REFRESH, false);
   auto nullChunkCb = [](void* raw, const uint8_t** outChunk) -> size_t {
     int& step = *static_cast<int*>(raw);
     static const uint8_t kRow[99] = {};
     if (step++ == 0) {
       *outChunk = kRow;
       return sizeof(kRow);
     }
     *outChunk = nullptr;
     return 99u;
   };
   int nullStep = 0;
   assert(!asyncFacade.writeGrayscalePlaneGateStream(freeink::FreeInkDisplay::GRAY_PLANE_LSB, &nullStep, +nullChunkCb));
   assert(!asyncFacade._bus.streamActive);
   assert(!asyncDrv._grayState.lsbValid && !asyncDrv._redRamSynced && !asyncDrv.isGray16CleanupActive());
   asyncFacade.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   assert(asyncDrv._redRamSynced);
 }

 // 6. Custom Uc8253X3Config with null bipolarScrub falls back to clean Half + preBwMid settle
 //    on Dark -> Light displayGray16Base[Async](FAST_REFRESH) and explicit HALF_REFRESH:
 {
   freeink::Uc8253X3Config noScrubCfg = freeink::uc8253X3DefaultConfig();
   noScrubCfg.lut16.bipolarScrub = {};
   freeink::FreeInkDisplay noScrubFac(0, 0, 0, 0, 0, 0);
   freeink::Uc8253X3Driver noScrubDrv(noScrubCfg);
   noScrubFac.setDisplayX3();
   noScrubFac.begin();
   noScrubFac._driver = &noScrubDrv;
   noScrubDrv.begin(noScrubFac._bus);
   noScrubFac.skipInitialResync();
   std::memcpy(noScrubFac.getFrameBuffer(), fb.data(), fb.size());
   noScrubFac.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);

   // Dark -> Light via displayGray16BaseAsync(FAST_REFRESH): promotes to Half -> cleanBaseNeeded (_half + _preBwMid):
   noScrubFac.setInverted(true);
   noScrubFac.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, false);
   noScrubFac.setInverted(false);
   const size_t refBeforeNoScrubDark = noScrubFac._bus.refreshHistory.size();
   std::memcpy(noScrubFac.getFrameBuffer(), fb2.data(), fb2.size());
   assert(!noScrubFac.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
   assert(!noScrubFac.isRefreshPending());
   assert(noScrubFac._bus.refreshHistory.size() == refBeforeNoScrubDark + 2);
   assert(noScrubFac._bus.refreshHistory[refBeforeNoScrubDark].ww0 == 0xAA &&
          noScrubFac._bus.refreshHistory[refBeforeNoScrubDark].bw0 == 0xAA);
   assert(noScrubFac._bus.refreshHistory.back().ww0 == 0x20 &&
          noScrubFac._bus.refreshHistory.back().bb0 == 0x10);

   // Explicit HALF_REFRESH from synced state also runs clean _half + _preBwMid:
   const size_t refBeforeNoScrubHalf = noScrubFac._bus.refreshHistory.size();
   noScrubFac.displayGray16Base(freeink::FreeInkDisplay::HALF_REFRESH, false);
   assert(noScrubFac._bus.refreshHistory.size() == refBeforeNoScrubHalf + 2);
   assert(noScrubFac._bus.refreshHistory[refBeforeNoScrubHalf].ww0 == 0xAA);
   assert(noScrubFac._bus.refreshHistory.back().ww0 == 0x20);
   noScrubFac.releaseBuffers();
   // Null-framebuffer Gray16 base calls safely no-op and still clear any one-shot refresh idle hook:
   assert(!noScrubFac.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
   noScrubFac.displayGray16Base(freeink::FreeInkDisplay::FAST_REFRESH, false);
 }

 // 7. Verify setRefreshIdleHook one-shot lifetime across displayGray16* calls:
 {
   struct HookRecorder : freeink::Uc8253X3Driver {
     void (*hook)(void*) = nullptr;
     void* hookCtx = nullptr;
     void setRefreshIdleHook(void (*h)(void*), void* c) override {
       hook = h;
       hookCtx = c;
     }
   } hookDrv;
   freeink::FreeInkDisplay hookFac(0, 0, 0, 0, 0, 0);
   hookFac.setDisplayX3();
   hookFac.begin();
   hookFac._driver = &hookDrv;
   hookDrv.begin(hookFac._bus);
   hookFac.skipInitialResync();
   std::memcpy(hookFac.getFrameBuffer(), fb.data(), fb.size());
   auto noopHook = [](void*) {};
   int dummyCtx = 42;
   hookFac.setRefreshIdleHook(+noopHook, &dummyCtx);
   assert(hookDrv.hook != nullptr && hookDrv.hookCtx == &dummyCtx);
   assert(hookFac.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
   assert(hookDrv.hook == nullptr && hookDrv.hookCtx == nullptr);
   hookFac.waitRefreshComplete();

   hookFac.setRefreshIdleHook(+noopHook, &dummyCtx);
   hookFac.displayGray16Pass1();
   assert(hookDrv.hook == nullptr && hookDrv.hookCtx == nullptr);

   hookFac.setRefreshIdleHook(+noopHook, &dummyCtx);
   hookFac.displayGray16Pass2(false);
   assert(hookDrv.hook == nullptr && hookDrv.hookCtx == nullptr);

   hookFac.releaseBuffers();
   hookFac.setRefreshIdleHook(+noopHook, &dummyCtx);
   assert(!hookFac.displayGray16BaseAsync(freeink::FreeInkDisplay::FAST_REFRESH));
   assert(hookDrv.hook == nullptr && hookDrv.hookCtx == nullptr);
 }

 // Global DRF VCOM safety invariant: ZERO refreshes across any bus ever fired at parked 0x00!
 for (uint8_t v : bus.vcomAtRefresh) {
   assert(v == freeink::x3_lut16::kVcomDcStandard || v == freeink::x3_lut16::kVcomPass2Fine || v == 0x2Cu || v == 0x48u);
 }
 for (const auto* b : {&facade._bus, &syncFacade._bus, &asyncFacade._bus}) {
   for (uint8_t v : b->vcomAtRefresh) {
     assert(v == freeink::x3_lut16::kVcomDcStandard || v == freeink::x3_lut16::kVcomPass2Fine);
   }
 }

 syncFacade.releaseBuffers();
 asyncFacade.releaseBuffers();
 facade.releaseBuffers();

 std::cout << "PASS: UC8253 rollback boot cleans, blocking polling, gray baseline, power, Direct grayscale, and two-pass 16-gray\\n";
}
""")
    exe = root / "test_uc8253_power"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-DARDUINO=1", "-DFREEINK_DRIVER_UC8253_X3=1",
                    "-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1",
                    "-I"+str(root), "-I"+str(root / "include"), str(root / "test.cpp"),
                    str(root / "src/FreeInkDisplay.cpp"),
                    str(root / "src/driver/Uc8253X3Driver.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
