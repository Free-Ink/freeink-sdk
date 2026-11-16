#include <BoardConfig.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "driver/Uc8253MurphyDriver.h"
#include "lut/Uc8253MurphyLuts.h"

using namespace freeink;

namespace {
constexpr uint16_t FB_W = 416, FB_H = 240, FB_WB = FB_W / 8, CTRL_W = 240, CTRL_H = 416, CTRL_WB = CTRL_W / 8;
constexpr size_t PLANE = static_cast<size_t>(FB_WB) * FB_H;

struct Bits {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(PLANE, 0xFF);  // white
  void set(uint16_t x, uint16_t y, bool on) {
    uint8_t& byte = bytes[static_cast<size_t>(y) * FB_WB + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
    byte = on ? byte | mask : byte & ~mask;
  }
  const uint8_t* data() const { return bytes.data(); }
};

Bits blank(uint8_t fill) {
  Bits bits;
  bits.bytes.assign(PLANE, fill);
  return bits;
}

// Framebuffer pixel (x,y) sits at controller (239-y, x).
bool ramBit(const std::vector<uint8_t>& ram, uint16_t x, uint16_t y) {
  const uint16_t cx = static_cast<uint16_t>(FB_H - 1 - y);
  return (ram[static_cast<size_t>(x) * CTRL_WB + (cx >> 3)] >> (7 - (cx & 7))) & 1;
}

const EpdBus::Write* last(const EpdBus& bus, uint8_t command) {
  for (auto it = bus.writes.rbegin(); it != bus.writes.rend(); ++it) {
    if (it->command == command) return &*it;
  }
  return nullptr;
}

unsigned count(const EpdBus& bus, uint8_t command) {
  unsigned n = 0;
  for (const auto& write : bus.writes) n += write.command == command;
  return n;
}

int netDrive(const uint8_t* lut) {
  int net = 0;
  for (size_t group = 0; group < 42; group += 7) {
    for (size_t phase = 1; phase <= 4; phase++) {
      const uint8_t byte = lut[group + phase];
      if ((byte >> 6) == 1) net += byte & 0x3F;
      if ((byte >> 6) == 2) net -= byte & 0x3F;
    }
  }
  return net;
}

int totalFrames(const uint8_t* lut) {
  int frames = 0;
  for (size_t group = 0; group < 42; group += 7) {
    for (size_t phase = 1; phase <= 4; phase++) frames += lut[group + phase] & 0x3F;
  }
  return frames;
}

Uc8253MurphyConfig withGray() {
  Uc8253MurphyConfig cfg = uc8253MurphyDefaultConfig();
  cfg.gray = {MURPHY_LUT_20_GRAY, MURPHY_LUT_21_GRAY, MURPHY_LUT_22_GRAY, MURPHY_LUT_23_GRAY, MURPHY_LUT_24_GRAY};
  return cfg;
}

void nudge(Uc8253MurphyDriver& driver, EpdBus& bus, const Bits& lsb, const Bits& msb) {
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.copyGrayscaleMsb(bus, msb.data());
  driver.displayGray(bus, nullptr, false, nullptr, false);
}

void testFastGivesWhiteTheLongerDriveAndVcomCoversIt() {
  assert(netDrive(MURPHY_LUT_22_FAST) == -20);  // black to white
  assert(netDrive(MURPHY_LUT_23_FAST) == 15);   // white to black
  // The longest LUT sets the refresh, and the IC fills what is shorter with 0 V: VCOM has to span it.
  for (const uint8_t* lut : {MURPHY_LUT_21_FAST, MURPHY_LUT_22_FAST, MURPHY_LUT_23_FAST, MURPHY_LUT_24_FAST}) {
    assert(totalFrames(lut) <= totalFrames(MURPHY_LUT_20_FAST));
  }
}

void testTheHostOwnsTheCadence() {
  EpdBus bus;
  Uc8253MurphyDriver driver;
  driver.begin(bus);
  Bits page = blank(0xFF);
  for (int i = 0; i < 30; i++) {
    bus.clear();
    driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
    assert(!std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_FAST, sizeof(MURPHY_LUT_22_FAST)));
  }
}

void testABoardCanStillAskTheDriverToPromote() {
  Uc8253MurphyConfig cfg = uc8253MurphyDefaultConfig();
  cfg.ghostClearInterval = 3;
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits page = blank(0xFF);
  for (int i = 0; i < 8; i++) {
    bus.clear();
    driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
    const bool full = !std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_DEFAULT, sizeof(MURPHY_LUT_22_DEFAULT));
    assert(full == (i % 4 == 3));
  }
}

void testWithoutAGrayBankNothingChanges() {
  EpdBus bus;
  Uc8253MurphyDriver driver;
  assert(!driver.grayscaleCapabilities().supported());
  driver.begin(bus);
  bus.clear();
  Bits lsb, msb;
  nudge(driver, bus, lsb, msb);
  assert(bus.writes.empty());
}

void testGrayBankIsBalancedAndSelectsByPlane() {
  for (const uint8_t* lut : {MURPHY_LUT_21_GRAY, MURPHY_LUT_22_GRAY, MURPHY_LUT_23_GRAY, MURPHY_LUT_24_GRAY}) {
    assert(netDrive(lut) == 0);
  }
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  assert(driver.grayscaleCapabilities().encoding == GrayscaleEncoding::OverlayMasks);
  assert(!driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  driver.begin(bus);
  bus.clear();

  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);  // light: LSB 0, MSB 1
  lsb.set(9, 7, true);  // dark: LSB 1, MSB 1
  msb.set(9, 7, true);
  nudge(driver, bus, lsb, msb);

  assert(count(bus, 0x12) == 1);
  const auto* dtm1 = last(bus, 0x10);
  const auto* dtm2 = last(bus, 0x13);
  assert(!ramBit(dtm1->bytes, 5, 7) && ramBit(dtm2->bytes, 5, 7));  // {NEW 1, OLD 0}: LUT 22
  assert(ramBit(dtm1->bytes, 9, 7) && ramBit(dtm2->bytes, 9, 7));   // {NEW 1, OLD 1}: LUT 21
  assert(!ramBit(dtm1->bytes, 20, 7) && !ramBit(dtm2->bytes, 20, 7));
  assert(!std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_GRAY, sizeof(MURPHY_LUT_22_GRAY)));
  assert(!std::memcmp(last(bus, 0x21)->bytes.data(), MURPHY_LUT_21_GRAY, sizeof(MURPHY_LUT_21_GRAY)));

  // LUTs before planes before the refresh: the nudge must not fire on stale RAM.
  size_t order[4] = {};
  for (size_t i = 0; i < bus.writes.size(); i++) {
    if (bus.writes[i].command == 0x24) order[0] = i;
    if (bus.writes[i].command == 0x10) order[1] = i;
    if (bus.writes[i].command == 0x13) order[2] = i;
    if (bus.writes[i].command == 0x12) order[3] = i;
  }
  assert(order[0] < order[1] && order[1] < order[2] && order[2] < order[3]);
}

void testFastDrivesGrayAwayAndOnlyGray() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);

  // Page A: black at the three test pixels, the first two of them gray after the nudge.
  Bits page = blank(0xFF);
  page.set(3, 3, false);
  page.set(4, 3, false);
  page.set(5, 3, false);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(3, 3, true);
  msb.set(4, 3, true);
  nudge(driver, bus, lsb, msb);

  // Page B keeps 3 and 5 black and clears 4.
  Bits next = page;
  next.set(4, 3, true);
  bus.clear();
  driver.display(bus, next.data(), page.data(), RefreshMode::Fast, false);
  const auto* old = last(bus, 0x10);
  const auto* fresh = last(bus, 0x13);
  assert(ramBit(old->bytes, 3, 3) && !ramBit(fresh->bytes, 3, 3));   // gray to black: WK, full drive
  assert(!ramBit(old->bytes, 4, 3) && ramBit(fresh->bytes, 4, 3));   // gray to white: KW, as any change
  assert(!ramBit(old->bytes, 5, 3) && !ramBit(fresh->bytes, 5, 3));  // clean black stays: KK, rests
  assert(ramBit(old->bytes, 100, 100) && ramBit(fresh->bytes, 100, 100));  // clean white stays: WW

  // The mark lasts one refresh: the gray is gone now.
  Bits third = next;
  bus.clear();
  driver.display(bus, third.data(), next.data(), RefreshMode::Fast, false);
  assert(!ramBit(last(bus, 0x10)->bytes, 3, 3));
}

void testGrayPixelsClearWithoutAPreviousFrame() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(3, 3, true);
  nudge(driver, bus, lsb, msb);
  Bits next = blank(0xFF);
  bus.clear();
  driver.display(bus, next.data(), nullptr, RefreshMode::Fast, false);
  assert(!ramBit(last(bus, 0x10)->bytes, 3, 3) && ramBit(last(bus, 0x13)->bytes, 3, 3));
  assert(ramBit(last(bus, 0x10)->bytes, 9, 9));
}

void testASecondNudgeWouldLiftGrayFurtherSoItIsRefused() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(3, 3, true);
  nudge(driver, bus, lsb, msb);
  bus.clear();
  nudge(driver, bus, lsb, msb);
  assert(count(bus, 0x12) == 0);

  Bits page = blank(0xFF);
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
  bus.clear();
  nudge(driver, bus, lsb, msb);
  assert(count(bus, 0x12) == 1);
}

void testANudgeRightAfterAPageSkipsTheResetAndSwapsThePlanes() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits page = blank(0xFF);
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
  bus.clear();

  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);  // light: MSB only
  nudge(driver, bus, lsb, msb);

  assert(count(bus, 0x01) == 0 && count(bus, 0x04) == 0);  // no init, no power on
  assert(count(bus, 0x12) == 1);
  assert(ramBit(last(bus, 0x10)->bytes, 5, 7) && !ramBit(last(bus, 0x13)->bytes, 5, 7));
}

void testAPoweredOffControllerTakesTheFullPathWithStraightPlanes() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits page = blank(0xFF);
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, true);  // the sunlight fix powers off
  bus.clear();

  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);
  nudge(driver, bus, lsb, msb);

  assert(count(bus, 0x01) == 1 && count(bus, 0x04) == 1);
  assert(!ramBit(last(bus, 0x10)->bytes, 5, 7) && ramBit(last(bus, 0x13)->bytes, 5, 7));
}

void testASleepEndsTheWarmPath() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  // Sleep resets the controller: a nudge after it must not assume the roles are traded.
  Bits page = blank(0xFF);
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
  driver.deepSleep(bus);
  driver.begin(bus);
  bus.clear();
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);
  nudge(driver, bus, lsb, msb);
  assert(count(bus, 0x01) == 1);
  assert(!ramBit(last(bus, 0x10)->bytes, 5, 7));
}

void testAnAbandonedPassShowsNothing() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  bus.clear();
  Bits lsb = blank(0x00);
  driver.copyGrayscaleLsb(bus, lsb.data());  // MSB never arrives
  driver.displayGray(bus, nullptr, false, nullptr, false);
  assert(bus.writes.empty());
  // Nor does a later page inherit it: nothing was left gray.
  Bits page = blank(0x00), next = blank(0x00);
  driver.display(bus, next.data(), page.data(), RefreshMode::Fast, false);
  assert(!ramBit(last(bus, 0x10)->bytes, 3, 3));
}

void testAFullRefreshClearsGrayWithoutMarks() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(3, 3, true);
  nudge(driver, bus, lsb, msb);
  Bits page = blank(0xFF);
  bus.clear();
  driver.display(bus, page.data(), page.data(), RefreshMode::Full, false);
  assert(last(bus, 0x10)->bytes == last(bus, 0x13)->bytes);
  bus.clear();
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
  assert(ramBit(last(bus, 0x10)->bytes, 3, 3));
}
}  // namespace

int main() {
  BoardConfig::ACTIVE.displayWidth = FB_W;
  BoardConfig::ACTIVE.displayHeight = FB_H;
  assert(CTRL_W == FB_H && CTRL_H == FB_W);
  testFastGivesWhiteTheLongerDriveAndVcomCoversIt();
  testTheHostOwnsTheCadence();
  testABoardCanStillAskTheDriverToPromote();
  testWithoutAGrayBankNothingChanges();
  testGrayBankIsBalancedAndSelectsByPlane();
  testFastDrivesGrayAwayAndOnlyGray();
  testGrayPixelsClearWithoutAPreviousFrame();
  testASecondNudgeWouldLiftGrayFurtherSoItIsRefused();
  testANudgeRightAfterAPageSkipsTheResetAndSwapsThePlanes();
  testAPoweredOffControllerTakesTheFullPathWithStraightPlanes();
  testASleepEndsTheWarmPath();
  testAnAbandonedPassShowsNothing();
  testAFullRefreshClearsGrayWithoutMarks();
  std::cout << "PASS: UC8253 Murphy gray nudge and gray-aware FAST\n";
}
