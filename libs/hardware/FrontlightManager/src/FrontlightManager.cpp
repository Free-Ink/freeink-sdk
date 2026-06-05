#include "FrontlightManager.h"

#if FREEINK_CAP_FRONTLIGHT
namespace {
uint32_t maxDuty(uint8_t bits) { return (1u << bits) - 1u; }
}  // namespace
#endif

void FrontlightManager::begin() {
#if FREEINK_CAP_FRONTLIGHT
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  if (fl.gpio == BoardConfig::PIN_UNASSIGNED) return;

#if defined(ARDUINO) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // Arduino-ESP32 3.x LEDC API.
  ledcAttach(fl.gpio, fl.pwmFrequency, fl.pwmResolutionBits);
#else
  // Arduino-ESP32 2.x fallback.
  ledcSetup(0, fl.pwmFrequency, fl.pwmResolutionBits);
  ledcAttachPin(fl.gpio, 0);
#endif
  _begun = true;
  setBrightness(0);
#endif
}

void FrontlightManager::setBrightness(uint8_t percent) {
#if FREEINK_CAP_FRONTLIGHT
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  if (!_begun || fl.gpio == BoardConfig::PIN_UNASSIGNED) return;
  if (percent > 100) percent = 100;
  _brightness = percent;
  if (percent > 0) _lastBrightness = percent;

  const uint32_t full = maxDuty(fl.pwmResolutionBits);
  uint32_t duty = (static_cast<uint32_t>(percent) * full) / 100u;
  if (!fl.activeHigh) duty = full - duty;

#if defined(ARDUINO) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(fl.gpio, duty);
#else
  ledcWrite(0, duty);
#endif
#else
  (void)percent;
#endif
}

void FrontlightManager::off() { setBrightness(0); }
void FrontlightManager::on() { setBrightness(_lastBrightness); }

void FrontlightManager::setColorTemperature(uint8_t warmPercent) {
  // Records the requested warm percentage; it drives no channel. Warm/cool mixing
  // on the boost-driver channels (GPIO6/7) is not implemented.
  _warmPercent = warmPercent > 100 ? 100 : warmPercent;
}
