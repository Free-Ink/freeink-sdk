/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */

#include <BoardConfig.h>
#include <BoardReadPico.h>

#include <cstdarg>

#if defined(ENABLE_SERIAL_LOG)
#include <esp_rom_sys.h>
#endif

#include "BoardReadPicoInternal.h"

// Board power hooks shared by the epdiy LCD adapter.
//
// Evidence for every pin, register and delay: MindReset/read_pico_firmware @ main
// components/{read_pico/read_pico_board.c, sy7636a/sy7636a.c, fca9555/*} — quoted
// at each step. Contract: docs/engineering/read-pico.md §1.4, §3.4.

namespace BoardReadPico {
namespace {

// Same ROM-console logging as BoardReadPico.cpp (gated on ENABLE_SERIAL_LOG, so
// a board library never depends on the consumer's log facade).
void logLine(const char* fmt, ...) {
#if defined(ENABLE_SERIAL_LOG)
  va_list args;
  va_start(args, fmt);
  esp_rom_vprintf(fmt, args);
  va_end(args);
#else
  (void)fmt;
#endif
}

// --- SY7636A register map and packed field values ---------------------------
// components/sy7636a/sy7636a.c:
//   REG_OPERATION 0x00, REG_VCOM_LSB 0x01, REG_VCOM_MSB 0x02, REG_VLDO 0x03,
//   REG_DELAY 0x06, REG_FAULT 0x07, REG_TEMP 0x08
//   OP_ON 0x80, OP_VCOMCTL 0x40, VLDO_RESERVED 0x06
//   SY7636A_POWER_CONFIG_DEFAULT(): vcom_mv 1250, vldo SY7636A_VLDO_1500,
//     discharge 0, dly_ms {2,2,2,2}, vcom_manual true
// and read_pico_board.c overrides only `sy.power.vcom_mv = 1290`.
constexpr uint8_t kSyRegOperation = 0x00;
constexpr uint8_t kSyRegVldo = 0x03;
constexpr uint8_t kSyRegDelay = 0x06;
constexpr uint8_t kSyRegFault = 0x07;
constexpr uint8_t kSyOpOn = 0x80;       // pack_op(on = true)
constexpr uint8_t kSyOpVcomCtl = 0x40;  // cfg.vcom_manual = true -> external VCOM_EN
// pack_vldo(SY7636A_VLDO_1500 = 3) = (3 << 5) | VLDO_RESERVED(0x06) = 0x66
constexpr uint8_t kSyVldo1500Packed = 0x66;
// pack_delay({2,2,2,2}): dly_enc(2) = 2, so (2 << 6) | (2 << 4) | (2 << 2) | 2
constexpr uint8_t kSyDelayPacked = 0xAA;
// SY7636A_CONFIG_DEFAULT(): en_settle_ms 20, pgood_timeout_ms 300,
// poweroff_hold_ms 500.
constexpr uint16_t kSyEnSettleMs = 20;
constexpr uint16_t kSyPgoodTimeoutMs = 300;
constexpr uint16_t kSyPoweroffHoldMs = 500;
// sy7636a.c power_on: 5 attempts 10 ms apart; the VCOM_EN raise is followed by
// a 5 ms settle; a PGOOD timeout clears OPERATION then holds 150 ms.
constexpr int kSyRetries = 5;
constexpr uint16_t kSyRetryGapMs = 10;
constexpr unsigned kVcomScanSettleUs = 2000;
constexpr uint16_t kSyFaultHoldMs = 150;

bool g_syEnOn = false;
bool g_railsOn = false;

bool syEn(bool on) {
  // IOE_SY_EN (1U << 3): LOW resets every SY7636A register and takes I2C off the
  // bus, so this is also the "PMIC is dead" state.
  if (!detail::ioeSetBit(READPICO_IOE_SY_EN, on)) return false;
  g_syEnOn = on;
  return true;
}

bool syVcomEn(bool on) { return detail::ioeSetBit(READPICO_IOE_VCOM_EN, on); }

// sy7636a.c `sy7636a_i2c_begin`: raise EN, then wait en_settle_ms for the
// digital core before the first register access.
bool syI2cBegin() {
  if (g_syEnOn) return true;
  if (!syEn(true)) return false;
  delay(kSyEnSettleMs);
  return true;
}

// sy7636a.c `sy7636a_i2c_end`: only when WE raised EN and the rails never came
// up — dropping EN resets every register.
void syI2cEndIfWoke(bool woke) {
  if (!woke || g_railsOn) return;
  (void)syEn(false);
}

}  // namespace

// ===========================================================================
// freeink::EpdiyLcdPowerHooks bodies (declared in BoardReadPico.h)
// ===========================================================================

bool epdPrepare() {
  // Park owned pins before LCD_CAM takes control. The epdiy adapter then routes
  // XSTL to DE and XCL to PCLK; board rail timing stays in these power hooks.
  bool ok = true;

  // Plain-GPIO timing lines. SPV idles LOW (read_pico_board.c drives it from the
  // LCD path; LilyGo's equivalent hook also parks EP_STV LOW). CKV / LE / XCL
  // idle LOW: no gate clock, no latch, no pixel clock.
  pinMode(READPICO_EP_SPV, OUTPUT);
  digitalWrite(READPICO_EP_SPV, LOW);
  pinMode(READPICO_EP_CKV, OUTPUT);
  digitalWrite(READPICO_EP_CKV, LOW);
  pinMode(READPICO_EP_XLE, OUTPUT);
  digitalWrite(READPICO_EP_XLE, LOW);
  pinMode(READPICO_EP_XCL, OUTPUT);
  digitalWrite(READPICO_EP_XCL, LOW);
  // Park XSTL LOW until the LCD adapter connects the DE signal.
  pinMode(READPICO_EP_XSTL, OUTPUT);
  digitalWrite(READPICO_EP_XSTL, LOW);

  // 16-bit data bus, all LOW.
  const int8_t dataPins[16] = {READPICO_EP_D0,  READPICO_EP_D1,  READPICO_EP_D2,  READPICO_EP_D3,
                               READPICO_EP_D4,  READPICO_EP_D5,  READPICO_EP_D6,  READPICO_EP_D7,
                               READPICO_EP_D8,  READPICO_EP_D9,  READPICO_EP_D10, READPICO_EP_D11,
                               READPICO_EP_D12, READPICO_EP_D13, READPICO_EP_D14, READPICO_EP_D15};
  for (const int8_t pin : dataPins) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }

  // Buzzer parked LOW (read_pico_buzzer.c: GPIO2 -> AO3400A gate, idle duty 0).
  pinMode(READPICO_BUZZER, OUTPUT);
  digitalWrite(READPICO_BUZZER, LOW);

  // FCA9555 Port-0 preload: MODE = 1, TP_RST released, and XOE / SY_EN / VCOM_EN
  // all LOW so no rail and no high voltage can come up before epdPowerOn() asks
  // for them (read_pico_board.c board_init `ioe_output = IOE_MODE | IOE_TP_RST`).
  if (!ready()) {
    ok = false;
  } else if (!detail::ioeSetOutput(READPICO_IOE_OUT0_INIT)) {
    ok = false;
  }
  return ok;
}

bool epdPowerOn() {
  // These hooks own every rail line for the epdiy LCD adapter.
  // Steps 1-4 are read-pico.md §1.4's verified board_poweron() order.
  if (g_railsOn) return true;

  // VCOM FIRST, read-only, BEFORE anything is energised.
  //
  // The panel's VCOM is paired with this glass at the factory and lives in the
  // PMU. A VCOM that does not match the panel forces a DC imbalance across it and
  // damages it permanently, so there is no default to fall back to: if the factory
  // value cannot be read and validated, every rail stays down and power-on fails.
  // The result is a dark panel plus a log line, which is recoverable — a guessed
  // VCOM is not. Doing the check here (rather than inside the write loop below)
  // also means a failure changes nothing at all on the board.
  const int vcomMv = pmuVcomMv();
  if (vcomMv < 0) {
    logLine("[RDP] refusing to power the panel: no factory VCOM from the PMU\r\n");
    return false;
  }

  // 1. MODE = 1, XOE = 0 in one expander commit (read_pico_board.c sets both
  //    state fields and calls board_set_ctrl once).
  uint8_t out0 = static_cast<uint8_t>(detail::ioeOutput() | (1U << READPICO_IOE_MODE));
  out0 = static_cast<uint8_t>(out0 & ~(1U << READPICO_IOE_XOE));
  if (!detail::ioeSetOutput(out0)) return false;

  // 2. SY_EN = 1 and wait for the PMIC digital core (en_settle_ms = 20 ms).
  const bool woke = !g_syEnOn;
  if (!syI2cBegin()) return false;
  // sy7636a_power_on() forces VCOM_EN low before programming.
  (void)syVcomEn(false);

  // 3. VCOM / VLDO / delays, then ON_OFF.
  bool written = false;
  for (int attempt = 0; attempt < kSyRetries && !written; ++attempt) {
    if (attempt) delay(kSyRetryGapMs);
    // vcomMv was read once, before anything was energised, and is never guessed.
    bool ok = detail::sySetVcom(vcomMv);
    if (ok) ok = detail::syWrite(kSyRegVldo, kSyVldo1500Packed);
    if (ok) ok = detail::syWrite(kSyRegDelay, kSyDelayPacked);
    if (ok) ok = detail::syWrite(kSyRegOperation, static_cast<uint8_t>(kSyOpOn | kSyOpVcomCtl));
    written = ok;
  }
  if (!written) {
    logLine("[RDP] SY7636A power_on register write failed\r\n");
    syI2cEndIfWoke(woke);
    return false;
  }

  // ... wait for PGOOD (FCA9555 P0.5), 300 ms timeout, polled like the reference
  // (esp_rom_delay_us(200) between samples; each sample is an I2C read, which
  // dominates the real cadence).
  bool good = false;
  const uint32_t start = millis();
  while (millis() - start < kSyPgoodTimeoutMs) {
    if (detail::ioePgoodLevel() > 0) {
      good = true;
      break;
    }
    delayMicroseconds(200);
  }
  if (!good) {
    uint8_t fault = 0xFF;
    (void)detail::syRead(kSyRegFault, fault);
    logLine("[RDP] SY7636A PGOOD timeout, fault=0x%02X\r\n", fault);
    (void)detail::syWrite(kSyRegOperation, 0x00);
    delay(kSyFaultHoldMs);
    syI2cEndIfWoke(true);
    return false;
  }

  g_railsOn = true;

  // 4. XOE = 1, rails on.
  out0 = static_cast<uint8_t>(detail::ioeOutput() | (1U << READPICO_IOE_XOE));
  if (!detail::ioeSetOutput(out0)) {
    epdPowerOff();
    return false;
  }
  logLine("[RDP] EPD rails on (SY7636A), XOE raised\r\n");
  return true;
}

bool epdSetScanActive(bool active) {
  if (!g_railsOn) return false;
  const uint8_t scanBits = (1U << READPICO_IOE_MODE) | (1U << READPICO_IOE_VCOM_EN);
  const uint8_t output = detail::ioeOutput();
  if (!detail::ioeSetOutput(active ? output | scanBits : output & ~scanBits)) return false;
  if (active) delayMicroseconds(kVcomScanSettleUs);
  return true;
}

void epdPowerOff() {
  // read_pico_board.c board_poweroff: XOE = 0, MODE = 1, 1 ms, then
  // sy7636a_power_off() -> VCOM_EN low, OPERATION = 0x00, poweroff_hold_ms
  // (500 ms) hold, then SY_EN = 0. §3.4 lists the first three and "PMIC off";
  // the 500 ms hold IS part of the PMIC's own off sequence, so it is kept here.
  uint8_t out0 = static_cast<uint8_t>(detail::ioeOutput() & ~(1U << READPICO_IOE_XOE));
  out0 = static_cast<uint8_t>(out0 | (1U << READPICO_IOE_MODE));
  (void)detail::ioeSetOutput(out0);
  delay(1);

  (void)syVcomEn(false);
  if (g_syEnOn) (void)detail::syWrite(kSyRegOperation, 0x00);
  delay(kSyPoweroffHoldMs);
  (void)syEn(false);
  g_railsOn = false;
  logLine("[RDP] EPD rails off\r\n");
}

}  // namespace BoardReadPico
