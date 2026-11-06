// CHSC6x (Murphy M3): a contact only reads as a long press once it has been held that long.
#include <cassert>
#include <cstdio>

#include <Wire.h>
#include <InputManager.h>

namespace {
constexpr unsigned long kSampleMs = 10;  // a host's input sampling cadence

// Captured on a Murphy M3: a finger on the panel, then the frame that follows the lift.
const std::vector<uint8_t> kContactFrame = {0x00, 0x00, 0x01, 0x80, 0x64, 0x01, 0x8B, 0x0D, 0x10};
const std::vector<uint8_t> kLiftFrame = {0x00, 0x00, 0x00, 0x40, 0x64, 0x01, 0x8B, 0x0D, 0x10};

InputManager in;
unsigned long longPressAt = 0;  // when the classifier fired, 0 = not yet

void sample() {
  fakeNow += kSampleMs;
  in.update();
  float nx = 0.0f;
  float ny = 0.0f;
  if (in.wasTouchLongPress(nx, ny)) {
    assert(longPressAt == 0 && "a contact long-presses once");
    longPressAt = fakeNow;
  }
}

void waitForRelease() {
  Wire.frame = kLiftFrame;
  while (in.isTouchPressed()) sample();
}
}  // namespace

int main() {
  // Keys idle high (active low), and pin 2 is the Down key here, not the Picco's power interrupt
  // that the shared stub guards.
  powerInterruptEnabled = false;
  gpioLevels[0] = gpioLevels[1] = gpioLevels[2] = HIGH;
  in.begin();
  assert(in.hasTouch());

  // A tap: every read straddles a millisecond tick, and the contact is over long before the
  // threshold.
  Wire.frame = kContactFrame;
  sample();
  assert(in.isTouchPressed());
  sample();
  waitForRelease();
  float nx = 0.0f;
  float ny = 0.0f;
  assert(in.wasTouchReleased() && in.wasTouchTap(nx, ny));
  assert(longPressAt == 0 && "a tap is not a long press");

  // A hold: the long press waits for the full threshold.
  sample();
  Wire.frame = kContactFrame;
  sample();
  assert(in.isTouchPressed());
  const unsigned long downAt = fakeNow;
  while (fakeNow - downAt < 700) sample();
  assert(longPressAt != 0 && "a real hold still long-presses");
  assert(longPressAt - downAt >= 490 && "not before it has been held");
  waitForRelease();

  std::puts("CHSC6x touch passed");
}
