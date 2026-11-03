#!/usr/bin/env python3
"""Paper Mono bring-up (PaperMonoBoard.h) against a model PMIC and IO expander."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
hardware = here.parents[2]
stubs = hardware / "BatteryMonitor/test/host/stubs"
SCENARIOS = ("healthy", "late-expander", "sd-first-late-expander", "missing-expander", "partial-setup-failure",
             "touch-mode-repair", "failed-read-during-touch", "touch-never-verifies")

with tempfile.TemporaryDirectory(prefix="papermono-boot-") as temporary:
    exe = Path(temporary) / "papermono_boot"
    args = [os.environ.get("CXX", "c++"), "-std=c++20", "-O0", "-Wall", "-Wextra",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DFREEINK_DEVICE_PAPERMONO=1",
            "-I", str(stubs), "-I", str(hardware / "BoardConfig/include")]
    subprocess.run([*args, str(here / "papermono_boot_test.cpp"), "-o", str(exe)], check=True)
    for scenario in SCENARIOS:
        subprocess.run([str(exe), scenario], check=True)
