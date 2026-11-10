#!/usr/bin/env python3
"""Exercise Murphy M4's production SSD1677 batch configurations."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
LIB = HERE.parents[1]
BOARD_CONFIG = LIB.parents[1] / "hardware/BoardConfig"

with tempfile.TemporaryDirectory(prefix="freeink-murphy-m4-display-") as temporary:
    root = Path(temporary)
    for folder in ("src/driver", "src/bus", "src/lut", "include"):
        (root / folder).mkdir(parents=True, exist_ok=True)
    for name in ("Ssd1677Driver.h", "Ssd1677Driver.cpp", "PanelDriver.h"):
        shutil.copy2(LIB / "src/driver" / name, root / "src/driver" / name)
    shutil.copy2(LIB / "test/host/pro_stubs/EpdBus.h", root / "src/bus/EpdBus.h")
    shutil.copy2(LIB / "include/GrayscaleCapabilities.h", root / "include/GrayscaleCapabilities.h")
    shutil.copy2(LIB / "src/lut/Ssd1677Luts.h", root / "src/lut/Ssd1677Luts.h")
    for name in ("Arduino.h", "BoardConfig.h", "SPI.h", "esp_heap_caps.h", "sdkconfig.h"):
        shutil.copy2(LIB / "test/host/pro_stubs" / name, root / name)

    executable = root / "test_murphy_m4"
    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter",
               "-Wno-unused-function",
               "-DARDUINO=1", "-DFREEINK_DEVICE_MURPHY_M4=1", "-DFREEINK_DEVICE_METALIO_EINK4=0",
               "-DFREEINK_DEVICE_PICCO=0", "-DFREEINK_DEVICE_STICKY=0", "-I" + str(root),
               "-I" + str(root / "include"), "-I" + str(BOARD_CONFIG / "include"),
               str(HERE / "test_murphy_m4.cpp"), str(root / "src/driver/Ssd1677Driver.cpp"), "-o", str(executable)]
    subprocess.run(command, check=True)
    for batch in ("first", "second"):
        subprocess.run([str(executable), batch], check=True)
        print(f"Murphy M4 {batch}-batch SSD1677 refresh config passed")