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
| **de-link** | ESP32-S3 | SSD1677 | 800×480 B/W + gray, frontlight | ✅ full (SD over SPI; 4-bit SDMMC is a follow-up) |
| **M5Stack PaperColor** | ESP32-S3 | ED2208 | 400×600 color | 🟡 driver stub + abstraction |
| **Murphy M3** | ESP32-S3 | UC8253 | 240×416 B/W, CHSC6x touch, PWM frontlight | 🟡 driver stub + abstraction |

X3 and X4 share the ESP32-C3 and one pin profile, so **a single firmware binary
drives both** — the panel is chosen at runtime via `setDisplayX3()`. Distinct-MCU
boards (S3) build their own binary, selected with a board macro.

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
| `-DFREEINK_DISPLAY_FLIPPED` (or `-DFLIPPED`) | vertically flip an SSD1677 panel |
| `-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1` | single framebuffer (uses controller RAM as previous frame) |
| `-DFREEINK_NET_WOLFSSL=1` | enable the wolfSSL TLS 1.3 transport in `SecureNet` |

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

Add the libraries you need as symlink `lib_deps` (names match the original SDK):

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
shim. The display lib needs `BoardConfig` and `SdFat` (provided by the consuming
project, as before).

## Adding a new device

1. Add a `BoardProfile` to `BoardConfig.h` (pins, geometry, controller, input
   style, optional touch/frontlight/audio) and a board macro for `ACTIVE`.
2. If it uses an existing controller, reuse that driver — supply a tuned config
   struct (its own LUTs/voltages) if the panel differs. If it's a new
   controller, add a `PanelDriver` in its own file + a `FREEINK_DRIVER_*` flag.
3. **Each device gets its own profile, board macro, and build env.** Two devices
   may share one binary *only* when they have an identical pinout and are
   distinguishable at runtime — that is the sole reason Xteink X3 and X4 share
   the default ESP32-C3 env (they're the same board, told apart by I2C
   fingerprinting + `setDisplayX3()`). Same MCU but different GPIOs, screen, or
   controller ⇒ a separate profile and a separate env, never an auto-shared bin.

## Repository layout

```
libs/
  display/FreeInkDisplay/   facade + EInkDisplay shim + per-controller drivers + LUTs
  hardware/BoardConfig/     board profiles & capability descriptors
  hardware/InputManager/    buttons (+ scaffolded touch)
  hardware/BatteryMonitor/  ADC battery + optional charge-sense
  hardware/SDCardManager/   SdFat-backed storage
  hardware/FrontlightManager/  PWM frontlight (de-link)
  network/SecureNet/        wolfSSL TLS 1.3 client + HTTP shim (opt-in)
```

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
