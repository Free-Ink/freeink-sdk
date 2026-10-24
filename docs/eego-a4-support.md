# EEGO A4 board support

Status: **implemented; hardware validation remains incomplete.** The SDK includes
an EEGO A4 board profile, display driver, touch backend, and frontlight backend.
The original pin map and controller details came from stock-firmware analysis
(see [Provenance](#provenance)); outstanding checks are listed below.

## Device identity

- Product: **EEGO Reader A4** (`eego_a4`, board tag `EA04`). Chinese-market e-reader.
- Firmware examined: `EEGOREAD_A4_130.bin` (5,816,736 bytes), app label
  `EegoRead-ESP32-1.3.0`, built with PlatformIO / Arduino-ESP32, ESP-IDF v5.5.4,
  compiled 2026-06-02.
- **Lineage: this is a CrossPoint fork.** The stock firmware is a rebrand of the
  CrossPoint reader from a snapshot around **May–early June 2026**, *before*
  CrossPoint moved onto the FreeInk SDK — so the shipped binary rides the older
  community-sdk, not this SDK. Proof: identical source-tree paths
  (`src/activities/reader/KOReaderSyncActivity.cpp`, the `EpubReaderActivity`
  sub-menus), the `Hal*` / `ActivityManager` / `GfxRenderer` architecture, and a
  byte-for-byte match of the `appendSegmentPatternBreaks(...)` hyphenator symbol
  to CrossPoint `lib/Epub/Epub/hyphenation/Hyphenator.cpp`. The CrossPoint name is
  stripped from the strings, so grep-by-name does not find it — compare
  architecture and symbols instead. OEM additions on top: a Tencent tcloudbase
  cloud push (`CloudPushActivity`), a WeChat-QR pairing + `/eego/upload*` local
  transfer server (`LocalTransferServer`), an `EEFONT`/`FONT_PACK` font protocol,
  and the A4 panel/touch drivers.

## Hardware

| Item | Value |
|------|-------|
| SoC | ESP32-S3 **N16R8** (16 MB DIO flash, 8 MB OPI PSRAM) |
| Panel | **UC8279C**, **768 × 552** (2 bpp), BUSY active-low |
| Panel RAM | 768 × **600**; host FB is 768×552 sent **bottom-up**, then 48 white rows |
| Framebuffer | 52,992 bytes (768/8 × 552); grayscale planes 52,992 B each, lazy from PSRAM |
| Display SPI | SCLK 42, MOSI 45, CS 21, DC 14, RST 13, BUSY 41 · **20 MHz** |
| MicroSD | dedicated **HSPI** bus: SCLK 39, MISO 40, MOSI 38, CS 47 · 20 MHz · power enable GPIO6 (active-low) |
| Buttons | UP 5 and DOWN 7 active-low; POWER 8 active-high with INPUT_PULLDOWN |
| Battery | **CW2015** fuel gauge at I²C 0x62 on SDA 2 / SCL 1 (shared with touch and RTC), probed at runtime; ADC GPIO 10 (divider ×1.559) when the gauge does not answer. Charge status GPIO 11 |
| Touch | **GSLX680** — SDA 2, SCL 1, RST 3, addr 0x40. Firmware blob uploaded at boot (`gsl/EegoA4GslFirmware.h`, extracted + hash-verified); backend in InputManager, with a multi-touch snapshot mirroring the GT911 contract (track IDs from each record's high nibble, up to 4 stored / 5 reported contacts). |
| Touch mapping | Calibration in `pollGslx680` returns panel-native X 0..767 / Y 0..551; profile swap/flip disabled |
| Screen key | GSL sentinel `rawX=0x03a0, rawY=0x1020`; short press = Back, 700 ms hold = Home |
| RTC | **PCF8563** at 0x51, on the **shared touch I2C bus** (SDA 2 / SCL 1), 400 kHz |
| Power latch | GPIO 4 (held to stay powered) |
| Deep sleep | send controller `0xE0 = 0x88`, float SDA/SCL, hold GPIO 3 (touch RST) low |
| Frontlight | Optional LM3630A at I²C 0x36 on SDA 2 / SCL 1, enable GPIO 12; runtime-probed by `FrontlightManager` through `BoardProfile::i2cFrontlight`. |
| UI scale | 1.2 |

### Refresh behaviour

B/W refresh cadence is controlled by the caller. FAST requests remain fast,
including after grayscale; HALF and FULL select the full waveform. The first
B/W paint after initialization uses the full waveform to clear unknown glass state.
After grayscale, the next B/W paint seeds the old plane with the inverse of the
target so every pixel is driven with the requested waveform. Each B/W paint then
copies the displayed frame into the old plane for the next differential update.
Anti-aliased (overlay) pages use stock V1.0.3's reader "Gray3" path. The B/W
base is stashed undisplayed; with its gray masks it becomes one three-level
frame (white / middle / black), sent as a per-pixel transition from the
previous Gray3 frame in a single refresh: unchanged white and middle pixels
hold, black is re-driven, and only changed pixels are cleared or nudged. A
cleanup request (non-FAST base) shows the page with black and white swapped,
then the page itself. After a B/W screen or four-gray image there is no
Gray3 history: the next page shows its base with a normal B/W refresh, which
fully clears the previous screen, and its grays follow as a Gray3 transition
from that base.

Full images (absolute/direct planes) use the stock four-gray waveform (five
42-byte records), which drives every pixel from any prior state, with no B/W
base pass. Planes go out MSB to DTM1 and LSB to DTM2, so
black/dark/light/white are 00/01/10/11.

Gray refreshes use the stock V1.0.3 panel session (PWR 0x03 00 72 72,
VCOM_DC 0x32); B/W uses a stronger drive (PWR 0x43 00 78 78 17, VCOM_DC 0x24)
because UI screens change with fast refreshes. Both use gate/source
0x65 00 00 00 00 and seed DTM1 white before power-on. Full refresh uses the stock GC waveform (0x50 = 0x97); fast refresh
uses the stock production "DU-factory-previous" waveform (0x50 = 0xd7), both
49-byte records. The planes are
lazily allocated, PSRAM first; if allocation fails the driver leaves the
existing B/W image up.

## SDK integration

Build with `-DFREEINK_DEVICE_EEGO_A4=1`; see the `eego_a4` environment in
[platformio.sample.ini](../platformio.sample.ini).

- `BoardConfig::EEGO_A4` selects the UC8279C driver, digital buttons, GSLX680
  touch, PCF8563 RTC, dedicated HSPI SD bus, and GPIO4 power latch.
- `Uc8279cA4Driver` uses the shared `PanelDriver` / `EpdBus` interfaces with
  external Full/Fast/Gray LUTs, bottom-up scan, and padding to 600 panel RAM rows.
- `InputManager` uploads the GSLX680 firmware and applies calibration in
  `pollGslx680`, returning panel-native coordinates without a second profile transform.
- `BatteryMonitor` probes the CW2015 once. When it answers, a sleeping gauge is
  woken and the 64-byte BATINFO battery profile is compared with the stock
  3.7 V 2000 mAh profile; an empty or different profile is uploaded (profile
  bytes, CONFIG update flag, MODE power-on reset) and read back. Voltage is
  VCELL × 305 µV; the percentage is the gauge SOC rounded, plus one, capped at
  100, matching the stock battery icon. Samples outside 2500–4500 mV or above
  100% are rejected. When the gauge does not answer, the ADC pin is used.
  Charging comes from GPIO 11; the gauge cannot observe it.
- `FrontlightManager` probes the optional LM3630A through
  `BoardProfile::i2cFrontlight`. The profile's PWM frontlight field is `NO_FRONTLIGHT`.
  The controller is fitted on some unlit units too, so a unit the stock
  firmware lists as unlit (by device UID, the low 24 bits of the factory MAC)
  reports no frontlight even when the controller answers.

## Open items (need a physical unit)

- Confirm 768×552 on real glass (ghosting, gray levels). The UC8279C bring-up now
  does the full power/booster/VCOM/PLL setup and uploads external Full/Fast/Gray
  LUTs (hash-verified), so refreshes actually run — validate quality on a unit.
- Check ghosting during long FAST-only menu navigation and AA page turns without
  automatic full cleanup after grayscale.
- Validate the LM3630A frontlight init sequence and brightness curve on a frontlit
  unit, and runtime absence handling on a unit without the chip.
- Re-verify the whole pin map on hardware: a frontlit-unit dump did NOT contain the
  `20 MHz` display clock or the `eego_a4` name the scaffold assumed (both units are
  pre-freeink community-sdk builds), so the borrowed pin values are provisional.
- Confirm navigation GPIOs 5/7 and battery divider ×1.559. The current profile
  sets POWER GPIO8 active-high with `INPUT_PULLDOWN`; an internal pull-up caused
  phantom presses. Charge-status GPIO11 is active-high in the profile.
- Validate GSLX680 touch on hardware: the firmware blob is byte-verified (SHA-256
  `076ac8…`) and the init/read sequence is RE-derived, but the
  panel-native calibration needs a corner-tap check. Multi-finger frames (track
  IDs, counts up to 5) are host-model-tested only — validate 2-/3-finger
  reports on real glass.
- Standby current with the GPIO4 latch + GPIO3-low sequence.
- CW2015 on hardware: the stock profile and register sequence are recovered
  from stock V1.0.3 and match a live unit's resident profile byte for byte, but
  the SDK's upload path and ADC fallback have only run against a host model.

## Provenance

The initial hardware map came from reverse-engineering the stock OEM firmware
`EEGOREAD_A4_130.bin`: the CrossPoint lineage, ESP32-S3, board tag `EA04`, the
UC8279C controller, 768×552 resolution, pin map, and touch/RTC calibration. Later corrections are reflected in the
current board profile. See
[Open items](#open-items-need-a-physical-unit) for the remaining hardware checks.
