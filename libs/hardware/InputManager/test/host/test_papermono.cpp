#include <InputManager.h>
#include <PaperMonoBoard.h>

#include <cassert>
#include <cstdio>

namespace {
uint8_t auxiliaryButtons = 0;
uint8_t buttonHook() { return auxiliaryButtons; }

void tick(InputManager& input, unsigned long now, unsigned sides, bool click = false) {
  fakeNow = now;
  gpioLevels[BoardConfig::ACTIVE.input.up] = sides & 1 ? LOW : HIGH;
  gpioLevels[BoardConfig::ACTIVE.input.down] = sides & 2 ? LOW : HIGH;
  freeink::papermono::powerButtonClicked = click;
  input.update();
}
}  // namespace

int main() {
  InputManager::setButtonHook(buttonHook);
  for (unsigned sides : {0u, 1u, 2u, 3u}) {
    InputManager input;
    tick(input, 0, sides);
    tick(input, 100, sides, true);
    assert(input.wasPressed(InputManager::BTN_POWER));
    assert(input.isPressed(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 0);
    fakeNow = 600;  // A pulse has no physical hold duration, even before the next poll.
    assert(input.getPowerButtonHeldTime() == 0);
    tick(input, 600, sides);
    assert(input.wasReleased(InputManager::BTN_POWER));
    assert(!input.isPressed(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 0);
    tick(input, 601, sides);
    assert(!input.wasReleased(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 0);
  }

  // An old single-side hold must not turn a PMIC click into a Power hold.
  for (unsigned sides : {1u, 2u}) {
    InputManager input;
    tick(input, 0, sides);
    tick(input, 1000, sides, true);
    assert(input.wasPressed(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 0);
    const auto sideAction = sides == 1 ? InputManager::BTN_BACK : InputManager::BTN_CONFIRM;
    assert(input.isPressed(sideAction));
    tick(input, 1600, sides);
    assert(input.wasReleased(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 0);
    assert(input.isPressed(sideAction));
  }

  // A click just before the chord matures does not create a second Power press.
  for (bool earlyClick : {false, true}) {
    InputManager input;
    const unsigned long start = earlyClick ? ULONG_MAX - 300 : 0;
    tick(input, start, 3);
    if (earlyClick) {
      tick(input, start + 100, 3, true);
      assert(input.wasPressed(InputManager::BTN_POWER));
      assert(input.getPowerButtonHeldTime() == 0);
    }
    tick(input, start + 650, 3);
    assert(input.wasPressed(InputManager::BTN_POWER) == !earlyClick);
    assert(input.isPressed(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 650);
    tick(input, start + 800, 3, true);  // PMIC activity must not downgrade an active chord.
    assert(!input.wasPressed(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 800);
    tick(input, start + 1000, 0);
    assert(input.wasReleased(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 1000);
    tick(input, start + 1100, 0, true);  // A fresh click must not inherit the completed chord.
    assert(input.wasPressed(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 0);
    tick(input, start + 2000, 0);
    assert(input.wasReleased(InputManager::BTN_POWER));
    assert(input.getPowerButtonHeldTime() == 0);
  }

  // A click on a physical transition must also allow the following chord to mature.
  InputManager input;
  tick(input, 0, 3, true);
  assert(input.wasPressed(InputManager::BTN_POWER));
  assert(input.getPowerButtonHeldTime() == 0);
  tick(input, 650, 3);
  assert(!input.wasPressed(InputManager::BTN_POWER));
  assert(input.getPowerButtonHeldTime() == 650);
  tick(input, 1000, 0);
  assert(input.wasReleased(InputManager::BTN_POWER));
  tick(input, 1100, 1, true);
  tick(input, 1600, 1);
  assert(input.getPowerButtonHeldTime() == 0);

  // Other auxiliary Power sources retain their measured duration, not the side-button age.
  auxiliaryButtons = 1u << InputManager::BTN_POWER;
  tick(input, 1700, 1);
  assert(input.wasPressed(InputManager::BTN_POWER));
  assert(input.getPowerButtonHeldTime() == 0);
  tick(input, 1800, 1);
  assert(input.getPowerButtonHeldTime() == 100);
  auxiliaryButtons = 0;
  tick(input, 2200, 1);
  assert(input.wasReleased(InputManager::BTN_POWER));
  assert(input.getPowerButtonHeldTime() == 500);
  InputManager::setButtonHook(nullptr);
  std::puts("Paper Mono PMIC click duration and two-button Power chords passed");
}
