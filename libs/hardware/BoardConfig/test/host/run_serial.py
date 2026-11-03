#!/usr/bin/env python3
"""Check native USB TX servicing and no-op selection for other transports."""
from pathlib import Path
import subprocess
import tempfile

INCLUDE = Path(__file__).resolve().parents[2] / "include"

with tempfile.TemporaryDirectory(prefix="freeink-serial-test-") as tmp:
    path = Path(tmp)
    (path / "hal").mkdir()
    (path / "BoardConfig.h").write_text("#pragma once\n")
    (path / "hal/usb_serial_jtag_ll.h").write_text(
        "#pragma once\nextern unsigned kicks;\n"
        "inline void usb_serial_jtag_ll_txfifo_flush() { ++kicks; }\n")
    (path / "test.cpp").write_text(
        "#include <SerialTransport.h>\n#include <cassert>\n"
        "unsigned kicks=0; int main() { BoardConfig::serviceSerialTx(); assert(kicks==EXPECTED); }\n")
    for name, mode, cdc, sticky, soc, simulator, expected in [
        ("native USB", 1, 1, 0, 1, 0, 1),
        ("TinyUSB", 0, 1, 0, 1, 0, 0),
        ("UART", 1, 0, 0, 1, 0, 0),
        ("Sticky bridge", 1, 1, 1, 1, 0, 0),
        ("MCU without USB Serial/JTAG", 1, 1, 0, 0, 0, 0),
        ("simulator", 1, 1, 0, 1, 1, 0),
    ]:
        flags = [f"-DARDUINO_USB_MODE={mode}", f"-DARDUINO_USB_CDC_ON_BOOT={cdc}",
                 f"-DFREEINK_DEVICE_STICKY={sticky}", f"-DSOC_USB_SERIAL_JTAG_SUPPORTED={soc}",
                 f"-DEXPECTED={expected}"]
        if simulator:
            flags.append("-DSIMULATOR")
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(path), "-I" + str(INCLUDE), str(path / "test.cpp"),
                        "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
        print(name + ": passed")
