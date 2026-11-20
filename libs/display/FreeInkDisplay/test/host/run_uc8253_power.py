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
    for folder in ("driver", "bus", "lut"):
        (root / folder).mkdir()
    for name in ("Uc8253X3Driver.cpp", "Uc8253X3Driver.h", "PanelDriver.h"):
        shutil.copy2(SOURCE / "driver" / name, root / "driver" / name)
    shutil.copy2(SOURCE / "lut/Uc8253X3Luts.h", root / "lut/Uc8253X3Luts.h")
    shutil.copy2(SOURCE / "lut/UltraChipDirectGrayLuts.h", root / "lut/UltraChipDirectGrayLuts.h")
    shutil.copy2(SOURCE.parent / "include/GrayscaleCapabilities.h", root / "GrayscaleCapabilities.h")
    panel = root / "driver/PanelDriver.h"
    panel.write_text(panel.read_text().replace("../../include/GrayscaleCapabilities.h", "../GrayscaleCapabilities.h"))
    (root / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#define HIGH 1
#define PROGMEM
#define log_e(...) std::fprintf(stderr, __VA_ARGS__)
inline unsigned long millis() { static unsigned long t; return ++t; }
inline void delay(unsigned long) {}
inline int digitalRead(int) { return 0; }
''')
    (root / "BoardConfig.h").write_text('''#pragma once
namespace BoardConfig {
inline struct { unsigned short displayWidth=792, displayHeight=528; unsigned displaySpiHz=0; } ACTIVE;
}
''')
    (root / "bus/EpdBus.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>
namespace freeink {
enum class BusyPolarity { X3TwoPhase };
class EpdBus {
  uint8_t command=0;
public:
  std::vector<uint8_t> oldPlane, newPlane, lastBank, rawRegisters;
  unsigned powerOns=0, refreshes=0, pollingRefreshWaits=0, interruptWaits=0;
  // Per refresh: bit 0 set when DTM2 (new) was all white, bit 1 when DTM1 (old) was.
  std::vector<uint8_t> planesAtRefresh;
  static bool white(const std::vector<uint8_t>& p) { for (uint8_t b : p) if (b != 0xFF) return false; return !p.empty(); }
  void cmd(uint8_t c) {
    command=c; if(c == 4) ++powerOns;
    if(c == 0x12) { ++refreshes; planesAtRefresh.push_back(uint8_t((white(newPlane) ? 1 : 0) | (white(oldPlane) ? 2 : 0))); }
  }
  void cmdData2(uint8_t c, uint8_t a, uint8_t b) { cmd(c); data(a); data(b); }
  void data(uint8_t) {}
  void data(const uint8_t* p, size_t n) {
    if(n == 42 || n == 49) {
      if(command == 0x20) rawRegisters.clear();
      rawRegisters.push_back(command);
      if(command == 0x20) lastBank.assign(p,p+n);
    }
  }
  void cmdData(uint8_t c, const uint8_t* p, size_t n) { cmd(c); data(p,n); }
  void sendPlaneFlipped(uint8_t c, const uint8_t* p, uint16_t h, uint16_t wb) {
    (c == 0x10 ? oldPlane : newPlane).assign(p,p+size_t(h)*wb);
  }
  void fillPlane(uint8_t c, uint8_t v, uint16_t h, uint16_t wb) {
    (c == 0x10 ? oldPlane : newPlane).assign(size_t(h)*wb,v);
  }
  void reset(int) {}
  void waitBusy(const char* tag) { if (!std::strcmp(tag, " X3_DRF")) ++pollingRefreshWaits; }
  void waitRefreshComplete(const char*) { ++interruptWaits; }
  struct Pins { int8_t busy=0; };
  Pins pins() const { return {}; }
  struct Transaction { void writeBytes(const uint8_t*, size_t) {} void end() {} };
  Transaction beginTxn() { return {}; }
};
}
''')
    (root / "test.cpp").write_text("""
#include <cassert>
#include <vector>
#include <iostream>
#include "driver/Uc8253X3Driver.h"
int main() {
 freeink::EpdBus bus; freeink::Uc8253X3Driver d;
 const auto caps = d.grayscaleCapabilities();
 assert(caps.supported() && caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
 assert(d.grayscaleCapabilities(freeink::GrayscaleMode::Direct).base == freeink::GrayscaleBase::Combined);
 std::vector<uint8_t> fb(792/8*528, 0xAA); d.begin(bus);
 // Rollback: both splash and first real screen clean; only the third paint is fast.
 assert(!d.supportsAsyncDisplay());
 assert(!d.displayStart(bus,fb.data(),nullptr,freeink::RefreshMode::Fast,false));
 assert(bus.refreshes == 2 && bus.pollingRefreshWaits == 2);
 assert(bus.oldPlane == fb && bus.newPlane == fb);
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
   assert(bus.newPlane == fb && bus.oldPlane == fb);
 }
 // A grayscale base after an AA page must not run as a plain differential. The reader's AA
 // pass leaves greys on the glass that cleanupGrayscaleBuffers() restores the RAM around but
 // does not remove; the differential base then gives those pixels the gentle WW settle and the
 // absolute gray pass idles the endpoints, so the page's text outline survives onto the next
 // grayscale image (X3, 2026-09-23). Nor may it go straight to the new base: a _half gives
 // the page's black text the same 24 frames of black as the white around it, and the text
 // shows through a cover's light gray (X3, 2026-11-08). The base scrubs the panel to white
 // first, then runs the differential from that white: two refreshes, the first with both
 // planes white, the second with the new frame against a white DTM1.
 const auto absolute = freeink::GrayscaleMode::Absolute;
 const auto aaPage = [&] {
   d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Fast,false);        // B/W base, as the reader pushes it
   d.copyGrayscaleLsb(bus, fb.data());
   d.copyGrayscaleMsb(bus, fb.data());
   d.displayGray(bus, fb.data(), false, nullptr, false);                      // overlay gc nudge: greys now on glass
   d.cleanupGrayscaleBuffers(bus, fb.data());                                 // RAM restored; glass untouched
 };
 aaPage();
 auto before = bus.refreshes;
 d.beginGrayscale(bus, fb.data(), absolute, freeink::RefreshMode::Half, false);
 assert(bus.refreshes == before + 2);
 assert(bus.planesAtRefresh[before] == 3);                                    // the scrub: both planes white
 assert(bus.planesAtRefresh[before + 1] == 2);                                // the base, from a white DTM1
 assert(bus.oldPlane == fb);                                                  // DTM1 mirrors the base again
 d.copyGrayscaleLsb(bus, fb.data());
 d.copyGrayscaleMsb(bus, fb.data());
 d.displayGray(bus, fb.data(), false, nullptr, true);                         // absolute pass: greys on glass again
 d.cleanupGrayscaleBuffers(bus, fb.data());
 // A half drives every pixel to its target, which is what retires the claim: the next
 // grayscale base may diff again.
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Half,false);
 before = bus.refreshes;
 d.beginGrayscale(bus, fb.data(), absolute, freeink::RefreshMode::Half, false);
 assert(bus.refreshes == before + 1);                                         // differential base
 assert(bus.planesAtRefresh[before] == 0);                                    // no scrub
 d.copyGrayscaleLsb(bus, fb.data());
 d.copyGrayscaleMsb(bus, fb.data());
 d.displayGray(bus, fb.data(), false, nullptr, true);
 d.cleanupGrayscaleBuffers(bus, fb.data());
 // A caller that asks for Full keeps the full path: its first refresh writes the new frame.
 aaPage();
 before = bus.refreshes;
 d.beginGrayscale(bus, fb.data(), absolute, freeink::RefreshMode::Full, false);
 assert(bus.refreshes > before + 1 && (bus.planesAtRefresh[before] & 1) == 0);
 std::cout << "PASS: UC8253 cold power-on, warm Full, power-off/wake, gray base scrubs after AA\\n";
}
""")
    exe = root / "test_uc8253_power"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-I"+str(root), str(root / "test.cpp"),
                    str(root / "driver/Uc8253X3Driver.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
