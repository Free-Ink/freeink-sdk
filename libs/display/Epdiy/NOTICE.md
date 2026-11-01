# Epdiy library notice

This directory is **not** MIT like the rest of the FreeInk SDK. It is licensed
under the GNU Lesser General Public License, version 3 (`LICENSE`, which builds
on the GNU GPL v3 in `COPYING.GPL-3.0`).

## Origin

- [EPDiy](https://github.com/vroland/epdiy) by Valentin Roland and contributors.
  Its firmware library is LGPL-3.0.
- Ported to the ESP32-S31 and ESP-IDF 6.1 by the board vendor, Metalio (the
  files credit Shenzhen Xinzhi Future Technology Co., Ltd.), in
  [CloudZao/Metalio-E-INK4-Plus](https://github.com/CloudZao/Metalio-E-INK4-Plus),
  commit `690a023c66862af103a3187464373ee7c5efcd14`, directory
  `main/boards/metalio-e-ink4-plus/display/`. That repository is published
  under MIT, but the EPDiy code it contains remains LGPL-3.0. The upstream EPDiy
  commit the vendor started from has not been identified.
- `src/waveforms/my_gray8_*.h` carry their own Apache-2.0 notice
  (`src/waveforms/LICENSE.gray8`).

## Changes made here (FreeInk, October 2026)

- Copied only the rendering core: `epdiy`, `render`, `highlevel`, `epd_board`
  (with the `epd_board_specific.h` declarations that `epdiy.h` includes),
  `output_common/`, `output_lcd/`, the RISC-V replacements for the Xtensa SIMD
  routines (`epdiy_riscv_ve_stub.c`) and the `my_waveform*` tables. The
  Metalio board glue, touch, fonts and images were left out; the FreeInk
  board glue for the Plus is separate MIT code.
- `src/displays.c` keeps only the `E0470A14_AF` panel, so the unused built-in
  waveform tables are not linked.
- `src/output_lcd/lcd_driver.c` passes LCD group 0 to `lcd_ll_select_clk_src()`
  and `lcd_ll_set_group_clock_coeff()`: the ESP-IDF 6.1 snapshot in
  pioarduino's Arduino 4.0 takes a group id there, not the device pointer.
- `src/output_common/lut.c` adds an ESP32-S31 pixel-pair lookup used when
  EPDiy is initialized with `EPD_LUT_64K`. The vendor's 1K lookup needs about
  37 us per 1216-pixel row on the S31, more than the 34 us each of the two
  line producers has, so the LCD stream underran. The 64K table is expanded
  from the vendor's 1K table each phase and keeps its bit order.
- `src/render.c` sizes each line queue at 104 rows instead of the vendor's 128,
  freeing ~14.6 KB of internal RAM; the 192-row prefill needs 96 per producer.
- `output_common/lut.S` is Xtensa-only and is excluded from the build in
  `library.json`.

## Compliance

This is not legal advice. Whoever distributes a firmware image that links this
library (it is statically linked) should, as LGPL-3.0 section 4 asks: state that
the image uses EPDiy under the LGPL-3.0, ship or point to the LGPL-3.0 and
GPL-3.0 texts, and point to the exact source commit, so a recipient can rebuild
the image with a modified copy of this library. Keep this directory's license
files with any copy of the source.
