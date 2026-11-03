#!/usr/bin/env python3
"""Compile the complete Pro drivers and display facade against a recording bus."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
LIB = HERE.parents[1]
with tempfile.TemporaryDirectory(prefix="freeink-pro-test-") as directory:
    root = Path(directory)
    for folder in ("src/driver", "src/bus", "src/lut", "include"):
        (root / folder).mkdir(parents=True, exist_ok=True)
    drivers = ("Ssd1677", "Uc8179", "Uc8279X4")
    for name in drivers:
        for ext in ("h", "cpp"):
            shutil.copy2(LIB / f"src/driver/{name}Driver.{ext}", root / f"src/driver/{name}Driver.{ext}")
    for header in (LIB / "src/driver").glob("*.h"):
        shutil.copy2(header, root / "src/driver" / header.name)
    shutil.copy2(LIB / "include/LgfxEpdConfig.h", root / "include/LgfxEpdConfig.h")
    shutil.copy2(LIB / "src/FreeInkDisplay.cpp", root / "src/FreeInkDisplay.cpp")
    shutil.copy2(LIB / "include/FreeInkDisplay.h", root / "include/FreeInkDisplay.h")
    shutil.copy2(LIB / "include/GrayscaleCapabilities.h", root / "include/GrayscaleCapabilities.h")
    for name in ("Ssd1677Luts.h", "Uc8279X3Luts.h", "UltraChipDirectGrayLuts.h"):
        shutil.copy2(LIB / f"src/lut/{name}", root / f"src/lut/{name}")
    for name in ("Arduino.h", "BoardConfig.h", "SPI.h", "esp_heap_caps.h", "sdkconfig.h"):
        shutil.copy2(HERE / "pro_stubs" / name, root / name)
    shutil.copy2(HERE / "pro_stubs/EpdBus.h", root / "src/bus/EpdBus.h")
    for single in (False, True):
        exe = root / ("single" if single else "dual")
        command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                   "-Wno-unused-parameter", "-Wno-unused-function", "-DBOARD_HAS_PSRAM=1",
                   "-DARDUINO=1", "-DFREEINK_DEVICE_METALIO_EINK4=1", "-I"+str(root), "-I"+str(root / "include")]
        if single:
            command += ["-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1"]
        command += [str(HERE / "test_pro.cpp"), str(root / "src/FreeInkDisplay.cpp")]
        command += [str(root / f"src/driver/{name}Driver.cpp") for name in drivers]
        subprocess.run(command + ["-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        subprocess.run([str(exe), "sticky"], check=True)
        subprocess.run([str(exe), "metalio"], check=True)

    # Header-check every concrete driver against the shared identity contract.
    # The IT8951 header only needs a declaration for its owned Arduino SPI reference.
    spi = root / "SPI.h"
    spi.write_text(spi.read_text() + "\nclass SPIClass;\n")
    # Compile the production selector with all three X4-family drivers enabled.
    exe = root / "controller-identity"
    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-DARDUINO=1",
               "-DFREEINK_DRIVER_SSD1677=1", "-DFREEINK_DRIVER_UC8179=1", "-DFREEINK_DRIVER_UC8279_X4=1",
               "-I" + str(root), "-I" + str(root / "include"),
               str(HERE / "test_controller_identity.cpp"), str(root / "src/FreeInkDisplay.cpp")]
    command += [str(root / f"src/driver/{name}Driver.cpp") for name in drivers]
    subprocess.run(command + ["-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print("SSD1677/UC8179/UC8279 facade controller identity and unselected state passed")
