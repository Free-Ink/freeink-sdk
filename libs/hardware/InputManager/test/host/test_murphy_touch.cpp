// Murphy M3 CHSC6x touch: mount transform, stop-separated read, scaled gesture
// thresholds, and held duration measured to the last real sample.
#include <BoardConfig.h>
#include <Wire.h>
#define private public
#include <InputManager.h>
#undef private

#include <cassert>
#include <cstdio>

static_assert(FREEINK_DEVICE_MURPHY && FREEINK_TOUCH_CHSC6X);

// Frame: data[3] bit 7 = touch present, data[4] = raw X, data[5..6] = raw Y big-endian.
static void setFrame(bool down, unsigned rawX = 0, unsigned rawY = 0) {
  uint8_t* f = Wire.frame;
  f[3] = down ? 0x80 : 0x00;
  f[4] = static_cast<uint8_t>(rawX);
  f[5] = static_cast<uint8_t>(rawY >> 8);
  f[6] = static_cast<uint8_t>(rawY);
}

static void step(InputManager& in, unsigned ms = 10) {
  fakeNow += ms;
  in.update();
}

// Sends one sample and returns the mapped point.
static InputManager::TouchPoint sample(InputManager& in, unsigned rawX, unsigned rawY) {
  setFrame(true, rawX, rawY);
  step(in);
  assert(in.isTouchPressed());
  return in.touchUpPoint;
}

// Lets the contact lapse; reports what the release update saw.
struct Release {
  bool swipe = false, tap = false;
  unsigned long held = 0;
};
static Release release(InputManager& in) {
  setFrame(false);
  Release r;
  for (int i = 0; i < 40 && !in.wasTouchReleased(); ++i) {
    step(in);
    if (in.wasTouchReleased()) {
      float a, b, c, d;
      r.swipe = in.wasSwipe(a, b, c, d);
      r.tap = in.wasTouchTap(a, b);
      r.held = in.lastTouchHeldMs();
    }
  }
  assert(!in.isTouchPressed());
  return r;
}

int main() {
  powerInterruptEnabled = false;  // the shared stub asserts pinMode(2) only while a wake interrupt is armed
  InputManager in;
  in.begin();
  assert(in.hasTouch());

  // 1. Mount transform. swapXY: mapped X comes from raw Y (24..398 -> 0..374),
  // mapped Y from raw X (24..224 -> 0..200), then flipY: y = 200 - y.
  //   raw (X=24,  Y=24)  -> (0,   200)
  //   raw (X=24,  Y=398) -> (374, 200)
  //   raw (X=224, Y=24)  -> (0,   0)
  //   raw (X=224, Y=398) -> (374, 0)
  struct Corner { unsigned rawX, rawY, x, y; };
  const Corner corners[] = {{24, 24, 0, 200}, {24, 398, 374, 200}, {224, 24, 0, 0}, {224, 398, 374, 0}};
  for (const auto& c : corners) {
    const auto p = sample(in, c.rawX, c.rawY);
    assert(p.valid && p.x == c.x && p.y == c.y);
    release(in);
  }

  // 2. The read is stop-separated: every endTransmission got stop=true.
  assert(!Wire.stops.empty());
  for (bool stop : Wire.stops) assert(stop);

  // 3. Scaled thresholds: 60*287/639 = 26, 28*287/639 = 12.
  assert(in.touchSwipeMinPx() == 26 && in.touchTapSlopPx() == 12);
  // 30 mapped px along X (raw Y moves 1:1) -> swipe, not tap.
  sample(in, 124, 100);
  sample(in, 124, 115);
  sample(in, 124, 130);
  auto r = release(in);
  assert(r.swipe && !r.tap);
  // 5 px -> tap, not swipe.
  sample(in, 124, 100);
  sample(in, 124, 105);
  r = release(in);
  assert(r.tap && !r.swipe);

  // 4. Held duration spans first to last real sample, not the 120 ms hold-over.
  setFrame(true, 124, 100);
  step(in);
  const unsigned long first = fakeNow;
  for (int i = 0; i < 9; ++i) step(in);  // samples every 10 ms
  const unsigned long span = fakeNow - first;
  r = release(in);
  assert(span == 90 && r.held == span);

  std::puts("Murphy M3 CHSC6x touch passed");
}
