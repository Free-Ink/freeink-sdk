#include <BatteryEstimate.h>

#include <cstdio>

namespace {
unsigned checks = 0, failures = 0;
#define CHECK(c)                                       \
  do {                                                 \
    ++checks;                                          \
    if (!(c)) {                                        \
      ++failures;                                      \
      std::printf("FAIL line %d: %s\n", __LINE__, #c); \
    }                                                  \
  } while (0)
BatteryMonitor::Status sample(uint16_t percentage, uint16_t mv, bool usb = false, bool charging = false,
                              bool full = false) {
  BatteryMonitor::Status s;
  s.percentageKnown = s.millivoltsKnown = s.chargingKnown = s.externalPowerKnown = s.chargeCompleteKnown = true;
  s.percentage = percentage;
  s.millivolts = mv;
  s.externalPower = usb;
  s.charging = charging;
  s.chargeComplete = full;
  return s;
}
}  // namespace

int main() {
  BatteryEstimate e;
  CHECK(!e.known());
  CHECK(e.update(sample(98, 4280, true, false, true), 0) == 100);
  CHECK(e.state().power == BatteryEstimate::Power::Full);
  CHECK(e.update(sample(98, 4280), 30000) == 100);  // unplug starts settling
  for (int i = 0; i < 4; ++i) CHECK(e.update(sample(98, 4280), 30000) == 100);
  for (int i = 0; i < 20; ++i) e.update(sample(98, 4280), 30000);
  CHECK(e.percentage() == 98);  // persistent lower reading eventually wins

  auto bad = sample(0, 0);
  bad.percentageKnown = bad.millivoltsKnown = false;
  CHECK(e.update(bad, 30000) == 98);
  auto unknown = sample(80, 4065);
  unknown.chargingKnown = unknown.externalPowerKnown = false;
  CHECK(e.update(unknown, 30000) == 98);
  CHECK(e.update(unknown, 8 * 60 * 60 * 1000) == 98);
  CHECK(e.update(sample(10, 3636), 30000) == 10);  // low voltage bypasses settling/filter
  CHECK(e.update(sample(0, 3300), 30000) == 0);

  BatteryEstimate charge;
  CHECK(charge.update(sample(95, 4306, true, true), 0) == 95);
  for (int i = 0; i < 60; ++i) charge.update(sample(96, 4314, true, true), 30000);
  CHECK(charge.percentage() == 96);  // no one-percent upward rounding trap
  CHECK(charge.update(sample(98, 4280, true, false, true), 30000) == 100);
  CHECK(charge.update(sample(30, 3730), 8 * 60 * 60 * 1000) == 30);  // real overnight loss
  CHECK(charge.update(sample(95, 4306, true, true), UINT32_MAX) == 95);

  BatteryEstimate toppingOff;
  CHECK(toppingOff.update(sample(98, 4304, true, true), 0) == 98);
  for (int i = 0; i < 20; ++i) toppingOff.update(sample(100, 4340, true, true), 60000);
  CHECK(toppingOff.percentage() == 100);
  CHECK(toppingOff.state().power == BatteryEstimate::Power::Charging);

  // Time on the cable alone cannot turn a lower voltage estimate into full.
  BatteryEstimate partialCharge;
  partialCharge.update(sample(98, 4304, true, true), 0);
  for (int i = 0; i < 120; ++i) partialCharge.update(sample(98, 4304, true, true), 60000);
  CHECK(partialCharge.percentage() == 98);

  BatteryEstimate implausible;
  CHECK(implausible.update(sample(50, 3812, true, false, true), 0) == 50);
  CHECK(implausible.state().power != BatteryEstimate::Power::Full);
  CHECK(implausible.update(sample(0, 6000), 30000) == 50);
  CHECK(implausible.update(sample(150, 4200), 30000) == 50);

  BatteryEstimate idle, unread, battery;
  CHECK(idle.update(sample(100, 4340, true), 0) == 100);
  auto unreadStatus = sample(100, 4340);
  unreadStatus.chargingKnown = unreadStatus.externalPowerKnown = false;
  CHECK(unread.update(unreadStatus, 0) == 99);
  CHECK(battery.update(sample(100, 4340), 0) == 100);

  BatteryEstimate restored;
  CHECK(restored.restore(e.state()));
  CHECK(restored.percentage() == e.percentage());
  CHECK(!restored.restore({100 * 256 + 1, 0, BatteryEstimate::Power::Battery}));
  CHECK(!restored.restore({50 * 256, BatteryEstimate::SETTLING_MS + 1, BatteryEstimate::Power::Battery}));
  CHECK(!restored.restore({50 * 256, 0, static_cast<BatteryEstimate::Power>(255)}));

  // Dense sampling and infrequent UI redraws should approximate the same
  // elapsed-time response, rather than moving one step per draw invocation.
  BatteryEstimate dense, sparse;
  dense.update(sample(80, 4065), 0);
  sparse.update(sample(80, 4065), 0);
  for (int i = 0; i < 10; ++i) dense.update(sample(70, 3955), 3000);
  sparse.update(sample(70, 3955), 30000);
  CHECK(dense.percentage() <= sparse.percentage() + 1 && sparse.percentage() <= dense.percentage() + 1);
  CHECK(sizeof(BatteryEstimate) <= 16);
  std::printf("%u estimator checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
