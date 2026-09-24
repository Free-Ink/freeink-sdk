# Xteink X3 — UC8279d controller variant

Newer X3 production units (Xteink heads-up, July 2026) ship the same ESP32-C3
board and 792×528 glass with a **UC8279d** panel controller in place of the
UC8253. Everything else — pinout, ADC ladder input, BQ27220/DS3231/QMI8658
peripherals, SD wiring — is unchanged. The variant has its own sibling profile,
`BoardConfig::XTEINK_X3_UC8279` (`Board::XteinkX3Uc8279`).

Build: nothing new — `-DFREEINK_DEVICE_X3=1` links both X3 drivers
(`FREEINK_DRIVER_UC8253_X3` and `FREEINK_DRIVER_UC8279`); which one runs is
decided at boot.

The detection section reflects the stock V6.3.15 protocol. The driver uses
recovered external waveforms and register initialization; the initial OTP-only
proposal is no longer the implementation.

## Runtime detection

After the X3 I2C fingerprint, `detectX3DisplayController()` uses the fixed X3
pins (SCLK 8 / SDA 10 / CS 21 / DC 4 / RST 5 / BUSY 6), even while the active
profile still names X4. `detectXteinkDisplayController()` uses the same X3
protocol when either X3 profile is already active.

The probe follows stock V6.3.15: RESET high 10 ms, low 50 ms, high 50 ms;
wait up to 300 ms for BUSY high; release SDA to input and read three bytes of
**VER (0x70)**, sampling after SCLK rises. A BUSY timeout is recorded but does
not suppress the read, as in stock firmware. The third byte selects the panel:
`0x66` confirms UC8279, `0xFF` assumes UC8253, other IDs are inconclusive and
leave the default UC8253 selected. FLG and MTP are not read or required.

The legacy five-byte VER out-param contains the three read bytes and two zeros;
the FLG out-param is zero (not sampled). Diagnostics expose `verBytesRead=3`
and `busyTimedOut`. Boot logs show `[XTDET] X3 stock probe VER=...` with the
selection and timeout status. X4-family probes retain their existing protocol.

Hardware validation remains necessary: collect these logs on affected units
and check cold boot and sleep/wake. This change updates identification only;
display refresh reset timing, SPI frequency, and waveforms are separate tests.

## Driver — `Uc8279Driver`

KW mode (`PSR KW/R=1`): 1-bpp, DTM1 = OLD plane, DTM2 = NEW plane,
differential refresh — the same paradigm as the UC8253 X3 driver, and a
near-identical command set (PSR/PON/POF, DTM1 `0x10`, DSP `0x11`, DRF `0x12`,
DTM2 `0x13`, CDI `0x50`, TRES `0x61`, DSLP `0x07`+`0xA5`).

The driver programs the panel explicitly with external LUTs (`PSR REG=1`).
The module's blank MTP cannot supply the factory defaults assumed by the original
OTP-only proposal. The recovered initialization script configures power,
booster, PLL, and a 792×528 partial window; RAM writes and refreshes use that
window to avoid the controller's native 800×600 stride.

- B/W refreshes use recovered full (GC) and fast (DU) banks.
- Overlay grayscale uses XTF_AA waveforms with a separate B/W conditioning pass.
- Absolute four-tone images use the XTH4 bank. Both grayscale modes support
  complete-plane and strip uploads; neither advertises asynchronous grayscale
  conditioning or staging while busy.
- Ordinary B/W refreshes support the split `displayStart()` / `displayFinish()`
  interface.

See the [driver implementation](../libs/display/FreeInkDisplay/src/driver/Uc8279Driver.cpp),
[waveform tables](../libs/display/FreeInkDisplay/src/lut/Uc8279X3Luts.h), and
[grayscale API guide](grayscale-capabilities.md). Run the
[display host tests](testing.md#display-drivers) for protocol regression coverage;
physical tone separation and refresh quality still require hardware checks.

## Useful UC8279 features not yet wired

- **AUTO (0x17)**: `PON→DRF→POF(→DSLP)` as one command — could shave host
  round-trips on sleepy ESL-style updates.
- **PBC (0x44)**: panel-break check via the CHKGI/CHKGO wire loop, if the
  module bonds it.
- **CRC (0x72)**: MTP integrity check over `0x000–0xFFF`.
- On-chip temperature readback (**TSC 0x40**) if the consumer ever wants it.
