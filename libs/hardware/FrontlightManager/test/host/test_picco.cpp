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
  // Vendor-table endpoints must stay unchanged, including fully warm/cool.
  static constexpr LightSetting minimum[] = {{1, 0, 24, 0}, {1, 1, 19, 0}, {1, 3, 17, 2}, {1, 2, 14, 7}, {2, 1, 10, 12},
                                             {1, 2, 7, 16}, {3, 1, 3, 21}, {2, 1, 0, 26}, {1, 1, 0, 26}, {0, 5, 0, 31}};
  static constexpr LightSetting maximum[] = {
      {136, 0, 31, 0},    {132, 69, 31, 31}, {127, 94, 31, 31}, {122, 108, 31, 31}, {115, 118, 31, 31},
      {107, 126, 31, 31}, {97, 133, 31, 31}, {82, 138, 31, 31}, {58, 143, 31, 31},  {0, 147, 0, 31}};
  for (unsigned ct = 0; ct < 10; ++ct) {
    light.off();
    light.setColorTemperature((ct * 100 + 4) / 9);
    light.setBrightness(1);
    assert(setting() == minimum[ct]);
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
        for (unsigned channel = 0; channel < 4; ++channel) {
          assert(std::abs(int(setting()[channel]) - int(previous[channel])) <= 2);
        }
      }
      assert(Wire.regs[5] <= 31 && Wire.regs[6] <= 31);
      if (ct == 0) assert(Wire.regs[4] == 0 && !(Wire.regs[0] & 2));
      if (ct == 9) assert(Wire.regs[3] == 0 && !(Wire.regs[0] & 4));
    }
    assert(changes >= 97);  // previously only eight settings across the whole slider
    assert(setting() == maximum[ct]);
    for (unsigned percent = 100; percent > 0; --percent) {
      light.setBrightness(percent);
      assert(setting() == ramp[percent - 1]);
    }
    assert(fakeNow == start);  // dragging must not add wake delays
    light.off();
    assert(!Wire.visible());
    light.on();
    assert(Wire.visible() && setting() == minimum[ct]);
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
