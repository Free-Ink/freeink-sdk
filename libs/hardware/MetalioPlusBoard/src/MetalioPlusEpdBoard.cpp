#include "MetalioPlusEpdBoard.h"

#include <Arduino.h>
#include <I2cBusLock.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

extern "C" {
#include <epd_display.h>
#include <epdiy.h>
#include <output_lcd/lcd_driver.h>
}

namespace freeink {
namespace metalioplus {
namespace {

constexpr const char* TAG = "metalioplus";

// TPS65185 registers.
constexpr uint8_t TPS_ENABLE = 0x01;
constexpr uint8_t TPS_VCOM1 = 0x03;
constexpr uint8_t TPS_VCOM2 = 0x04;
constexpr uint8_t TPS_UPSEQ0 = 0x09;
constexpr uint8_t TPS_UPSEQ1 = 0x0A;
constexpr uint8_t TPS_PG = 0x0F;
constexpr uint8_t TPS_REVID = 0x10;
constexpr uint8_t TPS_ENABLE_ALL_RAILS = 0x3F;
constexpr uint8_t TPS_PG_ALL_GOOD = 0xFA;
// Power-up order and delays used by the vendor firmware.
constexpr uint8_t TPS_UPSEQ0_VALUE = 0xE4;
constexpr uint8_t TPS_UPSEQ1_VALUE = 0x55;

constexpr uint16_t TPS_LINES = (1u << PIN_TPS_VCOM_CTRL) | (1u << PIN_TPS_PWRUP) | (1u << PIN_TPS_WAKEUP);

// Parallel bus, board rev 1.3/1.4.
constexpr gpio_num_t PIN_DATA[8] = {GPIO_NUM_8,  GPIO_NUM_9,  GPIO_NUM_10, GPIO_NUM_11,
                                    GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15};
constexpr gpio_num_t PIN_XCL = GPIO_NUM_16;
constexpr gpio_num_t PIN_XLE = GPIO_NUM_17;
constexpr gpio_num_t PIN_XOE = GPIO_NUM_18;
constexpr gpio_num_t PIN_XSTL = GPIO_NUM_4;
constexpr gpio_num_t PIN_MODE = GPIO_NUM_19;
constexpr gpio_num_t PIN_SPV = GPIO_NUM_35;
constexpr gpio_num_t PIN_BORDER = GPIO_NUM_37;
constexpr gpio_num_t PIN_CKV = GPIO_NUM_40;

constexpr gpio_num_t BUS_PINS[] = {PIN_DATA[0], PIN_DATA[1], PIN_DATA[2], PIN_DATA[3], PIN_DATA[4], PIN_DATA[5],
                                   PIN_DATA[6], PIN_DATA[7], PIN_XCL,     PIN_XLE,     PIN_XOE,     PIN_XSTL,
                                   PIN_SPV,     PIN_CKV,     PIN_MODE,    PIN_BORDER};

bool expanderOk = false;
bool tpsOk = false;
int vcomMv = DEFAULT_VCOM_MV;
// Requested TPS65185 control lines; written by boardSetCtrl().
bool tpsWakeup = false;
bool tpsPwrup = false;
bool tpsVcomCtrl = false;

bool tpsWrite(uint8_t reg, uint8_t value) {
  const I2cBusLock lock;
  Wire.beginTransmission(TPS65185_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool tpsRead(uint8_t reg, uint8_t& value) {
  const I2cBusLock lock;
  Wire.beginTransmission(TPS65185_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TPS65185_ADDR, static_cast<uint8_t>(1)) != 1) return false;
  value = Wire.read();
  return true;
}

// EPDiy calls set_ctrl around every frame; setOutputs() skips unchanged lines.
// Retried because a lost write while powering down would leave VCOM on the glass.
void writeTpsLines() {
  if (!expanderOk) return;
  const uint16_t value = (tpsWakeup ? 1u << PIN_TPS_WAKEUP : 0) | (tpsPwrup ? 1u << PIN_TPS_PWRUP : 0) |
                         (tpsVcomCtrl ? 1u << PIN_TPS_VCOM_CTRL : 0);
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (setOutputs(TPS_LINES, value)) return;
    delay(2);
  }
  ESP_LOGE(TAG, "TPS65185 control lines not written");
}

bool waitPowerGoodPin(uint32_t timeoutMs) {
  const uint32_t start = millis();
  do {
    uint16_t inputs = 0;
    if (readInputs(inputs) && (inputs & (1u << PIN_TPS_PWR_GOOD))) return true;
    delay(1);
  } while (millis() - start < timeoutMs);
  return false;
}

bool waitRailsGood(uint32_t timeoutMs) {
  const uint32_t start = millis();
  do {
    uint8_t pg = 0;
    if (tpsRead(TPS_PG, pg) && (pg & TPS_PG_ALL_GOOD) == TPS_PG_ALL_GOOD) return true;
    delay(1);
  } while (millis() - start < timeoutMs);
  return false;
}

void driveLow(gpio_num_t pin) {
  gpio_reset_pin(pin);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_set_level(pin, 0);
}

void startLcdBus() {
  const EpdDisplay_t* display = epd_get_display();
  lcd_bus_config_t bus = {};
  // 8-bit panel: the unused upper lanes repeat D7, as in the vendor firmware.
  for (int i = 0; i < 16; ++i) bus.data[i] = PIN_DATA[i < 8 ? i : 7];
  bus.clock = PIN_XCL;
  bus.ckv = PIN_CKV;
  bus.start_pulse = PIN_XSTL;
  bus.leh = PIN_XLE;
  bus.stv = PIN_SPV;
  // Timing from the vendor firmware; dummy clocks per E0470A14-AF section 6.3.1.
  const LcdEpdConfig_t config = {
      .pixel_clock = static_cast<size_t>(display->bus_speed) * 1000 * 1000,
      .ckv_high_time = 60,
      .line_front_porch = 4,
      .le_high_time = 9,
      .bus_width = display->bus_width,
      .bus = bus,
      .dummy_clocks_after_data = 10,
      .white_border_pixels = 0,
  };
  epd_lcd_init(&config, display->width, display->height);
}

void boardInit(uint32_t rowWidth) {
  (void)rowWidth;
  // Holds latched for deep sleep survive a CPU reset and would pin the bus low.
  for (gpio_num_t pin : BUS_PINS) gpio_hold_dis(pin);

  // Normally already done by BoardConfig::holdPowerRails() early in setup().
  expanderOk = ensureBooted(2000);
  if (!expanderOk) ESP_LOGE(TAG, "TCA9555 not found at 0x%02X; panel power unavailable", EXPANDER_ADDR);

  driveLow(PIN_XOE);
  driveLow(PIN_MODE);
  driveLow(PIN_BORDER);
  startLcdBus();
}

void boardSetCtrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
  if (mask->ep_output_enable) gpio_set_level(PIN_XOE, state->ep_output_enable ? 1 : 0);
  if (mask->ep_mode) gpio_set_level(PIN_MODE, state->ep_mode ? 1 : 0);
  if (mask->ep_output_enable || mask->ep_mode || mask->ep_stv) writeTpsLines();
}

void boardPowerOff(epd_ctrl_state_t* state) {
  const epd_ctrl_state_t mask = {.ep_output_enable = true, .ep_mode = true, .ep_stv = true};
  state->ep_stv = false;
  state->ep_output_enable = false;
  state->ep_mode = false;
  boardSetCtrl(state, &mask);
  esp_rom_delay_us(12);  // E0470A14-AF 6.1: XOE low at least 12 us before the rails drop
  tpsVcomCtrl = false;
  tpsPwrup = false;
  boardSetCtrl(state, &mask);
  delay(1);
  tpsWakeup = false;
  boardSetCtrl(state, &mask);
  tpsOk = false;
}

bool powerOnOnce(epd_ctrl_state_t* state) {
  const epd_ctrl_state_t mask = {.ep_output_enable = true, .ep_mode = true, .ep_stv = true};
  gpio_set_level(PIN_BORDER, 0);
  state->ep_output_enable = false;
  state->ep_stv = true;
  state->ep_mode = false;

  // WAKEUP high with PWRUP low puts the TPS65185 in standby, where I2C works.
  tpsWakeup = true;
  tpsPwrup = false;
  tpsVcomCtrl = false;
  boardSetCtrl(state, &mask);
  delay(50);

  uint8_t revision = 0;
  if (!tpsRead(TPS_REVID, revision)) {
    ESP_LOGE(TAG, "TPS65185 does not answer at 0x%02X", TPS65185_ADDR);
    return false;
  }
  if (!tpsWrite(TPS_UPSEQ0, TPS_UPSEQ0_VALUE) || !tpsWrite(TPS_UPSEQ1, TPS_UPSEQ1_VALUE)) {
    ESP_LOGE(TAG, "TPS65185 power-up sequence write failed");
    return false;
  }

  // The rising edge of PWRUP starts the boost converters.
  tpsPwrup = true;
  tpsVcomCtrl = true;
  boardSetCtrl(state, &mask);
  if (!waitPowerGoodPin(200)) {
    ESP_LOGE(TAG, "TPS65185 PWR_GOOD did not rise (REVID 0x%02X)", revision);
    return false;
  }

  const unsigned vcomCode = static_cast<unsigned>(vcomMv) / 10;  // 10 mV steps
  if (!tpsWrite(TPS_ENABLE, TPS_ENABLE_ALL_RAILS) || !tpsWrite(TPS_VCOM2, (vcomCode >> 8) & 0x01) ||
      !tpsWrite(TPS_VCOM1, vcomCode & 0xFF)) {
    ESP_LOGE(TAG, "TPS65185 enable/VCOM write failed");
    return false;
  }

  state->ep_sth = true;
  const epd_ctrl_state_t sthMask = {.ep_sth = true};
  boardSetCtrl(state, &sthMask);
  if (!waitRailsGood(200)) {
    ESP_LOGE(TAG, "TPS65185 rails not good");
    return false;
  }

  state->ep_output_enable = true;
  const epd_ctrl_state_t oeMask = {.ep_output_enable = true};
  boardSetCtrl(state, &oeMask);
  return true;
}

void boardPowerOn(epd_ctrl_state_t* state) {
  tpsOk = false;
  if (!expanderOk) expanderOk = ensureBooted();  // it may have answered since boot
  if (!expanderOk) return;
  // One attempt plus three full power-cycle retries, as in the vendor firmware.
  for (int attempt = 0; attempt <= 3; ++attempt) {
    if (powerOnOnce(state)) {
      tpsOk = true;
      return;
    }
    boardPowerOff(state);
    delay(100);
  }
  ESP_LOGE(TAG, "panel power-on failed after retries");
}

void boardDeinit() {
  gpio_set_level(PIN_XOE, 0);
  gpio_set_level(PIN_MODE, 0);
  epd_lcd_deinit();
  tpsVcomCtrl = false;
  tpsPwrup = false;
  tpsWakeup = false;
  writeTpsLines();
  tpsOk = false;
}

void boardMeasureVcom(epd_ctrl_state_t* state) { (void)state; }

float boardTemperature() { return 25.0f; }  // the vendor firmware also assumes 25 C

void boardSetVcom(int value) { vcomMv = value; }

const EpdBoardDefinition BOARD = {
    .init = boardInit,
    .deinit = boardDeinit,
    .set_ctrl = boardSetCtrl,
    .poweron = boardPowerOn,
    .measure_vcom = boardMeasureVcom,
    .poweroff = boardPowerOff,
    .set_vcom = boardSetVcom,
    .get_temperature = boardTemperature,
    .gpio_set_direction = nullptr,
    .gpio_read = nullptr,
    .gpio_write = nullptr,
};

}  // namespace

const EpdBoardDefinition& epdBoard() { return BOARD; }

bool panelPowerGood() { return tpsOk; }

}  // namespace metalioplus
}  // namespace freeink
