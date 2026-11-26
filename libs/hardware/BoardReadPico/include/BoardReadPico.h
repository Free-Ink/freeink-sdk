/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// FreeInk board support for Read Pico (小纸 Pico, RDP-G01-W): the FCA9555 expander,
// the SY7636A EPD PMIC power sequence, the CW32L010 PMU, the CST836U touch reset,
// the SD card detect and the buzzer idle state.
//
// This header is the interface the SDK seams call. It is deliberately NOT a full
// driver set: the panel bus lives in ReadPicoEpdiyConfig.cpp, the touch read lives in InputManager, and the IMU/RTC
// backends live in the Imu / Rtc libraries. The board only owns what has to be
// sequenced in hardware order and what sits on the expander or the PMU.
//
#include <Arduino.h>

#include "BoardReadPicoPins.h"

namespace BoardReadPico {

// Bring the board up: shared I2C, FCA9555 self-test + Port-0 config, the
// MODE/TP_RST preload, the PMU handshake, the SC7A20H identity probe, and
// the buzzer pin parked LOW. Idempotent. Returns false when
// the expander never answered — nothing else on this board can be sequenced
// without it, so callers should treat that as fatal.
bool begin();

// True once I2C + the FCA9555 came up (the SY7636A and the panel need it).
bool ready();

// FCA9555 INT# level. `fca9555.h`: INT# is open-drain and NOT latched, so reading
// the Input register releases it.
bool ioeIntAsserted();
void clearIoeInt();

// TF card detect, FCA9555 P0.6, ACTIVE-LOW (0 = card present). Returns false on
// any I2C read failure — a failed read must never be reported as "present"
// (read_pico_board.c `read_pico_sd_present`).
bool sdCardPresent();

// Pulse the CST836U active-low reset (P0.7) LOW for 10 ms, then release.
// After its own deep sleep the chip stops ACKing I2C entirely; this pulse is the
// only way back, and the caller must wait for the chip to boot afterwards
// (read_pico_board.c `read_pico_touch_reset`, cst836u.h RST_HOLD_MS/RST_BOOT_MS).
bool touchReset();

// Put the CST836U into deep sleep (command 0xA503, MSB first —
// cst836u.c `write_cmd` + CST836U_CMD_DEEPSLEEP). It ignores I2C afterwards
// until touchReset() pulses RST. Returns false if the write did not complete.
bool touchSleep();

// Return the virtual button for a raw horizontal point in the key strip.
uint8_t stripButtonMask(uint16_t rawX);

// The input hook samples the PMU on its normal cadence. This query is cache-only.
bool powerKeyPressed();
// True if the first valid PMU status at bring-up records a power-key wake.
bool wokeFromPowerKey();

// --- PMU seams --------------------------------------------------------------
// BatteryMonitor::PmuBatteryHook implementation: reads the CW32L010's 8-byte
// QUICK_BATTERY register (PMU_REG_QUICK_BATTERY 0x85: battery_mv u16 LE,
// soc_permille u16 LE with 0xFFFF = unknown, charge_state u8, flags u8, crc16
// over [0..5]). Returns false when the PMU is absent, the CRC fails, or the
// device reports the value as invalid. chargeState uses the PMU's own
// pmu_charge_state values (0 unknown, 1 not charging, 2 charging, 3 full, 4 fault).
bool pmuBattery(uint16_t& mV, uint16_t& socPermille, uint8_t& chargeState);

// Rtc::PmuTimeHooks implementations: PMU_CMD_TIME_GET (0x0007) / PMU_CMD_TIME_SYNC
// (0x0005). `synced` is the PMU's own calibration flag; a PMU that has never been
// synced returns unixSec = 0.
bool pmuTimeGet(uint32_t& unixSec, bool& synced);
bool pmuTimeSet(uint32_t unixSec);

// Cooperative power handoff. These are the endpoints of the reference firmware's
// read_pico_pmu_report_sleep() / report_ready() / power_off():
//   * pmuReportReady(): HOST_READY (0x0004) then wait for STATUS RUNNING.
//   * pmuSoftSleep():  ensure RUNNING, HOST_SOFT_SLEEP (0x0310), wait for STATUS
//                      SOFT_SLEEP. The PMU then drops the host EN rail.
//   * pmuPowerOff():   HOST_REQUEST_OFF (0x0311), wait for the
//                      SHUTDOWN_REQUESTED event / SHUTDOWN_PENDING state, ack it
//                      with SHUTDOWN_READY (0x0303), then wait for STATUS OFF.
// There is no esp_deep_sleep_start() path on this board: the PMU owns the rail.
bool pmuReportReady();
bool pmuSoftSleep();
bool pmuPowerOff();

// Panel VCOM in mV, READ-ONLY from the PMU (PMU_CMD_VCOM_GET 0x0510; accepted only
// when 500..2500 and a multiple of 10).
//
// There is deliberately no fallback: the value is paired with this glass at the
// factory, and driving the panel at a VCOM that does not match it causes a DC
// imbalance and permanent hardware damage. Returns -1 when the value is
// unavailable or fails the range check, and callers must treat -1 as "do not
// power the panel". Nothing in this port ever writes VCOM back to the PMU.
int pmuVcomMv();

// Board rail sequencing, implemented in ReadPicoPower.cpp.
bool epdPrepare();
bool epdPowerOn();
bool epdSetScanActive(bool active);
void epdPowerOff();

}  // namespace BoardReadPico
