#!/usr/bin/env python3
"""Run E1001 (UC8179 7.5") refresh checks against the real driver with a recording bus."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / "src"
with tempfile.TemporaryDirectory(prefix="e1001-test-") as directory:
    root = Path(directory)
    for folder in ("driver", "bus"):
        (root / folder).mkdir()
    for name in ("Uc8179E1001Driver.cpp", "Uc8179E1001Driver.h", "PanelDriver.h"):
        shutil.copy2(SOURCE / "driver" / name, root / "driver" / name)
    shutil.copy2(SOURCE.parent / "include/GrayscaleCapabilities.h", root / "GrayscaleCapabilities.h")
    panel = root / "driver/PanelDriver.h"
    panel.write_text(panel.read_text().replace("../../include/GrayscaleCapabilities.h", "../GrayscaleCapabilities.h"))
    (root / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
''')
    (root / "BoardConfig.h").write_text('''#pragma once
#define FREEINK_DRIVER_UC8179_E1001 1
namespace BoardConfig {
inline struct { unsigned short displayWidth=800, displayHeight=480; unsigned displaySpiHz=0; } ACTIVE;
}
''')
    (root / "bus/EpdBus.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace freeink {
enum class BusyPolarity { UcIdleHigh };
// Records the command stream. Bytes sent after DTM1 (0x10) / DTM2 (0x13) land in
// the matching plane; PTL (0x90) bytes are kept as the last partial window.
class EpdBus {
  uint8_t command=0;
public:
  std::vector<uint8_t> commands, ptl, lastPayload;
  std::vector<std::vector<uint8_t>> luts;  // payloads of 0x20..0x25, in order
  std::vector<uint8_t> oldPlane, newPlane, oldAtRefresh, newAtRefresh;
  unsigned refreshes=0, powerOns=0, powerOffs=0, deepSleeps=0, resets=0;
  bool powered=false, partialWindow=false, partialAtRefresh=false;
  void cmd(uint8_t c) {
    command=c;
    commands.push_back(c);
    lastPayload.clear();
    if(c==0x10) oldPlane.clear();
    if(c==0x13) newPlane.clear();
    if(c==0x90) ptl.clear();
    if(c==0x91) partialWindow=true;
    if(c==0x92) partialWindow=false;
    if(c>=0x20 && c<=0x25) luts.emplace_back();
    if(c==0x04) { ++powerOns; powered=true; }
    if(c==0x02) { ++powerOffs; powered=false; }
    if(c==0x07) ++deepSleeps;
    if(c==0x12) {
      ++refreshes;
      oldAtRefresh=oldPlane;
      newAtRefresh=newPlane;
      partialAtRefresh=partialWindow;
    }
  }
  void data(uint8_t d) { write(&d, 1); }
  void write(const uint8_t* p, size_t n) {
    lastPayload.insert(lastPayload.end(), p, p+n);
    if(command==0x10) oldPlane.insert(oldPlane.end(), p, p+n);
    if(command==0x13) newPlane.insert(newPlane.end(), p, p+n);
    if(command==0x90) ptl.insert(ptl.end(), p, p+n);
    if(command>=0x20 && command<=0x25) luts.back().insert(luts.back().end(), p, p+n);
  }
  void reset(int) { ++resets; }
  void waitBusy(const char*) {}
  void waitRefreshComplete(const char*) {}
  struct Txn { EpdBus* b; void writeBytes(const uint8_t* p, uint16_t n) { b->write(p, n); } };
  Txn beginTxn() { return {this}; }
};
}
''')
    (root / "esp_heap_caps.h").write_text("""#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
inline void* heap_caps_malloc(size_t n, int) { return malloc(n); }
""")
    exe = root / "test_e1001"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-I"+str(root), str(HERE / "test_e1001.cpp"),
                    str(root / "driver/Uc8179E1001Driver.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
