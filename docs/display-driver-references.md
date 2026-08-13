# Display driver reference coverage

Matching a controller family is not enough to copy an initialization sequence
or waveform. Glass, gate count, analog rails, OTP contents, temperature policy,
and panel mounting remain device-specific. Use the most exact source available
for each FreeInk target.

| FreeInk target | Controller/path | Primary source | Useful secondary source | Coverage notes |
|---|---|---|---|---|
| Xteink X4 / de-link | SSD1677, GDEQ0426T82 | X4 stock firmware and inherited community-sdk tuning | [GxEPD2 GDEQ0426T82 driver](https://github.com/ZinggJM/GxEPD2/blob/master/src/gdeq/GxEPD2_426_GDEQ0426T82.cpp) | Exact controller, glass, and 800×480 geometry. Compare command sequencing and RAM baseline handling directly; retain FreeInk's field-tested AA LUTs. |
| Xteink X4 Pro, UC8179 batch | UC8179, 800×480 visible / 800×600 addressed | X4 Pro factory firmware and hardware reference | [GxEPD2 GDEY075T7 driver](https://github.com/ZinggJM/GxEPD2/blob/master/src/gdey/GxEPD2_750_GDEY075T7.cpp) | Same controller family and visible resolution, but different glass and gate scan. Controller-level behavior such as PWS and old/new RAM lifecycle is useful; do not transplant TRES, booster, temperature, or LUT values wholesale. |
| Xteink X4 Pro, UC8279 batch | UC8279, 800×480 visible / 800×600 addressed | X4 Pro hardware reference and factory firmware | None in GxEPD2 | Keep separate from both the UC8179 X4 Pro driver and the UC8279d X3 driver. |
| Xteink X3, original batch | UC8253, 792×528 | X3 stock firmware and inherited community-sdk tuning | [GxEPD2 UC8253 drivers](https://github.com/ZinggJM/GxEPD2/tree/master/src/gdey) | Controller-family reference only. The X3 glass, addressing, and waveform banks are specific to the device. |
| Xteink X3, newer batch | UC8279d, 792×528 | X3 stock firmware, recovered LUTs, and UC8279d datasheet | None in GxEPD2 | Device-specific MTP/OTP behavior and 99-byte visible rows require the dedicated driver. |
| Murphy M3 | UC8253, 240×416 | Murphy vendor sequences and panel LUTs | [GxEPD2 GDEY037T03 driver](https://github.com/ZinggJM/GxEPD2/blob/master/src/gdey/GxEPD2_370_GDEY037T03.cpp) | Close panel-level comparison. Preserve Murphy's verified reset cadence, rotation, and supplied LUTs unless hardware testing supports a change. |
| M5Stack Paper Mono | SSD1677, 800×480 | M5 board implementation and on-glass measurements | [GxEPD2 SSD1677 drivers](https://github.com/ZinggJM/GxEPD2) | This panel uses a dedicated FreeInk driver with host-authored, panel-measured grayscale waveforms. |
| M5Paper v1.1 | IT8951E + ED047TC1 | M5Paper hardware and IT8951 firmware/protocol | [GxEPD2 IT8951 drivers](https://github.com/ZinggJM/GxEPD2/tree/master/src/it8951) | Protocol reference only: GxEPD2 lists other IT8951 panel/HAT combinations. VCOM, geometry, rotation, and waveform availability come from the attached controller. |
| LilyGo T5 S3 | Raw-parallel ED047TC1 via LovyanGFX | LilyGo hardware and LovyanGFX `Panel_EPD` | None directly applicable | There is no on-glass SPI controller to compare with GxEPD2's panel drivers. |
| M5Stack PaperColor | ED2208 / optional M5GFX backend | M5Stack vendor libraries and measured interrupted-refresh behavior | None in GxEPD2 | Six-color full-refresh operation is outside GxEPD2's matching controller set. |

## Change policy

- Exact panel matches may justify sequence-level comparisons, but still require
  hardware validation before replacing known-good FreeInk behavior.
- Controller-only matches are evidence for command semantics, RAM lifecycle,
  and optional registers—not for analog values, gate geometry, or LUT timing.
- Factory firmware or a device-specific vendor reference wins when it conflicts
  with a generic library using different glass.
- Waveforms and forced-temperature values are panel-specific. Never copy them
  solely because the controller name matches.
