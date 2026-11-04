#!/usr/bin/env python3
"""Compile the real Murphy UC8253 driver with a recording bus and host Arduino shims."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / "src"
STUBS = HERE / "pro_stubs"
with tempfile.TemporaryDirectory(prefix="uc8253_murphy-test-") as directory:
    root = Path(directory)
    for folder in ("driver", "bus", "lut"):
        (root / folder).mkdir()
    for name in ("Uc8253MurphyDriver.cpp", "Uc8253MurphyDriver.h", "PanelDriver.h"):
        shutil.copy2(SOURCE / "driver" / name, root / "driver" / name)
    shutil.copy2(SOURCE / "lut/Uc8253MurphyLuts.h", root / "lut/Uc8253MurphyLuts.h")
    shutil.copy2(SOURCE.parent / "include/GrayscaleCapabilities.h", root / "GrayscaleCapabilities.h")
    panel = root / "driver/PanelDriver.h"
    panel.write_text(panel.read_text().replace("../../include/GrayscaleCapabilities.h", "../GrayscaleCapabilities.h"))
    shutil.copy2(STUBS / "EpdBus.h", root / "bus/EpdBus.h")
    for name in ("Arduino.h", "esp_heap_caps.h", "BoardConfig.h"):
        shutil.copy2(STUBS / name, root / name)
    subprocess.check_call(
        [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wno-unused-parameter", f"-I{root}",
         str(HERE / "test_uc8253_murphy.cpp"), str(root / "driver/Uc8253MurphyDriver.cpp"),
         "-o", str(root / "test")]
    )
    subprocess.check_call([root / "test"])
