// EPDiy board definition and FreeInk EpdiyConfig for the Metalio E-Ink4-Plus.
//
// Panel power is a TPS65185 whose WAKEUP / PWRUP / VCOM_CTRL inputs and
// PWR_GOOD output sit on the TCA9555 (metalio_plus helpers in BoardConfig). The
// sequence, bus pins and line timing follow the vendor firmware
// (CloudZao/Metalio-E-INK4-Plus, display/epd_board_metalio_eink4_plus.c).

#include <BoardConfig.h>

#if FREEINK_DEVICE_METALIO_EINK4_PLUS

#include <EpdiyConfig.h>
#include <driver/gpio.h>
#include <epd_board.h>
#include <epdiy.h>

extern "C" {
#include "output_lcd/lcd_driver.h"  // EPDiy C header without C++ guards
}
#include "waveform/my_waveform.h"

namespace freeink {
namespace {

namespace mp = metalio_plus;

constexpr gpio_num_t PIN_XCL = GPIO_NUM_16;
constexpr gpio_num_t PIN_XLE = GPIO_NUM_17;
constexpr gpio_num_t PIN_XOE = GPIO_NUM_18;
constexpr gpio_num_t PIN_MODE = GPIO_NUM_19;  // hardware rev 1.4
constexpr gpio_num_t PIN_XSTL = GPIO_NUM_4;   // hardware rev 1.3/1.4
constexpr gpio_num_t PIN_SPV = GPIO_NUM_35;
constexpr gpio_num_t PIN_BORDER = GPIO_NUM_37;
constexpr gpio_num_t PIN_CKV = GPIO_NUM_40;
constexpr gpio_num_t PIN_DATA[8] = {GPIO_NUM_8,  GPIO_NUM_9,  GPIO_NUM_10, GPIO_NUM_11,
                                    GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15};

constexpr uint8_t TPS_REG_ENABLE = 0x01;
constexpr uint8_t TPS_REG_VCOM1 = 0x03;
constexpr uint8_t TPS_REG_VCOM2 = 0x04;
constexpr uint8_t TPS_REG_UPSEQ0 = 0x09;
constexpr uint8_t TPS_REG_UPSEQ1 = 0x0A;
constexpr uint8_t TPS_REG_PG = 0x0F;
constexpr uint8_t TPS_REG_REVID = 0x10;

constexpr uint16_t TPS_LINES =
    (1u << mp::PIN_TPS_WAKEUP) | (1u << mp::PIN_TPS_PWRUP) | (1u << mp::PIN_TPS_VCOM_CTRL);

int gVcomMv = 1200;  // vendor EPD_VCOM_MV
bool gPowered = false;

void outputLow(gpio_num_t pin) {
  gpio_hold_dis(pin);
  gpio_reset_pin(pin);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_set_level(pin, 0);
}

void boardInit(uint32_t epdRowWidth, const EpdInitConfig* initConfig) {
  (void)epdRowWidth;
  (void)initConfig;
  mp::ensureBooted(3000);
  mp::setOutputs(TPS_LINES, 0);
  // A previous deep sleep may have left the bus pins held.
  for (const gpio_num_t pin : PIN_DATA) gpio_hold_dis(pin);
  for (const gpio_num_t pin : {PIN_XCL, PIN_XLE, PIN_XSTL, PIN_SPV, PIN_CKV}) gpio_hold_dis(pin);
  outputLow(PIN_XOE);
  outputLow(PIN_MODE);
  outputLow(PIN_BORDER);

  const EpdDisplay_t* display = epd_get_display();
  LcdEpdConfig_t config = {};
  config.pixel_clock = static_cast<size_t>(display->bus_speed) * 1000 * 1000;
  config.ckv_high_time = 60;
  config.line_front_porch = 4;
  config.le_high_time = 9;
  config.bus_width = display->bus_width;
  config.bus.clock = PIN_XCL;
  config.bus.ckv = PIN_CKV;
  config.bus.leh = PIN_XLE;
  config.bus.start_pulse = PIN_XSTL;
  config.bus.stv = PIN_SPV;
  for (int i = 0; i < 16; ++i) config.bus.data[i] = PIN_DATA[i < 8 ? i : 7];
  config.dummy_clocks_after_data = 10;  // E0470A14-AF datasheet section 6.3.1 (vendor note)
  config.white_border_pixels = 0;
  epd_lcd_init(&config, display->width, display->height);
}

void boardDeinit() {
  gpio_set_level(PIN_XOE, 0);
  gpio_set_level(PIN_MODE, 0);
  epd_lcd_deinit();
  mp::setOutputs(TPS_LINES, 0);
  gPowered = false;
}

void boardSetCtrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
  if (mask->ep_output_enable) gpio_set_level(PIN_XOE, state->ep_output_enable ? 1 : 0);
  if (mask->ep_mode) gpio_set_level(PIN_MODE, state->ep_mode ? 1 : 0);
}

void boardPowerOff(epd_ctrl_state_t* state) {
  epd_ctrl_state_t mask = {};
  mask.ep_stv = true;
  mask.ep_output_enable = true;
  mask.ep_mode = true;
  state->ep_stv = false;
  state->ep_output_enable = false;
  state->ep_mode = false;
  boardSetCtrl(state, &mask);
  delayMicroseconds(12);  // XOE low >= 12 us before VPOS/VNEG off (vendor, panel spec 6.1)
  mp::setOutputs((1u << mp::PIN_TPS_PWRUP) | (1u << mp::PIN_TPS_VCOM_CTRL), 0);
  delay(1);
  mp::setOutputs(1u << mp::PIN_TPS_WAKEUP, 0);
  gPowered = false;
}

bool powerOnOnce(epd_ctrl_state_t* state) {
  gpio_set_level(PIN_BORDER, 0);
  epd_ctrl_state_t mask = {};
  mask.ep_output_enable = true;
  mask.ep_mode = true;
  mask.ep_stv = true;
  state->ep_output_enable = false;
  state->ep_stv = true;
  state->ep_mode = false;
  boardSetCtrl(state, &mask);

  // WAKEUP high, PWRUP low: STANDBY, registers reachable.
  if (!mp::setOutputs(TPS_LINES, 1u << mp::PIN_TPS_WAKEUP)) return false;
  delay(50);
  uint8_t revid = 0;
  if (!mp::tpsProbe() || !mp::tpsRead(TPS_REG_REVID, revid)) return false;
  if (!mp::tpsWrite(TPS_REG_UPSEQ0, 0xE4) || !mp::tpsWrite(TPS_REG_UPSEQ1, 0x55)) return false;

  // PWRUP rising edge starts the boost; wait for PWR_GOOD on P0.7.
  if (!mp::setOutputs(TPS_LINES, TPS_LINES)) return false;
  delay(50);
  bool good = false;
  for (int ms = 0; ms < 200 && !good; ++ms) {
    good = mp::tpsPowerGood();
    if (!good) delay(1);
  }
  if (!good) return false;

  const unsigned vcom = static_cast<unsigned>(gVcomMv) / 10;
  if (!mp::tpsWrite(TPS_REG_ENABLE, 0x3F) || !mp::tpsWrite(TPS_REG_VCOM2, (vcom & 0x100) >> 8) ||
      !mp::tpsWrite(TPS_REG_VCOM1, vcom & 0xFF)) {
    return false;
  }
  state->ep_sth = true;

  uint8_t pg = 0;
  bool railsGood = false;
  for (int tries = 0; tries < 200 && !railsGood; ++tries) {
    railsGood = mp::tpsRead(TPS_REG_PG, pg) && (pg & 0xFA) == 0xFA;
    if (!railsGood) delay(1);
  }
  if (!railsGood) return false;

  state->ep_output_enable = true;
  epd_ctrl_state_t outMask = {};
  outMask.ep_output_enable = true;
  boardSetCtrl(state, &outMask);
  return true;
}

void boardPowerOn(epd_ctrl_state_t* state) {
  // Initial attempt plus three complete power-cycle retries, as the vendor does.
  for (int attempt = 0; attempt <= 3; ++attempt) {
    if (powerOnOnce(state)) {
      gPowered = true;
      return;
    }
    boardPowerOff(state);
    delay(100);
  }
  log_e("[epd] TPS65185 power-on failed");
}

void boardMeasureVcom(epd_ctrl_state_t* state) { (void)state; }

void boardSetVcom(int value) { gVcomMv = value; }

float boardTemperature() { return 25.0f; }

const EpdBoardDefinition kBoard = {
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

const EpdiyConfig& metalioEInk4PlusEpdiyConfig() {
  static EpdiyConfig config = [] {
    my_waveform_init();  // must precede epd_hl_init()
    return EpdiyConfig{&kBoard, &E0470A14_AF, &MY_WAVEFORM, EPD_LUT_1K, 1200, 25};
  }();
  return config;
}

}  // namespace freeink

#endif  // FREEINK_DEVICE_METALIO_EINK4_PLUS
