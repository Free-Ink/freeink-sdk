#include <InputManager.h>
#include <Rtc.h>
#include <MurphyM4I2c.h>
#include <Wire.h>
#include <driver/i2c_master.h>

#include <cassert>
#include <cstdio>

namespace {

void setTouchFrame(const uint8_t count, const uint16_t rawX, const uint16_t rawY) {
  fakeTouchRegisters[0x02] = count;
  fakeTouchRegisters[0x03] = static_cast<uint8_t>((rawX >> 8) & 0x0F);
  fakeTouchRegisters[0x04] = static_cast<uint8_t>(rawX);
  fakeTouchRegisters[0x05] = static_cast<uint8_t>((rawY >> 8) & 0x0F);
  fakeTouchRegisters[0x06] = static_cast<uint8_t>(rawY);
}

void touchAndRtcShareNativeBus() {
  BoardConfig::ACTIVE = BoardConfig::MURPHY_M4;
  fakePinLevels[BoardConfig::ACTIVE.input.power] = HIGH;
  fakeTouchRegisters[0x80] = 0x16;
  fakeTouchRegisters[0x88] = 0x04;
  setTouchFrame(0, 0, 0);

  InputManager input;
  input.setMurphyM4Batch(freeink::MurphyM4Batch::First);
  input.begin();
  assert(input.hasTouch());
  assert(fakeStaticTaskCreateCalls == 0);  // begin() uses the synchronous polling path.
  assert(fakeI2cBusCreates == 1);
  assert(fakeI2cBusConfig.i2c_port == I2C_NUM_1);
  assert(fakeI2cBusConfig.sda_io_num == 13 && fakeI2cBusConfig.scl_io_num == 12);
  assert(fakeTouchDevice.address == 0x2E && fakeTouchDevice.frequency == 100000);

  setTouchFrame(1, 330, 150);
  fakeNow += 20;
  input.update();
  assert(input.wasTouchPressed() && input.isTouchPressed());
  const auto point = input.getTouchPoint();
  assert(point.valid && point.x == 150 && point.y == 177);

  setTouchFrame(0, 0, 0);
  fakeNow += 20;
  input.update();
  assert(input.wasTouchReleased() && !input.isTouchPressed());

  fakeRtcRegisters[0x1E] = 0x02;
  Rtc rtc;
  assert(rtc.begin());
  assert(fakeI2cBusCreates == 1 && fakeI2cDeviceAdds == 2);
  assert(fakeRtcDevice.address == 0x32 && fakeRtcDevice.frequency == 400000);
  assert(fakeWireCalls == 0);

  Rtc::DateTime time;
  assert(!rtc.now(time));  // RX8010 VLF marks the clock unset.
  const Rtc::DateTime expected{2026, 10, 11, 12, 34, 56, 0};
  assert(rtc.set(expected));
  assert(fakeRtcRegisters[0x1E] == 0x00);
  assert(rtc.now(time));
  assert(time.year == 2026 && time.month == 10 && time.day == 11);
  assert(time.hour == 12 && time.minute == 34 && time.second == 56 && time.weekday == 0);

  assert(input.prepareForDeepSleep());
  assert(fakePinLevels[BoardConfig::ACTIVE.touch.powerEnable] == HIGH);
  assert(!input.hasTouch());
  assert(input.reinitializeTouchAfterSharedReset());
  assert(input.hasTouch());
  assert(fakeStaticTaskCreateCalls == 1);  // the host rejects task creation, preserving the fallback.
  assert(fakeI2cBusCreates == 1 && fakeI2cDeviceAdds == 2);
}

}  // namespace

int main() {
  touchAndRtcShareNativeBus();
  std::puts("Murphy M4 native I2C touch and RTC integration passed");
}