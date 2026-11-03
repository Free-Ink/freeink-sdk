# Adding touch: devices and controllers

Touch support is split between `BoardConfig.h`, which says which controller
each device uses and which drivers a build compiles, and `InputManager`, which
holds the drivers. A build compiles only the drivers its selected
`FREEINK_DEVICE_*` profiles need, so a new device or controller has to be
registered in several places. This page lists every one.

## A new device that uses an existing controller

All three edits are in `libs/hardware/BoardConfig/include/BoardConfig.h`.

1. **`FREEINK_TOUCH_DEVICE_SELECTED`**: add `FREEINK_DEVICE_<NAME>`. This list
   drives the `FREEINK_CAP_TOUCH` default and the
   `FREEINK_TOUCH_ALL_DRIVERS` fallback.
2. **`FREEINK_TOUCH_<CONTROLLER>`**: add `FREEINK_DEVICE_<NAME>` to the flag of
   the controller the profile names, and add the device to the comment on that
   flag's `#ifndef` line.
3. **`FREEINK_CHECK_TOUCH_DRIVER`**: add
   `FREEINK_CHECK_TOUCH_DRIVER(FREEINK_DEVICE_<NAME>, <PROFILE>);` next to the
   other lines after `DEFAULT_DEVICE`. Add one line per profile the device can
   make `ACTIVE`, and add it for **touchless devices too**, so the check starts
   working if the profile gains a controller later.

## A new controller

Do everything in the device section above, plus the following.

`BoardConfig.h`:

4. **`enum class TouchController`**: append the new value at the end. Name it
   after the controller chip (`Gt911`, `Tma525c`), not after the board.
5. **The flag**: add an `#ifndef FREEINK_TOUCH_<CHIP>` block next to the other
   driver flags, written in the same form:
   `(FREEINK_CAP_TOUCH && (FREEINK_TOUCH_ALL_DRIVERS || FREEINK_DEVICE_<NAME>))`.
6. **`touchDriverCompiled()`**: add a `case` that returns the new flag. The
   switch has no `default`, so `-Wall` warns when a value is missing.

`libs/hardware/InputManager/`:

7. **`include/InputManager.h`**: declare the driver's `begin<Chip>()` /
   `poll<Chip>(unsigned long now)` and any helpers. Declarations are not
   gated; only definitions are.
8. **`src/InputManager.cpp`**:
   - Wrap the driver's definitions and its file-local data in
     `#if FREEINK_TOUCH_<CHIP>` … `#endif  // FREEINK_TOUCH_<CHIP>`. If two
     drivers share helpers, gate the helpers on the OR of both flags. The
     FT5x06 register helpers, used by FT5x06 and CST816S, are an example.
   - Add a gated branch in `beginTouch()` and a gated `case` in
     `serviceTouch()`. A controller with no branch falls through to the
     "no driver" log and touch stays off, so a missing branch never shows up
     as a build error.
   - If the controller needs a sleep command or pin parking before deep
     sleep, add it to `prepareForDeepSleep()`.
   - Check the per-controller special cases for one the new chip also needs:
     the controller-specific multi-contact queries, and CST816S's virtual buttons in
     `serviceTouch()`.
9. **Host test**: add `test/host/run_<device>_touch.sh` and its stubs, using
   `run_picco_touch.sh` or `run_eego.sh` as the template. Then run every
   InputManager suite plus the other suites that compile `BoardConfig.h`
   (AudioManager, HapticManager, BatteryMonitor, Imu). See
   [testing.md](testing.md).

Docs:

10. Update the **Capacitive touch** paragraph and the device's row in the
    **Supported devices** table in the root `README.md`.

## What happens when a step is missed

| Missed | Result |
|---|---|
| 1, `FREEINK_TOUCH_DEVICE_SELECTED`, device built alone | Touch is off by default (`FREEINK_CAP_TOUCH` is 0). If you force it on with `-D`, every driver is kept. |
| 2, controller flag, device built alone | Build error from the step 3 `static_assert`, or from the `DEFAULT_DEVICE` assert if step 3 was also missed. |
| 2 and 3, device sharing a binary with listed devices and not `DEFAULT_DEVICE` | **No build error.** The driver is compiled out, and touch logs `[touch] no driver for touch controller N` and stays off. This is the case the checklist guards against. |
| 6, `touchDriverCompiled()` case | `-Wswitch` warning, and the `static_assert`s fail for that controller. |
| 8, dispatch branch | Builds, but touch logs the missing-driver line and stays off. |

## Changing the controller at runtime

The driver flags only know about the profiles compiled into the build. Code
that writes a different `ACTIVE.touch.controller` after boot, for example after
probing which digitizer is fitted, must build with
`-DFREEINK_TOUCH_<CHIP>=1` for each controller it may switch to, or with
`-DFREEINK_TOUCH_ALL_DRIVERS=1`.
