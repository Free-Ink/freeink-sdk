#include "RecordingRtcBus.h"

int main() {
#if !FREEINK_CAP_RTC
  Rtc rtc;
  bool fired = true;
  assert(!rtc.begin() && !rtc.armWakeTimer(60) && !rtc.cancelWakeTimer());
  assert(!rtc.readWakeTimer(fired) && !fired);
  puts("RTC-disabled fallback PASS");
  return 0;
#else
  reset();
  // Snapshot precedes the destructive PMIC clear and survives later users.
  regs[0x6E][0x05] = 0x20;
  regs[0x6E][0x18] = 0x01;
  assert(freeink::papermono::ensureBooted());
  const auto snapshot = freeink::papermono::bootWakeState();
  assert(snapshot.valid && snapshot.source == 0x20 && snapshot.enabledPins == 1);
  assert(regs[0x6E][0x05] == 0);
  assert(freeink::papermono::ensureBooted());
  assert(freeink::papermono::bootWakeState().source == 0x20);

  reset();
  Rtc rtc;
  assert(rtc.begin());
  Rtc::DateTime time;
  regs[0x32][0x1D] = 0x02;
  assert(!rtc.now(time));
  regs[0x32][0x1D] = 0xBF;
  regs[0x32][0x1E] = 0x10;
  const Rtc::DateTime expected{2026, 10, 11, 12, 34, 56, 0};
  assert(rtc.set(expected));
  assert(regs[0x32][0x1E] == 0x10 && regs[0x32][0x1D] == 0xBD);
  bool stopped = false;
  for (const auto& w : writes)
    if (w.addr == 0x32 && w.reg == 0x1E && w.value == 0x50) stopped = true;
  assert(stopped && rtc.now(time));
  assert(time.year == 2026 && time.month == 10 && time.day == 11 && time.second == 56);

  for (const uint32_t seconds : {60U, 65535U, 65536U, 86400U}) {
    reset();
    regs[0x32][0x1D] = 0xAC;
    assert(rtc.armWakeTimer(seconds));
    const uint16_t count = regs[0x32][0x1A] | (uint16_t(regs[0x32][0x1B]) << 8);
    assert(count == (seconds > 65535 ? (seconds + 59) / 60 : seconds));
    assert((regs[0x32][0x1C] & 0x17) == (seconds > 65535 ? 0x13 : 0x12));
    assert(regs[0x32][0x1D] == 0xAC && (regs[0x32][0x1E] & 0x15) == 0x10);
    bool fired = true;
    assert(rtc.readWakeTimer(fired) && !fired);
    regs[0x32][0x1D] |= 0x10;
    assert(rtc.readWakeTimer(fired) && fired);
    assert(rtc.cancelWakeTimer());
    assert(!(regs[0x32][0x1C] & 0x10) && !(regs[0x32][0x1E] & 0x10));
    assert(regs[0x32][0x1D] == 0xAC);
  }
  reset();
  assert(!rtc.armWakeTimer(0) && !rtc.armWakeTimer(UINT32_MAX));
  for (uint8_t conflict : {0x80, 0x40, 0x20, 0x08}) {
    reset();
    regs[0x32][0x1E] = conflict;
    assert(!rtc.armWakeTimer(60));
    assert(regs[0x32][0x1E] == conflict);
  }
  reset();
  regs[0x32][0x1D] = 0x02;
  assert(!rtc.armWakeTimer(60));  // Unknown clock is not silently initialized.
  reset();
  dropCounter = true;
  assert(!rtc.armWakeTimer(60) && !(regs[0x32][0x1C] & 0x10));
  reset();
  dropEnable = true;
  assert(!rtc.armWakeTimer(60) && !(regs[0x32][0x1E] & 0x10));

  reset();
  assert(rtc.armWakeTimer(60));
  const int armOperations = operations;
  for (int failure = 1; failure <= armOperations; ++failure) {
    reset();
    failAt = failure;
    assert(!rtc.armWakeTimer(60));
    failAt = 0;
    assert(rtc.cancelWakeTimer());
    assert(!(regs[0x32][0x1C] & 0x10) && !(regs[0x32][0x1E] & 0x10));
  }
  reset();
  deadBus = true;
  assert(!rtc.cancelWakeTimer());
  bool fired = true;
  assert(!rtc.readWakeTimer(fired) && !fired);
  reset();
  BoardConfig::ACTIVE.sensors.rtcType = BoardConfig::RtcType::Pcf8563;
  assert(!rtc.armWakeTimer(60) && !rtc.cancelWakeTimer());

  // RX8010: the RX8130's time block, but flags at 0x1E and control at 0x1F.
  reset();
  BoardConfig::ACTIVE.sensors.rtcType = BoardConfig::RtcType::Rx8010;
  Rtc rx8010;
  regs[0x32][0x1E] = 0x02;  // VLF, as a chip that has never been set comes up
  regs[0x32][0x1F] = 0x84;  // TEST must not survive a set(); the other bits must
  assert(rx8010.begin());
  assert(regs[0x32][0x17] == 0xD8 && regs[0x32][0x31] == 0x08);
  assert(!rx8010.now(time));  // no time until one has been set
  regs[0x32][0x1D] = 0x02;    // VLF's bit in the RX8130's flag register means nothing here
  const Rtc::DateTime murphy{2026, 11, 5, 17, 6, 30, 4};
  regs[0x32][0x1E] = 0x3A;  // alarm, timer and update flags are not ours to clear
  assert(rx8010.set(murphy));
  assert(regs[0x32][0x1E] == 0x38 && regs[0x32][0x1F] == 0x04 && regs[0x32][0x1D] == 0x02);
  assert(regs[0x32][0x13] == 0x10);  // weekday is one-hot
  stopped = false;
  for (const auto& w : writes)
    if (w.addr == 0x32 && w.reg == 0x1F && w.value == 0x44) stopped = true;
  assert(stopped);
  assert(rx8010.now(time));
  assert(time.year == 2026 && time.month == 11 && time.day == 5 && time.hour == 17 && time.minute == 6 &&
         time.second == 30 && time.weekday == 4);
  assert(!rx8010.armWakeTimer(60));  // the wake timer is the RX8130's

  reset();
  assert(freeink::papermono::setRtcWake(true));
  assert(regs[0x6E][0x18] == 1 && !(regs[0x6E][0x19] & 1));
  assert((regs[0x6E][0x07] & 0x20) && (regs[0x6E][0x06] & 4));
  assert(freeink::papermono::setRtcWake(false));
  assert(!(regs[0x6E][0x18] & 1) && !(regs[0x6E][0x07] & 0x20));
  reset();
  regs[0x6E][0x18] = 0x10;
  assert(!freeink::papermono::setRtcWake(true) && regs[0x6E][0x18] == 0x10);
  reset();
  assert(freeink::papermono::setRtcWake(true));
  const int pmicOperations = operations;
  for (int failure = 1; failure <= pmicOperations; ++failure) {
    reset();
    failAt = failure;
    assert(!freeink::papermono::setRtcWake(true));
    failAt = 0;
    assert(freeink::papermono::setRtcWake(false));
    assert(!(regs[0x6E][0x18] & 1));
  }
  reset();
  hostMillis = 0;
  const auto abort = [](void*) { return millis() >= 120; };
  assert(!freeink::m5pm1::requestShutdown(abort));
  for (const auto& w : writes) assert(!(w.addr == 0x6E && w.reg == freeink::m5pm1::REG_SYS_CMD));
  printf("RTC and PMIC tests PASS: %d RTC fault points, %d PMIC fault points\n", armOperations, pmicOperations);
#endif
}
