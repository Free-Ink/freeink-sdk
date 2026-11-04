#include <algorithm>
#include <cassert>
#include <cstdio>
#include <Wire.h>
#include <BoardConfig.h>
#include <InputManager.h>

static void sample(InputManager& input, bool down) {
  Wire.touch = {uint8_t(down), 0, 120, 0, 200};
  fakeNow += 10;
  input.update();
}

int main() {
  std::fill(std::begin(gpioLevels), std::end(gpioLevels), HIGH);
  InputManager input;
  input.begin();
  assert(input.hasTouch() && input.supportsMultiTouch());
  assert(!input.touchContactPending());

  // Wake idle polling on the interrupt, before the first contact sample.
  touchIsr();
  assert(input.touchContactPending());
  Wire.failTouch = true;
  sample(input, true);
  assert(!input.isTouchPressed());
  Wire.failTouch = false;
  sample(input, true);  // INT is high, with no second interrupt.
  assert(input.isTouchPressed() && input.touchContactPending());
  sample(input, false);
  float nx, ny;
  assert(input.wasTouchTap(nx, ny));
  assert(!input.touchContactPending());
  assert(nx > 0.16f && nx < 0.17f && ny > 0.82f && ny < 0.83f);

  // A valid single-sample contact must become a tap.
  sample(input, true);
  assert(input.isTouchPressed() && input.wasTouchPressed());
  sample(input, false);
  assert(input.wasTouchTap(nx, ny) && !input.touchContactPending());

  // Missing the interrupt entirely must not disable an otherwise valid tap.
  sample(input, true);
  sample(input, true);
  sample(input, false);
  assert(input.wasTouchTap(nx, ny));

  // A held-low INT also wakes the idle loop without a captured edge.
  gpioLevels[BoardConfig::ACTIVE.touch.irq] = LOW;
  assert(input.touchContactPending());
  gpioLevels[BoardConfig::ACTIVE.touch.irq] = HIGH;
  std::puts("Metalio Plus IRQ recovery, idle wake, tap mapping and single-sample taps passed");
}
