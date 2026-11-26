/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// Internal transport shared by BoardReadPico.cpp and ReadPicoPower.cpp.
// Not part of the public API: only include/BoardReadPico*.h is installed, and
// none of this is declared there on purpose (see docs/engineering/read-pico.md
// §3.4 for the frozen public symbol list).

#include <Arduino.h>

namespace BoardReadPico {
namespace detail {

// --- I2C --------------------------------------------------------------------
// One recursive lock for the whole shared bus (SDA39/SCL40, 400 kHz). Every
// helper below takes it, so callers may nest.
void beginI2C();

// Register-oriented transfer used by every device on the bus. `r` uses a
// repeated START as required by the FCA9555. The SY7636A uses the
// STOP-then-read helper below. InputManager owns CST836U transfers.
bool i2cWrite(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len);
bool i2cRead(uint8_t addr, uint8_t reg, uint8_t* data, size_t len);

// --- FCA9555 expander (0x24) ------------------------------------------------
// Port-0 output shadow, mirroring read_pico_board.c's `ioe_output`: the reference
// keeps the whole byte and commits it, instead of a per-bit read-modify-write.
// Initialised to READPICO_IOE_OUT0_INIT (MODE high, TP_RST released, SY_EN and
// VCOM_EN low) by ioeConfigure().
uint8_t ioeOutput();
bool ioeSetOutput(uint8_t value);
bool ioeSetBit(uint8_t bit, bool high);
// Write CFG0, then read it back and compare — the reference's
// `fca9555_selftest` step for the direction register.
bool ioeConfigure();

bool ioeReadPort0(uint8_t& in0);
// -1 when the level could not be read, 1/0 otherwise.
int ioePgoodLevel();

// --- SY7636A EPD PMIC (0x62) -----------------------------------------------
// Register map from components/sy7636a/sy7636a.c: REG_OPERATION 0x00,
// REG_VCOM_LSB 0x01, REG_VCOM_MSB 0x02, REG_VLDO 0x03, REG_DELAY 0x06,
// REG_FAULT 0x07, REG_TEMP 0x08.
// Reads must be STOP-then-read ("手册要求先 STOP 再读，不能 Repeated START",
// sy7636a.c read_reg), so they use their own helpers rather than i2cRead().
bool syWrite(uint8_t reg, uint8_t value);
bool syRead(uint8_t reg, uint8_t& value);
bool sySetVcom(int mv);

// --- PMU (0x2A) -------------------------------------------------------------
// Whether the CW32L010 answered and its identity parsed.
bool pmuReady();
// Read + parse IDENTITY (32 B) and STATUS (64 B); also fixes up the sequence
// number from the PMU's last_command_sequence, which survives an ESP-only reset.
bool pmuInit();

}  // namespace detail
}  // namespace BoardReadPico
