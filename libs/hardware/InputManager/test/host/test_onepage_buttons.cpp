#include <InputManager.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iterator>

int main() {
  std::fill(std::begin(gpioLevels), std::end(gpioLevels), HIGH);
  InputManager input;
  input.begin();

  constexpr struct {
    int millivolts;
    uint8_t button;
  } keys[] = {{2592, InputManager::BTN_BACK},
              {1956, InputManager::BTN_LEFT},
              {1316, InputManager::BTN_RIGHT},
              {0, InputManager::BTN_CONFIRM}};

  assert(!input.isAdcLadderPhysicallyPressed());
  for (const auto& key : keys) {
    analogMilliVolts = key.millivolts;
    assert(input.isAdcLadderPhysicallyPressed());
    assert(!input.isPressed(key.button));
    input.update();
    fakeNow += 10;
    input.update();
    assert(input.wasPressed(key.button));

    analogMilliVolts = 3300;
    assert(!input.isAdcLadderPhysicallyPressed());
    assert(input.isPressed(key.button));
    input.update();
    fakeNow += 10;
    input.update();
    assert(input.wasReleased(key.button));
  }
  std::puts("OnePage front keys agree in raw and debounced input");
}
