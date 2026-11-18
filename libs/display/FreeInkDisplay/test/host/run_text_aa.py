#!/usr/bin/env python3
"""Verify optimized text AA against a scalar charge model under sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="freeink-text-aa-") as directory:
    exe = Path(directory) / "text-aa"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DBOARD_HAS_PSRAM=1",
                    "-I" + str(here / "pro_stubs"), "-I" + str(here.parents[1] / "src/driver"),
                    str(here / "test_text_aa.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
