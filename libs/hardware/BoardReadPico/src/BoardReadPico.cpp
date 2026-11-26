/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */

#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <BoardReadPico.h>
#include <InputManager.h>
#include <I2cBusLock.h>
#include <Rtc.h>
#include <Wire.h>
#if FREEINK_READPICO_DIAGNOSTICS
#include <esp_timer.h>
#endif

#include <atomic>
#include <cstdarg>
#include <cstring>

#if defined(ENABLE_SERIAL_LOG)
#include <esp_rom_sys.h>
#endif

#include "BoardReadPicoInternal.h"

// Read Pico board support. Every constant, address, polarity and delay below is
// quoted from MindReset/read_pico_firmware @ main (Apache-2.0) with the source
// file named at the point of use; nothing is inferred from another board.
// Hardware references are linked in docs/read-pico-support.md.

namespace BoardReadPico {
namespace {

// ---------------------------------------------------------------------------
// Logging. The SDK board libraries log through the ROM console (esp_rom_printf)
// rather than the consumer's LOG_*, because a board library must not depend on
// the consumer's log facade: BoardConfig.h::holdPowerRails() and
// InputManager.cpp already do exactly this, gated on ENABLE_SERIAL_LOG.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// FCA9555 Port-0. read_pico_board.c keeps the whole output byte in `ioe_output`
// and commits it with one write, because OUT0 is the only register involved and
// app_ioe.c freezes CFG/INV as read-only (changing a direction can drive a sense
// pin — PGOOD, card detect).
// ---------------------------------------------------------------------------
uint8_t g_ioeOutput = READPICO_IOE_OUT0_INIT;
bool g_ioeReady = false;

// ---------------------------------------------------------------------------
// PMU (CW32L010) frame protocol state. Format from
// components/read_pico_pmu/include/read_pico_pmu_protocol.h:
//   64-byte frame — magic 0xA5, header_version 1, kind, flags, protocol_major 1,
//   protocol_minor 1, sequence u16 LE, code u16 LE, status u16 LE,
//   payload_length u8, reserved u8, session_id u32 LE, payload[44], crc16 u16 LE
//   with CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) over the first 62 bytes.
// ---------------------------------------------------------------------------
constexpr uint8_t kPmuMagic = 0xA5;
constexpr uint8_t kPmuKindRequest = 0;
constexpr uint8_t kPmuRegIdentity = 0x00;
constexpr uint8_t kPmuRegStatus = 0x20;
constexpr uint8_t kPmuRegCommand = 0x80;
constexpr uint8_t kPmuRegResponse = 0x81;
constexpr uint8_t kPmuRegEventPeek = 0x82;
constexpr uint8_t kPmuRegEventAck = 0x83;
constexpr uint8_t kPmuRegQuickBattery = 0x85;

constexpr uint16_t kPmuCmdHostReady = 0x0004;
constexpr uint16_t kPmuCmdTimeSync = 0x0005;
constexpr uint16_t kPmuCmdTimeGet = 0x0007;
constexpr uint16_t kPmuCmdShutdownReady = 0x0303;
constexpr uint16_t kPmuCmdHostSoftSleep = 0x0310;
constexpr uint16_t kPmuCmdHostRequestOff = 0x0311;
constexpr uint16_t kPmuCmdActionPrepare = 0x0300;
constexpr uint16_t kPmuCmdActionCommit = 0x0301;
constexpr uint16_t kPmuCmdVcomGet = 0x0510;

constexpr uint16_t kPmuStatusOk = 0x0000;
constexpr uint16_t kPmuStatusAccepted = 0x0001;
constexpr uint16_t kPmuStatusSequenceConflict = 0x0016;
constexpr uint16_t kPmuStatusStaleSession = 0x0017;

constexpr uint8_t kPmuPwrOff = 0;
constexpr uint8_t kPmuPwrRunning = 3;
constexpr uint8_t kPmuPwrShutdownPending = 4;
constexpr uint8_t kPmuPwrSoftSleep = 8;

constexpr uint8_t kPmuEvtShutdownRequested = 0x22;
// STATUS.flags (read_pico_pmu.c `parse_status`: raw[4..7], u32 LE) bit 5. This is
// a LEVEL, not an event: it stays set for the whole press, which is what the
// shared input layer needs for its hold timer.
constexpr uint32_t kPmuStatusKeyPressed = 1u << 5;
constexpr uint8_t kPmuActionHostLogicalOff = 1;

constexpr size_t kPmuFrameSize = 64;
constexpr size_t kPmuPayloadSize = 44;
constexpr size_t kPmuIdentitySize = 32;
constexpr size_t kPmuStatusSize = 64;
constexpr size_t kPmuEventSize = 16;
// PMU_REG_QUICK_BATTERY: 8 bytes, no frame protocol — battery_mv u16 LE,
// soc_permille u16 LE (0xFFFF = unknown), charge_state u8, flags u8
// (bit0 battery_valid, bit1 soc_valid, bit2 charging_active), crc16 over [0..5].
constexpr size_t kPmuQuickBatterySize = 8;
constexpr uint16_t kPmuSocUnknown = 0xFFFF;

uint16_t g_pmuSeq = 1;     // first request sequence; 0 is skipped
uint32_t g_pmuBootId = 0;  // IDENTITY session_id; survives an ESP-only reset
// Input polling reads these caches outside command transactions; recovery may
// publish new values on the display/RTC task while the input task is running.
std::atomic<uint16_t> g_pmuLastEventId{0};  // STATUS.last_event_id
std::atomic<uint8_t> g_pmuPowerState{0xFF};
std::atomic<uint8_t> g_pmuPendingEvents{0};
// STATUS.flags bit 5: the PMU power key is held right now. Only ever written by
// a successful STATUS parse; the input hook clears it when a poll fails so a
// dropped I2C read cannot look like a stuck key.
std::atomic<bool> g_pmuKeyDown{false};
bool g_powerKeyWake = false;
std::atomic<uint8_t> g_pmuWakeReason{0};
constexpr uint8_t kPmuWakeKey = 2;
std::atomic<bool> g_pmuPresent{false};

// ---------------------------------------------------------------------------
constexpr unsigned long kPmuKeyPollMs = 50;

// ---------------------------------------------------------------------------
// Byte helpers. Explicit little-endian assembly, never a cast: the same
// convention as read_pico_pmu.c `rd16`/`rd32`/`wr16`/`wr32`.
// ---------------------------------------------------------------------------
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void wr32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

// CRC16-CCITT-FALSE, verbatim from read_pico_pmu.c `pmu_crc16_ccitt_false`.
uint16_t crc16CcittFalse(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  while (length--) {
    crc ^= static_cast<uint16_t>(*data++) << 8;
    for (uint8_t i = 0; i < 8; ++i) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

bool crcOk(const uint8_t* data, size_t cover, uint16_t expect) { return crc16CcittFalse(data, cover) == expect; }

// ---------------------------------------------------------------------------
// PMU transport. Retries on a stale session / sequence conflict exactly like
// read_pico_pmu.c `read_pico_pmu_cmd`: the CW32 keeps its own sequence counter
// across an ESP-only reset, so a mismatch has to be recovered by re-reading
// STATUS and resuming from last_command_sequence + 1.
// ---------------------------------------------------------------------------
bool pmuReadRegister(uint8_t reg, uint8_t* data, size_t len) {
  return detail::i2cRead(READPICO_PMU_ADDR, reg, data, len);
}

bool pmuWriteRegister(uint8_t reg, const uint8_t* data, size_t len) {
  return detail::i2cWrite(READPICO_PMU_ADDR, reg, data, len);
}

bool pmuParseIdentity(const uint8_t* raw) {
  if (raw[0] != 'P' || raw[1] != 'M' || raw[2] != 'U') return false;
  // IDENTITY: 'PMU' + fields, crc16 over the first 30 bytes at [30..31].
  if (!crcOk(raw, 30, rd16(&raw[30]))) return false;
  g_pmuBootId = rd32(&raw[12]);
  return true;
}

bool pmuParseStatus(const uint8_t* raw) {
  if (!crcOk(raw, 62, rd16(&raw[62]))) return false;
  g_pmuPowerState = raw[2];
  g_pmuWakeReason = raw[3];
  g_pmuPendingEvents = raw[24];
  g_pmuKeyDown = (rd32(&raw[4]) & kPmuStatusKeyPressed) != 0;
  // STATUS.last_cmd_seq at [40..41], last_event_id at [42..43] (read_pico_pmu.c
  // `parse_status`). The snapshot is only committed when the CRC is good,
  // because a STATUS shadow update can tear mid-read.
  return true;
}

// Resume the request sequence from the PMU's own last_command_sequence + 1,
// skipping 0 (read_pico_pmu.c `recover_seq`).
void pmuRecoverSequence() {
  uint8_t ident[kPmuIdentitySize];
  uint8_t status[kPmuStatusSize];
  if (pmuReadRegister(kPmuRegIdentity, ident, sizeof(ident))) {
    g_pmuPresent = pmuParseIdentity(ident);
  }
  if (pmuReadRegister(kPmuRegStatus, status, sizeof(status))) {
    if (pmuParseStatus(status)) {
      const uint32_t next = static_cast<uint32_t>(rd16(&status[40])) + 1U;
      g_pmuSeq = (next == 0 || next > 0xFFFFU) ? 1 : static_cast<uint16_t>(next);
    }
  }
}

void pmuFillRequest(uint8_t* req, uint16_t code, const uint8_t* payload, uint8_t plen) {
  memset(req, 0, kPmuFrameSize);
  req[0] = kPmuMagic;
  req[1] = 1;  // header_version
  req[2] = kPmuKindRequest;
  req[3] = 0;  // flags
  req[4] = 1;  // protocol_major
  req[5] = 1;  // protocol_minor
  wr16(&req[6], g_pmuSeq++);
  if (g_pmuSeq == 0) g_pmuSeq = 1;
  wr16(&req[8], code);
  wr16(&req[10], 0);  // status: 0 in a request
  req[12] = plen;
  req[13] = 0;  // reserved
  wr32(&req[14], g_pmuBootId);
  if (payload != nullptr && plen > 0) memcpy(&req[18], payload, plen);
  wr16(&req[62], crc16CcittFalse(req, 62));
}

bool pmuReadResponse(uint8_t* raw) {
  if (!pmuReadRegister(kPmuRegResponse, raw, kPmuFrameSize)) return false;
  if (raw[0] != kPmuMagic) return false;
  return crcOk(raw, 62, rd16(&raw[62]));
}

// Wait for the response carrying `seq`: up to 5 reads, 10 ms apart
// (read_pico_pmu.c `read_response_for`).
bool pmuReadResponseFor(uint16_t seq, uint8_t* raw) {
  for (int i = 0; i < 5; ++i) {
    if (!pmuReadResponse(raw)) return false;
    if (rd16(&raw[6]) == seq) return true;
    delay(10);
  }
  return false;
}

#if FREEINK_READPICO_DIAGNOSTICS
struct PmuTimingStats {
  uint32_t commands = 0, maxWaitUs = 0, maxHoldUs = 0;
};
PmuTimingStats g_pmuTiming;
// Declared after freeink::I2cBusLock, so this runs before the bus lock is released,
// including all early returns. Stats are serialized by that existing lock.
class PmuTiming {
  int64_t acquiredUs_;
  uint32_t waitUs_;

 public:
  explicit PmuTiming(int64_t waitingUs) : acquiredUs_(esp_timer_get_time()), waitUs_(acquiredUs_ - waitingUs) {}
  ~PmuTiming() {
    const uint32_t holdUs = esp_timer_get_time() - acquiredUs_;
    ++g_pmuTiming.commands;
    if (waitUs_ > g_pmuTiming.maxWaitUs) g_pmuTiming.maxWaitUs = waitUs_;
    if (holdUs > g_pmuTiming.maxHoldUs) g_pmuTiming.maxHoldUs = holdUs;
    logLine("[RDP] PMU #%lu wait=%luus hold=%luus max_wait=%luus max_hold=%luus\r\n",
            static_cast<unsigned long>(g_pmuTiming.commands), static_cast<unsigned long>(waitUs_),
            static_cast<unsigned long>(holdUs), static_cast<unsigned long>(g_pmuTiming.maxWaitUs),
            static_cast<unsigned long>(g_pmuTiming.maxHoldUs));
  }
};
#endif

// ponytail: hold the existing bus lock through the PMU round trip; use a
// separate PMU lock if the bounded response wait becomes a touch-latency issue.
bool pmuCommand(uint16_t code, const uint8_t* payload, uint8_t plen, uint8_t* responsePayload = nullptr,
                uint8_t* responseLength = nullptr) {
#if FREEINK_READPICO_DIAGNOSTICS
  const int64_t waitingUs = esp_timer_get_time();
#endif
  freeink::I2cBusLock lock;
#if FREEINK_READPICO_DIAGNOSTICS
  PmuTiming timing(waitingUs);
#endif
  if (responseLength != nullptr) *responseLength = 0;
  if (!g_pmuPresent || plen > kPmuPayloadSize || (plen != 0 && payload == nullptr)) return false;

  uint8_t req[kPmuFrameSize];
  uint8_t resp[kPmuFrameSize];
  bool haveResp = false;

  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt > 0) pmuRecoverSequence();
    pmuFillRequest(req, code, payload, plen);
    if (!pmuWriteRegister(kPmuRegCommand, req, sizeof(req))) return false;
    // The CW32 needs ~30 ms to latch the response (read_pico_pmu.c).
    delay(30);
    haveResp = pmuReadResponseFor(rd16(&req[6]), resp);
    if (!haveResp) continue;
    const uint16_t status = rd16(&resp[10]);
    if (status == kPmuStatusStaleSession || status == kPmuStatusSequenceConflict) continue;
    break;
  }
  if (!haveResp) return false;

  const uint16_t status = rd16(&resp[10]);
  if (status != kPmuStatusOk && status != kPmuStatusAccepted) return false;
  const uint8_t length = resp[12];
  if (length > kPmuPayloadSize) return false;
  if (responsePayload != nullptr) memcpy(responsePayload, &resp[18], length);
  if (responseLength != nullptr) *responseLength = length;
  return true;
}

// Poll STATUS only (read_pico_pmu.c `refresh_core(false)` without the battery /
// event reads we do not need here).
bool pmuPoll() {
  uint8_t status[kPmuStatusSize];
  if (!pmuReadRegister(kPmuRegStatus, status, sizeof(status))) return false;
  if (!pmuParseStatus(status)) return false;
  g_pmuLastEventId = rd16(&status[42]);
  return true;
}

bool pmuWaitPowerState(uint8_t want, int timeoutMs) {
  int steps = timeoutMs / 20;
  if (steps < 1) steps = 1;
  for (int i = 0; i < steps; ++i) {
    if (pmuPoll() && g_pmuPowerState == want) return true;
    delay(20);
  }
  return false;
}

bool pmuPeekEvent(uint16_t& eventId, uint8_t& type) {
  uint8_t raw[kPmuEventSize];
  if (!pmuReadRegister(kPmuRegEventPeek, raw, sizeof(raw))) return false;
  eventId = rd16(&raw[0]);
  type = raw[2];
  // An all-zero event_id means "no event"; the CRC covers [0..13]
  // (read_pico_pmu.c `parse_event`).
  return eventId != 0 && crcOk(raw, 14, rd16(&raw[14]));
}

bool pmuEventAck(uint16_t eventId) {
  uint8_t buf[4];
  wr16(buf, eventId);
  wr16(&buf[2], crc16CcittFalse(buf, 2));
  // Event ACK is a plain 4-byte register write, not a frame.
  return pmuWriteRegister(kPmuRegEventAck, buf, sizeof(buf));
}

// PMU_CMD_ACTION_*: PREPARE returns a token, COMMIT carries it back
// (read_pico_pmu.c `read_pico_pmu_action`).
bool pmuAction(uint8_t action, uint16_t delayMs, uint16_t reason) {
  uint8_t prep[5];
  prep[0] = action;
  wr16(&prep[1], delayMs);
  wr16(&prep[3], reason);
  uint8_t response[kPmuPayloadSize];
  uint8_t length;
  if (!pmuCommand(kPmuCmdActionPrepare, prep, sizeof(prep), response, &length) || length < 4) return false;
  uint8_t commit[4];
  wr32(commit, rd32(response));
  return pmuCommand(kPmuCmdActionCommit, commit, sizeof(commit));
}

// ---------------------------------------------------------------------------
// SC7A20H identity. Register map from components/sc7a20h/sc7a20h.c:
// WHO_AM_I 0x0F (expect 0x11), VERSION 0x70 (expect 0x28). Read-only: the Imu
// library owns the actual bring-up (soft reset 0x68 = 0xA5, CTRL5.BOOT), and a
// reset here would race it.
// ---------------------------------------------------------------------------
constexpr uint8_t kSc7a20hRegWhoAmI = 0x0F;
constexpr uint8_t kSc7a20hRegVersion = 0x70;
constexpr uint8_t kSc7a20hWhoAmIValue = 0x11;
constexpr uint8_t kSc7a20hVersionValue = 0x28;

bool accelProbe() {
  uint8_t who = 0;
  uint8_t ver = 0;
  if (!detail::i2cRead(READPICO_ACCEL_ADDR, kSc7a20hRegWhoAmI, &who, 1)) return false;
  (void)detail::i2cRead(READPICO_ACCEL_ADDR, kSc7a20hRegVersion, &ver, 1);
  const bool ok = (who == kSc7a20hWhoAmIValue) && (ver == kSc7a20hVersionValue);
  logLine("[RDP] SC7A20H WHO_AM_I=0x%02X (want 0x%02X) VER=0x%02X (want 0x%02X) %s\r\n", who, kSc7a20hWhoAmIValue, ver,
          kSc7a20hVersionValue, ok ? "ok" : "unexpected");
  return ok;
}

// ---------------------------------------------------------------------------
// Strip-key mapping. The three zones are a CrossMux policy choice, NOT a
// hardware fact: the reference firmware binds KEY1/KEY3 to the menu page step
// and KEY2 to "full GC16 redraw", because its own UI has a menu handle in the
// display's bottom bar (main/app/app_loop.c:200-229). CrossMux's InputManager
// seam can only emit `1 << BTN_*`, so the outer two keep the reference's
// previous/next intent and the MIDDLE zone becomes BACK (返回上一级) — the same
// "leave this level" role eegoA4's back button plays. Selecting moved to the
// touch panel, which the reader now accepts as taps, so the middle zone no longer
// has to be CONFIRM.
// ---------------------------------------------------------------------------
constexpr uint8_t kStripKey1Mask = static_cast<uint8_t>(1U << InputManager::BTN_UP);
constexpr uint8_t kStripKey2Mask = static_cast<uint8_t>(1U << InputManager::BTN_BACK);
constexpr uint8_t kStripKey3Mask = static_cast<uint8_t>(1U << InputManager::BTN_DOWN);

// |x - centre| <= PITCH/2 picks the zone; the zones are 160 px apart, so
// half-pitch is the natural split and the outer halves fall outside the panel.
uint8_t stripKeyFor(uint16_t x) {
  const int dx1 = static_cast<int>(x) - READPICO_KEY_CENTER_1;
  const int dx2 = static_cast<int>(x) - READPICO_KEY_CENTER_2;
  const int dx3 = static_cast<int>(x) - READPICO_KEY_CENTER_3;
  const int half = READPICO_KEY_PITCH / 2;
  const int a1 = dx1 < 0 ? -dx1 : dx1;
  const int a2 = dx2 < 0 ? -dx2 : dx2;
  const int a3 = dx3 < 0 ? -dx3 : dx3;
  if (a1 <= half && a1 <= a2 && a1 <= a3) return kStripKey1Mask;
  if (a2 <= half && a2 <= a3) return kStripKey2Mask;
  if (a3 <= half) return kStripKey3Mask;
  return 0;
}

uint8_t pollPowerButton() {
  static unsigned long lastPoll = 0;
  const freeink::I2cBusLock lock;
  const unsigned long now = millis();
  if (now - lastPoll >= kPmuKeyPollMs) {
    lastPoll = now;
    if (!g_pmuPresent || !pmuPoll()) g_pmuKeyDown = false;
  }
  return g_pmuKeyDown ? static_cast<uint8_t>(1U << InputManager::BTN_POWER) : 0;
}

}  // namespace

// ===========================================================================
// Internal transport (declared in BoardReadPicoInternal.h)
// ===========================================================================
namespace detail {

void beginI2C() {
  freeink::enableI2cBusLock();
  const freeink::I2cBusLock lock;
  Wire.begin(READPICO_I2C_SDA, READPICO_I2C_SCL);
  Wire.setClock(READPICO_I2C_HZ);
  Wire.setTimeOut(50);
}

bool i2cWrite(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len) {
  freeink::I2cBusLock lock;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (data != nullptr && len > 0) Wire.write(data, len);
  return Wire.endTransmission() == 0;
}

bool i2cRead(uint8_t addr, uint8_t reg, uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) return false;
  freeink::I2cBusLock lock;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  const uint8_t want = static_cast<uint8_t>(len);
  if (Wire.requestFrom(addr, want) != want) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < len; ++i) data[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

uint8_t ioeOutput() { return g_ioeOutput; }

bool ioeSetOutput(uint8_t value) {
  if (!detail::i2cWrite(READPICO_IOE_ADDR, READPICO_IOE_REG_OUT0, &value, 1)) return false;
  g_ioeOutput = value;
  return true;
}

bool ioeSetBit(uint8_t bit, bool high) {
  const freeink::I2cBusLock lock;
  const uint8_t mask = static_cast<uint8_t>(1U << bit);
  const uint8_t next = high ? static_cast<uint8_t>(g_ioeOutput | mask) : static_cast<uint8_t>(g_ioeOutput & ~mask);
  return ioeSetOutput(next);
}

// Port-0 self-test, mirroring read_pico_board.c `fca9555_selftest` minus the
// bus scan: ACK, CFG0 write + readback, CFG1 = 0xFF, and the MODE/TP_RST
// preload. SY_EN and VCOM_EN are deliberately left LOW so the panel's high
// voltage cannot come up by accident.
bool ioeConfigure() {
  // Presence probe: a register-pointer-only write ACKs when the expander is
  // there. Everything after this is the reference's fca9555_selftest minus the
  // bus scan: CFG0 write + readback, CFG1 = 0xFF, and the MODE/TP_RST preload.
  // SY_EN and VCOM_EN stay LOW so the panel's high voltage cannot come up by
  // accident.
  if (!i2cWrite(READPICO_IOE_ADDR, READPICO_IOE_REG_CFG0, nullptr, 0)) return false;
  const uint8_t cfg0 = READPICO_IOE_CFG0_EXPECT;
  if (!i2cWrite(READPICO_IOE_ADDR, READPICO_IOE_REG_CFG0, &cfg0, 1)) return false;
  uint8_t readback = 0;
  if (!i2cRead(READPICO_IOE_ADDR, READPICO_IOE_REG_CFG0, &readback, 1)) return false;
  if (readback != READPICO_IOE_CFG0_EXPECT) {
    logLine("[RDP] FCA9555 CFG0 wrote 0x%02X read 0x%02X\r\n", READPICO_IOE_CFG0_EXPECT, readback);
    return false;
  }
  const uint8_t cfg1 = READPICO_IOE_CFG1_UNUSED;
  if (!i2cWrite(READPICO_IOE_ADDR, READPICO_IOE_REG_CFG1, &cfg1, 1)) return false;
  g_ioeOutput = READPICO_IOE_OUT0_INIT;
  return ioeSetOutput(g_ioeOutput);
}

bool ioeReadPort0(uint8_t& in0) { return i2cRead(READPICO_IOE_ADDR, READPICO_IOE_REG_IN0, &in0, 1); }

int ioePgoodLevel() {
  uint8_t in0 = 0;
  if (!ioeReadPort0(in0)) return -1;
  return (in0 & (1U << READPICO_IOE_PGOOD)) ? 1 : 0;
}

// --- SY7636A ---------------------------------------------------------------
// sy7636a.c `read_reg`: "手册要求先 STOP 再读，不能 Repeated START" — STOP, then a
// separate read transaction.
bool syRead(uint8_t reg, uint8_t& value) {
  freeink::I2cBusLock lock;  // hold the bus across both transactions, not just each one
  Wire.beginTransmission(READPICO_SY_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  delayMicroseconds(50);
  if (Wire.requestFrom(static_cast<uint8_t>(READPICO_SY_ADDR), static_cast<uint8_t>(1)) != 1) {
    while (Wire.available()) Wire.read();
    return false;
  }
  value = static_cast<uint8_t>(Wire.read());
  return true;
}

bool syWrite(uint8_t reg, uint8_t value) {
  const uint8_t data[2] = {reg, value};
  freeink::I2cBusLock lock;
  Wire.beginTransmission(READPICO_SY_ADDR);
  Wire.write(data, sizeof(data));
  return Wire.endTransmission() == 0;
}

// sy7636a.c `write_vcom`: 9-bit code = mV / 10, LSB into REG_VCOM_LSB, the 9th
// bit into REG_VCOM_MSB bit 7 while preserving the rest of that register.
bool sySetVcom(int mv) {
  int code = mv / 10;
  if (code < 0) code = 0;
  if (code > 500) code = 500;
  uint8_t msb = 0;
  if (!syRead(0x02 /* REG_VCOM_MSB */, msb)) return false;
  if (!syWrite(0x01 /* REG_VCOM_LSB */, static_cast<uint8_t>(code))) return false;
  msb = static_cast<uint8_t>((msb & 0x7F) | ((code & 0x100) ? 0x80 : 0));
  return syWrite(0x02 /* REG_VCOM_MSB */, msb);
}

// --- PMU -------------------------------------------------------------------
bool pmuReady() { return g_pmuPresent; }

bool pmuInit() {
  uint8_t ident[kPmuIdentitySize];
  bool ok = false;
  for (int i = 0; i < 5 && !ok; ++i) {
    if (i) delay(20);
    if (pmuReadRegister(kPmuRegIdentity, ident, sizeof(ident))) ok = pmuParseIdentity(ident);
  }
  g_pmuPresent = ok;
  if (!ok) {
    logLine("[RDP] CW32 PMU 0x%02X not responding\r\n", READPICO_PMU_ADDR);
    return false;
  }

  pmuRecoverSequence();
  (void)pmuPoll();
  g_powerKeyWake = g_pmuWakeReason == kPmuWakeKey;

  // HOST_READY, then wait for STATUS RUNNING: without it the PMU times the host
  // boot out and drops the EN rail (read_pico_pmu.c `read_pico_pmu_init`).
  const uint8_t reason = 0;
  if (!pmuCommand(kPmuCmdHostReady, &reason, 1)) {
    logLine("[RDP] PMU HOST_READY failed\r\n");
    return true;  // the PMU answered; the handshake is retried on demand
  }
  if (!pmuWaitPowerState(kPmuPwrRunning, 1000)) {
    logLine("[RDP] PMU not RUNNING after HOST_READY (state %u)\r\n", g_pmuPowerState.load());
  }
  logLine("[RDP] PMU ready proto/fw parsed, session=%08lX state=%u\r\n", static_cast<unsigned long>(g_pmuBootId),
          g_pmuPowerState.load());
  return true;
}

}  // namespace detail

// ===========================================================================
// Public API
// ===========================================================================

bool begin() {
  detail::beginI2C();
  // FCA9555 INT# sense line (GPIO41). Unless it is an input the expander's
  // open-drain assertion has nothing to pull against.
  pinMode(READPICO_IOE_INT, INPUT_PULLUP);

  const bool ioeOk = detail::ioeConfigure();
  g_ioeReady = ioeOk;
  logLine("[RDP] FCA9555 0x%02X %s\r\n", READPICO_IOE_ADDR, ioeOk ? "ok" : "FAILED");

  // Buzzer parked LOW: GPIO2 drives an AO3400A gate and idles at duty 0
  // (read_pico_buzzer.c). Doing it here means a later LEDC setup starts silent.
  pinMode(READPICO_BUZZER, OUTPUT);
  digitalWrite(READPICO_BUZZER, LOW);

  if (!ioeOk) {
    // Nothing else on this board can be sequenced without the expander: it owns
    // XOE, the PMIC enable, the touch reset and the card detect.
    return false;
  }

  if (!detail::pmuInit()) logLine("[RDP] PMU unavailable; battery/RTC/power-off disabled\r\n");

  // Identity probes are advisory: a missing accelerometer must not fail begin().
  (void)accelProbe();

  // Publish the board-owned buttons, battery and clock to the SDK managers.
  InputManager::setButtonHook(&pollPowerButton);
  BatteryMonitor::setPmuBatteryHook(&pmuBattery);
  Rtc::setPmuTimeHooks({&pmuTimeGet, &pmuTimeSet});
  return true;
}

bool ready() { return g_ioeReady; }

bool ioeIntAsserted() {
  // FCA9555 INT# is a separate open-drain, ACTIVE-LOW line on GPIO41 — the host
  // reads it as a GPIO level, exactly as fca9555_int_level() does with the
  // handle's int_gpio and as main/sleep.c arms it for light-sleep wake. Note
  // GPIO41 is not an RTC-capable S3 pin, so it can never be a deep-sleep EXT
  // source (read_pico_board.h:28-30).
  //
  // UNVERIFIED: whether the expander's INT# net has its own external pull-up on
  // this board revision. begin() enables the S3's internal pull-up, which is
  // correct for an open-drain signal either way.
  return digitalRead(READPICO_IOE_INT) == LOW;
}

void clearIoeInt() {
  // INT# is NOT latched: reading the Input register releases it
  // (fca9555.h: "INT# 开漏、不锁存；读 Input 就松开"). Port 1 is unused here, so
  // Port 0 alone is enough — fca9555_clear_int() does the same.
  uint8_t in0 = 0;
  (void)detail::ioeReadPort0(in0);
}

bool sdCardPresent() {
  // Slot CD is active-low on FCA9555 P0.6. A failed read must NOT look like
  // "card present" (read_pico_board.c `read_pico_sd_present`).
  uint8_t in0 = 0xFF;
  if (!detail::ioeReadPort0(in0)) return false;
  return (in0 & (1U << READPICO_IOE_SD_CD)) == 0;
}

bool touchReset() {
  // Active-low, 10 ms low then release. This is the ONLY way back from the
  // chip's own deep sleep; the caller must let it boot (~50 ms) before the first
  // I2C read (read_pico_board.c `read_pico_touch_reset`, cst836u.h).
  if (!detail::ioeSetBit(READPICO_IOE_TP_RST, false)) return false;
  delay(10);
  return detail::ioeSetBit(READPICO_IOE_TP_RST, true);
}

bool touchSleep() {
  // CST836U_CMD_DEEPSLEEP 0xA503, MSB first (cst836u.c `write_cmd`). The chip
  // stops ACKing I2C until touchReset() pulses RST.
  const uint8_t cmd[2] = {static_cast<uint8_t>(0xA503 >> 8), static_cast<uint8_t>(0xA503 & 0xFF)};
  freeink::I2cBusLock lock;
  Wire.beginTransmission(READPICO_TP_ADDR);
  Wire.write(cmd, sizeof(cmd));
  return Wire.endTransmission() == 0;
}

uint8_t stripButtonMask(uint16_t rawX) { return stripKeyFor(rawX); }

bool powerKeyPressed() { return g_pmuKeyDown; }
bool wokeFromPowerKey() { return g_powerKeyWake; }

bool pmuBattery(uint16_t& mV, uint16_t& socPermille, uint8_t& chargeState) {
  mV = 0;
  socPermille = 0;
  chargeState = 0;
  if (!g_pmuPresent) return false;

  uint8_t raw[kPmuQuickBatterySize];
  if (!pmuReadRegister(kPmuRegQuickBattery, raw, sizeof(raw))) return false;
  if (!crcOk(raw, 6, rd16(&raw[6]))) return false;

  const uint8_t flags = raw[5];
  const bool batteryValid = (flags & 0x01) != 0;
  const bool socValid = (flags & 0x02) != 0;
  const uint16_t soc = rd16(&raw[2]);
  if (!batteryValid) return false;

  mV = rd16(&raw[0]);
  // 0xFFFF is the protocol's "unknown" sentinel.
  socPermille = socValid ? soc : kPmuSocUnknown;
  chargeState = raw[4];
  return mV != 0;
}

bool pmuTimeGet(uint32_t& unixSec, bool& synced) {
  unixSec = 0;
  synced = false;
  if (!g_pmuPresent) return false;
  uint8_t response[kPmuPayloadSize];
  uint8_t length;
  if (!pmuCommand(kPmuCmdTimeGet, nullptr, 0, response, &length)) return false;
  // 8-byte response: unix_seconds u32 (0 = never calibrated) + millis u16 +
  // synced u8 + rsvd u8 (read_pico_pmu_protocol.h PMU_CMD_TIME_GET).
  if (length < 7) return false;
  unixSec = rd32(response);
  synced = response[6] != 0;
  return unixSec != 0;
}

bool pmuTimeSet(uint32_t unixSec) {
  if (!g_pmuPresent) return false;
  uint8_t payload[4];
  wr32(payload, unixSec);
  return pmuCommand(kPmuCmdTimeSync, payload, sizeof(payload));
}

bool pmuReportReady() {
  if (!g_pmuPresent) return false;
  if (pmuPoll() && g_pmuPowerState == kPmuPwrRunning) return true;
  const uint8_t reason = 0;
  if (!pmuCommand(kPmuCmdHostReady, &reason, 1)) return false;
  return pmuWaitPowerState(kPmuPwrRunning, 1000);
}

bool pmuSoftSleep() {
  if (!g_pmuPresent) return false;
  if (!pmuPoll() || g_pmuPowerState != kPmuPwrRunning) {
    // SOFT_SLEEP is only accepted from RUNNING (read_pico_pmu.c
    // `read_pico_pmu_report_sleep`).
    if (!pmuReportReady()) return false;
  }
  if (!pmuCommand(kPmuCmdHostSoftSleep, nullptr, 0)) return false;
  // The PMU then drops the host EN rail; waiting for the state confirmation is
  // best-effort because we lose the bus when it does.
  return pmuWaitPowerState(kPmuPwrSoftSleep, 1000);
}

bool pmuPowerOff() {
  if (!g_pmuPresent) return false;
  (void)pmuPoll();

  // REQUEST_OFF payload {0, 1, 0} (read_pico_pmu.c `read_pico_pmu_power_off`).
  const uint8_t req[3] = {0, 1, 0};
  if (!pmuCommand(kPmuCmdHostRequestOff, req, sizeof(req))) {
    // Fallback: the two-step ACTION path (PREPARE token -> COMMIT).
    logLine("[RDP] PMU REQUEST_OFF failed, trying LOGICAL_OFF\r\n");
    if (!pmuAction(kPmuActionHostLogicalOff, 0, 1)) return false;
  }

  uint16_t eventId = 0;
  bool pending = false;
  for (int i = 0; i < 100; ++i) {
    if (!pmuPoll()) {
      delay(20);
      continue;
    }
    if (g_pmuPowerState == kPmuPwrShutdownPending) pending = true;
    if (g_pmuPendingEvents > 0) {
      uint16_t id = 0;
      uint8_t type = 0;
      if (pmuPeekEvent(id, type)) {
        if (type == kPmuEvtShutdownRequested) {
          eventId = id;
          pending = true;
          (void)pmuEventAck(id);
          break;
        }
        (void)pmuEventAck(id);
      }
      continue;
    }
    if (pending && eventId == 0 && g_pmuLastEventId != 0) {
      eventId = g_pmuLastEventId;
      break;
    }
    delay(20);
  }

  if (!pending) {
    logLine("[RDP] PMU never reached SHUTDOWN_PENDING (state %u)\r\n", g_pmuPowerState.load());
    return false;
  }
  if (eventId == 0) {
    const uint16_t lastEventId = g_pmuLastEventId.load();
    eventId = lastEventId != 0 ? lastEventId : 1;
  }

  uint8_t ready[2];
  wr16(ready, eventId);
  if (!pmuCommand(kPmuCmdShutdownReady, ready, sizeof(ready))) return false;

  for (int i = 0; i < 25; ++i) {
    (void)pmuPoll();
    if (g_pmuPowerState == kPmuPwrOff) return true;
    delay(20);
  }
  // The rail may already be down, which is the outcome we wanted; report the
  // uncertain case as failure so the caller can log it.
  return false;
}

int pmuVcomMv() {
  // READ-ONLY, and there is deliberately NO fallback value.
  //
  // The panel's VCOM is paired with this specific glass at the factory and stored
  // in the PMU. Nothing in this port ever writes it back. Driving the panel at a
  // VCOM that does not match it forces a DC imbalance across the glass, which is a
  // permanent hardware failure rather than a bad picture — so a unit whose factory
  // value cannot be read must not be energised at all.
  //
  // The reply is range-checked (500..2500 mV, multiple of 10) so a truncated or
  // corrupted frame cannot be mistaken for a real setting.
  //
  // Returns the factory value in mV, or -1 when it is unavailable / out of range.
  // Callers MUST read -1 as "do not power the panel", never as "use a default".
  uint8_t response[kPmuPayloadSize];
  uint8_t length;
  if (g_pmuPresent && pmuCommand(kPmuCmdVcomGet, nullptr, 0, response, &length)) {
    if (length >= 4 && response[2] != 0) {
      const int mv = static_cast<int>(rd16(response));
      if (mv >= 500 && mv <= 2500 && (mv % 10) == 0) {
        logLine("[RDP] panel VCOM %d mV (PMU factory value)\r\n", mv);
        return mv;
      }
      logLine("[RDP] PMU VCOM %d mV rejected (outside 500..2500 / not a multiple of 10)\r\n", mv);
      return -1;
    }
  }
  logLine("[RDP] PMU VCOM unavailable; refusing to guess a panel voltage\r\n");
  return -1;
}

}  // namespace BoardReadPico
