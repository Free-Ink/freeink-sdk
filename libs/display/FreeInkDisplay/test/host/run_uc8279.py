#!/usr/bin/env python3
"""Compile the real UC8279 driver with a recording bus and host Arduino shims."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / "src"
with tempfile.TemporaryDirectory(prefix="uc8279-test-") as directory:
    root = Path(directory)
    for folder in ("src/driver", "src/bus", "src/lut", "include"):
        (root / folder).mkdir(parents=True, exist_ok=True)
    for name in ("Uc8279Driver.cpp", "Uc8279Driver.h", "PanelDriver.h"):
        shutil.copy2(SOURCE / "driver" / name, root / "src/driver" / name)
    shutil.copy2(SOURCE / "FreeInkDisplay.cpp", root / "src/FreeInkDisplay.cpp")
    shutil.copy2(SOURCE / "lut/Uc8279X3Luts.h", root / "src/lut/Uc8279X3Luts.h")
    for inc_name in ("FreeInkDisplay.h", "GrayscaleCapabilities.h"):
        shutil.copy2(SOURCE.parent / "include" / inc_name, root / "include" / inc_name)
    (root / "SPI.h").write_text("#pragma once\n")
    (root / "sdkconfig.h").write_text("#pragma once\n")
    (root / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#define HIGH 1
#define LOW 0
#define PROGMEM
#define pgm_read_byte(p) (*reinterpret_cast<const uint8_t*>(p))
#define log_e(...) std::fprintf(stderr, __VA_ARGS__)
inline unsigned long millis() { static unsigned long t; return ++t; }
inline void delay(unsigned long) {}
inline int digitalRead(int) { return 0; }
''')
    (root / "BoardConfig.h").write_text('''#pragma once
#include <cstdint>
namespace BoardConfig {
enum class Board { XteinkX3, XteinkX3Uc8279 };
enum class DisplayController { UC8253, UC8279 };
inline struct {
  uint16_t displayWidth=792, displayHeight=528;
  uint32_t displaySpiHz=0;
  struct { int8_t sclk=0, mosi=0, cs=0, dc=0, rst=0, busy=0, powerEnable=-1; } display;
  Board board = Board::XteinkX3Uc8279;
  DisplayController displayController = DisplayController::UC8279;
} ACTIVE;
constexpr uint32_t MAX_FRAMEBUFFER_BYTES = (792 / 8) * 528;
inline void selectDevice(Board b) { ACTIVE.board = b; }
}
''')
    (root / "src/bus/EpdBus.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
namespace freeink {
enum class BusyPolarity { X3TwoPhase, ActiveLowReady };
struct EpdPins { int8_t sclk=0, mosi=0, cs=0, dc=0, rst=0, busy=0, powerEnable=-1; };
class EpdBus {
  uint8_t command=0;
  bool inPartial=false;
  uint16_t ptlGateStart=0;
  size_t ptlCursor=0;
  std::vector<uint8_t> curParam;
public:
  std::vector<uint8_t> oldPlane, newPlane, lastBank, rawRegisters, lastBwBank;
  unsigned refreshes=0, powerOffs=0;
  void cmd(uint8_t c) {
    if (command == 0x90 && curParam.size() == 9) {
      ptlGateStart = static_cast<uint16_t>((curParam[4] << 8) | curParam[5]);
      ptlCursor = size_t(ptlGateStart) * 99u;
    }
    curParam.clear();
    command=c;
    if(c==0x12) ++refreshes;
    if(c==0x02) ++powerOffs;
    if(c==0x91) inPartial=true;
    if(c==0x92) inPartial=false;
    if((c==0x10 || c==0x13) && inPartial) ptlCursor = size_t(ptlGateStart) * 99u;
  }
  void data(uint8_t v) { curParam.push_back(v); }
  void data(const uint8_t* p, size_t n) {
    curParam.assign(p, p + n);
    if (command == 0x90 && n == 9) {
      ptlGateStart = static_cast<uint16_t>((p[4] << 8) | p[5]);
      ptlCursor = size_t(ptlGateStart) * 99u;
    }
    if(command == 0x20 && n == 42) lastBwBank.assign(p,p+n);
    if(n == 49) {
      if(command == 0x20) rawRegisters.clear();
      rawRegisters.push_back(command);
      if(command == 0x20) lastBank.assign(p,p+n);
    }
  }
  void cmdData(uint8_t c, const uint8_t* p, size_t n) { cmd(c); data(p,n); }
  void sendPlaneFlippedInverted(uint8_t c, const uint8_t* p, uint16_t h, uint16_t wb) {
    auto& dst = c == 0x10 ? oldPlane : newPlane;
    dst.assign(p,p+size_t(h)*wb);
    for(auto& b : dst) b = ~b;
  }
  void sendPlaneFlipped(uint8_t c, const uint8_t* p, uint16_t h, uint16_t wb) {
    (c == 0x10 ? oldPlane : newPlane).assign(p,p+size_t(h)*wb);
  }
  void fillPlane(uint8_t c, uint8_t v, uint16_t h, uint16_t wb) {
    (c == 0x10 ? oldPlane : newPlane).assign(size_t(h)*wb,v);
  }
  using StreamChunkCallback = size_t (*)(void* ctx, const uint8_t** outChunk);
  size_t sendStream(uint8_t, StreamChunkCallback, void*) { return 0; }
  void begin(const EpdPins&, uint32_t, BusyPolarity, int8_t=-1, int8_t=-1) {}
  bool isBusy() const { return false; }
  void setBusyWaitHooks(void (*)(), void (*)()) {}
  void setBusyWaitSliceHook(bool (*)(int8_t, uint8_t)) {}
  void reset(int) {}
  void waitBusy(const char*) {}
  void waitBusy(BusyPolarity, const char*) {}
  void waitRefreshComplete(const char*) {}
  EpdPins pins() const { return {}; }
  struct Transaction {
    EpdBus* owner;
    void writeBytes(const uint8_t* p, size_t n) {
      if (!owner || !owner->inPartial) return;
      auto& dst = owner->command == 0x10 ? owner->oldPlane : owner->newPlane;
      if (owner->ptlCursor + n <= dst.size()) std::memcpy(dst.data() + owner->ptlCursor, p, n);
      owner->ptlCursor += n;
    }
    void end() {}
  };
  Transaction beginTxn() { return {this}; }
};
}
''')
    exe = root / "test_uc8279"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-DARDUINO=1", "-DFREEINK_DRIVER_UC8279=1",
                    "-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1",
                    "-I"+str(root), "-I"+str(root / "include"), str(HERE / "test_uc8279.cpp"),
                    str(root / "src/FreeInkDisplay.cpp"),
                    str(root / "src/driver/Uc8279Driver.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
