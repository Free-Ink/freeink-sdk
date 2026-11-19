# Agent notes

Changes that have to be repeated in several files. Follow the linked checklist
before calling any of them done.

- **New touch device or touch controller**: every edit is listed in
  [docs/adding-a-touch-controller.md](docs/adding-a-touch-controller.md). A missed
  step can build cleanly and ship a touchscreen that does nothing.
- **New device (any)**: see "Adding a new device" in [README.md](README.md).
- **Host tests**: [docs/testing.md](docs/testing.md). Run the suites for every
  library you touched, plus the suites that compile `BoardConfig.h` when you edit it.

## Flash size: unused features must cost nothing

Flash is limited on the devices this SDK targets, and every firmware that
links it pays for shared code. A new optional feature must not grow builds
that do not use it. Before adding one to shared
code, check each of these, and prefer a design that avoids the cost over one
that needs measuring:

- **Templates** (`list()`, `Frame<N>` and anything else instantiated per
  caller): code added inside a template is compiled into every instantiation.
  Put anything that does not depend on the template parameter in a
  non-template function and call it. A 10-line branch added to `list()` cost
  about 250 bytes per instantiation; moving the whole value-slot drawing into
  one shared function made every instantiation smaller.
- **Per-item and per-call structs** (`ListItem`, `ListProps` and similar):
  every field is initialized at every construction site and enlarges every
  array, vector and stack frame that holds one. Do not add a by-value
  sub-struct (`BitmapRef`, `TextStyle`, `AssetRef`) for an optional feature.
  Use a pointer that defaults to `nullptr`, or reuse an existing field.
- **Header-only `inline` functions** with more than a few lines that many
  callers use: mark them `__attribute__((noinline))` so there is one copy.
- **Device-specific code**: keep it behind the device's build flag or driver,
  not in a path every device compiles.

If a feature still adds code to callers that do not use it, measure it and
state the number in the PR description. Compile a small file that uses the
component with the target compiler at `-Os`, before and after the change, and
compare with `size` and `nm -S --size-sort`:

```sh
xtensa-esp32s3-elf-g++ -std=gnu++2a -Os -ffunction-sections -fdata-sections \
  -fno-exceptions -fno-rtti -Ilibs/ui/FreeInkUI/include -c probe.cpp -o probe.o
xtensa-esp32s3-elf-size probe.o
```

Growth for non-users needs a reason in the PR; "the feature needs it" is not
one when a pointer or a shared function would avoid it.

## 16-level images and gray calibration (EPDiy panels)

`displayGray16Image()` and the calibration hooks `setGray16Levels()` /
`getGray16Levels()` are implemented by `EpdiyDriver` only, and have been checked
on one device: the Metalio E-Ink4-Plus. On every other driver they return
false. Do not describe them as working elsewhere, and do not enable a consumer's
use of them for another board, until that panel has been measured.

How the calibration works, and what its three numbers per level mean, is in
[docs/metalio-eink4-plus-support.md](docs/metalio-eink4-plus-support.md)
("Calibrating the 16 levels"). In short: the panel's own table reaches every
gray by way of black and pushes a gray toward black again before clearing it; a
calibration table replaces that with numbers measured on the glass, so a level
is cleared with white frames alone.

Tuning is a loop between the device and a camera, so an agent can only run it
with a person:

1. The consumer firmware needs a way to set the numbers and draw bars of chosen
   levels on a running device (a serial command is enough).
2. Draw a grid of candidate levels beside black and white references, and ask
   for a photo taken straight on, in even light, with no reflection on the glass.
3. Measure each stripe's lightness between the two references. Compare a stripe
   with what is beside it, not with a distant part of the photo: the lighting
   changes across the screen by more than the differences being measured.
4. Pick the levels nearest an even ramp, load them, and photograph the ramp.
5. Have the person judge screen changes by eye, and ask whether anything left
   behind is lighter or darker than its surroundings. Those need opposite
   corrections.

To ask an agent for this: say which device, that it may use the serial port,
and that you will send a photo after each pattern. The agent must hold one
serial connection for the session (opening the port resets the device) and
release it before any flash. Numbers set this way are lost on reboot; the final
ones belong in the consumer's source.

When a draw is started from a task other than the one that normally renders,
hold the CPU at full speed for it: at the idle clock the panel's line feed
underruns and the frame fails.
