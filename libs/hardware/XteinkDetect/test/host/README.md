# Display probe regression test

From the SDK root:

```sh
mkdir -p .cache/x3-detection-build
c++ -std=c++17 -Wall -Wextra -Werror \
  -DFREEINK_DEVICE_X3=1 -DFREEINK_DEVICE_X4=1 \
  -Ilibs/hardware/XteinkDetect/test/host/stubs \
  -Ilibs/hardware/BoardConfig/include \
  -Ilibs/hardware/XteinkDetect/include \
  libs/hardware/XteinkDetect/test/host/test_display_probe.cpp \
  libs/hardware/XteinkDetect/src/XteinkDetect.cpp \
  -o .cache/x3-detection-build/test_display_probe
.cache/x3-detection-build/test_display_probe
```

The X4 Pro is an ESP32-S3 profile and can't share a binary with the C3 X3/X4,
so its cases (the BOE 0x40/0x41/0x42 panel IDs) build separately:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -DFREEINK_DEVICE_X4PRO=1 \
  -Ilibs/hardware/XteinkDetect/test/host/stubs \
  -Ilibs/hardware/BoardConfig/include \
  -Ilibs/hardware/XteinkDetect/include \
  libs/hardware/XteinkDetect/test/host/test_display_probe.cpp \
  libs/hardware/XteinkDetect/src/XteinkDetect.cpp \
  -o .cache/x3-detection-build/test_display_probe_x4pro
.cache/x3-detection-build/test_display_probe_x4pro
```

The test compiles the production detector and real board profiles against GPIO,
time, I2C, and NVS stubs. The simulated display rejects reads on the wrong clock
phase or with the wrong SDA mode and records command bytes, reset timestamps,
read lengths, and pin release. It covers stock X3 ID selection, absent FLG/MTP,
delayed/stuck BUSY, unknown IDs, direct X3 probing with the X4 boot profile,
controller promotion, and unchanged X4 VER/FLG/MTP transactions.

This verifies host protocol behavior, not the electrical behavior of a physical
display. Test affected hardware with the new `[XTDET] X3 stock probe` log,
including cold boots and sleep/wake.
