#include <cassert>
#include <cstdio>
#include <algorithm>
#include <Wire.h>
#include <BoardConfig.h>
#define private public
#include <InputManager.h>
#undef private

static void point(InputManager& input, unsigned x, unsigned y, bool down=true) {
  Wire.touch = {uint8_t(down), uint8_t(x >> 8), uint8_t(x), uint8_t(y >> 8), uint8_t(y)};
  fakeNow += 25;
  if (touchIsr) touchIsr();
  input.update();
}

int main() {
  static_assert(BoardConfig::MAX_FRAMEBUFFER_BYTES == 48000);
  static_assert(FREEINK_MCU_S3 && FREEINK_DRIVER_SSD1677 && FREEINK_CAP_TOUCH);
  static_assert(FREEINK_CAP_RTC && FREEINK_SD_SDMMC && FREEINK_BATTERY_I2C_GAUGE);
  static_assert(!FREEINK_CAP_FRONTLIGHT && FREEINK_CAP_AUDIO && FREEINK_CAP_MIC && FREEINK_CAP_IMU);
  static_assert(BoardConfig::METALIO_EINK4.display.dc == 13);
  static_assert(BoardConfig::METALIO_EINK4.display.rst == 18);
  static_assert(BoardConfig::METALIO_EINK4.sdmmc.busWidth == 1);
  static_assert(BoardConfig::METALIO_EINK4.sdmmc.d3 == -1);
  std::fill(std::begin(gpioLevels), std::end(gpioLevels), HIGH);

  // A transient boot failure is retryable. Safe output latches precede direction.
  Wire.failExpander = true;
  assert(!freeink::metalio::ensureBooted());
  Wire.failExpander = false;
  assert(freeink::metalio::ensureBooted());
  assert(Wire.writes[0][0] == 2 && Wire.writes[1][0] == 6);
  const unsigned out = Wire.expander[2] | (Wire.expander[3] << 8);
  assert((out & ((1 << 5) | (1 << 6) | (1 << 9) | (1 << 11))) ==
         ((1 << 5) | (1 << 6) | (1 << 9) | (1 << 11)));
  assert(!(out & ((1 << 1) | (1 << 4)))); // amp off, ESP32 route
  const auto writes = Wire.writes.size();
  assert(freeink::metalio::ensureBooted());
  assert(Wire.writes.size() == writes); // no reset when next manager begins

  InputManager input;
  // CST816S must be usable even when it NACKs all boot probes.
  Wire.failTouch = true;
  input.begin();
  assert(input.hasTouch());
  Wire.failTouch = false;
  assert(touchIsr);
  touchIsr(); // latch a pulse that has finished before update()
  point(input, 0, 0);
  assert(input.touchPressed && input.touchPoint.x == 0 && input.touchPoint.y == 479);
  point(input, 479, 799);
  assert(input.touchPoint.x == 799 && input.touchPoint.y == 0);
  point(input, 479, 799, false);
  assert(!input.touchPressed && input.touchReleasedEvent);

  touchIsr();
  point(input, 80, 900);
  assert(input.wasHomeKeyPressed() && !input.touchPressed);
  point(input, 80, 900, false);
  assert(input.wasHomeKeyTapped());
  touchIsr();
  point(input, 80, 900);
  fakeNow += 2000;
  point(input, 80, 900);
  assert(input.wasHomeKeyLongPressed());
  point(input, 80, 900, false);
  assert(!input.wasHomeKeyTapped());

  touchIsr();
  point(input, 240, 900);
  point(input, 240, 900);
  assert(input.isPressed(InputManager::BTN_DOWN) && !input.touchPressed);
  point(input, 240, 900, false);
  point(input, 240, 900, false);
  assert(!input.isPressed(InputManager::BTN_DOWN));
  touchIsr();
  point(input, 400, 900);
  point(input, 400, 900);
  assert(input.isPressed(InputManager::BTN_UP));
  Wire.failTouch = true;
  fakeNow += 150;
  input.update();
  fakeNow += 25;
  input.update();
  assert(!input.isPressed(InputManager::BTN_UP)); // bus loss releases held cover key
  Wire.failTouch = false;

  point(input, 120, 200);
  assert(input.touchPressed);
  fakeNow += 150;
  input.update(); // stale successful frame, IRQ idle
  assert(!input.touchPressed);

  Wire.expander[0] &= ~(1 << 7);
  fakeNow += 25;
  assert(input.getState() & (1 << InputManager::BTN_DOWN));
  Wire.failExpander = true;
  fakeNow += 25;
  assert(!(input.getState() & (1 << InputManager::BTN_DOWN)));
  Wire.failExpander = false;
  assert(freeink::metalio::powerOff());
  assert(Wire.expander[2] & (1 << 5)); // shared screen/card rail never cut
  std::puts("Metalio profile, expander boot/retry, buttons, touch, cover keys and shutdown passed");
}
