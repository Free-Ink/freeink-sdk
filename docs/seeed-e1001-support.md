# Seeed reTerminal E1001 support

`-DFREEINK_DEVICE_E1001=1` (`Board::SeeedE1001`, profile name `seeed_e1001`).

An ESP32-S3R8 board (8 MB PSRAM, 32 MB flash) behind a 7.5" 800x480 B/W e-paper
panel with a UC8179 controller, three keys, a microSD slot, a PCF8563 RTC, an
SHT40 temperature/humidity sensor and a battery charger. No touch, no frontlight.

## Pin evidence

The pins were confirmed on retail units with a bring-up sketch (panel, keys, SD
and both I2C buses), and the profile has since run in daily use on shipping
firmware (MicroSlate 3.0.x), so none of it is pending validation.

| Function | Pins |
|---|---|
| EPD SPI | SCK 7, MOSI 9, CS 10, DC 11, RST 12, BUSY 13 (no MISO, rail always on) |
| EPD SPI clock | 4 MHz (2 MHz in the bring-up sketch; 4 MHz is stable) |
| SD (SPI) | shares SCK 7 / MOSI 9 with the panel; MISO 8, CS 14, power enable 16 (HIGH) |
| Keys | GPIO3 (green, confirm; hold = sleep), GPIO4 up, GPIO5 down, all active-low |
| Battery | ADC GPIO1 through a 10k/10k divider; divider enable GPIO21 (HIGH) |
| I2C0 | SDA 19, SCL 20: PCF8563 RTC @ 0x51, SHT40 @ 0x44 |
| I2C1 | SDA 39, SCL 40: charger @ 0x6B |

The battery-divider enable is carried as power latch 0, so `holdPowerRails()`
drives it HIGH first thing in boot and the ADC reading means something.

## Panel

`Uc8179E1001Driver` is a separate, compact driver rather than a config for the
X4-family `Uc8179Driver`. That driver follows the X4 Pro's OEM firmware: it
addresses the panel as 800x600 and pads every transfer, reverses the row order
and carries the X4 Pro's power and grayscale configuration. The E1001's glass is a plain 7.5"
UC8179 module driven the GxEPD2 way: the host programs PWR (0x01) and BTST
(0x06), the panel is addressed at its visible size, and it runs in 1-bpp KW mode
(DTM1 = OLD plane, DTM2 = NEW plane). Geometry comes from the board profile.

* **Full refresh** (about 1.6 s): the OLD plane is loaded with the complement of
  the new frame so every pixel is driven, using the panel's OTP waveform.
  Afterwards the OLD plane is re-synced to the frame on screen.
* **Fast refresh**: the driver keeps a copy of the last frame, finds the
  changed rows and byte columns, and refreshes only that window (PTIN/PTL/PTOUT)
  without powering the panel down between updates. By default it uses the OTP
  waveform at TSSET 0x6E (about 870 ms). `uc8179E1001SetFastWaveform(true)`
  loads a shortened register waveform instead (16 frames per phase, about
  770 ms); it drives the ink less and can leave faint traces. 12 frames left
  visible traces on a real unit.
* Single-buffer only (`EINK_DISPLAY_SINGLE_BUFFER_MODE`); no grayscale.

## Not covered

* The SHT40 has no `EnvironmentSensor` backend wired for this board.
* The charger on I2C1 is not read; battery level comes from the ADC only.
