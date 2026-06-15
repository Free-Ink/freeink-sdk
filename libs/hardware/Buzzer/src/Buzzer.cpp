#include "Buzzer.h"

#include <BoardConfig.h>

#if FREEINK_CAP_BUZZER

namespace freeink {
namespace {
// 10-bit resolution like the vendor demo; ledcWriteTone drives a 50% square wave
// and manages the timer frequency per tone, so the base freq here is nominal.
constexpr uint8_t LEDC_RESOLUTION_BITS = 10;
constexpr uint32_t LEDC_BASE_FREQ = 2000;

uint8_t buzzerPin() { return static_cast<uint8_t>(BoardConfig::ACTIVE.audio.buzzer); }
}  // namespace

bool Buzzer::begin() {
  if (begun_) return true;
  if (BoardConfig::ACTIVE.audio.buzzer == BoardConfig::PIN_UNASSIGNED) return false;
  if (!ledcAttach(buzzerPin(), LEDC_BASE_FREQ, LEDC_RESOLUTION_BITS)) return false;
  ledcWrite(buzzerPin(), 0);  // start silent
  begun_ = true;
  return true;
}

void Buzzer::tone(uint32_t freqHz, uint32_t durationMs) {
  if (!begun_ || freqHz == 0) return;
  ledcWriteTone(buzzerPin(), freqHz);
  if (durationMs > 0) {
    delay(durationMs);
    noTone();
  }
}

void Buzzer::noTone() {
  if (!begun_) return;
  ledcWriteTone(buzzerPin(), 0);
  ledcWrite(buzzerPin(), 0);
}

void Buzzer::end() {
  if (!begun_) return;
  noTone();
  ledcDetach(buzzerPin());
  begun_ = false;
}

}  // namespace freeink

#else  // FREEINK_CAP_BUZZER — no buzzer on this board.

namespace freeink {
bool Buzzer::begin() { return false; }
void Buzzer::tone(uint32_t, uint32_t) {}
void Buzzer::noTone() {}
void Buzzer::end() {}
}  // namespace freeink

#endif  // FREEINK_CAP_BUZZER
