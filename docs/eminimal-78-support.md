# e-Minimal 7.8" support

`-DFREEINK_DEVICE_EMINIMAL=1` (`Board::EMinimal78`, profile name `eminimal_78`).

A DIY reader: an ESP32-S3 N16R8 module (16 MB flash, 8 MB octal PSRAM) driving a
Waveshare 7.8inch e-Paper HAT (IT8951E timing controller, 1872x1404 16-gray,
LUT "M841") over SPI. Five buttons, no touch, no frontlight, microSD on 1-bit
SDMMC. It is the first ESP32-S3 board on `It8951Driver`; M5Paper v1.1 (classic
ESP32) is the other.

## Build

See `[env:eminimal]` in `platformio.sample.ini`. Three things are required:

- `-DBOARD_HAS_PSRAM` and `memory_type = qio_opi`. The 329 KB framebuffer is
  larger than the S3's internal heap, so `FREEINK_FB_PSRAM` is on for this
  device.
- `-DUSE_BLOCK_DEVICE_INTERFACE=1` for the SDMMC card.
- `BoardEMinimal78` in `lib_deps`. It defines `eminimal78It8951Config()`, which
  `It8951Driver.cpp` selects for this device unless the build passes
  `-DFREEINK_IT8951_CONFIG`. Without it the driver fails to link.

## Pins

| Function | GPIO |
|---|---|
| IT8951 SPI | SCLK 14, MOSI 16, MISO 17, CS 18 |
| IT8951 HRDY | 38 |
| HAT power enable | 39 |
| IT8951 RST | not wired |
| SD (SDMMC, 1-bit) | CLK 12, CMD 11, D0 13 |
| SD power | 21, active-low (PNP high-side switch; the sleep path parks it high) |
| Buttons (active-low, internal pull-ups) | Back 7, Select 6, Up 4, Down 5, Power 15 |
| Battery ADC | 8 (ADC1_CH7), 2 x 470k divider with 100 nF across the lower leg |

GPIO 33-37 carry the octal PSRAM on an N16R8 and must stay clear.

Power (GPIO 15) is RTC-capable and is the ext1 deep-sleep wake source. The
profile leaves Left/Right unassigned; firmware written for left/right page
keys needs its own mapping for this board.

## IT8951 configuration

`eminimal78It8951Config()` (`libs/hardware/BoardEMinimal78`):

| Field | Value | Why |
|---|---|---|
| `miso` | 17 | The HAT has its own bus; the driver default (13) is this board's SD D0. |
| `spiHz` | 13.33 MHz | Commands and reads. 20 MHz corrupts the 20-word GET_DEV_INFO read on jumper wiring. |
| `loadSpiHz` | 20 MHz | Bulk image writes only. |
| `vcomMv` | 1920 | -1.92 V from the panel's FPC sticker. The controller arrived at -2.50 V and ghosted. |
| `fullMode` / `fastMode` / `grayMode` | 2 / 1 / 7 | GC16 / DU / DU4 on the M841 LUT, where A2 is 6 and DU4 is 7 (M5Paper's LUT has them the other way round). |
| `wakeScrub` | `WhiteGc16` | INIT alone, or INIT + INIT, leaves a shadow of the sleep cover. |
| `loadDepth` | `Bpp1` | B/W frames go up as a 1bpp bitmap (328 KB instead of 1.31 MB). Gray pages stay 4bpp. |
| `combinedGray` | `true` | Anti-aliased pages go up as one load and one DU4 refresh. |

The controller expands 2bpp input as `v << 2`, so white lands at 0xC;
`It8951LoadDepth::Bpp2` is not usable on this panel.

## Driver behaviour specific to this board

These paths exist in `It8951Driver` for any IT8951 board but stay off on
M5Paper:

- **Partial-area refresh.** When the frame memory mirrors the driver's last
  B/W snapshot, `display()` diffs the new frame against it and loads and
  refreshes only the changed rows. It needs load rotation 0 and a panel that
  reports the framebuffer's own geometry. M5Paper's panel reports portrait
  and is loaded at 90°, so it always takes the whole-frame path.
- **1bpp bitmap loads** (`loadDepth = Bpp1`, rotation 0 only).
- **Combined gray pages** (`combinedGray`). The driver advertises
  `GrayscaleBase::Combined`, `displayGrayscaleBase()` only stages the base, and
  `displayGray()` is the page's single refresh. Because that DU4 refresh is
  then the only drive the glass got, the next B/W screen change is promoted
  to GC16.

## Validation

Validated on hardware: boot, deep-sleep wake on Power, B/W and anti-aliased
pages, partial-area menu refreshes, 1bpp loads, and SD over 1-bit SDMMC
(about 1.9 MB/s). 4-bit SDMMC is untested (D1-D3 are not wired).
