#include "FrontlightManager.h"

#if FREEINK_CAP_FRONTLIGHT
#include "FrontlightManager.h"

#include <M5Pm1.h>
#include <Wire.h>
#ifdef FREEINK_FRONTLIGHT_LS
#include <driver/gpio.h>
#include <driver/ledc.h>
// esp_sleep_sub_mode_config lives in a private IDF header (no public API exists
// for balancing the refcounted RC_FAST keep-on the LEDC driver takes for
// KEEP_ALIVE channels — the driver manages it through this same header). Pinned
// IDF 5.5; re-check on IDF bumps.
#include <esp_private/esp_sleep_internal.h>
#endif

// Logging: use the firmware's Logging.h LOG_INF facility, NOT esp_log. The
// prebuilt Arduino sdkconfig sets CONFIG_LOG_DEFAULT_LEVEL_ERROR, so ESP_LOGI is
// compiled out, and esp_log writes to the IDF UART console rather than the
// firmware's Serial stream the monitor reads. LOG_INF routes through logPrintf
// to the firmware Serial and the crash-report ring buffer.
#include <Logging.h>

namespace {
constexpr uint32_t maxDuty(uint8_t bits) { return (1u << bits) - 1u; }

// Perception-weighted percent -> duty, gamma 1.6554: 16-bit fixed-point table
// of round(65535 * (pct/100)^1.6554). The exponent is log(2*1023)/log(100),
// picked so on a 10-bit duty range 1% rounds up to exactly 1 LSB (the dimmest
// possible light) and every 1% step maps to a distinct duty — gamma 2 was too
// steep and collapsed 1-4% into the same single-LSB duty.
static constexpr uint16_t GAMMA_TABLE[101] = {
    0,     32,    101,   197,   318,   460,   622,   803,   1001,  1217,  1449,  1697,  1960,  2237,  2529,
    2835,  3155,  3488,  3834,  4193,  4565,  4949,  5345,  5753,  6173,  6604,  7047,  7502,  7967,  8444,
    8931,  9429,  9938,  10457, 10987, 11527, 12077, 12638, 13208, 13789, 14379, 14979, 15588, 16208, 16836,
    17474, 18122, 18779, 19445, 20120, 20804, 21497, 22200, 22911, 23631, 24360, 25097, 25843, 26598, 27362,
    28134, 28914, 29703, 30500, 31306, 32120, 32942, 33772, 34611, 35457, 36312, 37175, 38045, 38924, 39811,
    40705, 41608, 42518, 43436, 44361, 45295, 46236, 47185, 48141, 49105, 50076, 51055, 52042, 53036, 54037,
    55046, 56062, 57086, 58117, 59155, 60200, 61253, 62313, 63380, 64454, 65535};

constexpr uint32_t perceptualDuty(uint32_t pct, uint32_t full) {
  if (pct == 0) return 0;
  if (pct > 100) pct = 100;
  const uint32_t duty = (full * GAMMA_TABLE[pct] + 32767u) / 65535u;
  return duty ? duty : 1u;
}

// Paper Mono: the PWM lives in the M5PM1 PMIC, not the ESP. PM1 GPIO3 routed to
// alt-function PWM0 drives the AW9967 frontlight driver. Duty register is
// 12-bit; the high byte's bit 4 is the channel-enable bit. Perception-weighted
// like M5Unified's bring-up: duty = brightness^2 scaled into 12 bits.
constexpr uint8_t PM1_PWM_ENABLE = 0x10;

void pm1FrontlightAttach(uint32_t freqHz) {
  freeink::m5pm1::beginBus();
  // GPIO3 to push-pull, alt-function PWM0.
  freeink::m5pm1::updateReg(freeink::m5pm1::REG_GPIO_DRV, 1u << 3, 0);
  freeink::m5pm1::updateReg(freeink::m5pm1::REG_GPIO_FUNC0, 0xC0, 0xC0);
  freeink::m5pm1::writeReg16(freeink::m5pm1::REG_PWM_FREQ_L, static_cast<uint16_t>(freqHz));
}

void pm1FrontlightWrite(uint32_t pct) {
  const uint32_t duty = (pct * pct * 4095u) / 10000u;  // 0-100% -> 12-bit, gamma ~2
  const uint8_t data[2] = {static_cast<uint8_t>(duty & 0xFF),
                           static_cast<uint8_t>(((duty >> 8) & 0x0F) | (duty ? PM1_PWM_ENABLE : 0))};
  freeink::m5pm1::writeBytes(freeink::m5pm1::REG_PWM0_DUTY_L, data, sizeof(data));
}

// Fixed LEDC channels for the Arduino-ESP32 2.x path (3.x keys by GPIO and allocates
// channels itself). Frontlight owns 0 (cool/primary) and 1 (warm); no other SDK LEDC
// user on a frontlight board takes these (the Buzzer uses the 3.x gpio-keyed API).
constexpr uint8_t LEDC_CH_COOL = 0;
constexpr uint8_t LEDC_CH_WARM = 1;

// Apply the board's output polarity to a logical 0..full LED duty.
uint32_t physicalDuty(uint32_t logicalDuty, uint32_t full, bool activeHigh) {
  return activeHigh ? logicalDuty : full - logicalDuty;
}

#ifdef FREEINK_FRONTLIGHT_LS
// Light-sleep-surviving LEDC: clock the timer from RC_FAST (~17.5 MHz on the
// S3 — the practical LEDC source that keeps running through light sleep at
// near-zero extra sleep power; XTAL can also be kept up but costs far more in
// sleep current), mark the
// channels KEEP_ALIVE, and disable the GPIO sleep-isolation override on the
// output pins (a documented gotcha: sleep entry reconfigures the pad and kills
// the PWM even when the clock survives). RC_FAST at 10 kHz supports up to
// 10-bit resolution (17.5 MHz / 10 kHz = 1750 >= 1024), so the board profiles'
// full duty range — including setBrightnessLevel's level-1 minimum step — stays
// expressible. Uses the IDF driver directly (fixed LEDC_TIMER_0 + the channel
// ids below) because the Arduino helpers don't expose sleep_mode; safe here
// because frontlight boards using this flag have no other LEDC consumer.
bool attachChannel(int8_t gpio, uint8_t ch, uint32_t freq, uint8_t bits) {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = static_cast<ledc_timer_bit_t>(bits);
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = freq;
  timer.clk_cfg = LEDC_USE_RC_FAST_CLK;
  if (ledc_timer_config(&timer) != ESP_OK) {
    // freq/bits exceed RC_FAST — leave the light unconfigured rather than
    // silently falling back to a clock that freezes in light sleep.
    return false;
  }
  ledc_channel_config_t chan = {};
  chan.gpio_num = gpio;
  chan.speed_mode = LEDC_LOW_SPEED_MODE;
  chan.channel = static_cast<ledc_channel_t>(ch);
  chan.intr_type = LEDC_INTR_DISABLE;
  chan.timer_sel = LEDC_TIMER_0;
  chan.duty = 0;
  chan.hpoint = 0;
  chan.sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE;
  // ledc_channel_config() disables the pad's sleep-isolation override itself
  // for KEEP_ALIVE channels (IDF 5.5), so no explicit gpio_sleep_sel_dis here.
  return ledc_channel_config(&chan) == ESP_OK;
}
void writeChannel(int8_t /*gpio*/, uint8_t ch, uint32_t duty) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(ch), duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(ch));
}
#elif defined(ARDUINO) && ESP_ARDUINO_VERSION_MAJOR >= 3
bool attachChannel(int8_t gpio, uint8_t /*ch*/, uint32_t freq, uint8_t bits) { return ledcAttach(gpio, freq, bits); }
void writeChannel(int8_t gpio, uint8_t /*ch*/, uint32_t duty) { ledcWrite(gpio, duty); }
#else
bool attachChannel(int8_t gpio, uint8_t ch, uint32_t freq, uint8_t bits) {
  ledcSetup(ch, freq, bits);
  ledcAttachPin(gpio, ch);
  return true;
}
void writeChannel(int8_t /*gpio*/, uint8_t ch, uint32_t duty) { ledcWrite(ch, duty); }
#endif
}  // namespace
#endif

void FrontlightManager::begin() {
#if FREEINK_CAP_FRONTLIGHT
  const auto& fl = BoardConfig::ACTIVE.frontlight;
#if FREEINK_DEVICE_EEGO_A4
  const auto& i2c = BoardConfig::ACTIVE.i2cFrontlight;
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::EegoA4 &&
      i2c.controller == BoardConfig::I2cFrontlightController::Lm3630a) {
    if (i2c.sda < 0 || i2c.scl < 0 || i2c.enable < 0 || i2c.address == 0) return;
    Wire.begin(i2c.sda, i2c.scl, i2c.i2cHz);
    Wire.setTimeOut(256);
    pinMode(i2c.enable, OUTPUT);
    digitalWrite(i2c.enable, LOW);

    // The OEM firmware contains an LM3630A path, but at least one retail EEGO
    // A4 revision has no frontlight hardware populated. Probe with the recovered
    // enable sequence so an absent option stays off and is not exposed as a
    // capability merely because its driver exists in a shared firmware image.
    digitalWrite(i2c.enable, HIGH);
    delay(2);
    Wire.beginTransmission(i2c.address);
    const bool detected = Wire.endTransmission() == 0;
    digitalWrite(i2c.enable, LOW);
    if (!detected) return;

    _begun = true;
    _brightness = 0;
    return;
  }
#endif
  if (fl.viaPm1Pwm) {
    pm1FrontlightAttach(fl.pwmFrequency);
    _begun = true;
    setBrightness(0);
    return;
  }
  if (fl.gpio == BoardConfig::PIN_UNASSIGNED) return;

  bool attachOk = attachChannel(fl.gpio, LEDC_CH_COOL, fl.pwmFrequency, fl.pwmResolutionBits);
  if (fl.gpioWarm != BoardConfig::PIN_UNASSIGNED) {
    attachOk = attachChannel(fl.gpioWarm, LEDC_CH_WARM, fl.pwmFrequency, fl.pwmResolutionBits) || attachOk;
  }
#ifdef FREEINK_FRONTLIGHT_LS
  // Defensive: a prior sleep cycle may have left the pads held (park() latches a
  // digital hold that survives deep sleep AND the wake reset while the _lsParked
  // DRAM flag is lost). Release any surviving hold here so begin() always starts
  // from a clean pad — a held pad silently ignores the LEDC drive below. The
  // release is unconditional (gpio_hold_dis on a non-held pad is a harmless no-op)
  // because we cannot trust _lsParked after a reset.
  for (const int8_t pin : {fl.gpio, fl.gpioWarm}) {
    if (pin >= 0) gpio_hold_dis(static_cast<gpio_num_t>(pin));
  }
  _lsParked = false;
  LOG_INF("FrontlightMgr", "begin: cleared any stale held pads");
  // The FIRST successful KEEP_ALIVE channel config takes a single refcounted +1
  // on the RC_FAST sleep sub-mode (esp_sleep_sub_mode_config; the driver's
  // global-clock latch means later configs don't take another), which would
  // keep RC_FAST — and the digital domain at its higher sleep bias — powered
  // through every light-sleep window from boot, even with the light off.
  // Balance it here and let apply() re-arm only while the light is actually
  // lit. attachOk is true when ANY channel config succeeded (exactly the
  // condition under which the driver's +1 was taken); the !_begun guard keeps a
  // hypothetical second begin() from decrementing twice.
  _lsAttachOk = attachOk;
  _lsKeepAliveArmed = false;
  if (attachOk && !_begun) {
    esp_sleep_sub_mode_config(ESP_SLEEP_DIG_USE_RC_FAST_MODE, false);
  }
#else
  (void)attachOk;
#endif
  _begun = true;
  setBrightness(0);
  LOG_INF("FrontlightMgr", "begin: attached gpio=%d warm=%d ok=%d", fl.gpio, fl.gpioWarm, attachOk ? 1 : 0);
#endif
}

#if FREEINK_CAP_FRONTLIGHT
#if FREEINK_DEVICE_EEGO_A4
bool FrontlightManager::lm3630aWrite(const uint8_t reg, const uint8_t value) {
  const auto& cfg = BoardConfig::ACTIVE.i2cFrontlight;
  Wire.beginTransmission(cfg.address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool FrontlightManager::lm3630aRead(const uint8_t reg, uint8_t& value) {
  const auto& cfg = BoardConfig::ACTIVE.i2cFrontlight;
  Wire.beginTransmission(cfg.address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(cfg.address, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) != 1) return false;
  value = Wire.read();
  return true;
}

bool FrontlightManager::lm3630aUpdate(const uint8_t reg, const uint8_t mask, const uint8_t value) {
  uint8_t current = 0;
  if (!lm3630aRead(reg, current)) return false;
  return lm3630aWrite(reg, static_cast<uint8_t>((current & ~mask) | (value & mask)));
}

bool FrontlightManager::configureLm3630a() {
  const auto& cfg = BoardConfig::ACTIVE.i2cFrontlight;
  digitalWrite(cfg.enable, HIGH);
  delay(2);
  Wire.beginTransmission(cfg.address);
  if (Wire.endTransmission() != 0) {
    digitalWrite(cfg.enable, LOW);
    return false;
  }

  // Exact EEGO A4 sequence recovered from HalBacklight/LM3630A in CrossLink.
  // Register meanings follow TI SNVS974B: filter, config, boost, max-current A/B,
  // control, then the two brightness banks.
  bool ok = lm3630aWrite(0x50, 0x03);
  ok = lm3630aUpdate(0x01, 0x07, 0x00) && ok;
  ok = lm3630aWrite(0x02, 0x38) && ok;
  ok = lm3630aUpdate(0x05, 0x1f, 0x10) && ok;
  ok = lm3630aUpdate(0x06, 0x1f, 0x10) && ok;
  ok = lm3630aUpdate(0x00, 0x14, 0x00) && ok;
  ok = lm3630aUpdate(0x00, 0x0b, 0x00) && ok;
  delay(2);
  ok = lm3630aWrite(0x03, 0x00) && ok;
  ok = lm3630aWrite(0x04, 0x00) && ok;
  _i2cConfigured = ok;
  if (!ok) digitalWrite(cfg.enable, LOW);
  return ok;
}

void FrontlightManager::applyLm3630a() {
  const auto& cfg = BoardConfig::ACTIVE.i2cFrontlight;
  if (_brightness == 0) {
    if (_i2cConfigured) {
      lm3630aWrite(0x03, 0);
      lm3630aWrite(0x04, 0);
    }
    digitalWrite(cfg.enable, LOW);
    _i2cConfigured = false;
    return;
  }
  if (!_i2cConfigured && !configureLm3630a()) return;

  const uint8_t level = static_cast<uint16_t>(_brightness) * 255 / 100;
  uint8_t warm = static_cast<uint16_t>(level) * _warmPercent / 100;
  uint8_t cool = static_cast<uint16_t>(level) * (100 - _warmPercent) / 100;
  // The IC ignores brightness codes 1..3; preserve a visible nonzero request.
  if (warm != 0 && warm < 4) warm = 4;
  if (cool != 0 && cool < 4) cool = 4;

  lm3630aUpdate(0x00, 0x80, 0x00);  // leave software sleep
  delay(2);
  lm3630aWrite(0x03, warm);
  lm3630aWrite(0x04, cool);
  lm3630aUpdate(0x00, 0x04, warm >= 4 ? 0x04 : 0x00);
  lm3630aUpdate(0x00, 0x02, cool >= 4 ? 0x02 : 0x00);
}
#endif

void FrontlightManager::apply() {
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  if (!_begun) return;
#if FREEINK_DEVICE_EEGO_A4
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::EegoA4 &&
      BoardConfig::ACTIVE.i2cFrontlight.controller == BoardConfig::I2cFrontlightController::Lm3630a) {
    applyLm3630a();
    return;
  }
#endif
  if (fl.viaPm1Pwm) {
    pm1FrontlightWrite(_brightness);
    return;
  }
  if (fl.gpio == BoardConfig::PIN_UNASSIGNED) return;

  const uint32_t full = maxDuty(fl.pwmResolutionBits);
  const bool dual = fl.gpioWarm != BoardConfig::PIN_UNASSIGNED;

  // Convert brightness to PWM precision BEFORE splitting it between channels.
  // Splitting integer percentages first loses both fractional parts at low
  // levels: brightness=1, warmth=50 previously became cool=0% + warm=0%.
  // Splitting the total duty also keeps cool+warm equal to the requested total.
  uint32_t totalDuty = 0;
  if (_useLevel && _brightnessLevel > 0) {
    const uint32_t n = static_cast<uint32_t>(_brightnessLevel - 1u);
    totalDuty = 1u + (n * n * (full - 1u)) / (254u * 254u);
  } else if (!_useLevel) {
    totalDuty = perceptualDuty(_brightness, full);
  }
  uint32_t warmDuty = 0;
  uint32_t coolDuty = totalDuty;
  if (dual) {
    warmDuty = (totalDuty * _warmPercent + 50u) / 100u;
    coolDuty = totalDuty - warmDuty;
  }
#ifdef FREEINK_FRONTLIGHT_LS
  updateLsKeepAlive(totalDuty != 0);
#endif
  writeChannel(fl.gpio, LEDC_CH_COOL, physicalDuty(coolDuty, full, fl.activeHigh));

  if (dual) {
    writeChannel(fl.gpioWarm, LEDC_CH_WARM, physicalDuty(warmDuty, full, fl.activeHigh));
  }
  LOG_INF("FrontlightMgr", "apply: brightness=%u level=%u totalDuty=%u coolDuty=%u warmDuty=%u lit=%d", _brightness,
           _brightnessLevel, totalDuty, coolDuty, warmDuty, totalDuty != 0 ? 1 : 0);
}

#ifdef FREEINK_FRONTLIGHT_LS
void FrontlightManager::updateLsKeepAlive(const bool lit) {
  // Refcounted, so strictly transition-edged: one +1 while lit, returned at 0.
  // Skipped when the attach failed (see begin()) — the driver never took its
  // +1 there, and RC_FAST keep-alive is moot without a working LS channel.
  if (!_lsAttachOk || lit == _lsKeepAliveArmed) return;
  esp_sleep_sub_mode_config(ESP_SLEEP_DIG_USE_RC_FAST_MODE, lit);
  _lsKeepAliveArmed = lit;
}

void FrontlightManager::park() {
  // Frontlight leakage through deep sleep (Xteink X4 Pro — Mark31415,
  // crosspoint-reader#3215). The channels are configured LEDC_SLEEP_MODE_KEEP_ALIVE
  // so the PWM keeps driving GPIO8/9 (cool/warm) through light sleep; at deep
  // sleep the panel rail is held up (PR #3215 holds power.latch0 / GPIO1 HIGH for
  // fast-wake), so the frontlight driver IC stays powered and the KEEP_ALIVE pad
  // keeps drawing quiescent + leakage current. Cut it at the source: drive both
  // pads LOW (active-high frontlight -> LED off, no booster bias) and hold them
  // LOW so the level survives deep sleep via gpio_deep_sleep_hold_en() (called by
  // PowerManager::deepSleep()). The LEDC peripheral clock (RC_FAST) is also
  // released so the driver's refcounted +1 is dropped and the clock can fully
  // stop in deep sleep. releaseOnWake() must undo this before begin() re-attaches
  // the LEDC channels on boot.
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  if (!_begun) return;
  LOG_INF("FrontlightMgr", "park: begun, driving pads LOW + hold");
  // Return the LEDC driver's refcounted RC_FAST keep-alive it took at attach, if
  // it is still armed (apply() re-arms only while lit; off()/setBrightness(0)
  // returns it, but be safe if the light was parked while lit).
  updateLsKeepAlive(false);
  // Tear down the KEEP_ALIVE LEDC channels so the pads no longer answer to the
  // peripheral; the explicit GPIO hold below then owns the pad level.
  ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
  if (fl.gpioWarm != BoardConfig::PIN_UNASSIGNED) {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
  }
  for (const int8_t pin : {fl.gpio, fl.gpioWarm}) {
    if (pin < 0) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the drive
    // below (same trap as HalPowerManager's latch loop).
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    gpio_hold_en(g);
    _lsParked = true;
  }
}

void FrontlightManager::releaseOnWake() {
  // Undo park() so begin() can re-attach the LEDC channels cleanly: drop the pad
  // holds (a held pad would make ledc_channel_config()'s drive a no-op) and clear
  // the parked flag. Called from the consumer at boot, before Frontlight.begin().
  //
  // CRITICAL: the release must be UNCONDITIONAL. park() latches a digital pad hold
  // (gpio_hold_en) that survives deep sleep AND the wake reset, but _lsParked is a
  // plain DRAM flag that is lost on the same reset. After a wake, _lsParked is
  // always false even though the pad is still held — gating the release on it would
  // leave the pad held forever (light dark until power-cycle). gpio_hold_dis on a
  // non-held pad is a harmless no-op, so releasing unconditionally is safe and
  // idempotent. Every other driver in this codebase releases holds unconditionally
  // before driving for exactly this reason.
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  for (const int8_t pin : {fl.gpio, fl.gpioWarm}) {
    if (pin < 0) continue;
    gpio_hold_dis(static_cast<gpio_num_t>(pin));
  }
  _lsParked = false;
  LOG_INF("FrontlightMgr", "releaseOnWake: cleared pad holds (unconditional)");
}
#endif
#endif

void FrontlightManager::setBrightness(uint8_t percent) {
#if FREEINK_CAP_FRONTLIGHT
  if (percent > 100) percent = 100;
  _brightness = percent;
  _brightnessLevel = (static_cast<uint16_t>(percent) * 255u) / 100u;
  _useLevel = false;
  if (percent > 0) _lastBrightness = percent;
  apply();
#else
  (void)percent;
#endif
}

void FrontlightManager::setBrightnessLevel(uint8_t level) {
#if FREEINK_CAP_FRONTLIGHT
  _brightnessLevel = level;
  _brightness = (static_cast<uint16_t>(level) * 100u) / 255u;
  _useLevel = true;
  apply();
#else
  (void)level;
#endif
}

void FrontlightManager::off() { setBrightness(0); }
void FrontlightManager::on() { setBrightness(_lastBrightness); }

void FrontlightManager::setColorTemperature(uint8_t warmPercent) {
#if FREEINK_CAP_FRONTLIGHT
  _warmPercent = warmPercent > 100 ? 100 : warmPercent;
  // Only re-drives hardware when a warm channel exists; on single-channel boards this just
  // records the request (apply() ignores _warmPercent without a second channel).
  apply();
#else
  (void)warmPercent;
#endif
}
