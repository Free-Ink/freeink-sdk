# Waveshare ESP32-S3-ePaper-3.97 support

`-DFREEINK_DEVICE_WS397=1` (`Board::WsEpaper397`, profile name `ws397`).

An ESP32-S3-WROOM-1-N16R8 carrier for a 3.97" 800x480 B/W e-paper panel, with an
AXP2101 PMIC, 4-bit SDMMC card slot, three side keys, PCF85063 RTC, QMI8658 IMU,
SHTC3 temp/humidity sensor and an ES8311 audio codec.

Product page: <https://www.waveshare.com/esp32-s3-epaper-3.97.htm> ·
Wiki: <https://docs.waveshare.com/ESP32-S3-ePaper-3.97>

## Pin evidence

Everything below comes from the vendor's own sources
(<https://github.com/waveshareteam/ESP32-S3-ePaper-3.97>), not from a datasheet
guess:

| Function | Pins | Source |
|---|---|---|
| EPD SPI | SCK 11, MOSI 12, CS 10, DC 9, RST 46, BUSY 3 | `Arduino/examples/02_E-Paper_Example/DEV_Config.h`, `ESP-IDF/08_.../components/epaper_port/epaper_port.h` |
| EPD SPI clock | 20 MHz | `epaper_port.c` (`clock_speed_hz`) |
| EPD power rail | AXP2101 **ALDO3** | `components/epaper_port/epaper_port.c` (`enapwrstate(ALDO3)`) |
| SD (SDMMC) | CLK 16, CMD 17, D0 15, D1 7, D2 8, D3 18 (4-bit) | `components/sdcard_bsp/sdcard_bsp.c` |
| Buttons | UP 4, OK 5, DOWN 6, BOOT 0 — all active-low | `components/button_bsp/button_bsp.c` |
| PMIC interrupt | GPIO38, active-low | NOT in the vendor sources — identified by probing during bring-up |
| Deep-sleep wake | GPIO0 | `Arduino/examples/.../user_config.h` (`ext_wakeup_pin_1`) |
| I2C bus | SDA 41, SCL 42 | `user_config.h` |
| PMIC | AXP2101 @ 0x34 | `components/axpPower/axp_prot.cpp` |
| RTC | PCF85063 @ 0x51 | `user_config.h` |
| Temp/humidity | SHTC3 @ 0x70 | `user_config.h` |
| IMU | QMI8658 (0x6B, 0x6A alt) | `components/qmi8658_bsp` |
| Audio | ES8311: MCLK 13, BCK 14, WS 47, DIN 21, DOUT 48, PA 39 | `components/es8311_bsp/es8311_bsp.h` |

## Panel

The controller is SSD1677-class and the vendor bring-up is byte-identical to the
Seeed Sticky's — booster `AE C7 C3 C0 80`, driver output scan `0x02`, data entry
`0x01`, border `0x01`, and update sequences `0x22 = F7` (full) / `FF` (partial) /
`D7` (fast). `ssd1677ActiveConfig()` therefore returns `ssd1677StickyConfig()` for
this board, including its grayscale LUT.

## AXP2101 (Axp2101.h)

The PMIC is load-bearing, not an accessory:

* **ALDO3 is the e-paper rail.** The panel is dead until it is enabled, so
  `EpdBus::begin()` calls `axp2101::setEpdPower(true)` through the board hook
  (the profile's `display.powerEnable` stays unassigned), and
  `PowerManager::powerDownRailsForSleep()` drops it before deep sleep.
* **It is the only battery telemetry** — no ADC divider, no gauge chip.
  `GaugeType::Axp2101` reads SoC (0xA4), VBAT (0x34/0x35) and charge state
  (0x01[7:5]).
* **It owns the power key.** PWRKEY does a hardware 1 s power-on / 4 s power-off
  that firmware never sees, and `axp2101::shutdown()` (COMMON_CONFIG[0]) is a full
  software power-off.

## Buttons

Four usable keys: the three-way side rocker (UP 4 / OK 5 / DOWN 6) plus BOOT
(GPIO0). BOOT is both Back and the power button — hold to sleep, press to wake —
because it is the pin the vendor arms for deep-sleep wake and the only key that can
serve as one.

There are no Left/Right keys, so `KeyboardEntryActivity` falls back to walking the
key grid in reading order with Up/Down (`hasHorizontalKeys()`); every key stays
reachable from one button pair. That fallback keys off the profile, so it switches
itself off on any board that does wire a horizontal key.

### The case-labelled PWR key is not wired to the CPU (as far as bring-up could tell)

The board has a fifth, PWR-labelled key that the firmware cannot see. It is absent
from the vendor's `button_bsp.c`, and a bring-up probe that watched every unclaimed
GPIO (1, 2, 13, 14, 21, 38, 39, 40, 45, 47, 48) plus the AXP2101 interrupt-status
registers caught nothing while the key was pressed — including no `PKEY` bits in
`INTSTS2` with the PWRKEY short-press interrupt enabled. Holding it past the
PMIC's 4 s power-off time did not cut power either (the board was on USB).

Do NOT map GPIO38 as that key. It carries the AXP2101's active-low interrupt
output, which sits LOW for as long as any PMIC interrupt is pending — a gauge
"new SoC" interrupt during the probe read as a 55 ms button press, and mapping the
pin as a key reads as one held down forever. That line is useful, just not as a
button: it is how a future change could react to PMIC events without polling.

Pins deliberately left out of that probe, and therefore still unchecked: 19/20
(native USB), 33-37 (octal PSRAM), 43/44 (UART).

## Flashing

The board powers itself down a few seconds after reset unless firmware is running
and holding the PMIC, so a plain `pio run -t upload` loses the port mid-write.
Flash with the chip parked in the ROM downloader (`esptool --after no-reset`), or
in slices that resume across power cuts. Once CrossPoint is running it calls
`axp2101::ensureBooted()` and the board stays up normally.

## Confirmed on hardware

Bring-up on a unit, reading an EPUB end to end:

* Panel bring-up through the PMIC rail: `EpdBus` -> `axp2101::setEpdPower(true)` ->
  SSD1677 responds, full refresh in 2.08 s.
* `NO_FLIP` is the correct mount orientation — text renders upright.
* PCF85063 RTC answers (`[CLK] SDK RTC found`).
* 4-bit SDMMC mounts and carries the section cache under `/.crosspoint/`.
* Buttons page through a book; BOOT wakes the board out of deep sleep without
  falling into the ROM downloader, so GPIO0 doubling as Back/power holds up.
* Grayscale renders (`planes buffered: 2`), ~910 ms per page turn.
* ~240 KB of 329 KB heap free while reading (219 KB low-water).

## Pending hardware validation

* **Grayscale LUT** — reused from the Sticky; retune if a unit shows banding on
  covers.
* **Battery curve** — the AXP2101 fuel gauge learns its curve at runtime; SoC may
  be coarse until it has seen a full charge cycle.
* **SPI clock** — 20 MHz is the vendor value and it is NOT worth raising: a page
  turn measures `wait=407ms` (BW waveform) + `gray_display=226ms` against
  `display=24ms` of actual SPI traffic, so the 40 MHz default would save ~10 ms of
  910. The panel's waveforms are the floor here, not the bus.
* SHTC3 has no `EnvironmentSensor` backend yet (`tempHumidityAddr` is left 0), and
  the ES8311 codec / NS4150B amp are wired but unused (`NO_AUDIO`).
