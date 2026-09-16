# Metalio SC7A20H accelerometer

The board owner identified the sensor as SC7A20H. Select
`FREEINK_DEVICE_METALIO_EINK4=1` and add the `Imu` library; its capability is
enabled automatically. Applications choose how to use readings—no page-turn,
rotation or vibration behavior is attached by the SDK.

```cpp
#include <Imu.h>

Imu motion;

bool startMotion() {
  return motion.begin();
}

void pollMotion() {
  Imu::Sample sample;
  if (motion.read(sample)) {
    // sample.ax, sample.ay, sample.az: acceleration in g, in sensor axes.
    // sample.gx, sample.gy, sample.gz: zero; motion.hasGyroscope() is false.
    // Apply the application's motion policy here.
  }
}

bool suspendMotion() { return motion.sleep(); }
bool resumeMotion() { return motion.wake(); }
```

Serialize calls with other shared-I²C hardware access. `read()` returns false
before initialization, while asleep, without a fresh sample, or on a bus error;
it leaves the output unchanged on failure. Polling around every 10–20 ms is
suitable for the configured rate. Do not infer board/display orientation from
the sensor axes until its physical mounting is checked.

## Configuration and evidence

The implementation follows Silan's
[SC7A20H datasheet v1.1](https://pf.unikeyic.com/datasheet/202511/59f66cd3197e2534279e18eca6a9f806.pdf):

- Probe 0x19, then 0x18; accept WHO_AM_I `0x11` plus VERSION `0x28` before writes.
- High-performance mode, 100 Hz, ±2 g, block-data update, little-endian output.
  SC7A20H's HR bit is in `0x1F`, not the LIS3DH HR location.
- Read six bytes from auto-increment address `0xA8` after data-ready; discard the
  low four non-data bits. Section 13.13's conversion table gives `0x4000 = 1 g`.
- Disable FIFO, high-pass filtering and interrupt routing. On address 0x18,
  disable the grounded address pin's internal pull-up while preserving other bits.
- Sleep stops sampling; wake restores it and waits one sample period.

The supplied GPIO map places the interrupt on TCA9555 P1.4. This backend polls
the sensor; it does not configure an interrupt or ESP32 motion wakeup.

## Validation

```sh
sh libs/hardware/Imu/test/host/run.sh
```

Tests exercise the actual driver against a simulated I²C bus: both addresses,
wrong IDs/versions, initialization failure cleanup and retries, signed/full-scale
conversion, no-data and short-read behavior, sleep/wake errors, disabled stubs,
and existing LSM6DS3/QMI8658 behavior. The combined Metalio ESP32-S3 smoke firmware
also links the enabled driver with speaker, microphone and the other peripherals.

Physical validation is pending: verify the responding address, axis signs with
each face upward, approximately 1 g total at rest, and sleep current.
