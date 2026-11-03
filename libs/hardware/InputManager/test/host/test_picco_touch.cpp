// Picco TMA525C (Parade PIP): mode check, bootloader launch, touch decode, lift-off.
#include <InputManager.h>
#include <Wire.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

static std::vector<uint8_t> touchReport(uint16_t x, uint16_t y, uint8_t event, uint8_t count = 1) {
  return {17, 0, 0x01, 0, 0, count, 0, 0x00, static_cast<uint8_t>(event << 5), static_cast<uint8_t>(x),
          static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8), 0, 0, 0, 0};
}

struct Finger {
  uint8_t id;
  uint16_t x, y;
  uint8_t event = 0;
};

static std::vector<uint8_t> contacts(std::initializer_list<Finger> fingers) {
  std::vector<uint8_t> report(7 + fingers.size() * 10, 0);
  report[0] = static_cast<uint8_t>(report.size());
  report[2] = 1;
  report[5] = static_cast<uint8_t>(fingers.size());
  unsigned offset = 7;
  for (const auto& f : fingers) {
    report[offset + 1] = f.id | (f.event << 5);
    report[offset + 2] = f.x & 0xff;
    report[offset + 3] = f.x >> 8;
    report[offset + 4] = f.y & 0xff;
    report[offset + 5] = f.y >> 8;
    offset += 10;
  }
  return report;
}

static void send(InputManager& input, std::vector<uint8_t> report) {
  Wire.push(report);
  fakeNow += 20;
  input.update();
}

static bool rotated(InputManager& input) {
  float degrees, x, y;
  unsigned long duration;
  return input.wasMultiTouchRotation(degrees, x, y, duration);
}

static void rotatePair(InputManager& input) {
  send(input, contacts({{7, 240, 300}, {3, 240, 500}}));
  const auto snapshot = input.getTouchSnapshot();
  assert(snapshot.count == 2 && snapshot.reportedCount == 2 && snapshot.idsStable);
  assert(snapshot.points[0].id == 7 && snapshot.points[1].id == 3);
  assert(!input.wasTouchPressed());  // two fingers never become a single tap
  // Reverse record order while preserving IDs, then turn the pair 90 degrees.
  send(input, contacts({{3, 340, 400}, {7, 140, 400}}));
}

int main() {
  // Bootloader at power-up: the driver must send launch-app with the PIP CRC.
  gpioLevels[2] = gpioLevels[5] = gpioLevels[9] = gpioLevels[21] = gpioLevels[6] = 1;
  Wire.mode = 0xFF;
  Wire.sync();
  InputManager in;
  // A held wake button with a stale level interrupt must be disarmed before pinMode.
  gpioLevels[2] = 0;
  powerInterruptEnabled = true;
  in.begin();
  assert(!powerInterruptEnabled);
  gpioLevels[2] = 1;
  assert(in.hasTouch() && in.supportsMultiTouch());
  bool launched = false;
  for (const auto& w : Wire.writes)
    if (w.size() == 13 && w[7] == 0x3B) {
      launched = true;
      assert(w[10] == 0x20 && w[11] == 0xC7 && w[12] == 0x17);  // CRC-16/CCITT-FALSE(01 3B 00 00) = 0xC720
    }
  assert(launched);

  // Finger down at raw (100, 200): stock orientation (480-100, 800-200) = (380, 600),
  // swapXY onto the 800x480 panel frame -> (600, 380), flipY (hardware-confirmed) -> (600, 99).
  Wire.push(touchReport(100, 200, 0));
  fakeNow += 20;
  in.update();
  assert(in.wasTouchPressed() && in.isTouchPressed());
  auto p = in.getTouchPoint();
  assert(p.valid && p.x == 600 && p.y == 99);

  // Lift-off record releases.
  Wire.push(touchReport(100, 200, 3));
  fakeNow += 20;
  in.update();
  assert(!in.isTouchPressed() && in.wasTouchReleased());

  // A still finger may stop producing reports: the touch stays down and long
  // press fires; only a missed lift-off is released, after the safety net.
  Wire.push(touchReport(240, 400, 0));
  fakeNow += 20;
  in.update();
  assert(in.isTouchPressed());
  bool longPress = false;
  for (int i = 0; i < 30; ++i) {
    fakeNow += 20;
    in.update();
    float nx, ny;
    longPress |= in.wasTouchLongPress(nx, ny);
  }
  assert(in.isTouchPressed() && longPress);
  fakeNow += 3100;
  in.update();
  assert(!in.isTouchPressed());
  // Complete contacts and stable IDs feed the existing SDK rotation recognizer.
  rotatePair(in);
  send(in, contacts({}));
  float degrees, cx, cy;
  unsigned long duration;
  assert(in.wasMultiTouchRotation(degrees, cx, cy, duration));
  assert(std::abs(degrees + 90.0f) < 0.1f);
  assert(cx > 0.49f && cx < 0.51f && cy > 0.49f && cy < 0.51f);
  float sx, sy, ex, ey;
  uint8_t n;
  assert(!in.wasTouchTap(cx, cy) && !in.wasSwipe(sx, sy, ex, ey));
  assert(!in.wasMultiTouchSwipe(n, sx, sy, ex, ey, duration));
  in.update();
  assert(!rotated(in));

  // First-finger lift finalizes once, even when a lift record precedes the
  // remaining live contact. A later all-up frame cannot emit it again.
  rotatePair(in);
  send(in, contacts({{7, 140, 400, 3}, {3, 340, 400, 2}}));
  assert(rotated(in) && in.getTouchSnapshot().count == 1);
  send(in, contacts({}));
  assert(!rotated(in));
  assert(!in.wasTouchTap(cx, cy) && !in.wasSwipe(sx, sy, ex, ey));

  // One finger lifting as another lands must not turn the gap between them
  // into a single-finger swipe, tap, or long press. Fresh contacts still work.
  send(in, contacts({{7, 100, 200}}));
  send(in, contacts({{7, 100, 200, 3}, {3, 100, 600}}));
  send(in, contacts({}));
  assert(!in.wasSwipe(sx, sy, ex, ey) && !in.wasTouchTap(cx, cy) && !rotated(in));
  in.update();
  send(in, contacts({{3, 100, 600}}));
  send(in, contacts({}));
  assert(in.wasTouchTap(cx, cy));
  in.update();

  // Missing lift-off times out without inventing a completed rotation.
  rotatePair(in);
  fakeNow += 3100;
  in.update();
  assert(!in.isTouchPressed() && !rotated(in) && in.getTouchSnapshot().count == 0);
  in.update();

  // Truncated reports, duplicate IDs, and out-of-range points cancel a turn.
  for (unsigned fault = 0; fault < 4; ++fault) {
    rotatePair(in);
    auto broken = contacts({{7, 140, 400}, {3, 340, 400}});
    if (fault == 0) {
      broken.resize(17);
      broken[0] = 17;
    }  // declares two, carries one
    if (fault == 1) broken[18] = 7;  // duplicate track ID
    if (fault == 2) {
      broken[9] = 0xff;
      broken[10] = 0xff;
    }
    if (fault == 3) broken[5] = 5;  // over the bounded contact capacity
    send(in, broken);
    send(in, contacts({}));
    assert(!rotated(in));
    in.update();
  }

  // Real panel capability decides whether settings can expose multi-touch.
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    Wire.maxContacts = 1;
    Wire.replySysinfo = scenario != 1;
    Wire.sysinfoLength = scenario == 2 ? 50 : 51;
    Wire.sysinfoCommand = scenario == 3 ? 3 : 2;
    InputManager single;
    const auto start = fakeNow;
    single.begin();
    assert(single.hasTouch() && !single.supportsMultiTouch());
    assert(fakeNow - start < 1000);  // bounded probe, touch still available
    send(single, contacts({{0, 100, 200}}));
    send(single, contacts({}));
    assert(single.wasTouchTap(cx, cy));
  }
  std::puts("Picco TMA525C touch passed");
}
