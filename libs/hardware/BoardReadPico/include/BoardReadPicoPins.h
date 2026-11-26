/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// Read Pico (小纸 Pico, RDP-G01-W) pin map — the parallel-EPD bus, the FCA9555
// Port-0 line table, the I2C device addresses and the board-support-only pins
// that do not live in the BoardProfile.
//
// Sources (all MindReset/read_pico_firmware @ main, Apache-2.0):
//   components/read_pico/read_pico_board.c   D0..D15, EPD_XLE/XSTL/XCL/SPV/CKV,
//                                            IOE_MODE/XOE/CW_INT/SY_EN/SY_VCOM_EN/
//                                            SY_PGOOD/SD_CD/TP_RST, IOE_CONFIG_PORT0
//   components/read_pico/read_pico_sd.c      SD_PIN_CLK/CMD/D0
//   components/read_pico/read_pico_buzzer.c  BUZZER_GPIO
//   components/read_pico/include/read_pico_board.h
//                                            READ_PICO_IOE_INT_GPIO, READ_PICO_TP_INT_GPIO
//   components/read_pico/read_pico_init.c    READ_PICO_PCLK_MHZ, READ_PICO_VCOM_MV,
//                                            READ_PICO_ACCEL_INT1
//   components/fca9555/include/fca9555.h     FCA9555_ADDR_DEFAULT, register offsets
//   components/sy7636a/include/sy7636a.h     SY7636A_ADDR_DEFAULT
//   components/cst836u/include/cst836u.h     CST836U_ADDR_DEFAULT
//   components/sc7a20h/include/sc7a20h.h     SC7A20H address
//   components/read_pico_pmu/include/read_pico_pmu_protocol.h  PMU_I2C_ADDR
//   main/ui/ui_menu.h                        UI_KEY_AREA_TOP / UI_KEY_PITCH / centres
// See docs/engineering/read-pico.md for the full contract.
//
// Every value below is quoted from the reference firmware; nothing here is
// inferred from another board.

// --- 16-bit parallel EPD data bus -------------------------------------------
// read_pico_board.c: `#define D0 GPIO_NUM_4` … `#define D15 GPIO_NUM_45`
// (D0..D14 = GPIO 4..18 contiguous, D15 = GPIO 45).
#define READPICO_EP_D0 4
#define READPICO_EP_D1 5
#define READPICO_EP_D2 6
#define READPICO_EP_D3 7
#define READPICO_EP_D4 8
#define READPICO_EP_D5 9
#define READPICO_EP_D6 10
#define READPICO_EP_D7 11
#define READPICO_EP_D8 12
#define READPICO_EP_D9 13
#define READPICO_EP_D10 14
#define READPICO_EP_D11 15
#define READPICO_EP_D12 16
#define READPICO_EP_D13 17
#define READPICO_EP_D14 18
#define READPICO_EP_D15 45

// --- EPD control lines ------------------------------------------------------
// read_pico_board.c: EPD_XLE/EPD_XSTL/EPD_XCL/EPD_SPV/EPD_CKV.
// LCD_CAM generates HSYNC/DE/PCLK; CKV uses RMT.
#define READPICO_EP_XLE 3    // XLE  (latch enable)   -> LCD_CAM HSYNC
#define READPICO_EP_XSTL 46  // XSTL (start pulse, horizontal) -> LCD_CAM DE
#define READPICO_EP_XCL 21   // XCL  (pixel clock)    -> LCD_CAM PCLK
#define READPICO_EP_SPV 47   // SPV  (start pulse, vertical)
#define READPICO_EP_CKV 48   // CKV  (gate clock)     -> RMT

// --- Shared I2C bus ---------------------------------------------------------
// read_pico_firmware/README.md (Pinout) + read_pico_board.c `board_init`:
// SCL 40 / SDA 39 at 400 kHz.
#define READPICO_I2C_SDA 39
#define READPICO_I2C_SCL 40
#define READPICO_I2C_HZ 400000u

// --- Interrupts -------------------------------------------------------------
// FCA9555 INT# on GPIO41 — not an RTC-capable S3 pin, so light-sleep GPIO wake
// only (read_pico_board.h:28-30; main/sleep.c lock_arm_ioe_wakeup).
#define READPICO_IOE_INT 41
// CST836U INT on GPIO43 — open-drain, active-low, 10 k pull-up on the FPC
// (read_pico_board.h:31-33; read_pico_firmware/README.md Pinout).
#define READPICO_TP_INT 43
// SC7A20H INT1 (read_pico_init.c `READ_PICO_ACCEL_INT1 GPIO_NUM_1`).
#define READPICO_ACCEL_INT1 1

// --- Storage ----------------------------------------------------------------
// read_pico_sd.c: SD_PIN_CLK 38 / SD_PIN_CMD 42 / SD_PIN_D0 44, 1-bit SDMMC,
// SDMMC_FREQ_HIGHSPEED (40 MHz), SDMMC_SLOT_FLAG_INTERNAL_PULLUP, cd/wp = NC.
#define READPICO_SD_CLK 38
#define READPICO_SD_CMD 42
#define READPICO_SD_D0 44

// --- Buzzer -----------------------------------------------------------------
// read_pico_buzzer.c: BUZZER_GPIO GPIO_NUM_2, LEDC timer0 / channel0,
// LEDC_TIMER_10_BIT, idle duty 0 (GPIO2 -> AO3400A gate, high = on).
#define READPICO_BUZZER 2

// --- I2C device addresses (7-bit) -------------------------------------------
#define READPICO_IOE_ADDR 0x24    // FCA9555_ADDR_DEFAULT (A2=1 A1=0 A0=0)
#define READPICO_PMU_ADDR 0x2A    // PMU_I2C_ADDR (CW32L010)
#define READPICO_SY_ADDR 0x62     // SY7636A_ADDR_DEFAULT (EPD PMIC)
#define READPICO_TP_ADDR 0x15     // CST836U_ADDR_DEFAULT
#define READPICO_ACCEL_ADDR 0x19  // SC7A20H (WHO_AM_I 0x11, VERSION 0x28)

// --- FCA9555 register map ---------------------------------------------------
// fca9555.h: FCA9555_REG_IN0 0 / IN1 1 / OUT0 2 / OUT1 3 / INV0 4 / INV1 5 /
// CFG0 6 / CFG1 7, little-endian with Port 0 in the low byte.
#define READPICO_IOE_REG_IN0 0x00
#define READPICO_IOE_REG_OUT0 0x02
#define READPICO_IOE_REG_CFG0 0x06
#define READPICO_IOE_REG_CFG1 0x07

// FCA9555 Port-0 bit offsets (read_pico_board.c `1U << n`; the names match
// main/apps/app_ioe.c IOE_BIT_*).
#define READPICO_IOE_MODE 0     // OUT: EPD MODE pin, held HIGH by init/poweron/poweroff
#define READPICO_IOE_XOE 1      // OUT: EPD output enable, LOW while powering up
#define READPICO_IOE_CW_INT 2   // IN : CW32L010 PMU interrupt (light-sleep wake)
#define READPICO_IOE_SY_EN 3    // OUT: SY7636A enable; LOW resets every PMIC register
#define READPICO_IOE_VCOM_EN 4  // OUT: external VCOM enable, active during scans
#define READPICO_IOE_PGOOD 5    // IN : SY7636A power good
#define READPICO_IOE_SD_CD 6    // IN : TF card detect, ACTIVE-LOW (0 = present)
#define READPICO_IOE_TP_RST 7   // OUT: CST836U ACTIVE-LOW reset
// read_pico_board.c: `#define IOE_CONFIG_PORT0 0x64` — bits 2, 5, 6 as inputs and
// everything else as output. Port 1 is unused (CFG1 = 0xFF). app_ioe.c shows the
// same 0x64 as IOE_CFG0_EXPECT and deliberately treats CFG/INV as read-only:
// changing a direction can drive a sense pin (PGOOD, card detect).
#define READPICO_IOE_CFG0_EXPECT 0x64
#define READPICO_IOE_CFG1_UNUSED 0xFF
// read_pico_board.c board_init: `ioe_output = IOE_MODE | IOE_TP_RST;` — the
// power-on preload (MODE high, TP_RST released), SY_EN and VCOM_EN left LOW.
#define READPICO_IOE_OUT0_INIT ((1U << READPICO_IOE_MODE) | (1U << READPICO_IOE_TP_RST))

// --- Panel ------------------------------------------------------------------
// VCOM is NOT a build-time constant here. It is paired with the glass at the
// factory and stored in the PMU, and this port only ever reads it through
// BoardReadPico::pmuVcomMv(). There is deliberately no fallback macro: the vendor
// demo hardcodes 1290 mV, but programming a value that does not match the panel
// forces a DC imbalance across the glass and damages it permanently, so an
// unreadable factory value must leave the panel unpowered instead.
//
// read_pico_init.c: READ_PICO_PCLK_MHZ 18 (the board default is 12 MHz; 18 MHz is
// the bench-stable value and drives the blanking/CKV re-solve).
#define READPICO_PCLK_HZ 18000000u
// read_pico_epd_timing.h: READ_PICO_EPD_PCLK_MIN_MHZ 12 / _MAX_MHZ 24 — the
// supported board range the 18 MHz sits inside. The cap is DMA feed capability,
// not a panel spec, and the 120 MHz flash/PSRAM timing that would raise it is
// deliberately not adopted (read-pico.md 2.7).
#define READPICO_PCLK_MIN_HZ 12000000u
#define READPICO_PCLK_MAX_HZ 24000000u

// --- Capacitive key strip ---------------------------------------------------
// main/ui/ui_menu.h: the touch panel is TALLER than the display and three
// capacitive keys sit in the undrawn strip below the image, hit-tested by raw
// coordinate because there is nothing to draw. Measured centres x = 80/240/400,
// y ~= 1500, pitch 160; UI_KEY_AREA_TOP 1300 is the split, chosen so a tap there
// cannot steal from the display's bottom bar (display y max = 1215).
#define READPICO_KEY_AREA_TOP 1300
#define READPICO_KEY_PITCH 160
#define READPICO_KEY_CENTER_1 80
#define READPICO_KEY_CENTER_2 240
#define READPICO_KEY_CENTER_3 400
