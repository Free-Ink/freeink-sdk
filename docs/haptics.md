# Haptic feedback

`HapticManager` is a consumer-controlled vibration output. Firmware chooses
when to pulse or play a pattern; InputManager and FreeInkUI do not trigger it.
The first backend is Metalio E-Ink 4's active-high motor on GPIO44.

## Setup

Add the library alongside BoardConfig in your consumer's PlatformIO dependencies:

```ini
lib_deps =
  BoardConfig=symlink://freeink-sdk/libs/hardware/BoardConfig
  HapticManager=symlink://freeink-sdk/libs/hardware/HapticManager
```

`FREEINK_CAP_HAPTIC` defaults on for Metalio. Setting it to `0` builds inert
methods without timer/PWM dependencies. Other boards default to no haptics;
enabling the capability alone does not assign a motor pin. The board profile's
`haptic` field specifies the GPIO, polarity and PWM frequency. Select the active
board before `begin()`; call `end()` before switching boards.

```cpp
#include <HapticManager.h>

auto& haptic = HapticManager::getInstance();

void setupHaptics() {
  if (!haptic.begin()) return;  // unsupported board or initialization failed
  haptic.setIntensity(200);    // master intensity, 0..255
}

// Call these from whichever consumer events should have feedback.
void onAcceptedAction() {
  haptic.pulse(35, 255);       // 35 ms, full intensity scaled by the master level
}

void onNotification() {
  const HapticManager::Step pattern[] = {
      {30, 180},              // duration in milliseconds, intensity 0..255
      {60, 0},                // silent pause
      {50, 255},
  };
  haptic.play(pattern, 3, 2);  // play this pattern twice
}
```

## Playback contract

- `begin()` attaches PWM and starts silent. `supported()` checks the board and
  build capability; `present()` indicates successful initialization.
- `pulse(durationMs = 35, intensity = 255)` and `play(steps, count, repeats = 1)`
  return immediately after scheduling. They require successful initialization and
  `enabled() == true`, and return whether playback started.
- A successful request replaces the active pattern. Up to 32 steps are copied
  into the manager, so the caller may reuse or destroy its array after `play()`.
  There is no allocation per pulse/pattern and no application `update()` loop.
- Zero-intensity steps are pauses. Zero-duration steps, zero repeats, null/empty
  arrays, and patterns longer than 32 steps are rejected without changing current
  playback. Repeats are finite (1–65535). Ramps can be expressed as successive
  steps with different intensities.
- `setIntensity(0..255)` scales every step, including one already playing.
  Zero mutes the output while the timeline continues. This is PWM drive duty,
  not a calibrated measure of perceived vibration strength.
- `stop()` immediately cancels playback and silences the motor.
  `setEnabled(false)` also stops and rejects new requests; enabling does not
  resume a cancelled pattern. Settings are in RAM; the consumer owns persistence.
- `isPlaying()` includes silent pauses. Timer/PWM failures stop playback. A lost
  PWM output is detached and driven inactive; `present()` then becomes false.
- `end()` stops and releases the PWM channel. Call it **before sleep, power-off,
  or assigning the pin to another peripheral**. `begin()` can attach again.
  One timer and mutex remain allocated for the singleton's lifetime so queued
  callbacks never reference a destroyed object.

Calls are serialized with the timer task and may be made from application tasks,
including an input worker. Do not call them from interrupt handlers. ESP timer
dispatch is not hard real-time: a busy timer task can delay a transition. On late
dispatch, elapsed steps are skipped instead of replaying a burst of old pulses.

## Metalio hardware notes

Metalio uses 8-bit LEDC PWM at 20 kHz. Full intensity is the same continuous HIGH
drive used by the vendor's 35 ms pulse. Reduced-intensity PWM and motor response
still need physical validation; very low duty cycles may not start the motor.
The frequency is board configuration, not a consumer pattern's repetition rate.

The old `freeink::metalio::vibrate()` helper remains for compatibility. It blocks
and writes GPIO44 directly; **do not mix it with HapticManager while PWM is
attached**. Use `pulse()` for new integrations. Motor startup through HapticManager
first completes Metalio board bring-up so a later display/input initialization
does not reconfigure the PWM pin.

The driver uses [Arduino-ESP32 LEDC](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/ledc.html)
and task-dispatched [ESP timers](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/esp_timer.html).

## Verification

```sh
sh libs/hardware/HapticManager/test/host/run.sh
```

The tests compile the production manager, board profiles and InputManager with
recording PWM/timer adapters. They cover intensity and polarity, copied patterns,
repeats, pauses, replacement, delayed/stale callbacks, stop/disable/end,
initialization and playback failures, and disabled/absent hardware. They also
verify that screen and cover-key events do not automatically vibrate.
