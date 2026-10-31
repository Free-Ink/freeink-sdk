// Picco: ADC battery + SGM41562 charger @0x03 (CHRG_STAT REG08[4:3], input present REG08[1],
// ship mode REG06 bit 5) against a register-file model of the charger.
#include <cstdio>
#include <vector>

#include "../../src/BatteryMonitor.cpp"

namespace {
int checksRun = 0;
int checksFailed = 0;
#define CHECK(condition)                                               \
  do {                                                                 \
    ++checksRun;                                                       \
    if (!(condition)) {                                                \
      ++checksFailed;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                  \
  } while (0)

uint8_t regs[16] = {};
bool present = true;
}  // namespace

bool hostI2cWrite(const uint8_t addr, const std::vector<uint8_t>& bytes) {
  if (addr != 0x03 || !present || bytes.size() != 2 || bytes[0] >= sizeof(regs)) return false;
  regs[bytes[0]] = bytes[1];
  return true;
}
bool hostI2cRead(const uint8_t addr, const uint8_t reg, uint8_t* out, const size_t length) {
  if (addr != 0x03 || !present || length != 1 || reg >= sizeof(regs)) return false;
  *out = regs[reg];
  return true;
}

int main() {
  BatteryMonitor bm;
  bool known = false;

  struct Case {
    uint8_t reg08;
    bool charging;
    bool usb;
  } cases[] = {{0x00, false, false}, {0x0A, true, true}, {0x12, true, true}, {0x1A, false, true}, {0x18, false, false}};
  for (const Case& c : cases) {
    regs[0x08] = c.reg08;
    CHECK(bm.isCharging() == c.charging);
    CHECK(bm.isExternalPowerPresent(&known) == c.usb && known);
    const BatteryMonitor::Status s = bm.readStatus();
    CHECK(s.supported && s.chargingKnown && s.charging == c.charging);
    CHECK(s.externalPowerKnown && s.externalPower == c.usb);
  }

  // LiHV curve (stock tables), x2 divider: retain single-percent estimates
  // above and below 70%. Charging uses its own voltage table.
  regs[0x08] = 0x00;
  hostAdcMv = 2100;
  CHECK(bm.readPercentage() == 92);
  hostAdcMv = 2050;  // 4100 mV -> 83%
  CHECK(bm.readPercentage() == 83);
  hostAdcMv = 2015;  // 4030 mV -> 76%, previously rounded to 80
  CHECK(bm.readPercentage() == 76);
  hostAdcMv = 2005;  // 4010 mV -> 74%, previously rounded to 70
  CHECK(bm.readPercentage() == 74);
  uint16_t percent = 0;
  CHECK(bm.readPercentageChecked(percent) && percent == 74);
  const BatteryMonitor::Status batteryStatus = bm.readStatus();
  CHECK(batteryStatus.percentageKnown && batteryStatus.percentage == 74);
  hostAdcMv = 1965;  // 3930 mV -> 66%
  CHECK(bm.readPercentage() == 66);
  regs[0x08] = 0x12;  // fast charging
  hostAdcMv = 2152;   // 4304 mV under charge -> 94%
  CHECK(bm.readPercentage() == 94);
  hostAdcMv = 2170;  // above 4305 -> full
  CHECK(bm.readPercentage() == 100);
  hostAdcMv = 0;  // failed read never reads full
  CHECK(bm.readPercentage() == 0);

  // Both curves must stay bounded and monotonic, and expose every integer
  // percentage below 70 rather than only the old ten-point notches.
  for (const uint8_t chargeState : {0x00, 0x12}) {
    regs[0x08] = chargeState;
    uint16_t previous = 0;
    uint16_t nextBelow70 = 0;
    for (hostAdcMv = 1; hostAdcMv <= 2250; ++hostAdcMv) {
      const uint16_t current = bm.readPercentage();
      CHECK(current >= previous && current <= 100);
      if (current == nextBelow70 && nextBelow70 < 70) ++nextBelow70;
      previous = current;
    }
    CHECK(previous == 100 && nextBelow70 == 70);
  }
  regs[0x08] = 0x00;

  // Stock charger init: gated on REG0B == 0, then read-modify-write per step.
  regs[0x0B] = 0x01;
  CHECK(!BatteryMonitor::configureCharger());
  CHECK(regs[0x04] == 0x00);  // nothing written
  regs[0x0B] = 0x00;
  regs[0x01] = 0xFF;  // other bits must survive
  regs[0x04] = 0x01;
  regs[0x05] = 0xFF;
  regs[0x0D] = 0x07;
  regs[0x06] = 0x00;
  CHECK(BatteryMonitor::configureCharger());
  CHECK(regs[0x04] == 0xAB);  // 4.35 V code in [7:1], bit 0 kept
  CHECK(regs[0x02] == 0x3E && regs[0x06] == 0x80);
  CHECK(regs[0x05] == 0x9F);  // bits 6:5 cleared
  CHECK(regs[0x0D] == 0x4F);  // [7:3] = 0x48, [2:0] kept
  CHECK(regs[0x01] == 0x17);  // bits 7:5 and 3 cleared
  CHECK(regs[0x0C] == 0x00);

  regs[0x06] = 0x81;
  CHECK(BatteryMonitor::enterShipMode());
  CHECK(regs[0x06] == 0xA1);  // BATFET off set, other bits kept

  present = false;
  CHECK(!bm.isExternalPowerPresent(&known) && !known);
  CHECK(!bm.readStatus().chargingKnown);
  CHECK(!BatteryMonitor::enterShipMode());

  std::printf("%d checks, %d failures\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
