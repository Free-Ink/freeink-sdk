#include <FrontlightManager.h>
#include <Logging.h>
#include <Wire.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace {
using LightSetting = std::array<uint8_t, 4>;
LightSetting setting() { return {Wire.regs[3], Wire.regs[4], Wire.regs[5], Wire.regs[6]}; }

void testBrightnessRamp(FrontlightManager& light) {
  for (unsigned warmth = 0; warmth <= 100; ++warmth) {
    light.off();
    light.setColorTemperature(warmth);
    light.setBrightness(1);
    const auto minimum = setting();
    if (warmth == 0) assert((minimum == LightSetting{1, 0, 24, 0}));
    if (warmth == 100) assert((minimum == LightSetting{0, 5, 0, 31}));
    const auto start = fakeNow;
    std::array<LightSetting, 100> ramp{};
    unsigned changes = 1;
    for (unsigned percent = 1; percent <= 100; ++percent) {
      light.setBrightness(percent);
      assert(Wire.visible());
      ramp[percent - 1] = setting();
      if (percent > 1) {
        const auto& previous = ramp[percent - 2];
        if (setting() != previous) ++changes;
        for (unsigned channel = 0; channel < 2; ++channel) {
          assert(std::abs(int(setting()[channel]) - int(previous[channel])) <= 4);
        }
      }
      assert(Wire.regs[5] <= 31 && Wire.regs[6] <= 31);
      assert(Wire.regs[3] <= 177 && Wire.regs[4] <= 184);
      assert(Wire.regs[3] || !Wire.regs[5]);
      assert(Wire.regs[4] || !Wire.regs[6]);
      assert(bool(Wire.regs[0] & 4) == bool(Wire.regs[3]));
      assert(bool(Wire.regs[0] & 2) == bool(Wire.regs[4]));
      if (warmth == 0) assert(Wire.regs[4] == 0 && !(Wire.regs[0] & 2));
      if (warmth == 100) assert(Wire.regs[3] == 0 && !(Wire.regs[0] & 4));
    }
    assert(changes >= 90);  // previously only eight settings across the whole slider
    if (warmth == 0) assert((setting() == LightSetting{177, 0, 31, 0}));
    if (warmth == 50) assert((setting() == LightSetting{152, 163, 31, 31}));
    if (warmth == 100) assert((setting() == LightSetting{0, 184, 0, 31}));
    for (unsigned percent = 100; percent > 0; --percent) {
      light.setBrightness(percent);
      assert(setting() == ramp[percent - 1]);
    }
    assert(fakeNow == start);  // dragging must not add wake delays
    light.off();
    assert(!Wire.visible());
    light.on();
    assert(Wire.visible() && setting() == minimum);
  }
}
}  // namespace

int main() {
  FrontlightManager light;
  light.begin();
  assert(light.present());
  light.setColorTemperature(100);
  light.setBrightness(0);
  light.setBrightness(55);  // same nonzero saved level as the hardware report
  assert(Wire.visible());   // first on must work without moving a slider
  assert(Wire.rejectedWrites == 0);
  assert(Wire.regs[4] > 0 && (Wire.regs[0] & 2));
  const auto awakeTime = fakeNow;
  light.setBrightness(56);
  light.setColorTemperature(0);
  assert(Wire.visible() && fakeNow == awakeTime);  // no wake delay during slider updates
  assert(Wire.regs[3] > 0 && (Wire.regs[0] & 4));
  const auto cool = Wire.regs[3];
  light.off();
  assert(!Wire.visible());
  light.on();
  assert(Wire.visible() && Wire.regs[3] == cool && fakeNow >= awakeTime + 2);
  light.off();
  Wire.failRegister = 0;  // cannot leave standby: log and leave configuration retryable
  light.on();
  assert(!Wire.visible() && loggedErrors > 0);
  light.setBrightness(56);
  assert(Wire.visible());
  testBrightnessRamp(light);
  std::puts(
      "Picco frontlight: first on, wake timing, saved level, warm/cool, retry, and smooth brightness ramp passed");
}
