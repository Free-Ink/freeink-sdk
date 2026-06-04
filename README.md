# FreeInk SDK

A hardware-independent SDK for building e-paper reader firmware. FreeInk
abstracts every device-specific detail — display controller, waveforms/LUTs,
GPIOs, bus speeds, input style, touch, frontlight, audio — behind small,
injectable interfaces, so the firmware calls one generic API and gets
device-specific behavior. Adding a new device means adding data (a board
profile + a driver config), not editing the generic code.

It is **drop-in compatible** with firmware written against the original
`EInkDisplay` / `InputManager` / `BatteryMonitor` / `SDCardManager` / `BoardConfig`
API: switching to FreeInk is a matter of repointing the library path.

## Credit & lineage

FreeInk is an independent, clean-room reorganization **based on** the work of
the **OpenX4 E-Paper Community SDK** (`open-x4-epaper/community-sdk`, MIT) and its
contributors — in particular **CidVonHighwind** for the original `EInkDisplay`
driver and the X3/X4 waveform work, and the community device ports (M5Stack
PaperColor, Murphy M3, and the `community-sdk-de-link` ESP32-S3 port). The
register sequences and waveform LUTs for the SSD1677 and UC8253 panels are
derived from that project. Huge thanks to everyone who reverse-engineered and
tuned those panels.

FreeInk is **not a fork** and has no build-time or runtime dependency on the
upstream repository — it has its own history and its own architecture. Where the
upstream interleaved every device in one monolithic driver, FreeInk splits each
controller into a standalone, compile-time-selectable driver behind a stable
facade.

> License note: this repository is distributed under the **MIT License** (see
> `LICENSE`) — the same permissive, open-source terms as the upstream. Portions
> are derived from MIT-licensed upstream code; that attribution is preserved in
> `NOTICE`.

## Architecture

```
firmware  ─calls─▶  EInkDisplay  (alias of freeink::FreeInkDisplay, the facade)
                          │ owns framebuffer + geometry, selects a driver at begin()
                          ▼
                    PanelDriver  (interface)
              ┌───────────┼───────────────┬───────────────┐
        Ssd1677Driver  Uc8253X3Driver  Ed2208M5Driver  Uc8253MurphyDriver
         (X4/de-link)     (X3)          (M5, stub)       (Murphy, stub)
                          │  all share
                          ▼
                       EpdBus  (SPI/GPIO framing, BUSY polarity, reset, mirror)
```

- **`FreeInkDisplay`** (facade, exposed as `EInkDisplay`) owns the framebuffer(s)
  and geometry and delegates every panel operation to a `PanelDriver`. It
  preserves the full public API, including the `FULL_REFRESH` / `HALF_REFRESH` /
  `FAST_REFRESH` modes and the grayscale / anti-aliased dual-plane path
  (`copyGrayscaleBuffers` → `displayGrayBuffer`, `writeGrayscalePlaneStrip`).
- **`PanelDriver`** — one implementation per controller, in its own file. Each
  driver owns its register sequences and cross-call state, and takes its
  waveforms/LUTs/tunables as an injected **config** (e.g. `Ssd1677Config`,
  `Uc8253X3Config`) so per-device tuning is data, not code.
- **`EpdBus`** — shared SPI/GPIO helper, parameterized by SPI clock and BUSY
  polarity (per-controller default, board-overridable via
  `BoardConfig::ACTIVE.displaySpiHz`).
- **`BoardConfig`** — the one compile-time-selected description of a device:
  pins, geometry, controller, input style, touch, frontlight, audio.

### Nothing device-specific is hardcoded in generic code

GPIOs come from the `EInkDisplay` constructor (firmware passes
`BoardConfig::ACTIVE.display.*`) and from `BoardConfig`. SPI clocks have a
controller default and a board override. Waveforms/LUTs, booster values, scan
direction, and refresh temperatures are injected via the driver config struct. A
new device fills in values; the generic driver consumes them.

## Supported devices

| Device | MCU | Controller | Panel | Status |
|---|---|---|---|---|
| **Xteink X4** | ESP32-C3 | SSD1677 | 800×480 B/W + 4-level gray | ✅ full |
| **Xteink X3** | ESP32-C3 | UC8253 | 792×528 B/W + 4-level gray | ✅ full (runtime-selected) |
| **de-link** | ESP32-S3 | SSD1677 | 800×480 B/W + gray, frontlight | ✅ display + frontlight + native 4-bit SDMMC SD |
| **M5Stack PaperColor** | ESP32-S3 | ED2208 | 400×600 color | 🟡 display driver stub |
| **Murphy M3** | ESP32-S3 | UC8253 | 240×416 B/W, CHSC6x touch, PWM frontlight | 🟡 display stub; **touch + frontlight implemented** |

X3 and X4 share the ESP32-C3 and a pinout, so **a single firmware binary drives
both** — it carries *both* board profiles (`XTEINK_X4` and `XTEINK_X3`) and picks
one at runtime via `setDisplayX3()`, which swaps the active profile and driver.
Distinct-MCU boards (S3) build their own binary, selected with a board macro.

Every SDK library compiles cleanly on both ESP32-C3 and ESP32-S3. A consumer can
still block a multi-MCU build by hardcoding chip-specific code in *its own* layer
(deep-sleep wakeup, panic backtrace, flash pins) — see
[`docs/consumer-mcu-portability.md`](docs/consumer-mcu-portability.md) for the
exact changes (worked through against CrossPoint's HAL).

### M5Stack PaperColor refresh behavior

The PaperColor is natively a **six-color (Spectra 6), full-refresh** panel: a
complete OTP waveform takes **~15 s** — unusable for reading. To get
reading-compatible speeds, FreeInk's native driver **interrupts the refresh at
~340 ms**. The colors settle in order with **white settling last**, so cutting
off early leaves the panel **black or yellow** (depending on the inversion /
polarity selected) rather than white — and FreeInk exploits that to produce a
fast, high-contrast monochrome image. A true white background / full color
requires running the complete waveform (`requestCompleteWaveformNextRefresh()`).

Two backends are selectable for this device:
- **Native ED2208 (default)** — the fast interrupted-refresh path above.
- **M5 official (`-DFREEINK_M5_OFFICIAL=1`)** — wraps M5's own **M5Unified + M5GFX**
  stack for users who prefer the vendor path (slower, but standard). This pulls
  the M5 libraries only on that env; M5GFX owns the bus (`usesExternalBus()`).

**Capacitive touch** is implemented for two controllers (gated by
`FREEINK_CAP_TOUCH`): **CHSC6x** (Murphy M3 — IRQ-driven, ported from the upstream
driver) and **GT911** (LilyGo — polled, raw register reads + the reset/address
dance). The InputManager exposes `hasTouch/isTouchPressed/wasTouchPressed/
wasTouchReleased/getTouchPoint`; coordinates are delivered raw-panel-oriented and
the app owns rotation. A LilyGo T5 S3 Pro Lite GT911 touch config
(`BoardConfig::LILYGO_T5_PRO_GT911`) is ready to drop into a LilyGo profile once
that board's display driver lands.

## Build composition — devices × capabilities

A build is composed along two axes.

**Devices** (`-DFREEINK_DEVICE_<NAME>`) declare which hardware the binary supports.
Each device pulls in its panel driver, adds its board profile to the runtime
registry, and turns on its default capabilities. Compose any set that shares an
MCU (a C3-vs-S3 mix is a compile error):

| Pass | Result |
|---|---|
| `-DFREEINK_DEVICE_X4` | X4 only — links just SSD1677 (tightest) |
| `-DFREEINK_DEVICE_X3 -DFREEINK_DEVICE_X4` | X3 **and** X4 in one C3 binary, runtime-selected via `setDisplayX3()` |
| `-DFREEINK_DEVICE_DELINK` | de-link (S3, SSD1677 + frontlight) |
| `-DFREEINK_DEVICE_M5` | M5 PaperColor (S3, ED2208 + color) |
| `-DFREEINK_DEVICE_MURPHY` | Murphy M3 (S3, UC8253 + touch + frontlight) |
| *(none)* | defaults from the legacy `-DBOARD_*` macro, else **X3 + X4** |

Multiple **different-pinout** devices on one MCU are runtime-selected: `ACTIVE`
defaults to a compile-time default and the consumer calls
`BoardConfig::selectDevice(...)` after its own detection (the SDK doesn't ship a
detector; X3/X4 detection stays in the consumer, e.g. CrossPoint's fingerprint).

**Capabilities** (`-DFREEINK_CAP_<NAME>`) gate feature *code* to keep binaries
tight. Each defaults on when an included device needs it; force with `=0`/`=1`:

| Flag | Gates | Default |
|---|---|---|
| `FREEINK_CAP_TOUCH` | capacitive touch decoder (InputManager) | on if a device has touch |
| `FREEINK_CAP_FRONTLIGHT` | PWM frontlight (FrontlightManager) | on if a device has a frontlight |
| `FREEINK_CAP_COLOR` | color panel code | on for M5 |
| `FREEINK_CAP_AUDIO` | audio output (scaffold) | off |
| `FREEINK_CAP_NET_TLS13` | wolfSSL TLS 1.3 (≡ `FREEINK_NET_WOLFSSL`) | off |

**Other flags:**

| Flag | Effect |
|---|---|
| `-DBOARD_DELINK` / `_M5STACK_PAPERCOLOR` / `_MURPHY_M3` | legacy single-device selection (maps to the matching `FREEINK_DEVICE_*`) |
| `-DFREEINK_DISPLAY_FLIPPED` (or `-DFLIPPED`) | back-compat alias for `BoardProfile.orientation = MIRROR_Y` on SSD1677 |
| `-DFREEINK_SD_SDMMC=1` | use the native 4-bit SDMMC backend (needs `-DUSE_BLOCK_DEVICE_INTERFACE=1`); auto-on for de-link |
| `-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1` | single framebuffer (uses controller RAM as previous frame) |
| `-DFREEINK_NET_WOLFSSL=1` | enable the wolfSSL TLS 1.3 transport in `SecureNet` |

Panel **orientation/mirroring** is per-board data, not a flag: set `BoardProfile.orientation`
(`NO_FLIP`, `MIRROR_X`, `MIRROR_Y`, or `ROTATE_180`). The SSD1677 driver applies it in
hardware (mirrorX via RAM column addressing, mirrorY via gate scan). 90°/270° need a
software transpose and are a follow-up.

## Networking — TLS 1.3 (`SecureNet`)

The precompiled mbedTLS in the ESP-IDF/pioarduino package ships TLS 1.3 as empty
stubs, so `WiFiClientSecure` / `esp_http_client` cannot reach TLS-1.3-only servers
(e.g. KOSync at `kosync.ak-team.com:3042` — handshake fails with `-0x7780`). A
`-D` flag can't change a precompiled `.a`, and a from-source ESP-IDF rebuild is a
heavier path. `SecureNet` brings its own TLS stack — **wolfSSL compiled from
source** — which supports TLS 1.3 + PSA and bypasses system mbedTLS entirely:

- `freeink::SecureClient` — an Arduino `Client` doing TLS 1.3 over `WiFiClient`.
- `freeink::SecureHttpClient` — an `HTTPClient`-compatible shim so existing call
  sites switch with minimal churn.

Opt-in: `-DFREEINK_NET_WOLFSSL=1` plus a wolfSSL source `lib_dep`. With the flag
off it compiles to an inert stub, so the rest of the SDK builds without wolfSSL.

## Using FreeInk from PlatformIO

See **[`platformio.sample.ini`](platformio.sample.ini)** for a complete, ready-to-copy
configuration — it mirrors the toolchain/flags verified against the CrossPoint
firmware and includes per-device build envs (`xteink`, `xteink_x4`, `m5paper`,
`delink`, `murphy`) wired with the right `FREEINK_DEVICE_*` flags.

Already on CrossPoint? **[`platformio.crosspoint.sample.ini`](platformio.crosspoint.sample.ini)**
mirrors the exact working setup: drop it into the CrossPoint repo as
`platformio.local.ini`, point the paths at your FreeInk SDK checkout, and
`pio run -e default` builds the X3+X4 C3 binary against this SDK with **no source
changes** (the compat shim preserves every include path and class name).

The minimum is to add the libraries you need as symlink `lib_deps` (names match
the original SDK):

```ini
lib_deps =
  BoardConfig=symlink://path/to/freeink-sdk/libs/hardware/BoardConfig
  EInkDisplay=symlink://path/to/freeink-sdk/libs/display/FreeInkDisplay
  InputManager=symlink://path/to/freeink-sdk/libs/hardware/InputManager
  BatteryMonitor=symlink://path/to/freeink-sdk/libs/hardware/BatteryMonitor
  SDCardManager=symlink://path/to/freeink-sdk/libs/hardware/SDCardManager
  ; optional:
  FrontlightManager=symlink://path/to/freeink-sdk/libs/hardware/FrontlightManager
  SecureNet=symlink://path/to/freeink-sdk/libs/network/SecureNet
```

`#include <EInkDisplay.h>` and the `EInkDisplay` type keep working via the compat
shim. The display libs depend on `BoardConfig`; `SdFat` is pulled in
automatically as a dependency of `SDCardManager`.

## Adding a new device

1. Add a `BoardProfile` to `BoardConfig.h` (pins, geometry, controller, input
   style, optional touch/frontlight/audio) and a board macro for `ACTIVE`.
2. If it uses an existing controller, reuse that driver — inject a tuned **config
   struct** (its own LUTs/waveforms) without editing the driver: define
   `const Uc8253X3Config& yourConfig();` (or `Ssd1677Config`) in `namespace
   freeink` and build with `-DFREEINK_UC8253_X3_CONFIG=yourConfig` (or
   `-DFREEINK_SSD1677_CONFIG=...`). If it's a new controller, add a `PanelDriver`
   in its own file + `FREEINK_DRIVER_*` flag.

   **Resolution is always a `BoardProfile` field** — `displayWidth/Height`. Every
   driver reads its geometry from the active profile (and `getDisplayWidth()/
   Height()` pass it to firmware); no driver special-cases its own size. The
   config struct is purely waveforms/LUTs. These are orthogonal: a different-size
   UC8253 panel sets its size in its profile and its waveforms in a config.
3. **Each device gets its own profile, board macro, and build env.** Two devices
   may share one binary when they're distinguishable at runtime — that is how
   Xteink X3 and X4 ride one ESP32-C3 env: **two full profiles** (`XTEINK_X4`
   800×480/SSD1677 and `XTEINK_X3` 792×528/UC8253) compile into the same bin, and
   `setDisplayX3()` swaps the active profile + driver after I2C fingerprinting.
   They happen to share a pinout, but each is a real profile — not one profile
   doing double duty. Same MCU but different GPIOs, screen, or controller ⇒ a
   separate profile and a separate env, never an auto-shared bin.

### Devices backed by external libraries

A `PanelDriver` doesn't have to emit raw SPI — it can wrap a third-party display
library. Some panels are best driven by an existing library rather than a
hand-rolled controller sequence (e.g. the LilyGo T5 S3 Pro and M5 Paper S3 use
[`EPD_Painter`](https://github.com/tonywestonuk/EPD_Painter); GT911 touch boards
use `SensorLib`). FreeInk pulls these in **per device**, so builds that don't use
them stay lean:

1. Put the external `#include` and the driver code **inside the device's
   `#if FREEINK_DEVICE_<NAME>` guard.** PlatformIO's LDF (chain mode) only links
   the external library when that device's code actually compiles — other devices
   (X4/X3/M5) are unaffected.
2. Add the external library to **that device's env `lib_deps`** in your
   `platformio.ini` (see `platformio.sample.ini`). It's installed for that env
   only.
3. Implement the device's `PanelDriver` as a thin wrapper over the library's API
   (init/draw/refresh/sleep), exactly like the native drivers — the facade can't
   tell the difference.

This keeps the SDK's display surface uniform (`EInkDisplay` everywhere) while
letting each device bring whatever rendering stack it needs.

## Repository layout

```
libs/
  display/FreeInkDisplay/   facade + EInkDisplay shim + per-controller drivers + LUTs
  hardware/BoardConfig/     board profiles & capability descriptors
  hardware/InputManager/    buttons + capacitive touch (CHSC6x, GT911)
  hardware/BatteryMonitor/  ADC battery + optional charge-sense
  hardware/SDCardManager/   SdFat-backed storage
  hardware/FrontlightManager/  PWM frontlight (de-link)
  network/SecureNet/        wolfSSL TLS 1.3 client + HTTP shim (opt-in)
```

## Upstream contributors

FreeInk's e-paper driver and hardware libraries stand on the work of the OpenX4
E-Paper Community SDK and its forks. Thank you to everyone who built what this is
based on:

- **[CidVonHighwind](https://github.com/CidVonHighwind)** — the original
  `EInkDisplay` driver that everything here descends from.
- **[Dave Allie](https://github.com/daveallie)** — core maintainer of the upstream
  SDK: SdFat/exFAT storage, `displayWindow` partial updates, single- and
  dual-buffer modes, the `deepSleep` power-off fix, and bringing the original
  driver into the SDK.
- **[zgredex](https://github.com/zgredex)** — factory-LUT grayscale, the
  VCOM-restore fix on `setCustomLUT(false)`, and making `grayscaleRevert`
  idempotent with a documented contract.
- **[Justinian](https://github.com/juicecultus)** — X3 grayscale LUTs and fast-diff
  BB reinforcement (plus a build fix).
- **[Jeremy Klein](https://github.com/jeremydk)** — `skipInitialResync()`, row-band
  streaming of grayscale planes to controller RAM (the strip-grayscale path), and
  the X3 post-full ghosting fix.
- **[Chun Ming Lee](https://github.com/leecming82)** — the X3 "turbo" LUTs (from
  papyrix) and an X4 smearing fix.
- **[Maik Allgöwer](https://github.com/allgoewer)** — the sunlight-fading fix
  (power the panel down after a refresh).
- **[Alasdair MacLeod](https://github.com/v1amacl7)** &
  **[LSTAR](https://github.com/LSTAR1900)** — grayscale cleanup after
  anti-aliased refreshes.
- **[CaptainFrito](https://github.com/CaptainFrito)** — transparent image drawing
  for icons, plus early InputManager and SD-card work.
- **[Dexif](https://github.com/dexif)** — BatteryMonitor support for Arduino-ESP32
  Core 3.x.
- **[marcinoktawian](https://github.com/marcinoktawian)** — separate power-button
  hold timing in InputManager.
- **[Jonas Diemer](https://github.com/jonasdiemer)** — guard serial output on a
  valid `Serial`.
- **[Yaroslav Nychkalo](https://github.com/gebeto)** — the `SDCardManager`
  `rename` method.
- **[Ian Chasse](https://github.com/iandchasse)** — the ESP32-S3 port and the
  warm/cool PWM frontlight in [`community-sdk-de-link`](https://github.com/iandchasse/community-sdk-de-link),
  which the de-link board support is based on.

If your work is here and you'd like your credit corrected or a link added, open
an issue or email hello@freeink.org.

## License

**MIT License** (`LICENSE`) — open source and permissive: use, modify, and ship
closed-source or commercial derivatives freely.

Derived in part from the MIT-licensed OpenX4 E-Paper Community SDK; that
attribution is retained in `NOTICE`.

### Commercial use & sponsorship

Commercial use is welcome and completely free — the MIT license asks nothing of
you. That said, if FreeInk powers a product you sell, please consider
**[sponsoring the project](https://app.royalty.dev/Free-Ink/freeink-sdk)** to
help fund ongoing maintenance, new device support, and waveform tuning. It's
entirely voluntary and always appreciated. Sponsorship or support enquiries:
hello@freeink.org.
