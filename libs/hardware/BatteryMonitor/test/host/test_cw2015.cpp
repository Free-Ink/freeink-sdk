// CW2015 fuel gauge (EEGO A4) against a model gauge: the boot probe with ADC
// fallback, the BATINFO compare/upload sequence, and the voltage/percent read.
#include <array>
#include <cstdio>
#include <string>
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

// Registers 0x00..0x0B from a stock boot log on an A4 (VERSION 0x73, CONFIG 0x1A
// with UFG set, MODE 0x00), with the stock target profile resident.
struct FakeGauge {
  bool present = true;
  std::array<uint8_t, 0x100> regs{};
  std::vector<std::string> writes;  // "reg=value"
  int probes = 0;

  FakeGauge() {
    const uint8_t boot[12] = {0x73, 0x00, 0x29, 0x99, 0x00, 0xf5, 0x9f, 0xff, 0x1a, 0x00, 0x00, 0x00};
    std::copy(boot, boot + 12, regs.begin());
    std::copy(CW2015_BATINFO, CW2015_BATINFO + 64, regs.begin() + 0x10);
  }
  static std::string hex(unsigned v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%02X", v);
    return b;
  }
};
FakeGauge gauge;

void reset(FakeGauge fresh = FakeGauge()) {
  gauge = fresh;
  cw2015State = Cw2015State();
  hostMillis = 0;
  BoardConfig::ACTIVE = BoardConfig::EEGO_A4;
}

}  // namespace

bool hostI2cWrite(uint8_t addr, const std::vector<uint8_t>& bytes) {
  if (addr != 0x62 || !gauge.present) return false;
  if (bytes.empty()) {
    ++gauge.probes;
    return true;
  }
  if (bytes.size() == 2) {
    gauge.regs[bytes[0]] = bytes[1];
    gauge.writes.push_back(FakeGauge::hex(bytes[0]) + "=" + FakeGauge::hex(bytes[1]));
  }
  return true;
}

bool hostI2cRead(uint8_t addr, uint8_t reg, uint8_t* out, size_t length) {
  if (addr != 0x62 || !gauge.present) return false;
  for (size_t i = 0; i < length; ++i) out[i] = gauge.regs[(reg + i) & 0xFF];
  return true;
}

int main() {
  CHECK(BoardConfig::EEGO_A4.batteryGauge.gaugeAddr == 0x62);
  CHECK(BoardConfig::EEGO_A4.batteryGauge.gaugeType == BoardConfig::GaugeType::Cw2015);
  CHECK(BoardConfig::EEGO_A4.batteryGauge.i2cSda == 2 && BoardConfig::EEGO_A4.batteryGauge.i2cScl == 1);

  // Resident profile matches, UFG set: nothing is written. The boot-log sample
  // reads VCELL 0x2999 -> 3248 mV, SOC 0x00F5 -> 1% -> 2% shown, as stock logged.
  {
    reset();
    BatteryMonitor monitor;
    const auto status = monitor.readStatus();
    CHECK(status.supported);
    CHECK(status.millivoltsKnown && status.millivolts == 3248);
    CHECK(status.percentageKnown && status.percentage == 2);
    CHECK(gauge.writes.empty());
    monitor.readPercentage();
    CHECK(gauge.probes == 1);  // probed once per boot
  }

  // Charging comes from the STAT pin (GPIO11, active-high); the gauge cannot see it.
  {
    reset();
    BatteryMonitor monitor;
    hostDigitalPin = HIGH;
    CHECK(monitor.isCharging());
    hostDigitalPin = LOW;
    CHECK(!monitor.isCharging());
  }

  // Empty profile: stock's upload, byte for byte.
  {
    FakeGauge empty;
    std::fill(empty.regs.begin() + 0x10, empty.regs.begin() + 0x50, 0);
    empty.regs[0x08] = 0x18;  // UFG clear
    empty.regs[0x0A] = 0x30;
    reset(empty);
    BatteryMonitor monitor;
    monitor.readPercentage();
    std::vector<std::string> expected;
    for (int i = 0; i < 64; ++i) expected.push_back(FakeGauge::hex(0x10 + i) + "=" + FakeGauge::hex(CW2015_BATINFO[i]));
    expected.push_back("08=1A");  // CONFIG |= UFG
    expected.push_back("0A=3F");  // MODE |= power-on reset
    expected.push_back("0A=30");  // MODE &= 0xF0
    CHECK(gauge.writes == expected);
    CHECK(hostMillis >= 20);
    CHECK(std::equal(CW2015_BATINFO, CW2015_BATINFO + 64, gauge.regs.begin() + 0x10));
  }

  // A different profile is replaced; a matching one with UFG clear is kept.
  {
    FakeGauge other;
    other.regs[0x20] ^= 0xFF;
    reset(other);
    BatteryMonitor().readPercentage();
    CHECK(gauge.writes.size() == 67);
    CHECK(std::equal(CW2015_BATINFO, CW2015_BATINFO + 64, gauge.regs.begin() + 0x10));
    CHECK(cw2015UploadProfile(0x62));  // reads back profile + UFG

    FakeGauge noUfg;
    noUfg.regs[0x08] = 0x18;
    reset(noUfg);
    BatteryMonitor().readPercentage();
    CHECK(gauge.writes.empty());
  }

  // A gauge left in sleep mode is woken before anything else.
  {
    FakeGauge asleep;
    asleep.regs[0x0A] = 0xC0;
    reset(asleep);
    BatteryMonitor().readPercentage();
    CHECK(!gauge.writes.empty() && gauge.writes.front() == "0A=00");
    CHECK(gauge.writes.size() == 1);
  }

  // Conversion bounds: SOC 100.00% shows 100 (not 101); out-of-range samples fail.
  {
    reset();
    BatteryMonitor monitor;
    gauge.regs[0x04] = 0x64;
    gauge.regs[0x05] = 0x00;
    uint16_t pct = 0;
    CHECK(monitor.readPercentageChecked(pct) && pct == 100);
    gauge.regs[0x04] = 0x32;
    gauge.regs[0x05] = 0x80;  // 50.5% rounds to 51, shown 52
    CHECK(monitor.readPercentageChecked(pct) && pct == 52);
    gauge.regs[0x05] = 0x7F;  // 50.496% rounds to 50, shown 51
    CHECK(monitor.readPercentageChecked(pct) && pct == 51);
    gauge.regs[0x04] = 0x64;
    gauge.regs[0x05] = 0x01;  // above 100.00%
    pct = 7;
    CHECK(!monitor.readPercentageChecked(pct) && pct == 7);
    gauge.regs[0x04] = 0x32;
    gauge.regs[0x02] = 0x1F;  // 0x1F99 -> 2471 mV, below 2500
    CHECK(!monitor.readPercentageChecked(pct));
    gauge.regs[0x02] = 0x29;
    gauge.regs[0x0A] = 0xC0;  // asleep after setup
    CHECK(!monitor.readPercentageChecked(pct));
    gauge.regs[0x0A] = 0x00;
    CHECK(monitor.readPercentageChecked(pct) && pct == 51);
  }

  // No gauge on the bus: one probe, then the GPIO10 ADC through the x1.559 divider.
  {
    FakeGauge absent;
    absent.present = false;
    reset(absent);
    BatteryMonitor monitor;
    hostAdcMv = 2500;
    CHECK(monitor.readMillivolts() == 3897);
    const auto status = monitor.readStatus();
    CHECK(status.supported && status.millivoltsKnown && status.millivolts == 3897);
    CHECK(status.percentageKnown && status.percentage == BatteryMonitor::percentageFromMillivolts(3897));
    uint16_t pct = 0;
    CHECK(monitor.readPercentageChecked(pct) && pct == status.percentage);
    CHECK(gauge.writes.empty());
  }

  std::printf("%d/%d CW2015 checks passed\n", checksRun - checksFailed, checksRun);
  return checksFailed == 0 ? 0 : 1;
}
