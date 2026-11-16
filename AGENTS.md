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
