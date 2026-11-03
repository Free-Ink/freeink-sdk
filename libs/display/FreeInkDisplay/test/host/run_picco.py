#!/usr/bin/env python3
"""Compile the SSD1677/SSD2677 drivers as Picco and Sticky builds and check each panel."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
LIB = HERE.parents[1]


def build(board, device_macro, test_cpp, directory):
    """Build test_cpp against the SSD1677/SSD2677 drivers as a `board` build."""
    root = Path(directory)
    for folder in ("src/driver", "src/bus", "src/lut", "include"):
        (root / folder).mkdir(parents=True, exist_ok=True)
    for name in ("Ssd1677", "Ssd2677"):
        for ext in ("h", "cpp"):
            shutil.copy2(LIB / f"src/driver/{name}Driver.{ext}", root / f"src/driver/{name}Driver.{ext}")
    shutil.copy2(LIB / "src/driver/PanelDriver.h", root / "src/driver/PanelDriver.h")
    shutil.copy2(LIB / "include/GrayscaleCapabilities.h", root / "include/GrayscaleCapabilities.h")
    for name in ("Ssd1677Luts.h", "Ssd2677Luts.h"):
        shutil.copy2(LIB / f"src/lut/{name}", root / f"src/lut/{name}")
    shutil.copy2(HERE / "pro_stubs/EpdBus.h", root / "src/bus/EpdBus.h")
    for name in ("esp_heap_caps.h", "sdkconfig.h"):
        shutil.copy2(HERE / "pro_stubs" / name, root / name)
    # SPI stub with the shared-bus pin re-attach the Sticky panel probe uses.
    (root / "SPI.h").write_text((HERE / "pro_stubs/SPI.h").read_text() + """
struct spi_t {};
inline int fakeSpiRestarts;
struct FakeSpi {
  spi_t* bus() { static spi_t s; return &s; }
  int8_t pinSS() { return -1; }
  void end() {}
  bool begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss) { fakeSpiRestarts += sck == 12 && miso == 40 && mosi == 11 && ss == -1; return true; }
};
inline FakeSpi SPI;
""")
    board_h = (HERE / "pro_stubs/BoardConfig.h").read_text().replace("MetalioEInk4 };", "MetalioEInk4, Picco };")
    (root / "BoardConfig.h").write_text(board_h.replace("Board board=Board::XteinkX4Pro;", "struct { int8_t miso=40; } sd;\n  " + f"Board board=Board::{board};").replace(
        "inline bool isX4Classic() { return false; }",
        "inline bool isX4Classic() { return false; }\ninline bool isPicco() { return ACTIVE.board == Board::Picco; }"))
    # Arduino stub plus a fake SSD1677 that answers CMD 0x2E with fakePanelId.
    arduino = (HERE / "pro_stubs/Arduino.h").read_text().replace("inline int digitalRead(int) { return 0; }\n", "")
    arduino += """
#define LOW 0
#define INPUT 1
#define OUTPUT 3
#define INPUT_PULLUP 5
inline uint8_t fakePanelId[10];
inline uint8_t fakePanelRev[3];
inline int fakePanelCommand = -1;
inline int fakeLevels[64], fakeModes[64], fakeBit, fakeShift;
inline void delayMicroseconds(unsigned) {}
typedef int gpio_num_t;
inline bool fakeHoldReleased[64];
inline void gpio_hold_dis(gpio_num_t p) { fakeHoldReleased[p] = true; }
inline void pinMode(int p, int m) { fakeModes[p] = m; }
inline void digitalWrite(int p, int v) {
  const auto& d = BoardConfig::ACTIVE.display;
  if (p == d.cs && !v && fakeLevels[p]) fakeBit = fakeShift = 0;  // new transaction
  const bool rising = p == d.sclk && v && !fakeLevels[p];
  fakeLevels[p] = v;
  if (!rising || fakeLevels[d.cs]) return;
  if (!fakeLevels[d.dc]) {  // command phase: shift in MOSI
    fakeShift = (fakeShift << 1) | fakeLevels[d.mosi];
    if (++fakeBit == 8) { fakePanelCommand = fakeShift & 0xFF; fakeBit = 0; }
  } else if (fakeModes[d.mosi] != OUTPUT) {
    ++fakeBit;  // read phase: next bit on each clock
  }
}
inline int digitalRead(int p) {
  const auto& d = BoardConfig::ACTIVE.display;
  if (p != d.mosi || fakeModes[p] == OUTPUT) return 0;
  if (fakePanelCommand == 0x70) return (fakePanelRev[(fakeBit / 8) % 3] >> (7 - fakeBit % 8)) & 1;
  if (fakePanelCommand != 0x2E) return 0;
  return (fakePanelId[(fakeBit / 8) % 10] >> (7 - fakeBit % 8)) & 1;
}
struct FakeSerial { explicit operator bool() const { return false; } template <class... A> void printf(const char*, A...) {} void flush() {} };
inline FakeSerial Serial;
"""
    (root / "Arduino.h").write_text('#include "BoardConfig.h"\n' + arduino)
    shutil.copy2(LIB / "src/FreeInkDisplay.cpp", root / "src/FreeInkDisplay.cpp")
    shutil.copy2(LIB / "include/FreeInkDisplay.h", root / "include/FreeInkDisplay.h")
    exe = root / board.lower()
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    "-Wno-unused-function", "-Wno-unused-variable", "-DARDUINO=1", f"-D{device_macro}=1", "-DFREEINK_DRIVER_SSD1677=1",
                    "-I" + str(root), "-I" + str(root / "include"), str(HERE / test_cpp),
                    str(root / "src/FreeInkDisplay.cpp"), str(root / "src/driver/Ssd1677Driver.cpp"),
                    str(root / "src/driver/Ssd2677Driver.cpp"), "-o", str(exe)], check=True)
    return exe


with tempfile.TemporaryDirectory(prefix="freeink-picco-test-") as directory:
    exe = build("Picco", "FREEINK_DEVICE_PICCO", "test_picco.cpp", directory)
    for panel in ("yrd", "depg", "opm", "unknown", "se0400"):
        subprocess.run([str(exe), panel], check=True)
with tempfile.TemporaryDirectory(prefix="freeink-sticky-test-") as directory:
    exe = build("Sticky", "FREEINK_DEVICE_STICKY", "test_sticky_ssd2677.cpp", directory)
    subprocess.run([str(exe)], check=True)
    subprocess.run([str(exe), "no-old-plane"], check=True)
    subprocess.run([str(exe), "ssd1677"], check=True)
