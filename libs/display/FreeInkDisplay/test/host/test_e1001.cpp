#include <cassert>
#include <iostream>
#include <vector>

#include "driver/Uc8179E1001Driver.h"

// Seeed reTerminal E1001: full refresh, windowed fast refresh, deep sleep and the
// register-waveform switch, checked against the real driver on a recording bus.
int main() {
  using namespace freeink;
  constexpr uint16_t W = 800, H = 480, WB = W / 8;
  constexpr size_t bytes = size_t(WB) * H;
  std::vector<uint8_t> fb(bytes);
  for (size_t i = 0; i < bytes; ++i) fb[i] = static_cast<uint8_t>(i * 37);

  EpdBus bus;
  auto& driver = uc8179E1001Driver();
  const auto g = driver.geometry();
  assert(g.width == W && g.height == H && g.widthBytes == WB && "geometry must come from the board profile");
  driver.begin(bus);
  assert(bus.resets == 1);

  // Full refresh: NEW = frame, OLD = its complement so every pixel is driven,
  // then OLD is re-synced to the frame on screen.
  driver.display(bus, fb.data(), nullptr, RefreshMode::Full, false);
  assert(bus.refreshes == 1 && !bus.partialAtRefresh);
  assert(bus.newAtRefresh == fb);
  for (size_t i = 0; i < bytes; ++i) assert(bus.oldAtRefresh[i] == static_cast<uint8_t>(~fb[i]));
  assert(bus.oldPlane == fb && "OLD plane must hold the shown frame after a full refresh");
  assert(bus.powered && "turnOff=false keeps the panel powered");

  // A fast refresh with nothing changed sends nothing.
  driver.display(bus, fb.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.refreshes == 1 && "an unchanged frame must not refresh");

  // A fast refresh of a small change refreshes only its byte-aligned window.
  // Rows 100..109, pixels 203..250 -> bytes 25..31 -> x 200..255.
  for (int y = 100; y < 110; ++y)
    for (int x = 203; x <= 250; ++x) fb[size_t(y) * WB + x / 8] ^= static_cast<uint8_t>(0x80 >> (x % 8));
  const unsigned powerOnsBefore = bus.powerOns, powerOffsBefore = bus.powerOffs;
  driver.display(bus, fb.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.refreshes == 2 && bus.partialAtRefresh);
  const std::vector<uint8_t> window = {0, 200, 0, 255, 0, 100, 0, 109, 0x01};
  assert(bus.ptl == window && "partial window must be the byte-aligned dirty box");
  assert(bus.newAtRefresh.size() == size_t(7) * 10);
  for (int r = 0; r < 10; ++r)
    for (int c = 0; c < 7; ++c) assert(bus.newAtRefresh[size_t(r) * 7 + c] == fb[size_t(100 + r) * WB + 25 + c]);
  assert(bus.oldPlane == bus.newAtRefresh && "OLD window must be re-synced after the fast refresh");
  assert(bus.powered && bus.powerOns == powerOnsBefore && bus.powerOffs == powerOffsBefore &&
         "a panel left powered by the full refresh goes straight into fast updates");

  // A second fast refresh reuses partial mode: no re-init, no extra power-on.
  fb[size_t(300) * WB + 10] ^= 0xFF;
  driver.display(bus, fb.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.refreshes == 3 && bus.powerOns == powerOnsBefore && bus.powerOffs == powerOffsBefore);
  const std::vector<uint8_t> single = {0, 80, 0, 87, 1, 44, 1, 44, 0x01};
  assert(bus.ptl == single);

  // The register waveform: the next fast refresh re-initialises and uploads
  // six 42-byte tables (VCOM, WW, KW, WK, KK, border).
  uc8179E1001SetFastWaveform(true);
  bus.luts.clear();
  fb[size_t(5) * WB] ^= 0x01;
  driver.display(bus, fb.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.luts.size() == 6);
  for (const auto& lut : bus.luts) assert(lut.size() == 42);
  assert(bus.luts[2][0] == 0x5A && bus.luts[3][0] == 0x84 && bus.luts[2][1] == 16 && "KW/WK drive, 16 frames");
  uc8179E1001SetFastWaveform(false);
  bus.luts.clear();
  fb[size_t(6) * WB] ^= 0x01;
  driver.display(bus, fb.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.luts.empty() && "back on the built-in waveform: no register tables");

  // Deep sleep powers down and loses controller RAM: the next fast request is
  // promoted to a full refresh.
  driver.deepSleep(bus);
  assert(!bus.powered && bus.deepSleeps == 1);
  assert(bus.commands.back() == 0x07 && bus.lastPayload == std::vector<uint8_t>{0xA5});
  driver.begin(bus);
  fb[0] ^= 0xFF;
  driver.display(bus, fb.data(), nullptr, RefreshMode::Fast, true);
  assert(!bus.partialAtRefresh && bus.newAtRefresh == fb && "fast after deep sleep must be a full refresh");
  assert(!bus.powered && "turnOff=true powers the panel down after a full refresh");

  // displayWindow before a valid baseline falls back to a full refresh.
  driver.requestResync(0);
  const unsigned before = bus.refreshes;
  driver.displayWindow(bus, fb.data(), nullptr, 0, 0, 8, 8, false);
  assert(bus.refreshes == before + 1 && !bus.partialAtRefresh);

  std::cout << "E1001 UC8179: full, windowed fast, waveform switch and deep sleep passed\n";
  return 0;
}
