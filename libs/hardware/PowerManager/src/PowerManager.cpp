#include "PowerManager.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/soc_caps.h>
#if SOC_PM_SUPPORT_EXT1_WAKEUP
#include <driver/rtc_io.h>
#endif
#if FREEINK_DEVICE_WS397
#include <Axp2101.h>
#endif

namespace freeink {
namespace {
int8_t powerPin() { return BoardConfig::ACTIVE.input.power; }
bool powerActiveHigh() { return BoardConfig::ACTIVE.input.powerActiveHigh; }

// Records a deepSleep() abort (see deepSleep()) across the esp_restart() that
// follows it, so the next boot can report it via takeAbortedSleepInfo().
// RTC_NOINIT_ATTR survives esp_restart() (a warm reset, same RTC-memory domain)
// but is garbage on a cold boot -- magic-guarded like main.cpp's silentRebootMagic.
constexpr uint32_t ABORTED_SLEEP_MAGIC = 0x41424f52;  // 'ABOR'

struct AbortedSleepRecord {
  bool aborted;
  int wakeupCause;
  int wakePinLevel;
};
}  // namespace

RTC_NOINIT_ATTR uint32_t abortedSleepMagic;
RTC_NOINIT_ATTR AbortedSleepRecord abortedSleepRecord;

void PowerManager::armWakeOnPins(uint64_t gpioMask, bool wakeLow) {
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  // Xtensa (S3/S2, classic ESP32): RTC ext1. Pins must be RTC GPIOs.
  //
  // The classic ESP32 RTC has no "any low" mode — only ESP_EXT1_WAKEUP_ALL_LOW
  // ("wake when ALL selected pins are low"). For a single wake pin (the common
  // power-button case) ALL_LOW and ANY_LOW are identical; a multi-pin low wake on
  // classic ESP32 fires only when every pin is low. S2/S3 expose ANY_LOW directly.
#if defined(CONFIG_IDF_TARGET_ESP32)
  const esp_sleep_ext1_wakeup_mode_t lowMode = ESP_EXT1_WAKEUP_ALL_LOW;
#else
  const esp_sleep_ext1_wakeup_mode_t lowMode = ESP_EXT1_WAKEUP_ANY_LOW;
#endif
  esp_sleep_enable_ext1_wakeup(gpioMask, wakeLow ? lowMode : ESP_EXT1_WAKEUP_ANY_HIGH);
  // ext1 hands each pad to the RTC domain for sleep, where the digital pull from
  // pinMode() does not apply and esp_sleep_config_gpio_isolate() floats the pad.
  // Set the idle-level pull in the RTC domain and keep that domain powered so it
  // holds; a pin without an external resistor otherwise floats and wakes at random.
#if SOC_PM_SUPPORT_RTC_PERIPH_PD
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#endif
  for (int pin = 0; pin < 64; ++pin) {
    if (((gpioMask >> pin) & 1) == 0) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    if (wakeLow) {
      rtc_gpio_pulldown_dis(g);
      rtc_gpio_pullup_en(g);
    } else {
      rtc_gpio_pullup_dis(g);
      rtc_gpio_pulldown_en(g);
    }
  }
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
  // RISC-V (C3/C6/H2): the deep-sleep "gpio" wakeup source.
  esp_deep_sleep_enable_gpio_wakeup(gpioMask, wakeLow ? ESP_GPIO_WAKEUP_GPIO_LOW : ESP_GPIO_WAKEUP_GPIO_HIGH);
#else
#error "FreeInk PowerManager: target has no supported deep-sleep GPIO wakeup source"
#endif
}

bool PowerManager::armPowerButtonWakeup() {
  const int8_t pin = powerPin();
  if (pin < 0) return false;
  const bool activeHigh = powerActiveHigh();

  // Hold the idle level with the opposite pull so the line is defined in sleep.
  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  armWakeOnPins(1ULL << pin, /*wakeLow=*/!activeHigh);
  return true;
}

void PowerManager::waitForPowerButtonRelease() {
  const int8_t pin = powerPin();
  if (pin < 0) return;
  const bool activeHigh = powerActiveHigh();

  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  const int pressedLevel = activeHigh ? HIGH : LOW;
  while (digitalRead(pin) == pressedLevel) {
    delay(50);
  }
  // The wake source is level-triggered and armed right after this returns; let
  // the contacts settle so release bounce cannot wake the board in hand.
  delay(50);
}

namespace {
// Drive a rail-enable pin to `offLevel` and latch it so the level survives deep
// sleep (requires gpio_deep_sleep_hold_en(), done in deepSleep()). gpio_hold_dis
// first: a hold left over from a previous cycle would make the writes no-ops.
void holdRailOff(int8_t pin, uint8_t offLevel) {
  if (pin < 0) return;
  const auto g = static_cast<gpio_num_t>(pin);
  gpio_hold_dis(g);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, offLevel);
  gpio_hold_en(g);
}
}  // namespace

void PowerManager::powerDownRailsForSleep() {
  const auto& b = BoardConfig::ACTIVE;
#if FREEINK_DEVICE_METALIO_EINK4
  if (BoardConfig::isMetalioEInk4()) {
    metalio::setAmplifier(false);
    digitalWrite(metalio::VIBRATION_GPIO, LOW);
    // Keep the shared display/SD rail up, as required by the vendor shutdown.
  }
#endif
#if FREEINK_DEVICE_WS397
  // The EPD rail is an AXP2101 LDO, not a GPIO, so holdRailOff() below cannot
  // reach it — drop it here or the panel stays powered all through deep sleep.
  axp2101::setEpdPower(false);
#endif
  // Keep RESET defined through deep sleep, but never drive an unpowered panel's
  // input HIGH: on boards with a gated EPD rail (Sticky), that can back-power the
  // controller through its RESET protection diode and turn sleep into a
  // milliamp-level drain. Hold RESET LOW alongside a switched-off rail. Boards
  // whose panel rail remains powered (X4 Pro) keep RESET HIGH so a UC8179 cannot
  // drift out of DSLP and restart its analog booster. EpdBus and XteinkDetect
  // release the hold before issuing a reset pulse on wake.
#if FREEINK_DEVICE_PICCO
  // Park the SD bus LOW before the GPIO0 rail drops, as stock does
  // (FUN_420098f0), so the bus cannot back-feed the unpowered card.
  // SdmmcBlockDevice::begin() releases the holds on wake.
  for (const int8_t pin : {b.sdmmc.clk, b.sdmmc.cmd, b.sdmmc.d0, b.sdmmc.d1, b.sdmmc.d2, b.sdmmc.d3}) {
    holdRailOff(pin, LOW);
  }
#endif
#if FREEINK_DEVICE_PICCO
  // Picco's panel rail is the gated GPIO0 (carried as sd.powerEnable, not
  // display.powerEnable), so the panel IS unpowered in deep sleep — hold RESET
  // LOW. The heuristic below would otherwise pick HIGH (no display.powerEnable)
  // and back-power the off SSD1677 through its RESET diode — a mA-level deep-sleep
  // drain. Stock never holds the display pins (they go high-Z); LOW is the SDK's
  // gated-rail equivalent, and the wake path (piccoReadId) releases the hold
  // before the reset pulse.
  const uint8_t resetSleepLevel = LOW;
#else
  const uint8_t resetSleepLevel = b.display.powerEnable >= 0 ? LOW : HIGH;
#endif
  holdRailOff(b.display.rst, resetSleepLevel);
  holdRailOff(b.display.powerEnable, LOW);
  // SD enable OFF = the inactive level: LOW for active-high enables, HIGH for the
  // active-low ones (e.g. X4 Pro's GPIO5, which powers the card while held LOW).
  holdRailOff(b.sd.powerEnable, b.sd.powerActiveHigh ? LOW : HIGH);
  holdRailOff(b.touch.powerEnable, b.touch.powerEnableActiveHigh ? LOW : HIGH);
  // The mic enable also carries a polarity flag; OFF is the inactive level.
  holdRailOff(b.mic.enable, b.mic.enableActiveHigh ? LOW : HIGH);
}

void PowerManager::deepSleep() {
  esp_sleep_config_gpio_isolate();
#if !FREEINK_MCU_C61
  gpio_deep_sleep_hold_en();
#endif
  esp_deep_sleep_start();
  // esp_deep_sleep_start() does not return in normal operation -- reaching
  // this line means sleep entry was rejected/aborted by the SoC rather than
  // the next boot being a fresh wake. A busy-loop here draws full-clock
  // current (~20-30mA) indefinitely and never responds to the power button,
  // making an aborted sleep entry indistinguishable from a real drain bug.
  // Record the abort (RTC_NOINIT_ATTR survives the reset below) so the next
  // boot can report it via takeAbortedSleepInfo(), then reset for real via
  // esp_restart() -- itself [[noreturn]], satisfying this function's contract.
  const int8_t pin = powerPin();
  abortedSleepRecord.aborted = true;
  abortedSleepRecord.wakeupCause = static_cast<int>(esp_sleep_get_wakeup_cause());
  abortedSleepRecord.wakePinLevel = pin >= 0 ? digitalRead(pin) : -1;
  abortedSleepMagic = ABORTED_SLEEP_MAGIC;
  esp_restart();
}

void PowerManager::deepSleepUntilPowerButton() {
  waitForPowerButtonRelease();
  armPowerButtonWakeup();
  deepSleep();
}

PowerManager::AbortedSleepInfo PowerManager::takeAbortedSleepInfo() {
  AbortedSleepInfo info;
  if (abortedSleepMagic == ABORTED_SLEEP_MAGIC && abortedSleepRecord.aborted) {
    info.aborted = true;
    info.wakeupCause = abortedSleepRecord.wakeupCause;
    info.wakePinLevel = abortedSleepRecord.wakePinLevel;
  }
  // Clear so a stale record doesn't get reported again on a later boot, and
  // re-stamp the magic so an uninitialized cold-boot read still reports false
  // rather than following a garbage `aborted` bit.
  abortedSleepRecord.aborted = false;
  abortedSleepMagic = ABORTED_SLEEP_MAGIC;
  return info;
}

}  // namespace freeink
