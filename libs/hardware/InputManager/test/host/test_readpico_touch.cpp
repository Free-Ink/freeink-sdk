#include <BoardReadPico.h>
#include <InputManager.h>
#include <Wire.h>

#include <cassert>
#include <cmath>

namespace {
bool powerDown = false;
bool asleep = false;


void point(int slot, uint16_t x, uint16_t y) {
  auto* record = Wire.cst836uFrame.data() + 3 + slot * 6;
  record[0] = x >> 8;
  record[1] = x & 0xff;
  record[2] = (slot << 4) | (y >> 8);
  record[3] = y & 0xff;
  ++Wire.cst836uFrame[2];
}

void clearFrame() {
  Wire.cst836uFrame.fill(0);
  Wire.cst836uFrame[3] = Wire.cst836uFrame[9] = 0xc0;  // invalid point slots
}

void tick(InputManager& input, unsigned long ms = 30) {
  fakeNow += ms;
  input.update();
}
}  // namespace

namespace BoardReadPico {
bool touchReset() { return true; }
bool touchSleep() {
  asleep = true;
  return true;
}
uint8_t stripButtonMask(uint16_t x) { return x == READPICO_KEY_CENTER_2 ? 1U << InputManager::BTN_BACK : 0; }
bool powerKeyPressed() { return powerDown; }
}  // namespace BoardReadPico

int main() {
  InputManager input;
  clearFrame();
  input.setButtonHook([]() -> uint8_t { return powerDown ? 1U << InputManager::BTN_POWER : 0; });
  input.begin();
  assert(input.hasTouch());
  assert(input.supportsMultiTouch());
  tick(input);

  point(0, 0, 0);
  tick(input);
  float x, y;
  assert(input.wasTouchPressedAt(x, y));
  assert(std::abs(x) < 0.002f && std::abs(y - 1.0f) < 0.002f);
  clearFrame();
  tick(input);
  assert(input.wasTouchTap(x, y));

  point(0, 100, 100);
  point(1, 200, 200);
  tick(input);
  clearFrame();
  tick(input);
  assert(!input.wasTouchTap(x, y));

  point(0, READPICO_KEY_CENTER_2, READPICO_KEY_AREA_TOP + 1);
  tick(input);
  tick(input);
  assert(input.isPressed(InputManager::BTN_BACK));
  assert(!input.wasTouchPressedAt(x, y));
  Wire.failTouch = true;
  tick(input);
  tick(input);
  assert(!input.isPressed(InputManager::BTN_BACK));
  assert(!input.wasTouchTap(x, y));

  powerDown = true;
  assert(input.isPowerButtonPhysicallyPressed());
  tick(input);
  tick(input);
  assert(input.isPressed(InputManager::BTN_POWER));
  assert(input.prepareForDeepSleep() && asleep);
}
