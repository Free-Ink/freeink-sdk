#!/usr/bin/env python3
"""Run A4 refresh-policy checks against the real driver with a recording bus."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / "src"
with tempfile.TemporaryDirectory(prefix="uc8279c-a4-test-") as directory:
    root = Path(directory)
    for folder in ("driver", "bus", "lut"):
        (root / folder).mkdir()
    for name in ("Uc8279cA4Driver.cpp", "Uc8279cA4Driver.h", "PanelDriver.h"):
        shutil.copy2(SOURCE / "driver" / name, root / "driver" / name)
    shutil.copy2(SOURCE / "lut/Uc8279cA4Luts.h", root / "lut/Uc8279cA4Luts.h")
    shutil.copy2(SOURCE.parent / "include/GrayscaleCapabilities.h", root / "GrayscaleCapabilities.h")
    panel = root / "driver/PanelDriver.h"
    panel.write_text(panel.read_text().replace("../../include/GrayscaleCapabilities.h", "../GrayscaleCapabilities.h"))
    (root / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#define HIGH 1
#define LOW 0
inline void digitalWrite(int, int) {}
#define PROGMEM
#define log_e(...) std::fprintf(stderr, __VA_ARGS__)
inline unsigned long millis() { static unsigned long t; return ++t; }
inline void delay(unsigned long) {}
inline int digitalRead(int) { return 0; }
''')
    (root / "BoardConfig.h").write_text('''#pragma once
namespace BoardConfig {
inline struct { unsigned short displayWidth=768, displayHeight=552; unsigned displaySpiHz=0; } ACTIVE;
}
''')
    (root / "bus/EpdBus.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace freeink {
enum class BusyPolarity { ActiveLow };
class EpdBus {
  uint8_t command=0;
public:
  unsigned refreshes=0, powerOffs=0;
  uint8_t interval=0;
  size_t lutBytes=0;
  std::vector<std::vector<uint8_t>> lut20s;
  std::vector<uint8_t> oldPlane, newPlane, oldAtRefresh, newAtRefresh;
  void cmd(uint8_t c) {
    command=c;
    if(c==0x10) oldPlane.clear();
    if(c==0x13) newPlane.clear();
    if(c==0x02) ++powerOffs;
    if(c==0x12) {
      ++refreshes;
      oldAtRefresh=oldPlane;
      newAtRefresh=newPlane;
    }
  }
  void cmdData(uint8_t c, const uint8_t* p, size_t n) {
    if(c==0x50 && n==1) interval=*p;
    if(c==0x20) { lutBytes=n; lut20s.emplace_back(p, p+n); }
    cmd(c);
  }
  void fillPlane(uint8_t c, uint8_t value, uint16_t height, uint16_t widthBytes) {
    cmd(c);
    (c==0x10 ? oldPlane : newPlane).assign(size_t(height)*widthBytes,value);
  }
  void waitBusy(const char*) {}
  struct Pins { int8_t rst=0; };
  Pins pins() const { return {}; }
  struct Txn { EpdBus* b; void writeBytes(const uint8_t* p, size_t n) { b->rawWriteBytes(p, n); } };
  Txn beginTxn() { return {this}; }
  void rawWriteBytes(const uint8_t* p, size_t n) {
    if(command==0x10 || command==0x13) {
      auto& plane=command==0x10 ? oldPlane : newPlane;
      plane.insert(plane.end(),p,p+n);
    }
  }
};
}
''')
    (root / "esp_heap_caps.h").write_text("#pragma once\n#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\ninline void* heap_caps_malloc(size_t n, int) { return malloc(n); }\n")
    exe = root / "test_uc8279c_a4"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-I"+str(root), str(HERE / "test_uc8279c_a4.cpp"),
                    str(root / "driver/Uc8279cA4Driver.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
