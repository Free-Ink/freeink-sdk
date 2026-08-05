# Xteink X3 — UC8279d controller variant

Newer X3 production units (Xteink heads-up, July 2026) ship the same ESP32-C3
board and 792×528 glass with a **UC8279d** panel controller in place of the
UC8253. Everything else — pinout, ADC ladder input, BQ27220/DS3231/QMI8658
peripherals, SD wiring — is unchanged. The variant has its own sibling profile,
`BoardConfig::XTEINK_X3_UC8279` (`Board::XteinkX3Uc8279`).

Build: nothing new — `-DFREEINK_DEVICE_X3=1` links both X3 drivers
(`FREEINK_DRIVER_UC8253_X3` and `FREEINK_DRIVER_UC8279`); which one runs is
decided at boot.

**Everything below is written from the UC8279d_B 0.1 datasheet (Dec 2025) and
is Pending hardware validation — no UC8279 X3 unit has been on the bench yet.**

## Runtime detection

`XteinkDetect::selectXteinkDevice()` now runs a second fingerprint on a
confirmed X3: `detectX3DisplayController()` bit-bangs a half-duplex 4-wire SPI
read on the X3 display pins (SCLK 8 / SDA 10 / CS 21 / DC 4 / RST 5 / BUSY 6)
after a reset pulse, and reads the UC8279's **VER (0x70)** — reserved `0x00`,
`CHIP_VER` (datasheet default `0x03`), 24-bit `LUT_VER` — and **FLG (0x71)**
status. Signature match (leading `0x00`, non-floating CHIP_VER, FLG idle with
`BUSY_N=1`) in two passes that agree byte-for-byte confirms a UC8279; anything
else conservatively resolves to the shipping UC8253. Raw bytes are exposed via
out-params for bring-up logging.

**Pending:** what the UC8253 actually answers to `0x70` (UC815x-family REV
places the revision in the first byte, which the matcher relies on), and
whether production MTP programs `CHIP_VER` to something other than `0x03` (the
matcher deliberately doesn't pin the exact value).

## Driver — `Uc8279Driver`

KW mode (`PSR KW/R=1`): 1-bpp, DTM1 = OLD plane, DTM2 = NEW plane,
differential refresh — the same paradigm as the UC8253 X3 driver, and a
near-identical command set (PSR/PON/POF, DTM1 `0x10`, DSP `0x11`, DRF `0x12`,
DTM2 `0x13`, CDI `0x50`, TRES `0x61`, DSLP `0x07`+`0xA5`).

v1 uses the **factory OTP waveforms** (`PSR REG=0`): the 4K MTP carries 12
temperature-range LUT sets, each with its own frame rate and rail voltages, and
`TS_AUTO` re-senses temperature before every booster enable — so PWR/PLL/VDCS
stay at silicon defaults and every refresh is temperature-compensated by the
controller. Consequences, all **Pending** bench tuning:

- Full/Half/Fast currently run the same OTP waveform (likely a full GC-style
  flash on every page turn). Fast page turns need custom register banks
  (`REG=1`, commands `0x20`–`0x24`) — note the UC8279 LUT format is
  **group-based** (7-byte groups, 7 groups per LUT in KW mode), *not* the
  UC8253's 43-byte format, so the X3's six tuned banks cannot be copied over.
- No grayscale yet (`supportsStripGrayscale()` false); the X3 reader's 4-level
  AA path needs UC8279-format gray banks tuned on hardware.
- TRES is programmed 792×528. The UC8253 X3 init programs VRES=600 (OEM scans
  the full gate count); if the panel image is offset/compressed, try `0x02 0x58`
  (see the note in `Uc8279Driver::initController`).
- CDI default drives the border white each refresh (`0x97`); the datasheet
  default (`0xD7`) floats it instead. Injectable via `Uc8279Config`
  (`-DFREEINK_UC8279_CONFIG=yourConfig`, same idiom as the other drivers).

## Useful UC8279 features not yet wired

- **AUTO (0x17)**: `PON→DRF→POF(→DSLP)` as one command — could shave host
  round-trips on sleepy ESL-style updates.
- **PBC (0x44)**: panel-break check via the CHKGI/CHKGO wire loop, if the
  module bonds it.
- **CRC (0x72)**: MTP integrity check over `0x000–0xFFF`.
- On-chip temperature readback (**TSC 0x40**) if the consumer ever wants it.
