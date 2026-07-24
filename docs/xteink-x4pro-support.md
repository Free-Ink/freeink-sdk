# Xteink X4 Pro

ESP32-S3 (16 MB flash, 8 MB PSRAM) e-reader. **SSD1677 800×480 B/W** panel (same
controller as X4 / de-link / Sticky), **GT911** capacitive touch, and a **dual
warm/cold color-temperature frontlight**. Distinct device from the ESP32-C3
`XTEINK_X4` — it has its own S3 profile, `BoardConfig::XTEINK_X4_PRO`.

Build: `-DFREEINK_DEVICE_X4PRO=1` (see `platformio.sample.ini` `[env:x4pro]`).
`FREEINK_DRIVER_SSD1677`, `FREEINK_CAP_TOUCH`, and `FREEINK_CAP_FRONTLIGHT`
auto-enable. The SD path additionally requires `USE_BLOCK_DEVICE_INTERFACE=1` in
the consumer build (the `x4pro` env defines it).

This doc reflects a hardware bring-up + reverse-engineering session. Items are
marked **Confirmed on hardware** where directly probed, **Corrected** where an
earlier claim has since been disproven on the bench, or **Pending** where still
inferred from the firmware dump only.

## Display — SSD1677, 800×480

**Confirmed on hardware — the panel WORKS.** It paints normally using the
**plain X4 OTP waveform** (`ssd1677DefaultConfig`) — **no** custom LUT, **no**
explicit voltages, **no** PMIC, **no** GPIO power-enable. The entire earlier
"resets and runs waveforms but develops no pixels" saga had a single root cause:
**the display pins were wrong.**

### Root cause — wrong display pins (solved)

**Corrected.** Earlier RE read the wrong firmware image (**app0**), which gave a
scrambled pin map: **CS and DC were swapped, and SCLK/MOSI were in the wrong
order.** With those wrong, the controller reset and ran full-length waveforms but
clocked commands to the wrong GPIOs, so it never developed an image. Once the
pins were corrected, the standard X4 waveform paints normally — **no external
supply or special config is involved.**

There is **no** external EPD PMIC and **no** external charge pump. The SSD1677
drives its high-voltage rails from its **internal booster** (`0x0C` soft-start),
exactly like the X4 and Sticky. Any earlier "EPD power-on / 0x63 PMIC / dead
rails" narrative is deleted — see the [I²C bus map](#i²c-bus-3938--device-map)
for what 0x63 actually is (a battery fuel gauge).

### Confirmed display pinout

**Confirmed on hardware.** The pinout was found by a raw bit-banged pin sweep on
hardware, and independently corroborated by an **app1** driver-usage RE:

| Signal | GPIO |
|---|---|
| SCLK | 12 |
| MOSI | 11 |
| MISO | — (write-only) |
| CS | 13 |
| DC | 18 |
| RST | 14 |
| BUSY | 6 (input, busy = HIGH) |

SPI clock 5 MHz. Native landscape scan (`NO_FLIP`). GPIO7 is *not* a display
enable — it is the Right nav button (see
[Input](#input--digital-buttons--capacitive-home)).

**RE cross-check (app1 driver-usage):** confirms **CS = 13** (driven every byte
in `writeCommand`/`writeData`), **DC = 18** (driven only for commands),
**RST = 14** (reset-pulse fn), **BUSY = 6** (busy-poll loop; input, active-high).
Only **SCLK/MOSI** could not be pinned from the RE — they came from the SPI
constructor argument order, which is ambiguous — and were settled by hardware as
**SCLK = 12, MOSI = 11**.

### Recovered OEM command stream (reference)

The command stream below was recovered from the OEM firmware and matches the live
controller. Key code offsets (app1 IROM):
`writeCommand 0x4201a2d4`, `init 0x4201a568`, `fullUpdate 0x4201a628`,
`fastUpdate 0x4201a6c8`, `waitBusy 0x42014f58`.

The **Display Update Control 2 (0x22)** values that distinguish this board from
the other SSD1677 boards:

| Mode | X4 Pro | X4 default | Sticky |
|---|---|---|---|
| Full | 0xF7 | 0xFC | 0xFF |
| Fast | 0xC7 | — | — |

Full and fast refresh use the panel's built-in OTP waveform (no custom LUT); only
partial refresh loads a custom LUT.

The sequence below was recovered from the OEM firmware and verified against the
live controller:

- **INIT** (`0x4201a568`): SW RESET `0x12`, then a **fixed `delay(10)`** — *not*
  a BUSY-wait. Then temp sensor `0x18 = 0x80` (internal); booster
  `0x0C = AE C7 C3 C0 80`; driver output control `0x01 = DF 01 02` (gate lines
  `0x01DF` = 479 → MUX 480, `SM = 1` scan direction); border `0x3C = 0x80`.
  RAM window: data-entry `0x11 = 0x01`, X range via `0x44`, Y range via `0x45`,
  cursor via `0x4E`/`0x4F`; width `0x320` = 800, height `0x1E0` = 480.
- **FULL** refresh (`0x4201a628`): `0x21 = 0x00`, `0x22 = 0xF7`, `0x20`, wait
  BUSY.
- **FAST** refresh (`0x4201a6c8`): `0x22 = 0xC7`, `0x20`, wait BUSY.
- **PARTIAL** refresh: loads a custom `0x32` LUT plus `0x03` (VGH) = `0x17`,
  `0x04` (VSH1/VSH2/VSL) = `41 A8 32`, `0x2C` (VCOM) = `0x30`.

## Touch — GT911

| Signal | GPIO |
|---|---|
| I²C SDA | 39 |
| I²C SCL | 38 |
| INT | **10** |
| RST | **4** |
| Power enable | **GPIO2 (active-LOW)** |

Address 0x5D (alt 0x14), 400 kHz, on the shared bus with the RTC (0x51) and gauge
(0x63). Recovered from app1's `XTEink::GT911Driver` via Ghidra and confirmed on
hardware with an interactive probe. **Bringing this panel up took three non-obvious
pieces, each of which had the touch controller completely silent until fixed:**

- **Power rail — GPIO2 driven LOW.** The GT911 sits on a rail gated by GPIO2, and
  the OEM drives **GPIO2 LOW** (with GPIO1 HIGH) at boot. Until GPIO2 is pulled low
  the controller is unpowered and never ACKs — an idle i2c scan shows only the RTC
  and gauge. Modeled as an **active-low touch power-enable** (`powerEnableActiveHigh
  = false`). Driving GPIO2 *high* (the naive "enable") keeps it dark.
- **Pins — RST=GPIO4, INT=GPIO10.** The reset line that is level-toggled low→high is
  GPIO4; the address-select line (driven, then floated to input) is GPIO10. (An
  earlier RE had these two reversed, which is why the first hardware attempt failed.)
- **Config — self-loaded, no host upload.** Like the Sticky board (same controller),
  the GT911 loads its own internal config on the standard reset dance; the SDK never
  uploads a config table. Earlier the config registers read back all-zero and the
  panel didn't scan — that was purely a **too-short reset** not triggering the
  internal config load. The SDK's reset (10 ms / 10 ms / 50 ms / 50 ms, INT→INPUT)
  makes it self-load and scan. (Confirmed there is no GT911 config table anywhere in
  the OEM dump — app0, app1, spiffs, or nvs — so the OEM relies on self-load too.)

Once powered and reset:

- **Reset dance:** RST LOW, INT = select-level, 2 ms → RST HIGH, 8 ms → INT to
  INPUT+pull, 60 ms, then probe. INT level as RST rises selects the address (LOW → 0x5D).
- **Coordinates:** status at `0x814E` (bit7 ready, low nibble count); points read
  from `0x8150`, 8 bytes each, **X-lo at byte 0** → `gt911CoordsAtByte0 = true`.
- **Orientation:** the GT911 is mounted **portrait** — reports **X: 0–480, Y: 0–800**
  on the 800×480 landscape panel — so the profile sets **`swapXY = true`**.
  `flipX`/`flipY` pending a corner-tap test.

The **Home pad** is a GT911 capacitive key bit (status `0x10`), not a GPIO —
RE-confirmed the OEM keys off exactly `0x814E & 0x10`, so our handling matches.

Prior notes had INT=21 (app0), then INT=4/RST=10 — both wrong; the confirmed wiring
is **INT=10, RST=4, power=GPIO2-low** above.

## Input — digital buttons + capacitive Home

**Confirmed on hardware.** This section previously described an ADC resistor
ladder; that was wrong for this variant. The device physically has only:

- **Left** nav button — **GPIO0**
- **Right** nav button — **GPIO7**
- **Power** button — **GPIO3**
- **Home** — via the **GT911 touch controller** (a capacitive key bit, not a GPIO)

The nav/power buttons are plain **digital, active-low** (`INPUT_PULLUP`, no ADC
rail needed) — confirmed by a pull-up edge test on hardware; this is **not** an
ADC ladder. GPIO7 reads `INPUT_PULLUP`, confirming it is the Right button and not
a display enable.

The SDK profile uses `InputStyle::DigitalButtons`:

- Left (GPIO0) → Up / previous page
- Right (GPIO7) → Down / next page (a reader needs paging)
- Power = GPIO3
- Back / confirm via touch + the GT911 Home key

Note: **GPIO0 is a boot-strap pin.** It works fine as a button as long as it is
not held during reset.

### Vestigial ADC ladder (Corrected — unused)

**Corrected:** earlier RE proposed an **ADC resistor ladder** (GPIO10 +
thresholds) as this unit's input path. Hardware disproves that — the ladder is
**vestigial firmware**; this unit uses the digital buttons above. For the record,
the dump still contains a 4-threshold ADC ladder matcher (`0x4201f734`, raw-12bit
defaults BACK ≈ 3580 / OK ≈ 2728 / UP ≈ 1514 / DOWN ≈ 0, ±319 window,
inferred input GPIO10), but it is **not wired** on this variant and should not be
mistaken for a live path.

## Power rails

The board-init function fills a per-pin config table (mode + intended level)
consumed by an `applyPinConfig` helper, which only issues `digitalWrite` for
OUTPUT pins.

- **GPIO1 (peripheral rail — held HIGH).** Driven OUTPUT HIGH first in board-init
  and held. It does not visibly affect display or SD on the bench, but it **is**
  required (with GPIO2 low) for the **touch** rail: the GT911 only powers up with
  GPIO1 HIGH. Carried as `power.latch0` and asserted early to match the OEM.
- **GPIO2 (Confirmed — touch power-enable, ACTIVE-LOW).** The OEM drives it OUTPUT
  **LOW** at boot, and that is what powers the **GT911 touch controller**: with GPIO2
  high (or floating) the GT911 is unpowered and silent on the i2c bus; pulling it low
  brings it up at 0x5D. Modeled as the touch power-enable (active-low). See
  [Touch](#touch--gt911). (Earlier notes had its role unconfirmed and mistakenly
  drove it high.)
- **GPIO5 (Confirmed — SD power-enable, ACTIVE-LOW).** It gates the SD card's data
  path. The OEM `mountSD` pulses it HIGH→LOW before each mount attempt and runs the
  card with it held **LOW**; holding it HIGH breaks every block read (`0x107`). See
  [Storage](#storage--sd-card).
- **GPIO7** is `INPUT_PULLUP` — it is the **Right button**, not a rail/enable.

Note: the display's high-voltage supply is the **SSD1677's internal booster**
(`0x0C` soft-start), not any external PMIC or GPIO rail (see
[Display](#display--ssd1677-800480)). 0x63 on the I²C bus is a **battery fuel
gauge**, not a display supply.

## Storage — SD card

**SDMMC, not SPI — CONFIRMED WORKING on hardware.** The slot is driven as **native
SDMMC** (`esp_driver_sdmmc`); SPI-mode CMD0 is silent, which is why an SPI card path
never worked. 1-bit mode, slot 1, internal pull-ups, 40 MHz.

| SDMMC signal | GPIO |
|---|---|
| CLK | 41 |
| CMD | 42 |
| DAT0 | 40 |
| power-enable | 5 (active-LOW) |

DAT1/2/3 are unused in 1-bit. (These CLK/CMD/DAT0 pins happen to match app0's; the
earlier note that they were "the wrong variant" was itself mistaken — they are
correct.)

**The mount sequence (from app1's `mountSD`, and required on hardware).** Two
non-obvious details, both reproduced in `SdmmcBlockDevice::begin`:

1. **Power-cycle GPIO5 and retry the WHOLE mount.** GPIO5 is an active-LOW enable
   gating the card's data path. Before each attempt, pulse it HIGH (80 ms) → LOW
   (120 ms), then run `sdmmc_card_init` **and a real sector-0 read**. Retrying only
   `card_init` leaves SdFat's first (un-retried) block read to hit a still-marginal
   data path and fail with `sdmmc_read_sectors_dma … 0x107`. Validating a real read
   before publishing the card means the retry actually covers block I/O.
2. **Leave GPIO5 LOW.** Driving it HIGH after init (an earlier misreading of the
   OEM) breaks every subsequent read with `0x107`. It must stay in the LOW state the
   validated read succeeded under.

Reads/writes also bounce through a `MALLOC_CAP_DMA` buffer, since SdFat's caches may
be in PSRAM or unaligned.

**SDK changes:** `FREEINK_SD_SDMMC` includes X4PRO, and the consumer build must
define `USE_BLOCK_DEVICE_INTERFACE=1` (the `x4pro` env does).

## I²C bus 39/38 — device map

**Confirmed on hardware** (on-hardware i2c scan of SDA 39 / SCL 38, 400 kHz):

| Address | Device |
|---|---|
| 0x51 | **RTC** — BM8563 (PCF8563-compatible), initializes on hardware |
| 0x63 | **CW2017 battery fuel gauge** — an i2c register dump showed the classic BATINFO battery-model curve at regs `0x10`–`0x3F`; CW2017's default address is 0x63. **Not** a display PMIC. |

The **GT911 touch** is on the same bus at **0x5D** (INT=GPIO4, RST=GPIO10; see
[Touch](#touch--gt911)).

## RTC / USB / battery

- **RTC: BM8563** (PCF8563 register-compatible) at I²C **0x51** on the **39/38
  bus** (SDA 39 / SCL 38, 400 kHz) — **confirmed found and initializing on
  hardware**.
- **USB: GPIO19 = D−, GPIO20 = D+** (ESP32-S3 native USB; OEM uses USB-MSC card
  transfer + CDC). Do **not** repurpose them, and do not run any I²C/GPIO probe
  across them at boot — e.g. the Xteink C3 X3/X4 detect fingerprint pokes
  SDA20/SCL0, which on this S3 would land on D+ and the boot strap (this is why
  `XteinkDetect` compiles to a no-op unless an Xteink profile is in the build).
- **Battery: CW2017 I²C fuel gauge at 0x63** on the 39/38 bus, wired into
  `BatteryMonitor` via `GaugeType::Cw2017`. The CW2017 reports 0% until an 80-byte
  **BATINFO** battery profile is loaded (regs `0x10`–`0x5F`), so init verifies the
  resident profile and re-uploads the OEM table (recovered from app1's
  `Cw2017PowerHal`) if it's missing, then reads **SoC from reg 0x04** and **VCELL
  from regs 0x02/0x03** (14-bit, `mV = (raw·5 + 8) >> 4`). Charging state is not
  observable from the gauge (no charger IC on this bus); the VBUS/USB-detect pin was
  **not conclusively identified**.

## Frontlight — dual warm/cold PWM

**Confirmed on hardware** — dual-channel color temperature, and the identities
are now nailed down: **cool/white = GPIO8, warm = GPIO9**, both **active-HIGH**
(driving each pin high lights that LED). This **matches the dump**, and the
profile's `FrontlightConfig{8, 10000, 10, true, 9}` is **correct**. Two LEDC
channels, mixed by `FrontlightManager` (`setBrightness` = total level,
`setColorTemperature` = warm/cool split).

| Channel | GPIO | Color | LEDC ch |
|---|---|---|---|
| `frontlight.gpio` | 8 | cool/white | 4 |
| `frontlight.gpioWarm` | 9 | warm | 5 |

10 kHz, 10-bit, **active-high**.

## Partitions (16 MB, dual-OTA)

| Label | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x009000 | 0x005000 |
| otadata | data/ota | 0x00E000 | 0x002000 |
| app0 | app/ota_0 | 0x010000 | 0x7E0000 |
| app1 | app/ota_1 | 0x7F0000 | 0x7E0000 |
| spiffs | data/spiffs | 0xFD0000 | 0x014000 |
| coredump | data/coredump | 0xFE4000 | 0x01C000 |

**Note — two app images; the device boots app1 (CONFIRMED).** The dump carries
two application images: **app0 @ 0x10000** = `ESP32S3_X4_TL` (a **different
variant** — Arduino-era, hardcoded pins), and **app1 @ 0x7F0000** =
`XTEink X4 Pro` / `ESP32S3_X4_TL_SSD1677` (the real firmware — native
`esp_driver_sdmmc`, `XTEink::SSD1677_800x480`, `XTEink::GT911Driver`,
`XTEink::EPDPanelInterface`). The active boot slot (otadata) selects **app1**.

**This was the root cause of the display saga:** every early RE pass read **app0 —
the wrong variant** — which gave scrambled display pins (CS/DC swapped, SCLK/MOSI
wrong order). The corrected, hardware-confirmed **display pinout `SCLK=12 / MOSI=11
/ CS=13 / DC=18 / RST=14 / BUSY=6`** is now known-good (see
[Display](#display--ssd1677-800480)). Display, buttons (0/7/3), frontlight (8/9),
RTC (0x51), and SDMMC (41/42/40 + GPIO5) are all confirmed working on hardware.

## Status — working on hardware

**All peripherals are up:** display, buttons, frontlight, RTC, SDMMC, **GT911 touch +
capacitive Home key**, and the **CW2017 battery percentage**. Touch wiring in the SDK
profile: `touch.powerEnable = GPIO2` (active-low, `powerEnableActiveHigh = false`),
INT=10/RST=4, `swapXY = true`, `gt911CoordsAtByte0 = true`, no config upload.

## Pending — minor / optional

- **Touch flip** — `flipX`/`flipY` still to confirm with a corner-tap test (taps
  register and navigate; only the axis mirroring may need a tweak).
- **Deep-sleep power** — SD/touch enables are active-low on this board; the sleep path
  drives them to their off level by polarity (implemented) — worth a power-draw check.
- **USB-MSC** ("USB Transfer" — SD over USB) is present in stock firmware but not ported.
- **Panel orientation** (ships `NO_FLIP`; native SSD1677 scan is 800×480 landscape).
