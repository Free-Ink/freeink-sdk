// X4 Pro GT911 INT wake (FREEINK_GT911_INT_WAKE) against a model controller:
// config read, checksum patch, Config_Fresh consumption, read-compare-skip, the
// status clear, and the I2C-failure fallback (retry, then withdraw the INT as a
// wake source until the status register is reachable again). Built once with
// the flag and once without; without it the driver must not touch the config.
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
constexpr int8_t INT_PIN = WireStub::INT_PIN;

// A device boot. begin()'s reset dance raises RST, which makes the model
// controller reload its stored table (gt911_stubs/Wire.h).
void boot(InputManager& in) {
  Wire.resetCounters();
  gpioModes[INT_PIN] = 0;
  loggedErrors = loggedInfos = 0;
  in.begin();
  assert(in.hasTouch());
}

void poll(InputManager& in) {
  fakeNow += 20;
  in.update();
}

#if FREEINK_GT911_INT_WAKE
void testPatchAppliedOncePerBootWhenDroppedOnReset() {
  Wire.loadFactory(1);  // falling edge, the X4 Pro factory mode
  Wire.persistent = false;
  for (int bootNo = 0; bootNo < 3; ++bootNo) {
    InputManager in;
    boot(in);
    // Whole block read in three bounded chunks plus the Module_Switch1
    // readback, and exactly one short apply: Config_Version, Module_Switch1,
    // checksum + Config_Fresh.
    assert(Wire.configReads == 4);
    assert(Wire.configWriteTxns == 3 && Wire.applies == 1);
    assert(Wire.mem[WireStub::CFG] == 0x00);
    assert(Wire.activeIntMode() == 2 && (Wire.mem[WireStub::SWITCH1] & ~0x03) == 0x30);
    assert(WireStub::checksum(&Wire.mem[WireStub::CFG]) == Wire.mem[WireStub::CHK]);
    assert(Wire.mem[WireStub::FRESH] == 0);
    // The report latched by the re-read is cleared, so INT idles released.
    assert(Wire.mem[WireStub::STATUS] == 0 && gpioLevels[INT_PIN] == HIGH);
    assert(gpioModes[INT_PIN] == INPUT_PULLUP);
    assert(in.touchWakeIrqPin() == INT_PIN && in.touchWakeIrqActiveLow());
    assert(loggedErrors == 0 && std::strstr(lastLog, "patched this boot"));

    // Polling never writes the config again.
    for (int i = 0; i < 5; ++i) poll(in);
    assert(Wire.configWriteTxns == 3 && Wire.applies == 1);
    // A second begin() resets the controller, which drops the patch again:
    // exactly one more apply, no more.
    Wire.resetCounters();
    in.begin();
    assert(Wire.configWriteTxns == 3 && Wire.applies == 1 && in.touchWakeIrqPin() == INT_PIN);
  }
}

void testRebeginRefusedWithdrawsPin() {
  // A first begin() arms the pin; a later begin() resets the controller back
  // to its edge mode and this time the table is refused. The earlier result
  // must not survive: the line only pulses now.
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  assert(in.touchWakeIrqPin() == INT_PIN);
  Wire.acceptConfig = false;
  Wire.resetCounters();
  in.begin();
  assert(Wire.applies == 1 && Wire.activeIntMode() == 1);
  assert(in.touchWakeIrqPin() == -1);
  Wire.acceptConfig = true;
}

void testNoWriteWhenModeAlreadyLowLevel() {
  // Panel that keeps an applied table: the second boot reads low level back
  // from its own table and is never written.
  Wire.loadFactory(1);
  Wire.persistent = true;
  {
    InputManager in;
    boot(in);
    assert(Wire.applies == 1 && in.touchWakeIrqPin() == INT_PIN);
  }
  {
    InputManager in;
    boot(in);
    assert(Wire.configReads == 4 && Wire.configWriteTxns == 0 && Wire.applies == 0);
    assert(in.touchWakeIrqPin() == INT_PIN && std::strstr(lastLog, "already set"));
  }
  Wire.persistent = false;

  // Factory table already in low-level mode: never written.
  Wire.loadFactory(2);
  InputManager in;
  boot(in);
  assert(Wire.configWriteTxns == 0 && Wire.applies == 0);
  assert(Wire.mem[WireStub::CFG] == 0x41);  // version untouched
  assert(in.touchWakeIrqPin() == INT_PIN && loggedErrors == 0);
}

void testRefusals() {
  // Checksum mismatch: the table is not understood, so nothing is written.
  Wire.loadFactory(1);
  Wire.factory[5] ^= 0xFF;
  {
    InputManager in;
    boot(in);
    assert(Wire.configWriteTxns == 0 && in.touchWakeIrqPin() == -1);
    assert(loggedErrors == 1 && std::strstr(lastLog, "checksum"));
    assert(gpioModes[INT_PIN] != INPUT_PULLUP);
  }

  // Controller never clears Config_Fresh: one apply, bounded wait, no retry.
  Wire.loadFactory(1);
  Wire.acceptConfig = false;
  {
    InputManager in;
    const unsigned long before = fakeNow;
    boot(in);
    assert(Wire.applies == 1 && Wire.configWriteTxns == 3);
    assert(fakeNow - before < 1000);
    assert(in.touchWakeIrqPin() == -1);
    assert(loggedErrors == 1 && std::strstr(lastLog, "Config_Fresh"));
    for (int i = 0; i < 5; ++i) poll(in);
    assert(Wire.applies == 1 && in.touchWakeIrqPin() == -1);
    // The register holds the patch, but the controller still runs edge mode.
    assert(Wire.activeIntMode() == 1 && (Wire.mem[WireStub::SWITCH1] & 0x03) == 2);
    // Never offered as a wake source: a bus fault costs one attempt per poll
    // and logs nothing, as without the flag.
    Wire.busDead = true;
    Wire.resetCounters();
    poll(in);
    poll(in);
    Wire.busDead = false;
    assert(Wire.statusReadAttempts == 2 && loggedErrors == 1);
  }
  Wire.acceptConfig = true;

  // Table consumed but the INT mode did not change: the readback guard holds.
  Wire.loadFactory(1);
  Wire.honorIntMode = false;
  {
    InputManager in;
    boot(in);
    assert(Wire.applies == 1 && in.touchWakeIrqPin() == -1);
    assert(loggedErrors == 1 && std::strstr(lastLog, "not low level"));
  }
  Wire.honorIntMode = true;

  // Profile that claims an active-high INT: not even read.
  Wire.loadFactory(1);
  BoardConfig::ACTIVE.touch.irqActiveLow = false;
  {
    InputManager in;
    boot(in);
    assert(Wire.configReads == 0 && Wire.configWriteTxns == 0 && in.touchWakeIrqPin() == -1);
    assert(loggedErrors == 1);
  }
  BoardConfig::ACTIVE.touch.irqActiveLow = true;
}

void testStatusReadFailureWithdrawsWake() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  assert(in.touchWakeIrqPin() == INT_PIN);

  // Dead bus with a report pending: INT is held LOW and cannot be cleared.
  Wire.report(0x81);
  Wire.busDead = true;
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusReadAttempts == 3);  // bounded retry
  assert(in.touchWakeIrqPin() == -1);    // not armed while INT may be stuck
  assert(loggedErrors == 1);
  // Further polls: a single attempt each and no further log.
  poll(in);
  poll(in);
  assert(Wire.statusReadAttempts == 5 && loggedErrors == 1 && in.touchWakeIrqPin() == -1);
  assert(gpioLevels[INT_PIN] == LOW);

  // Bus back: the pending report is read and cleared, and the pin returns.
  Wire.busDead = false;
  Wire.report(0x80);
  const unsigned infos = loggedInfos;
  poll(in);
  assert(Wire.mem[WireStub::STATUS] == 0 && gpioLevels[INT_PIN] == HIGH);
  assert(in.touchWakeIrqPin() == INT_PIN && loggedInfos == infos + 1);

  // A transient glitch inside the retry budget never withdraws the pin.
  Wire.report(0x80);
  Wire.failStatusClears = 2;
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusClearAttempts == 3 && Wire.statusClears == 1);
  assert(in.touchWakeIrqPin() == INT_PIN && loggedErrors == 1);

  // Recovery with nothing pending: a clean all-zero status read is enough,
  // and it costs no write.
  Wire.busDead = true;
  poll(in);
  assert(in.touchWakeIrqPin() == -1 && loggedErrors == 2);
  Wire.busDead = false;
  Wire.report(0);
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusClearAttempts == 0 && in.touchWakeIrqPin() == INT_PIN);
}

void testStatusClearFailureWithdrawsWake() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);

  // Status reads fine, but the clear keeps NACKing: INT stays asserted.
  Wire.report(0x80);
  Wire.failStatusClears = 100;
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusClearAttempts == 3 && in.touchWakeIrqPin() == -1 && loggedErrors == 1);
  poll(in);
  assert(Wire.statusClearAttempts == 4 && loggedErrors == 1);

  Wire.failStatusClears = 0;
  poll(in);
  assert(Wire.mem[WireStub::STATUS] == 0 && in.touchWakeIrqPin() == INT_PIN);
}

void testInitialClearFailure() {
  // The table is consumed, but the clear of the report it latched fails: the
  // line is configured yet withheld until a poll clears the register.
  Wire.loadFactory(1);
  Wire.failStatusClears = 3;
  InputManager in;
  boot(in);
  assert(Wire.applies == 1 && Wire.activeIntMode() == 2);
  assert(gpioLevels[INT_PIN] == LOW && in.touchWakeIrqPin() == -1 && loggedErrors == 1);
  poll(in);
  assert(gpioLevels[INT_PIN] == HIGH && in.touchWakeIrqPin() == INT_PIN);
}

void testNotReadyLatchedStatusIsCleared() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  // Nonzero status without the buffer-ready bit still holds INT LOW.
  Wire.report(0x01);
  assert(gpioLevels[INT_PIN] == LOW);
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusClears == 1 && gpioLevels[INT_PIN] == HIGH);
  // The all-zero idle pass stays a single read, no write.
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusReadAttempts == 1 && Wire.statusClearAttempts == 0);
}
#else
void testFlagOffLeavesConfigAlone() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  assert(Wire.configReads == 0 && Wire.configWriteTxns == 0 && Wire.applies == 0);
  assert(Wire.activeIntMode() == 1 && gpioModes[INT_PIN] != INPUT_PULLUP);
  assert(in.touchWakeIrqPin() == -1);
  // Polling is unchanged: a failed status read is a single attempt.
  Wire.busDead = true;
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusReadAttempts == 1);
  Wire.busDead = false;
}
#endif
}  // namespace

int main() {
#if FREEINK_GT911_INT_WAKE
  testPatchAppliedOncePerBootWhenDroppedOnReset();
  testRebeginRefusedWithdrawsPin();
  testNoWriteWhenModeAlreadyLowLevel();
  testRefusals();
  testStatusReadFailureWithdrawsWake();
  testStatusClearFailureWithdrawsWake();
  testInitialClearFailure();
  testNotReadyLatchedStatusIsCleared();
  std::puts("GT911 INT wake (FREEINK_GT911_INT_WAKE=1) passed");
#else
  testFlagOffLeavesConfigAlone();
  std::puts("GT911 INT wake (FREEINK_GT911_INT_WAKE=0) passed");
#endif
}
