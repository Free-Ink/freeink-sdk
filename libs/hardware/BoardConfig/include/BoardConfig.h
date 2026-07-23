#pragma once

// FreeInk SDK — board hardware profiles + build composition.
//
// A BoardProfile describes a device's pinout, screen, and capabilities. The
// runtime-active profile is BoardConfig::ACTIVE; drivers (display / input /
// power) read from it so the same code adapts to any board.
//
// A build is composed along two axes:
//   * DEVICES   (-DFREEINK_DEVICE_<NAME>) — which hardware the binary supports.
//   * CAPABILITIES (-DFREEINK_CAP_<NAME>) — which feature code is compiled in.
//
// Devices that share a binary must share an MCU and (to be runtime-selected)
// supply their own detection in the consumer. X3 and X4 are two profiles in one
// ESP32-C3 binary, picked at runtime via EInkDisplay::setDisplayX3() (which calls
// selectDevice); ACTIVE defaults to a compile-time default until then.

#include <Arduino.h>
#include <driver/gpio.h>  // gpio_hold_dis in releaseSdRail()

// ============================================================================
// Build composition — devices x capabilities
// ============================================================================

// --- 1) Devices are selected explicitly --------------------------------------
// A build declares its hardware with one or more -DFREEINK_DEVICE_<NAME> in its
// platformio env (see platformio.sample.ini). There is no default and no
// inference from board macros — pick your device(s) by setting the flag(s). The
// coherence check below errors if none (or an incompatible mix) is selected.

// Normalize device flags to 0/1.
#ifndef FREEINK_DEVICE_X4
#define FREEINK_DEVICE_X4 0
#endif
#ifndef FREEINK_DEVICE_X3
#define FREEINK_DEVICE_X3 0
#endif
#ifndef FREEINK_DEVICE_X4PRO
#define FREEINK_DEVICE_X4PRO 0
#endif
#ifndef FREEINK_DEVICE_M5
#define FREEINK_DEVICE_M5 0
#endif
#ifndef FREEINK_DEVICE_MURPHY
#define FREEINK_DEVICE_MURPHY 0
#endif
#ifndef FREEINK_DEVICE_DELINK
#define FREEINK_DEVICE_DELINK 0
#endif
#ifndef FREEINK_DEVICE_LILYGO
#define FREEINK_DEVICE_LILYGO 0
#endif
#ifndef FREEINK_DEVICE_M5PAPER
#define FREEINK_DEVICE_M5PAPER 0
#endif
#ifndef FREEINK_DEVICE_STICKY
#define FREEINK_DEVICE_STICKY 0
#endif

// --- 2) Coherence: exactly one MCU family, at least one device ---------------
#if !(FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3 || FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_M5 || FREEINK_DEVICE_MURPHY || \
      FREEINK_DEVICE_DELINK || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_M5PAPER || FREEINK_DEVICE_STICKY)
#error \
    "FreeInk: no device selected. Pass at least one -DFREEINK_DEVICE_<NAME> (X4, X3, X4PRO, M5, MURPHY, DELINK, LILYGO, M5PAPER, STICKY) in your build env — see platformio.sample.ini."
#endif
// Each device belongs to one MCU family; a binary targets exactly one. X3/X4 are
// ESP32-C3; M5 PaperColor/Murphy/de-link/LilyGo are ESP32-S3; M5Paper v1.1 is the
// classic ESP32 (ESP32-D0WDQ6). The three families differ in deep-sleep wakeup,
// SPI peripheral count, and toolchain, so they never share a binary.
#define FREEINK_MCU_C3 (FREEINK_DEVICE_X3 || FREEINK_DEVICE_X4)
#define FREEINK_MCU_S3                                                                                    \
  (FREEINK_DEVICE_M5 || FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_DELINK || FREEINK_DEVICE_LILYGO ||        \
   FREEINK_DEVICE_STICKY || FREEINK_DEVICE_X4PRO)
#define FREEINK_MCU_ESP32 (FREEINK_DEVICE_M5PAPER)
#if (FREEINK_MCU_C3 + FREEINK_MCU_S3 + FREEINK_MCU_ESP32) != 1
#error \
    "FreeInk: all selected devices must share one MCU family — ESP32-C3 (X3/X4), ESP32-S3 (M5/Murphy/de-link/LilyGo/Sticky/X4Pro), or ESP32 (M5Paper). Build one binary per family."
#endif

// --- 3) Derive panel drivers from the device set -----------------------------
// Sticky reuses SSD1677: its 800x480 panel rides a 24-pin FPC whose GDR/RESE/BS1
// + dual VSH1/VSH2 + external VGH/VGL/VSL/VCOM charge pump is the SSD1677
// application circuit (same controller + resolution as X4 / de-link).
// X4 Pro is a distinct ESP32-S3 device (NOT the C3 X4): same SSD1677 controller and
// 800x480 panel as X4/de-link/Sticky, recovered from its OEM firmware dump — see
// docs/xteink-x4pro-support.md.
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_DELINK || FREEINK_DEVICE_STICKY || FREEINK_DEVICE_X4PRO
#define FREEINK_DRIVER_SSD1677 1
#else
#define FREEINK_DRIVER_SSD1677 0
#endif
#if FREEINK_DEVICE_X3
#define FREEINK_DRIVER_UC8253_X3 1
#else
#define FREEINK_DRIVER_UC8253_X3 0
#endif
// M5 PaperColor has two interchangeable display backends: the fast hand-rolled
// ED2208 driver (default), or M5's official M5GFX/M5Unified path (opt in with
// -DFREEINK_M5_OFFICIAL=1, which pulls the M5 libraries — see platformio.sample).
#if FREEINK_DEVICE_M5 && defined(FREEINK_M5_OFFICIAL) && FREEINK_M5_OFFICIAL
#define FREEINK_DRIVER_M5_OFFICIAL 1
#define FREEINK_DRIVER_ED2208 0
#elif FREEINK_DEVICE_M5
#define FREEINK_DRIVER_ED2208 1
#define FREEINK_DRIVER_M5_OFFICIAL 0
#else
#define FREEINK_DRIVER_ED2208 0
#define FREEINK_DRIVER_M5_OFFICIAL 0
#endif
#if FREEINK_DEVICE_MURPHY
#define FREEINK_DRIVER_UC8253_MURPHY 1
#else
#define FREEINK_DRIVER_UC8253_MURPHY 0
#endif
// LilyGo T5 S3: raw-parallel ED047TC1 via LovyanGFX (M5GFX). External-bus driver.
#if FREEINK_DEVICE_LILYGO
#define FREEINK_DRIVER_LGFX_EPD 1
#else
#define FREEINK_DRIVER_LGFX_EPD 0
#endif
// M5Paper v1.1: ED047TC1 behind an IT8951E timing controller (its own framebuffer
// SRAM, 16-bit-word SPI with MISO reads). The driver owns its SPI end to end.
#if FREEINK_DEVICE_M5PAPER
#define FREEINK_DRIVER_IT8951 1
#else
#define FREEINK_DRIVER_IT8951 0
#endif

// --- 4) Derive default capabilities (override with -DFREEINK_CAP_*=0/1) -------
#ifndef FREEINK_CAP_TOUCH
#define FREEINK_CAP_TOUCH                                                                         \
  (FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_M5PAPER || FREEINK_DEVICE_STICKY || \
   FREEINK_DEVICE_X4PRO)
#endif
#ifndef FREEINK_CAP_FRONTLIGHT
#define FREEINK_CAP_FRONTLIGHT \
  (FREEINK_DEVICE_DELINK || FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_X4PRO)
#endif
// BLE HID host. The BleKeyboardHost lib pairs/connects to Bluetooth Low Energy
// HID peripherals such as keyboards and page turners and emits translated key
// events; it compiles its NimBLE central code only when this is set, otherwise
// it links stub bodies and pulls in no BLE code at all. Default off: it's an
// opt-in feature, not board-derived. ESP32-C3/S3 targets only (BLE required).
#ifndef FREEINK_CAP_BLE_HID_HOST
#ifdef FREEINK_CAP_BLE_KEYBOARD
#define FREEINK_CAP_BLE_HID_HOST FREEINK_CAP_BLE_KEYBOARD
#else
#define FREEINK_CAP_BLE_HID_HOST 0
#endif
#endif
#ifndef FREEINK_CAP_BLE_KEYBOARD
#define FREEINK_CAP_BLE_KEYBOARD FREEINK_CAP_BLE_HID_HOST
#endif
// Scan-list policy for the BLE HID host. Default hides anonymous non-HID
// advertisers so firmware pairing UIs are not filled with random beacon
// addresses. Set -DFREEINK_BLE_HID_SHOW_UNNAMED_DEVICES=1 during bring-up to
// include connectable unnamed devices as probe candidates. Devices advertising
// HID are always kept, even without a name.
#ifndef FREEINK_BLE_HID_SHOW_UNNAMED_DEVICES
#define FREEINK_BLE_HID_SHOW_UNNAMED_DEVICES 0
#endif
// Security policy for BLE HID host pairing. Default to Just Works bonding
// because many page-turner remotes have no input/display capability and reject
// mandatory MITM/passkey pairing. Firmware that specifically wants keyboard
// passkey pairing can opt in with -DFREEINK_BLE_HID_REQUIRE_MITM=1.
#ifndef FREEINK_BLE_HID_REQUIRE_MITM
#define FREEINK_BLE_HID_REQUIRE_MITM 0
#endif

// I2C fuel-gauge battery backend. Compiled in when a build contains a gauge
// device (X3's BQ27220, or LilyGo's BQ27220+BQ25896). Selection is then *runtime*
// per active profile (BatteryMonitor uses the gauge only when
// ACTIVE.batteryGauge.gaugeAddr != 0) — required because X3 (gauge) and X4 (ADC)
// share one C3 binary.
#ifndef FREEINK_BATTERY_I2C_GAUGE
#define FREEINK_BATTERY_I2C_GAUGE \
  (FREEINK_DEVICE_X3 || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_STICKY || FREEINK_DEVICE_X4PRO)
#endif
#ifndef FREEINK_CAP_COLOR
#define FREEINK_CAP_COLOR (FREEINK_DEVICE_M5)
#endif
#ifndef FREEINK_CAP_AUDIO
#define FREEINK_CAP_AUDIO (FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_M5)
#endif
// Microphone capture (PDM in). Separate from FREEINK_CAP_AUDIO (output): the
// Sticky has a PDM mic but no output codec. The Microphone lib compiles its
// i2s_pdm RX path only when this is set; otherwise it links stub bodies.
#ifndef FREEINK_CAP_MIC
#define FREEINK_CAP_MIC (FREEINK_DEVICE_STICKY)
#endif
// On-board I2C sensors. Each lib (Rtc / EnvironmentSensor / Imu) compiles its
// I2C driver only when its flag is set; otherwise it links stub bodies.
#ifndef FREEINK_CAP_RTC
#define FREEINK_CAP_RTC (FREEINK_DEVICE_X3 || FREEINK_DEVICE_STICKY || FREEINK_DEVICE_X4PRO)
#endif
#ifndef FREEINK_CAP_TEMP_HUMIDITY
#define FREEINK_CAP_TEMP_HUMIDITY (FREEINK_DEVICE_STICKY)
#endif
#ifndef FREEINK_CAP_IMU
#define FREEINK_CAP_IMU (FREEINK_DEVICE_X3 || FREEINK_DEVICE_STICKY)
#endif
// LEDC PWM buzzer (tone beeper). The Buzzer lib drives the AudioConfig.buzzer
// pin; on for boards that wire one (Sticky GPIO48, Murphy GPIO46). Separate from
// FREEINK_CAP_AUDIO — a buzzer is a tone device, not a WAV/codec output.
#ifndef FREEINK_CAP_BUZZER
#define FREEINK_CAP_BUZZER (FREEINK_DEVICE_STICKY || FREEINK_DEVICE_MURPHY)
#endif
#ifndef FREEINK_CAP_LED
#define FREEINK_CAP_LED (FREEINK_DEVICE_M5)
#endif
#ifndef FREEINK_CAP_NET_TLS13
#if defined(FREEINK_NET_WOLFSSL)
#define FREEINK_CAP_NET_TLS13 1
#else
#define FREEINK_CAP_NET_TLS13 0
#endif
#endif

// Place the facade framebuffer(s) in PSRAM (heap, MALLOC_CAP_SPIRAM) instead of
// static DRAM .bss. Default on for M5Paper v1.1: the classic ESP32 has tight
// internal DRAM but 8MB PSRAM, and the 63KB 540x960 framebuffer does not fit in
// .bss alongside the firmware. Every other device keeps the static DRAM array.
// (The prebuilt Arduino-ESP32 libs disable BSS-in-PSRAM, so this is a runtime
// heap allocation, not EXT_RAM_BSS_ATTR.)
#ifndef FREEINK_FB_PSRAM
#define FREEINK_FB_PSRAM (FREEINK_DEVICE_M5PAPER)
#endif

// SD transport. de-link (4-bit) and X4 Pro (1-bit) are wired for SDMMC; SdFat
// can't drive SDIO, so they get a native esp-idf SDMMC block device behind
// SDCardManager. Every other board stays on SdFat-over-SPI. The consumer's build
// must define USE_BLOCK_DEVICE_INTERFACE=1 for the SdFat FsVolume these mount on.
// Override with -DFREEINK_SD_SDMMC=0/1.
#ifndef FREEINK_SD_SDMMC
#define FREEINK_SD_SDMMC (FREEINK_DEVICE_DELINK || FREEINK_DEVICE_X4PRO)
#endif

// Serial log transport hint for consumer firmware. Boards can share the same MCU
// but expose logs differently: LilyGo T5 S3 is monitored over native USB CDC,
// while Sticky bring-up is more reliable through the IDF/ROM console path.
#define FREEINK_LOG_TRANSPORT_SERIAL 0
#define FREEINK_LOG_TRANSPORT_USB_CDC_WRITE 1
#define FREEINK_LOG_TRANSPORT_ROM_PRINTF 2
#ifndef FREEINK_LOG_TRANSPORT
#if FREEINK_DEVICE_LILYGO
#define FREEINK_LOG_TRANSPORT FREEINK_LOG_TRANSPORT_USB_CDC_WRITE
#elif FREEINK_DEVICE_STICKY
#define FREEINK_LOG_TRANSPORT FREEINK_LOG_TRANSPORT_ROM_PRINTF
#else
#define FREEINK_LOG_TRANSPORT FREEINK_LOG_TRANSPORT_SERIAL
#endif
#endif

// Bidirectional serial transport exposed by the board's physical USB-C port.
// Most boards route it through Arduino's selected Serial implementation, while
// Sticky's on-board WCH bridge is wired to UART0 instead of native USB CDC.
#if FREEINK_DEVICE_STICKY
#define FREEINK_SERIAL_HAS_TX_TIMEOUT 0
#else
#define FREEINK_SERIAL_HAS_TX_TIMEOUT (ARDUINO_USB_CDC_ON_BOOT)
#endif

namespace BoardConfig {

#if FREEINK_DEVICE_STICKY
inline HardwareSerial& serialTransport() { return Serial0; }
#else
inline auto& serialTransport() { return Serial; }
#endif

// Physical device family. X3 and X4 are sibling devices on the same ESP32-C3
// board (identical pinout, different panel/size): both profiles compile into the
// C3 binary and one is chosen at runtime (setDisplayX3() -> selectDevice).
enum class Board : uint8_t {
  XteinkX4,
  XteinkX3,
  XteinkX4Pro,  // ESP32-S3 sibling of the C3 X4: SSD1677 + GT911 touch + warm/cold frontlight
  M5StackPaperColor,
  MurphyM3,
  DeLink,
  LilyGoT5S3,
  M5PaperV11,
  Sticky,
};

// How the board reports button presses.
enum class InputStyle : uint8_t {
  XteinkAdcLadder,         // resistor ladder on two ADC pins (X3/X4)
  DigitalButtons,          // plain active-low GPIO buttons
  DigitalConfirmBackHold,  // confirm held > N ms synthesizes BACK (M5 PaperColor)
  DigitalConfirmPowerHold, // confirm click, power hold on a shared GPIO
  DigitalFiveKey,          // 3 physical GPIO keys + synthesized events (Murphy M3)
};

// Panel controller silicon. Drivers are selected from this at begin().
// LgfxEpd = a raw-parallel EPD with no on-glass controller, driven via LovyanGFX
// (e.g. ED047TC1 on LilyGo T5 S3).
enum class DisplayController : uint8_t { SSD1677, UC8253, ED2208, LgfxEpd, IT8951 };

// Optional capacitive touch controller.
enum class TouchController : uint8_t { None, Chsc6x, Gt911 };

// Optional audio output path. Murphy M3 ships an ES8388-compatible stereo
// codec (I2S slave, control over the shared touch I2C bus) — the contract was
// recovered from the OEM firmware dump; see the consumer's audio notes.
// M5 PaperColor ships an ES8311 mono codec + AW8737A speaker amp — the
// contract comes from the official pin map and M5Unified's speaker bring-up.
enum class AudioOutput : uint8_t { None, I2sDac, I2sEs8388, I2sEs8311, PwmBuzzer };

// Optional addressable RGB LED strip. PaperColor has two RGB LEDs on GPIO21
// behind the M5PM1 LDO3V3 RGB rail.
enum class LedColorOrder : uint8_t { RGB, GRB };

constexpr int8_t PIN_UNASSIGNED = -1;

struct DisplayPins {
  int8_t sclk;
  int8_t mosi;
  int8_t cs;
  int8_t dc;
  int8_t rst;
  int8_t busy;
  int8_t powerEnable;
};

struct SdPins {
  int8_t sclk;
  int8_t miso;
  int8_t mosi;
  int8_t cs;
  int8_t powerEnable;
  bool separateSpi;
  uint32_t spiHz;  // 0 = use the SD manager default (40 MHz)
  // Polarity of powerEnable. true (default) = active-high (drive HIGH to power the
  // card, LOW to cut it) as on most boards. false = active-LOW enable (e.g. X4 Pro's
  // GPIO5, which gates the card while held LOW); the sleep path must then drive it
  // HIGH to power the card down. Defaulted so existing initializers stay valid.
  bool powerActiveHigh = true;
};

// 4-bit SDMMC/SDIO wiring (e.g. de-link). SdFat can't drive SDIO, so a board with
// busWidth != 0 gets the native esp-idf SDMMC block device instead of SPI/SdFat.
struct SdmmcPins {
  int8_t clk;
  int8_t cmd;
  int8_t d0;
  int8_t d1;
  int8_t d2;
  int8_t d3;
  uint8_t busWidth;  // 0 = not an SDMMC board (use SdPins/SPI), 1 or 4 = SDMMC
};

// The I2C fuel-gauge silicon a board carries. Each type has its own register map and
// init, so BatteryMonitor dispatches on it. Bq27220: TI command registers, no profile
// upload (LilyGo/X3). Cw2017: CellWise gauge that needs an 80-byte BATINFO battery
// profile loaded before it reports a valid SoC (Xteink X4 Pro).
enum class GaugeType : uint8_t { Bq27220, Cw2017 };

// I2C fuel-gauge / charger wiring (e.g. BQ27220 + BQ25896 on LilyGo T5 S3). When
// gaugeAddr != 0 (and FREEINK_BATTERY_I2C_GAUGE is set), BatteryMonitor reads the
// gauge over I2C instead of an ADC pin. chargerAddr is optional (0 = none) and
// only used for charge status.
struct BatteryGaugeConfig {
  int8_t i2cSda;
  int8_t i2cScl;
  uint32_t i2cHz;
  uint8_t gaugeAddr;    // BQ27220 = 0x55; CW2017 = 0x63; 0 = no I2C gauge (use ADC)
  uint8_t chargerAddr;  // BQ25896 = 0x6B; 0 = none
  // Arduino I2C controller index: 0 = Wire, 1 = Wire1. Default 0. Set to 1 on
  // boards where the gauge sits on a different physical bus than another I2C
  // peripheral (e.g. Sticky's GT911 touch on Wire/SDA3-SCL2 vs gauge on
  // Wire1/SDA1-SCL0) so they don't fight over one controller. Only honored on
  // multi-bus SoCs (SOC_I2C_NUM > 1); single-bus parts (ESP32-C3) ignore it.
  uint8_t i2cBus = 0;
  GaugeType gaugeType = GaugeType::Bq27220;  // register map / init to use
};

struct InputPins {
  int8_t back;
  int8_t confirm;
  int8_t left;
  int8_t right;
  int8_t up;
  int8_t down;
  int8_t power;
  bool powerActiveHigh;  // true = pressed reads HIGH (INPUT_PULLDOWN); false = active-LOW (INPUT_PULLUP)
};

// Capacitive touch panel description (TouchController::None disables it).
struct TouchConfig {
  TouchController controller;
  int8_t sda;
  int8_t scl;
  int8_t irq;
  int8_t reset;
  uint8_t i2cAddress;
  uint16_t rawMinX, rawMaxX;  // raw controller range, mapped to display coords
  uint16_t rawMinY, rawMaxY;
  bool synthesizeConfirm;  // emit a CONFIRM button event on tap
  uint8_t i2cAddressAlt;   // alternate I2C address to probe (GT911 0x14; 0 = none)
  bool irqActiveLow;       // touch IRQ asserted LOW (CHSC6x)
  // GT911 point-frame layout: false = datasheet standard (track-id at 0x8150, so
  // coords start at byte 1); true = coords start at byte 0 (no track-id), as seen
  // on M5Paper's GT911 which boots without a reset/config dance. Ignored (CHSC6x).
  bool gt911CoordsAtByte0;
  // Touch power-rail enable (active-high). PIN_UNASSIGNED on boards whose touch
  // controller is always powered; driven HIGH before the reset/probe on boards
  // that gate it (e.g. Sticky's TOUCH_EN). Default keeps existing initializers valid.
  int8_t powerEnable = PIN_UNASSIGNED;
  // Touch-to-panel mounting correction, applied to the raw coords so the touch
  // frame aligns with the display's NATIVE (panel) frame before orientation
  // mapping. swapXY first (digitizer rotated 90° vs panel, e.g. Sticky's portrait
  // sensor on a landscape panel), then per-axis flip. rawMinX/MaxX/etc describe the
  // POST-swap (panel) axes. Defaults = aligned. The display orientation is handled
  // separately by GfxRenderer::tapToLogical, so taps follow rotation automatically.
  bool swapXY = false;
  bool flipX = false;
  bool flipY = false;
  // Capacitive home key below the panel, reported by the touch controller itself
  // (GT911 "have key" status bit 0x10, surfaced as InputManager::wasHomeKeyPressed()).
  // Lets firmware move "exit to home" off a swipe gesture on boards that have one.
  bool hasHomeKey = false;
};

// PWM frontlight description (gpio == PIN_UNASSIGNED disables it).
struct FrontlightConfig {
  int8_t gpio;  // primary channel: the sole LED on a single-channel board, or the "cool"
                // channel of a warm/cool pair.
  uint32_t pwmFrequency;
  uint8_t pwmResolutionBits;
  bool activeHigh;
  // Optional second PWM channel for a warm/cool color-temperature frontlight (e.g. the
  // Xteink X4 Pro: cool=gpio GPIO8, warm=gpioWarm GPIO9). PIN_UNASSIGNED on single-channel
  // boards (de-link / LilyGo / Murphy), where setColorTemperature() stays a no-op. The warm
  // channel shares the primary's frequency / resolution / active level. FrontlightManager
  // treats `gpio` as cool and `gpioWarm` as warm; if a board's pair is physically reversed,
  // the color-temperature direction inverts (cosmetic, and user-flippable in firmware).
  int8_t gpioWarm = PIN_UNASSIGNED;
};

// Audio output description (AudioOutput::None disables it).
struct AudioConfig {
  AudioOutput output;
  int8_t bclk;    // I2S bit clock (unused for PWM buzzer)
  int8_t lrclk;   // I2S word select (unused for PWM buzzer)
  int8_t dout;    // I2S data out, or the PWM pin for a buzzer
  int8_t mclk;    // I2S master clock (PIN_UNASSIGNED if not wired)
  int8_t enable;  // codec power / rail enable pin (PIN_UNASSIGNED if none)
  bool enableActiveHigh;
  int8_t ampEnable;  // separate speaker-amp enable (e.g. AW8737A SPK_EN), held
                     // high only while playing; PIN_UNASSIGNED if none. Active-high.
  int8_t codecSda;   // codec control I2C — may be a shared bus (e.g. touch)
  int8_t codecScl;
  uint8_t codecAddr;  // 7-bit codec address, 0 = no control codec
  int8_t buzzer;      // separate LEDC tone pin (PIN_UNASSIGNED if none)
};

struct LedConfig {
  int8_t data;
  uint8_t count;
  LedColorOrder colorOrder;
  bool pmicRgbPower;  // true = enable M5PM1 RGB LED power rail before use
};

// Microphone input path (MicInput::None disables it). PDM mics (e.g. the Sticky's
// MSM261DDB020) need a clock out + data in; `enable` powers the mic rail.
enum class MicInput : uint8_t { None, Pdm };
struct MicConfig {
  MicInput input;
  int8_t clk;     // PDM clock (output to mic)
  int8_t data;    // PDM data (input from mic)
  int8_t enable;  // mic power/enable pin (PIN_UNASSIGNED if none)
  bool enableActiveHigh;
};

enum class RtcType : uint8_t { None, Pcf8563, Ds3231 };
enum class ImuType : uint8_t { None, Lsm6ds3, Qmi8658 };

// On-board I2C sensors sharing one bus (e.g. the Sticky's RTC + temp/humidity +
// IMU on SDA1/SCL0, the same bus as its fuel gauge). Each addr is 0 when that
// sensor is absent; the matching sensor lib reads its addr from here.
struct SensorsConfig {
  int8_t i2cSda;
  int8_t i2cScl;
  uint32_t i2cHz;
  uint8_t rtcAddr;           // PCF8563 = 0x51, DS3231 = 0x68; 0 = none
  uint8_t tempHumidityAddr;  // SHT40 = 0x44; 0 = none
  uint8_t imuAddr;           // LSM6DS3TR-C = 0x6A, QMI8658 = 0x6B/0x6A; 0 = none
  uint8_t i2cBus = 0;        // 0 = Wire, 1 = Wire1 on multi-bus SoCs
  RtcType rtcType = RtcType::None;
  ImuType imuType = ImuType::None;
};

// How the panel is mounted relative to the driver's native scan. Any board injects
// its own mirroring here; a 180° rotation is mirrorX && mirrorY. (90°/270° need a
// software transpose — they swap width/height and aren't expressible by panel RAM
// addressing alone — so they are not a flag here.)
struct DisplayOrientation {
  bool mirrorX;  // reverse source/column (X) order
  bool mirrorY;  // reverse gate/row (Y) order
};

// Power-rail latch pins a battery-powered board must drive HIGH early in boot
// to keep itself on (PWR_HOLD / PWR_LOCK style latches, e.g. the Sticky's
// GPIO45/46). Board truth lives here; asserting them is firmware policy — see
// holdPowerRails(). Releasing the pins later is a software power-off.
struct PowerConfig {
  int8_t latch0 = PIN_UNASSIGNED;
  int8_t latch1 = PIN_UNASSIGNED;
};

struct BoardProfile {
  Board board;
  const char* name;
  InputStyle inputStyle;
  DisplayController displayController;
  uint16_t displayWidth;
  uint16_t displayHeight;
  DisplayPins display;
  uint32_t displaySpiHz;  // 0 = use the panel driver's controller-appropriate default
  SdPins sd;
  InputPins input;
  int8_t batteryAdc;
  int8_t batteryChargeStatus;
  float batteryDividerMultiplier;
  int8_t usbDetect;
  TouchConfig touch;
  FrontlightConfig frontlight;
  AudioConfig audio;
  LedConfig leds;
  DisplayOrientation orientation;   // panel mount transform (mirrorX/mirrorY)
  SdmmcPins sdmmc;                  // 4-bit SDMMC wiring (busWidth 0 = use SPI/SdFat)
  BatteryGaugeConfig batteryGauge;  // I2C fuel gauge (gaugeAddr 0 = use ADC pin)
  // Microphone (PDM in). Defaulted so existing profiles need no change; a board
  // with a mic sets it. PIN_UNASSIGNED is -1 — do NOT rely on zero-init here.
  MicConfig mic = {MicInput::None, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, true};
  // On-board I2C sensors (RTC / temp+humidity / IMU). Defaulted to "none"; a
  // board with sensors sets the bus pins + each present sensor's address.
  SensorsConfig sensors = {PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 0, 0, 0, 0, RtcType::None, ImuType::None};
  // UI scale multiplier the firmware applies to its theme metrics and chrome fonts.
  // 1.0 keeps the original button-era pixel sizes. Touch devices bump this so rows,
  // buttons, and tap targets are finger-sized: these panels are ~220-235 PPI, so a
  // 30px row is only ~3mm. Per-board and hand-tuned (PPI alone can't tell the 4.26"
  // X4 from the 3.97" Sticky); the firmware owns how it maps to metrics/fonts.
  float uiScale = 1.0f;
  // Power-rail latch pins (see PowerConfig). Defaulted so existing profiles
  // need no change; a board with a latch sets it.
  PowerConfig power = {};
};

constexpr TouchConfig NO_TOUCH = {TouchController::None,
                                  PIN_UNASSIGNED,
                                  PIN_UNASSIGNED,
                                  PIN_UNASSIGNED,
                                  PIN_UNASSIGNED,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  false,
                                  0,
                                  false,
                                  false};

// LilyGo T5 S3 Pro Lite GT911 touch (shared I2C bus). The digitizer reports a
// portrait 540x960 frame on the landscape 960x540 panel, so swap axes into the
// panel-native display frame before app-level orientation mapping.
constexpr TouchConfig LILYGO_T5_PRO_GT911 = {
    TouchController::Gt911, 39, 40, 3, 9, 0x5D, 0, 959, 0, 539, false, 0x14, false, true, PIN_UNASSIGNED, true,
    false, true};  // powerEnable, swapXY=true, flipX=false, flipY=true
constexpr FrontlightConfig NO_FRONTLIGHT = {PIN_UNASSIGNED, 0, 0, true};
constexpr AudioConfig NO_AUDIO = {AudioOutput::None,
                                  PIN_UNASSIGNED,
                                  PIN_UNASSIGNED,
                                  PIN_UNASSIGNED,
                                  PIN_UNASSIGNED,
                                  PIN_UNASSIGNED,
                                  true,
                                  PIN_UNASSIGNED,
                                  PIN_UNASSIGNED,
                                  PIN_UNASSIGNED,
                                  0,
                                  PIN_UNASSIGNED};
constexpr LedConfig NO_LEDS = {PIN_UNASSIGNED, 0, LedColorOrder::GRB, false};
constexpr LedConfig M5_PAPERCOLOR_LEDS = {21, 2, LedColorOrder::GRB, true};  // bench-verified GRB

// Defaults matching the BoardProfile member initializers, so a profile can set a
// trailing field (e.g. uiScale) positionally without spelling out the literals.
constexpr MicConfig NO_MIC = {MicInput::None, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, true};
constexpr SensorsConfig NO_SENSORS = {PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 0, 0, 0, 0, RtcType::None, ImuType::None};

// Murphy M3 audio, recovered from the OEM firmware: ES8388-compatible codec at
// 7-bit I2C 0x10 on the shared touch bus (SDA=13/SCL=12, 100 kHz), I2S master
// on BCLK=40/WS=39/DOUT=41/MCLK=42 (DIN unused). GPIO43 is driven HIGH by the
// stock board init and is preserved here as the enable line (not proven to be
// audio-specific, but the OEM bring-up notes say keep it high). GPIO46 carries
// a separate LEDC tone/buzzer path. No separate amp-enable pin.
constexpr AudioConfig MURPHY_AUDIO = {AudioOutput::I2sEs8388, 40, 39, 41,   42, 43, true,
                                      PIN_UNASSIGNED,         13, 12, 0x10, 46};

// M5 PaperColor audio, from the official pin map (docs.m5stack.com/en/core/
// PaperColor) and M5Unified's speaker bring-up: ES8311 mono codec at 7-bit I2C
// 0x18 on the system bus (SDA=3/SCL=2 — shared with the M5PM1 PMIC, same
// 100 kHz), I2S master on BCLK=40/WS=41/DOUT=38. The MCLK line (GPIO42) is
// deliberately left unwired: like M5Unified, the codec derives its clock from
// BCLK (reg 0x01=0xB5 / 0x02=0x18), which makes the init sample-rate-agnostic.
// GPIO45 (AUDIO_PWR_EN) powers the codec/mic rail; GPIO46 (SPK_EN) enables the
// AW8737A speaker amp and is raised only while playing. The ES7210 mic ADC
// (0x40) is not driven.
constexpr AudioConfig M5_PAPERCOLOR_AUDIO = {
    AudioOutput::I2sEs8311, 40, 41, 38, PIN_UNASSIGNED, 45, true, 46, 3, 2, 0x18, PIN_UNASSIGNED};

// Sticky has no output codec (PDM mic in only) — just the LEDC buzzer on GPIO48,
// driven by the Buzzer lib. output=None so hasAudio() stays false; the buzzer
// field carries the tone pin (mirrors how MURPHY_AUDIO carries its buzzer).
constexpr AudioConfig STICKY_AUDIO = {AudioOutput::None,    PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED,
                                      PIN_UNASSIGNED,       PIN_UNASSIGNED, true,           PIN_UNASSIGNED,
                                      PIN_UNASSIGNED,       PIN_UNASSIGNED, 0,              48};
constexpr DisplayOrientation NO_FLIP = {false, false};   // native scan
constexpr DisplayOrientation ROTATE_180 = {true, true};  // upside-down mount
constexpr DisplayOrientation MIRROR_X = {true, false};   // horizontal mirror
constexpr DisplayOrientation MIRROR_Y = {false, true};   // vertical mirror
constexpr SdmmcPins NO_SDMMC = {
    PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0};
constexpr BatteryGaugeConfig NO_GAUGE = {PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 0, 0};  // ADC battery

// --- Xteink X4 — ESP32-C3, SSD1677 (800x480) ---------------------------------
// X4 display SPI clock. Default 20 MHz = SSD1677 datasheet max for write mode
// (Solomon Systech SSD1677, MCU Serial Interface AC Characteristics: "MCU
// interface: SPI serial peripheral, Maximum 20MHz for write"; fSCL Write = 20 MHz.
// https://files.waveshare.com/upload/2/2a/SSD1677_1.0.pdf). The plane writes are
// ~38 ms/refresh at 20 MHz. Define -DFREEINK_X4_OVERCLOCK_SPI to run 40 MHz — the
// (out-of-spec, 2x datasheet) clock the CrossPoint / Witch Reader fork used, which
// halves that to ~19 ms (~17-20 ms/refresh faster) but can glitch plane writes on
// marginal wiring. Opt-in only; validate on your hardware. (NB: the Ssd1677Driver
// 0-default of 40 MHz is likewise over spec for boards that leave displaySpiHz 0.)
#ifdef FREEINK_X4_OVERCLOCK_SPI
#define FREEINK_X4_DISPLAY_SPI_HZ 40000000u
#else
#define FREEINK_X4_DISPLAY_SPI_HZ 20000000u
#endif
constexpr BoardProfile XTEINK_X4 = {Board::XteinkX4,
                                    "xteink_x4",
                                    InputStyle::XteinkAdcLadder,
                                    DisplayController::SSD1677,
                                    800,
                                    480,
                                    {8, 10, 21, 4, 5, 6, PIN_UNASSIGNED},
                                    FREEINK_X4_DISPLAY_SPI_HZ,  // displaySpiHz — see FREEINK_X4_OVERCLOCK_SPI above
                                    {PIN_UNASSIGNED, 7, PIN_UNASSIGNED, 12, PIN_UNASSIGNED, false, 0},
                                    {0, 1, 2, 3, 4, 5, 3, false},
                                    0,
                                    PIN_UNASSIGNED,
                                    2.0f,
                                    20,
                                    NO_TOUCH,
                                    NO_FRONTLIGHT,
                                    NO_AUDIO,
                                    NO_LEDS,
                                    NO_FLIP,
                                    NO_SDMMC,
                                    NO_GAUGE};

// --- Xteink X3 — ESP32-C3, UC8253 (792x528) ----------------------------------
// Same board/pinout as X4; differs only in panel controller + size. Selected at
// runtime (setDisplayX3) so one C3 binary drives both. Keeping it a real sibling
// profile means resolution comes from BoardProfile for X3 just like every other
// device — the panel driver never special-cases its own geometry.
constexpr BoardProfile XTEINK_X3 = {
    Board::XteinkX3,
    "xteink_x3",
    InputStyle::XteinkAdcLadder,
    DisplayController::UC8253,
    792,
    528,
    {8, 10, 21, 4, 5, 6, PIN_UNASSIGNED},
    20000000,  // displaySpiHz: 20 MHz = UC8253 datasheet max. UC8253 datasheet (UltraChip / Good Display),
               // features: "Clock rate up to 20MHz" (serial write timing TSCYCW).
               // (https://www.elecrow.com/download/product/DIE01237S/UC8253_Datasheet.pdf)
               // Witch Reader (a CrossPoint fork) ran a conservative 16 MHz; 20 MHz is in-spec and ~25% faster
               // on plane writes. Falls back to the driver's 16 MHz default if set to 0.
    {PIN_UNASSIGNED, 7, PIN_UNASSIGNED, 12, PIN_UNASSIGNED, false, 0},
    {0, 1, 2, 3, 4, 5, 3, false},
    0,
    PIN_UNASSIGNED,
    2.0f,
    20,
    NO_TOUCH,
    NO_FRONTLIGHT,
    NO_AUDIO,
    NO_LEDS,
    NO_FLIP,
    NO_SDMMC,
    {20, 0, 400000, 0x55, 0},  // BQ27220 fuel gauge (0x55) on SDA20/SCL0; no charger IC
    NO_MIC,
    {20, 0, 400000, 0x68, 0, 0x6B, 0, RtcType::Ds3231, ImuType::Qmi8658}};

// --- M5Stack PaperColor — ESP32-S3, ED2208 color panel, M5PM1 PMIC -----------
constexpr BoardProfile M5STACK_PAPER_COLOR = {Board::M5StackPaperColor,
                                              "m5stack_papercolor",
                                              InputStyle::DigitalConfirmBackHold,
                                              DisplayController::ED2208,
                                              400,
                                              600,
                                              {15, 13, 44, 43, 12, 11, PIN_UNASSIGNED},
                                              0,  // displaySpiHz: 0 -> ED2208 driver default (4 MHz)
                                              {15, 14, 13, 47, PIN_UNASSIGNED, false, 0},
                                              {1, 1, PIN_UNASSIGNED, PIN_UNASSIGNED, 10, 9, 1, false},
                                              PIN_UNASSIGNED,
                                              PIN_UNASSIGNED,
                                              2.0f,
                                              PIN_UNASSIGNED,
                                              NO_TOUCH,
                                              NO_FRONTLIGHT,
                                              M5_PAPERCOLOR_AUDIO,
                                              M5_PAPERCOLOR_LEDS,
                                              NO_FLIP,
                                              NO_SDMMC,
                                              NO_GAUGE};

// --- Murphy M3 (CrowPanel 3.7") — UC8253, CHSC6x touch, PWM frontlight --------
constexpr BoardProfile MURPHY_M3 = {
    Board::MurphyM3,
    "murphy_m3",
    InputStyle::DigitalFiveKey,
    DisplayController::UC8253,
    // Framebuffer is landscape 416x240: the panel is a 240x416 controller held
    // rotated 90°, and the Murphy driver rotates each plane into controller RAM.
    416,
    240,
    {4, 3, 5, 6, 7, 8, PIN_UNASSIGNED},
    0,  // displaySpiHz: 0 -> Murphy UC8253 driver default (4 MHz)
    {39, 13, 40, 10, PIN_UNASSIGNED, true, 0},
    {PIN_UNASSIGNED, 0, PIN_UNASSIGNED, PIN_UNASSIGNED, 1, 2, 0, false},
    9,               // batteryAdc: stock firmware samples analogRead(9) for battery voltage
    PIN_UNASSIGNED,  // batteryChargeStatus: not identified
    3.030303f,       // stock firmware scales ADC by 0.0016 / 0.33, implying a 1:0.33 divider
    PIN_UNASSIGNED,
    {TouchController::Chsc6x, 13, 12, 44, 45, 0x2e, 24, 224, 24, 398, false, 0, true, false},
    {48, 25000, 10, true},
    // NOTE: the SPI SD pin guess above (39/13/40) predates the OEM firmware
    // audio recovery and conflicts with the proven I2S pins (39/40/41/42) and
    // shared I2C (13). Audio is the verified owner of those pins.
    MURPHY_AUDIO,
    NO_LEDS,
    NO_FLIP,
    NO_SDMMC,
    NO_GAUGE};

// --- de-link (X4-class GDEQ0426T82 panel on ESP32-S3) — SSD1677 + frontlight ---
// Reuses the SSD1677 driver (same controller/panel as X4); differs at the board
// level: S3 MCU, SDMMC SD, warm/cool PWM frontlight.
//
// Orientation: this profile ships NO_FLIP (X4 orientation). A board that mounts
// the panel rotated sets `ROTATE_180` (or a mirror) here, and the SSD1677 driver
// applies it in hardware (mirrorX via RAM addressing, mirrorY via gate scan). Any
// board injects its own mount transform the same way.
constexpr BoardProfile DE_LINK = {Board::DeLink,
                                  "de_link",
                                  InputStyle::XteinkAdcLadder,
                                  DisplayController::SSD1677,
                                  800,
                                  480,
                                  {8, 10, 21, 4, 5, 6, PIN_UNASSIGNED},
                                  0,  // displaySpiHz: SSD1677 default (40 MHz)
                                  // SD on de-link is 4-bit SDMMC. SdFat can't drive SDIO, so SDCardManager
                                  // mounts an FsVolume on a native esp-idf SDMMC block device (FREEINK_SD_SDMMC);
                                  // the wiring is in the sdmmc field below. These SPI sd pins are unused.
                                  {39, 38, 40, 41, PIN_UNASSIGNED, true, 0},
                                  {0, 1, 2, 3, 4, 5, 3, true},  // power button active-HIGH (INPUT_PULLDOWN) on de-link
                                  4,  // batteryAdc GPIO4
                                  PIN_UNASSIGNED,
                                  2.0f,
                                  PIN_UNASSIGNED,
                                  NO_TOUCH,
                                  // Primary brightness PWM (GPIO5). Warm/cool/rail/fault pins (GPIO6/7/17/18)
                                  // are not driven.
                                  {5, 20000, 8, true},
                                  NO_AUDIO,
                                  NO_LEDS,
                                  NO_FLIP,
                                  {39, 40, 38, 48, 42, 41, 4},  // SDMMC 4-bit: CLK39 CMD40 D0=38 D1=48 D2=42 D3=41
                                  NO_GAUGE};

// --- LilyGo T5 S3 4.7" (ED047TC1 raw-parallel EPD) — ESP32-S3 -----------------
// 960x540 16-gray raw parallel panel driven via LovyanGFX (FREEINK_DRIVER_LGFX_EPD);
// the panel can't power up without the board's PMIC (TPS65185) + PCA9535 expander
// sequence, which the board injects through LgfxEpdConfig::power (see the LilyGo
// support doc). Geometry is the physical/native landscape scan size; app-level
// orientation handles rotated reader layouts. Display + GT911 touch + PWM backlight + the I2C fuel gauge
// (BQ27220/BQ25896) are wired here. The user button (behind the PCA9535 expander),
// PCF85063 RTC, and LoRa/GPS remain board-support — see docs/lilygo-t5s3-support.md.
constexpr BoardProfile LILYGO_T5S3 = {
    Board::LilyGoT5S3,
    "lilygo_t5s3",
    InputStyle::DigitalButtons,  // only BOOT (GPIO0) is a direct GPIO; the user
                                 // button is behind the PCA9535 expander (board-support)
    DisplayController::LgfxEpd,
    960,
    540,
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED,
     PIN_UNASSIGNED},                            // no SPI display pins: parallel bus lives in LgfxEpdConfig
    0,                                           // displaySpiHz n/a (external bus)
    {14, 21, 13, 12, PIN_UNASSIGNED, false, 0},  // SD over SPI: SCLK14 MISO21 MOSI13 CS12
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0,
     false},         // power=BOOT (GPIO0), active-low
    PIN_UNASSIGNED,  // batteryAdc: none — uses the I2C fuel gauge below
    PIN_UNASSIGNED,
    2.0f,
    PIN_UNASSIGNED,
    LILYGO_T5_PRO_GT911,  // GT911 touch (SDA39 SCL40 INT3 RST9, 0x5D, portrait sensor -> landscape panel)
    {11, 5000, 8, true},  // backlight: BL_EN GPIO11, PWM 5 kHz / 8-bit, active-high
    NO_AUDIO,
    NO_LEDS,
    NO_FLIP,
    NO_SDMMC,
    {39, 40, 400000, 0x55, 0x6B},  // BQ27220 gauge (0x55) + BQ25896 charger (0x6B) on SDA39/SCL40
    NO_MIC,
    NO_SENSORS,
    1.2f,  // uiScale: 4.7" 960x540 touch (~234 PPI) — finger-sized chrome, like Sticky
    // Power latch: main-power MOSFET on GPIO2, driven HIGH first thing in boot
    // via holdPowerRails() or the board powers off when USB is unplugged.
    {2}};

// --- M5Paper v1.1 4.7" (ED047TC1 behind an IT8951E controller) — ESP32 --------
// 540x960 16-gray panel driven through an IT8951E timing controller over SPI
// (MOSI12 MISO13 SCLK14 CS15, HRDY/busy GPIO27, EPD power-enable GPIO23). The
// framebuffer is landscape 960x540 (byte-aligned; 540 is not a multiple of 8) and
// the IT8951 driver rotates it onto the portrait panel — the rotation is an
// injectable driver-config field, so a board that mounts the panel differently
// flips it without code changes. GT911 touch (I2C SDA21/SCL22, INT36) is reused
// from InputManager. Battery is read on the GPIO35 ADC. The 3-position rotary
// switch maps push=CONFIRM(38), left(39), right(37).
//
// System note: M5Paper latches its own power through a MOSFET on GPIO2 — it must
// be driven HIGH at boot or the device powers off the moment USB is unplugged.
// The profile's power.latch0 carries it, asserted by holdPowerRails() (call it
// first thing in setup(), like the Sticky). EXT power (GPIO5) and EPD power
// (GPIO23) gate the peripheral and panel rails; the IT8951 driver asserts GPIO23
// (the EPD rail) itself.
constexpr BoardProfile M5PAPER_V11 = {
    Board::M5PaperV11,
    "m5paper_v11",
    InputStyle::DigitalButtons,
    DisplayController::IT8951,
    960,  // landscape framebuffer (byte-aligned); driver rotates onto the 540x960 panel
    540,
    {14, 12, 15, PIN_UNASSIGNED, PIN_UNASSIGNED, 27, 23},  // SCLK14 MOSI12 CS15, no DC/RST, HRDY27, EPD_PWR_EN23
    0,                                                     // displaySpiHz: 0 -> IT8951 driver default (10 MHz)
    {14, 13, 12, 4, PIN_UNASSIGNED, false, 20000000},      // SD shares the SPI bus: SCLK14 MISO13 MOSI12, CS4. 20 MHz
                                                           // (not the 40 MHz default): the bus is shared with the EPD,
                                                           // and 40 MHz gives SdFat READ_TIMEOUT on the CSD/data read.
    // Rotary wheel is M5Paper's only button input (3 positions: 37, push=38, 39,
    // active-low via external pull-ups). The two sides MUST drive page navigation —
    // CrossPoint's reader pages on BTN_UP/BTN_DOWN (the fixed side buttons), so
    // up=37, down=39 (matches the physical wheel orientation; direction is also
    // user-swappable in settings). The push (38) is CONFIRM and doubles as the
    // power/wake button: 38 is an RTC GPIO, so it's the ext1 deep-sleep wake source.
    // Back/Left/Right have no GPIO (only 3 wheel inputs) — M5Paper uses its GT911
    // touch for those. So confirm and power share pin 38 by design.
    {PIN_UNASSIGNED, 38, PIN_UNASSIGNED, PIN_UNASSIGNED, 37, 39, 38, false},
    35,  // batteryAdc GPIO35 (2:1 divider; pending hardware validation)
    PIN_UNASSIGNED,
    2.0f,
    PIN_UNASSIGNED,
    // GT911 touch, shared I2C SDA21/SCL22, INT36, 0x5D (alt 0x14), no reset GPIO.
    // The GT911 reports in the silicon's native PORTRAIT frame (540x960), but the IT8951
    // driver rotates the 960x540 framebuffer 90° onto that silicon, so the renderer + tap
    // pipeline (GfxRenderer::tapToLogical) work in the 960x540 FRAMEBUFFER frame. Align
    // touch to that frame like every other board (cf. Sticky): swapXY=true with rawMax in
    // FB-landscape order (959 x 539). Without the swap the tap normalizes over 540x960 while
    // tapToLogical scales by 960x540 — aspect/axis-swapped coords: the back corner still
    // roughly hits but mid-screen taps are way off (worst along the tall 960 axis).
    // flipY=true keeps the back gesture upright (silicon (0,0) = physical top-left, from the
    // working back-gesture corner); flipX=false (X was never mirrored).
    {TouchController::Gt911, 21, 22, 36, PIN_UNASSIGNED, 0x5D, 0, 959, 0, 539, false, 0x14, false,
     true,  // gt911CoordsAtByte0: reports coords at byte 0 (no track-id) on M5Paper
     PIN_UNASSIGNED, true, false, true},  // powerEnable, swapXY=true, flipX=false, flipY=true
    NO_FRONTLIGHT,
    NO_AUDIO,
    NO_LEDS,
    NO_FLIP,
    NO_SDMMC,
    NO_GAUGE,
    NO_MIC,
    NO_SENSORS,
    1.2f,  // uiScale: 4.7" 960x540 touch (~234 PPI) — finger-sized chrome, like Sticky
    // Power latch: main-power MOSFET on GPIO2, driven HIGH first thing in boot
    // via holdPowerRails() or the board powers off when USB is unplugged.
    {2}};

// --- Sticky (Seeed Sticky) — ESP32-S3R8, SSD1677 + GT911 touch ---------------
// 3.97" 800x480 B/W e-paper on a 24-pin FPC, controller confirmed SSD1677 by the
// vendor peripheral demo (pin_config.h: "E-paper SSD1677 (SPI)") — same driver,
// controller, and resolution as X4/de-link; its GDR/RESE/BS1 + dual VSH1/VSH2 +
// external VGH/VGL/VSL/VCOM charge pump is the SSD1677 reference circuit.
// Capacitive GT911 touch on its own I2C bus, MicroSD over SPI (shared display
// bus), BQ27220 fuel gauge, PDM mic + buzzer. Pins are triple-sourced (V01
// schematic 2026-06-05 + porting spec + vendor demo pin_config.h).
//
// Pending hardware validation:
//   * orientation — panel mount transform unknown; ships NO_FLIP (set ROTATE_180/
//     a mirror here once the reader's "up" is confirmed on a unit).
//   * MicroSD shares the display SPI bus; the vendor demo doesn't exercise SD, so
//     bus-sharing / CS arbitration with the panel needs a hardware check.
//   * PDM mic pins (19/20/38) are from the schematic/spec; no vendor demo uses the
//     mic, so they're unconfirmed in code.
constexpr BoardProfile STICKY = {
    Board::Sticky,
    "sticky",
    InputStyle::DigitalConfirmPowerHold,  // shared OK/PWR: click confirms, hold sleeps
    DisplayController::SSD1677,
    800,
    480,
    {13, 14, 15, 16, 17, 18, 47},  // SCK13 MOSI(SDI)14 CS15 DC16 RST17 BUSY18, EP_PWR_EN47
    0,  // displaySpiHz: 0 -> SSD1677 driver default (40 MHz), as on X4/de-link (same controller). The
        // vendor peripheral demo clocks it at a conservative 10 MHz; if the SD-shared bus proves flaky on
        // hardware, pin this to 10000000.
    // SD over SPI, sharing the display's SPI bus: SCLK13 / MOSI14 / MISO12 (the
    // vendor demo's pin_config.h confirms these as the EPD bus pins), SD_CS8,
    // SD_PWR_EN10. SD bus-sharing is inferred (the demo doesn't exercise SD) —
    // verify CS/transactions don't collide with the panel on hardware.
    {13, 12, 14, 8, 10, false, 0},
    // up5 down6; OK/confirm == power button GPIO4 (vendor demo: PIN_BTN_OK = PIN_POWER_BTN).
    // back/left/right come from touch. Active-low (10K pull-ups to VDD_3V3, button to GND).
    {PIN_UNASSIGNED, 4, PIN_UNASSIGNED, PIN_UNASSIGNED, 5, 6, 4, false},
    PIN_UNASSIGNED,  // batteryAdc: none — uses the I2C fuel gauge below
    40,              // batteryChargeStatus: CHARGE_STATE GPIO40 (from BQ25616)
    2.0f,
    PIN_UNASSIGNED,  // usbDetect: PWR_IN_VOLT (GPIO9 ADC) is board-support, not a digital detect
    // GT911 touch on its own I2C bus (SDA3 SCL2 INT21 RST41, 0x5D alt 0x14). GT911
    // reports pixel coords, so raw range == panel size; standard datasheet frame
    // layout (RST wired -> reset/config dance runs, track-id present).
    // gt911CoordsAtByte0=true: this panel's GT911 reports coords at byte 0 (no
    // track-id), like M5Paper — confirmed by raw point dumps during bring-up.
    // Portrait digitizer on a landscape panel: swapXY + flip both maps the sensor
    // frame onto the panel-native frame (confirmed by corner + menu bring-up taps).
    // rawMax* are the panel axes (post-swap). powerEnable=GPIO42 (TOUCH_EN).
    {TouchController::Gt911, 3, 2, 21, 41, 0x5D, 0, 799, 0, 479, false, 0x14, false, true, 42, true, true, true},
    NO_FRONTLIGHT,  // e-paper, no frontlight (charge LED is board-support)
    STICKY_AUDIO,   // no output codec; LEDC buzzer on GPIO48 (Buzzer lib). PDM mic is separate (mic field)
    NO_LEDS,        // charge-state LED is charger-driven, not an addressable strip
    NO_FLIP,        // mount orientation pending validation; see note above
    NO_SDMMC,       // SD is SPI, not 4-bit SDMMC
    // BQ27220 fuel gauge at 0x55 on the BFG/MISC I2C bus: SDA=GPIO1, SCL=GPIO0.
    // NOTE: GPIO0 is an ESP32-S3 strapping pin — the board init must not leave a
    // pull state that corrupts boot mode. No I2C charger (BQ25616 status is GPIO40).
    // Bus 1 (Wire1): the GT911 touch above owns Wire (SDA3/SCL2); the gauge is on a
    // separate physical bus, so it gets the second I2C controller to avoid a clash.
    {1, 0, 400000, 0x55, 0, 1},
    // Microphone: PDM mic (MSM261DDB020) — PDM_CLK GPIO19, PDM_DATA GPIO20, mic
    // power/enable (PDM_EN) GPIO38 (active-high via a load switch).
    {MicInput::Pdm, 19, 20, 38, true},
    // Sensors on the shared sensor I2C bus (SDA1/SCL0, same as the fuel gauge):
    // PCF8563 RTC (0x51), SHT40 temp/humidity (0x44), LSM6DS3TR-C IMU (0x6A).
    {1, 0, 400000, 0x51, 0x44, 0x6A, 1, RtcType::Pcf8563, ImuType::Lsm6ds3},
    1.2f,  // uiScale: touch device, 3.97" 800x480 — bump chrome to finger size
    // Power latch: PWR_HOLD GPIO45 + PWR_LOCK GPIO46, driven HIGH first thing in
    // boot (the vendor demo's first init step) — see holdPowerRails().
    {45, 46}};

// --- Xteink X4 Pro — ESP32-S3, SSD1677 (800x480) + GT911 touch + warm/cold frontlight ---
// Recovered from the OEM flash dump (x4pro_flash_dump.bin); full evidence and confidence
// levels in docs/xteink-x4pro-support.md. This is a DISTINCT device from the C3
// `XTEINK_X4` above: same panel controller/size, but an ESP32-S3 with 8 MB PSRAM, a
// GT911 capacitive digitizer, and a dual warm/cold color-temperature frontlight.
//
// Confidence summary:
//   CONFIRMED : display SPI + panel pins, GT911 controller/address, ADC-ladder input style.
//   HIGH      : GT911 I2C/INT/RST pins, SD SPI bus + CS (m_csPin=GPIO45) + enable GPIO5 (driven
//     HIGH), the BM8563 RTC (0x51 on the shared touch bus), and the GPIO1 master rail (driven
//     HIGH first in board init) — all from the board pin-init table at IROM 0x420a2240.
//   PENDING hardware validation: panel orientation (ships NO_FLIP), touch swap/flip, the exact
//     frontlight GPIO(s)/freq (warm+cold; the SDK models one channel — primary brightness here),
//     the GPIO5 SD-enable role and GPIO2 (a board-init output driven LOW, role unknown), and
//     battery/VBUS pins. The ADC-ladder pins are UNKNOWN — GPIO1/GPIO2 (the old guess) are power
//     outputs, not ladder inputs. See the findings doc before trusting any PENDING value.
constexpr BoardProfile XTEINK_X4_PRO = {
    Board::XteinkX4Pro,
    "xteink_x4_pro",
    InputStyle::DigitalButtons,  // confirmed on hardware: plain active-low GPIO buttons, not the OEM ADC ladder
    DisplayController::SSD1677,
    800,
    480,
    // SSD1677 SPI — CONFIRMED ON HARDWARE via a raw bit-banged pin sweep (the panel
    // painted with these and only these): SCLK=12 MOSI=11 (write-only, no MISO)
    // CS=13 DC=18 RST=14 BUSY=6. Note vs the RE guesses: SCLK/MOSI are app0's order
    // (the app1 RE's 11/12 was backwards) and CS/DC are swapped from app0's 18/13.
    // The plain X4 OTP waveform develops the image — no custom LUT/voltages/PMIC
    // needed. GPIO1 also triggers a refresh when toggled (likely a panel power
    // enable), but the panel works without driving it, so powerEnable stays unset.
    {12, 11, 13, 18, 14, 6, PIN_UNASSIGNED},
    5000000,  // displaySpiHz: the OEM clocks the panel at 5 MHz (SPISettings 0x4C4B40). Conservative
              // vs the SSD1677 datasheet write max; raise once validated for faster refresh on hardware.
    // SD is native SDMMC (see the sdmmc field below) — the card is silent to SPI-mode CMD0 on
    // hardware. This SPI SdPins entry is retained only for its powerEnable=GPIO5, the SD enable
    // used by the SDMMC mount path. GPIO5 is ACTIVE-LOW: SdmmcBlockDevice pulses it HIGH→LOW
    // before each mount attempt and runs the card with it held LOW (matching the OEM mountSD;
    // holding it HIGH breaks every block read with 0x107). The bus pins (SCLK41 MISO40 MOSI42
    // CS45) are the SPI view of the same slot and are unused now that busWidth!=0 routes through
    // the SDMMC block device. Trailing false = powerEnable is active-LOW, so the sleep path drives
    // GPIO5 HIGH to power the card down.
    {41, 40, 42, 45, 5, true, 0, false},
    // Digital buttons, confirmed on hardware (watch-up edge test): two physical nav keys —
    // Left=GPIO0, Right=GPIO7 — plus Power=GPIO3, all active-LOW (INPUT_PULLUP, no rail needed).
    // The two keys map to the reader's page pair (Up=prev / Down=next), so Left→up, Right→down;
    // back/confirm come from the GT911 (touch + the capacitive Home key). NOTE: GPIO0 is a boot
    // strap — fine as a button as long as it isn't held during reset.
    // {back, confirm, left, right, up, down, power, powerActiveHigh}
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 7, 3, false},
    PIN_UNASSIGNED,  // batteryAdc: monitoring exists ("Battery Meter"/"Low battery") but pin not isolated
    PIN_UNASSIGNED,  // batteryChargeStatus
    2.0f,
    PIN_UNASSIGNED,  // usbDetect: USB-MSC/VBUS-detect present; GPIO10 is a candidate (unconfirmed)
    // GT911 touch on the SHARED I2C bus SDA39/SCL38 (with RTC 0x51 + CW2017 gauge 0x63),
    // INT=GPIO4 RST=GPIO10, addr 0x5D (INT held LOW at reset; alt 0x14), 400 kHz — INT/RST/addr
    // and the coord layout confirmed from app1's XTEink::GT911Driver ctor + reset routine via
    // Ghidra. The GT911 is mounted PORTRAIT (reports X:0..480, Y:0..800) on the 800x480 landscape
    // panel, so swapXY=true rotates the digitizer to the panel frame; rawMax describe the
    // post-swap 800x480 panel axes. Coords start at byte 0 of the 0x8150 read (X-lo at 0x8150,
    // track-id is at 0x814F, before the window) → gt911CoordsAtByte0=true. flipX/flipY pending a
    // hardware tap test (firmware applies none). {ctrl,sda,scl,irq,rst,addr,rawMinX,rawMaxX,...}
    {TouchController::Gt911, 39, 38, 4, 10, 0x5D, 0, 799, 0, 479, false, 0x14, false, true, PIN_UNASSIGNED,
     true, false, false, true},  // hasHomeKey: capacitive home pad under the bezel (GT911 key bit)
    // Frontlight: dual warm/cold LEDC PWM with color temperature (NVS lightWarmValue/
    // lightColdValue/lightCT/lightBri/lightOn). Recovered from the OEM LEDC init (IROM
    // 0x420a2130 → helper 0x420a20c0): two channels — GPIO8 on LEDC ch4 and GPIO9 on ch5 —
    // both at 10 kHz / 10-bit, active-HIGH (init drives the pin LOW = off, brightness raises
    // duty). The SDK's FrontlightConfig models ONE channel, so this carries GPIO8 as the
    // primary brightness pin, GPIO9 as the warm channel — FrontlightManager mixes them for
    // color-temperature control. Which of GPIO8/GPIO9 is physically warm vs cold is not yet
    // known; if reversed, the CT direction just inverts (user-flippable).
    {8, 10000, 10, true, 9},
    NO_AUDIO,
    NO_LEDS,
    NO_FLIP,  // panel mount transform pending hardware; native SSD1677 scan is 800x480 landscape
    // SD is native SDMMC, NOT SPI: the card the OEM reads is silent to SPI-mode CMD0.
    // CONFIRMED on hardware: 1-bit, slot 1, CLK=41 CMD=42 DAT0=40, internal pull-ups, 40 MHz.
    // D1/D2/D3 are UNUSED in 1-bit. Mounts reliably via SdmmcBlockDevice, which power-cycles
    // the GPIO5 enable (see the SPI SdPins powerEnable above) and validates a real sector-0
    // read per attempt. {clk,cmd,d0,d1,d2,d3,busWidth}
    {41, 42, 40, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 1},
    // CW2017 fuel gauge at I2C 0x63 on the SHARED touch/RTC bus SDA39/SCL38, 400 kHz, Wire.
    // BatteryMonitor uploads the 80-byte BATINFO battery profile (recovered from app1's
    // XTEink Cw2017PowerHal via Ghidra) if the gauge hasn't got one, then reads SoC from
    // reg 0x04. No charger IC on the gauge bus. {sda,scl,hz,gaugeAddr,chargerAddr,bus,type}
    {39, 38, 400000, 0x63, 0, 0, GaugeType::Cw2017},
    NO_MIC,
    // BM8563 RTC (PCF8563 register-compatible, class XTEink::BM8563Driver in the dump) at I2C
    // 0x51, sharing the GT911 touch bus SDA39/SCL38 at 400 kHz (recovered: driver init at IROM
    // 0x420a2834 adds device 0x51; the bus object is configured with {39,38,400000}). Bus 0
    // (Wire), matching the touch driver so both drive the same peripheral on the shared pins.
    {39, 38, 400000, 0x51, 0, 0, 0, RtcType::Pcf8563, ImuType::None},  // temp/hum + IMU: none
    1.2f,  // uiScale: 800x480 touch device — finger-sized chrome, like the other touch boards
    // Master peripheral-rail enable on GPIO1: the OEM board-init drives it HIGH first, before
    // any SPI/display/SD bring-up (recovered: standalone OUTPUT, level=1, acted on first in
    // board_begin at IROM 0x420a23dc). Carried as power.latch0 so holdPowerRails() asserts it
    // early — without it the panel rail and the SD slot both stay unpowered (the bring-up
    // symptom: EPD BUSY never asserts, SD returns 0xFF). GPIO2 is a second board-init output
    // driven LOW (role unknown); not modeled here. NOTE: GPIO1/GPIO2 are therefore NOT the ADC
    // button ladder — that earlier assumption was wrong; the ladder pins remain unconfirmed.
    {1}};

// Largest framebuffer (bytes) over the devices compiled into this build, derived
// from the profiles above. The display facade sizes its static framebuffer to
// this so one binary holds whichever panel is runtime-selected; a single-device
// build gets exactly that panel's size. Adding a device adds one term here — no
// device names leak into the display code.
constexpr uint32_t cmax(uint32_t a, uint32_t b) { return a > b ? a : b; }
constexpr uint32_t panelBytes(const BoardProfile& p) {
  return static_cast<uint32_t>(p.displayWidth / 8) * p.displayHeight;
}
constexpr uint32_t MAX_FRAMEBUFFER_BYTES = cmax(
    cmax(cmax(FREEINK_DEVICE_X4 ? panelBytes(XTEINK_X4) : 0u, FREEINK_DEVICE_X3 ? panelBytes(XTEINK_X3) : 0u),
         cmax(FREEINK_DEVICE_M5 ? panelBytes(M5STACK_PAPER_COLOR) : 0u,
              FREEINK_DEVICE_MURPHY ? panelBytes(MURPHY_M3) : 0u)),
    cmax(cmax(cmax(FREEINK_DEVICE_DELINK ? panelBytes(DE_LINK) : 0u,
                   FREEINK_DEVICE_LILYGO ? panelBytes(LILYGO_T5S3) : 0u),
              cmax(FREEINK_DEVICE_M5PAPER ? panelBytes(M5PAPER_V11) : 0u,
                   FREEINK_DEVICE_X4PRO ? panelBytes(XTEINK_X4_PRO) : 0u)),
         FREEINK_DEVICE_STICKY ? panelBytes(STICKY) : 0u));

// Compile-time default device — the profile ACTIVE starts as. With a single
// device in the build this is the only device; with several same-MCU devices it
// is the boot default until the consumer calls selectDevice().
#if FREEINK_DEVICE_M5
constexpr BoardProfile DEFAULT_DEVICE = M5STACK_PAPER_COLOR;
#elif FREEINK_DEVICE_MURPHY
constexpr BoardProfile DEFAULT_DEVICE = MURPHY_M3;
#elif FREEINK_DEVICE_DELINK
constexpr BoardProfile DEFAULT_DEVICE = DE_LINK;
#elif FREEINK_DEVICE_LILYGO
constexpr BoardProfile DEFAULT_DEVICE = LILYGO_T5S3;
#elif FREEINK_DEVICE_M5PAPER
constexpr BoardProfile DEFAULT_DEVICE = M5PAPER_V11;
#elif FREEINK_DEVICE_STICKY
constexpr BoardProfile DEFAULT_DEVICE = STICKY;
#elif FREEINK_DEVICE_X4PRO
constexpr BoardProfile DEFAULT_DEVICE = XTEINK_X4_PRO;
#elif FREEINK_DEVICE_X3 && !FREEINK_DEVICE_X4
constexpr BoardProfile DEFAULT_DEVICE = XTEINK_X3;  // X3-only binary
#else
// X4-only or the dual X3+X4 C3 binary: boot as X4, runtime-swap to X3 on detect.
constexpr BoardProfile DEFAULT_DEVICE = XTEINK_X4;
#endif

// Runtime-active profile. Defaults to DEFAULT_DEVICE — identical to the old
// compile-time behavior when only one device is in the build. A consumer that
// ships multiple same-MCU devices in one binary calls selectDevice() after its
// own hardware detection, before any pin is used.
inline BoardProfile ACTIVE = DEFAULT_DEVICE;

// Set ACTIVE to one of the devices compiled into this build. Returns false (and
// leaves ACTIVE unchanged) if `which` was not included via -DFREEINK_DEVICE_*.
inline bool selectDevice(Board which) {
  switch (which) {
#if FREEINK_DEVICE_X4
    case Board::XteinkX4:
      ACTIVE = XTEINK_X4;
      return true;
#endif
#if FREEINK_DEVICE_X3
    case Board::XteinkX3:
      ACTIVE = XTEINK_X3;
      return true;
#endif
#if FREEINK_DEVICE_M5
    case Board::M5StackPaperColor:
      ACTIVE = M5STACK_PAPER_COLOR;
      return true;
#endif
#if FREEINK_DEVICE_MURPHY
    case Board::MurphyM3:
      ACTIVE = MURPHY_M3;
      return true;
#endif
#if FREEINK_DEVICE_DELINK
    case Board::DeLink:
      ACTIVE = DE_LINK;
      return true;
#endif
#if FREEINK_DEVICE_LILYGO
    case Board::LilyGoT5S3:
      ACTIVE = LILYGO_T5S3;
      return true;
#endif
#if FREEINK_DEVICE_M5PAPER
    case Board::M5PaperV11:
      ACTIVE = M5PAPER_V11;
      return true;
#endif
#if FREEINK_DEVICE_STICKY
    case Board::Sticky:
      ACTIVE = STICKY;
      return true;
#endif
#if FREEINK_DEVICE_X4PRO
    case Board::XteinkX4Pro:
      ACTIVE = XTEINK_X4_PRO;
      return true;
#endif
    default:
      break;
  }
  return false;
}

inline bool isM5StackPaperColor() { return ACTIVE.board == Board::M5StackPaperColor; }
inline bool isMurphyM3() { return ACTIVE.board == Board::MurphyM3; }
inline bool isDeLink() { return ACTIVE.board == Board::DeLink; }
inline bool isM5PaperV11() { return ACTIVE.board == Board::M5PaperV11; }
inline bool isSticky() { return ACTIVE.board == Board::Sticky; }
inline bool isX4Pro() { return ACTIVE.board == Board::XteinkX4Pro; }
inline bool hasTouch() { return ACTIVE.touch.controller != TouchController::None; }
inline bool hasHomeKey() { return ACTIVE.touch.hasHomeKey; }
inline bool hasPwmFrontlight() { return ACTIVE.frontlight.gpio != PIN_UNASSIGNED; }
inline bool hasAudio() { return ACTIVE.audio.output != AudioOutput::None; }

// Assert the board's power-rail latch pins. Battery-latched boards (e.g. the
// Sticky) must call this first thing in setup() or the board powers off when
// the user releases the power button. Releasing the pins (driving them LOW)
// is a software power-off. No-op on boards without a latch.
inline void holdPowerRails() {
  for (const int8_t pin : {ACTIVE.power.latch0, ACTIVE.power.latch1}) {
    if (pin >= 0) {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, HIGH);
    }
  }
}

// Rescue the SD power rail before first display use. A previous firmware's
// sleep path may have latched the rail off with gpio_hold_en — a state that
// survives reset and reflashing — and on boards where SD shares the display's
// SPI bus an unpowered card clamps SCLK/MOSI so the panel never hears a
// command. Releases the hold, powers the card, and deselects its CS.
// SDCardManager::begin() does this itself; apps that skip SD should call this
// once before display.begin(). No-op on boards without a switched SD rail.
inline void releaseSdRail() {
  if (ACTIVE.sd.powerEnable >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(ACTIVE.sd.powerEnable));
    pinMode(ACTIVE.sd.powerEnable, OUTPUT);
    // Drive the enable to its ON level: HIGH for active-high rails, LOW for the
    // active-low ones (X4 Pro's GPIO5 powers the card while held LOW).
    digitalWrite(ACTIVE.sd.powerEnable, ACTIVE.sd.powerActiveHigh ? HIGH : LOW);
  }
  if (ACTIVE.sd.cs >= 0) {
    pinMode(ACTIVE.sd.cs, OUTPUT);
    digitalWrite(ACTIVE.sd.cs, HIGH);
  }
}
inline bool hasMic() { return ACTIVE.mic.input != MicInput::None; }
inline bool hasBuzzer() { return ACTIVE.audio.buzzer != PIN_UNASSIGNED; }
inline bool hasRtc() { return ACTIVE.sensors.rtcAddr != 0; }
inline bool hasTempHumidity() { return ACTIVE.sensors.tempHumidityAddr != 0; }
inline bool hasImu() { return ACTIVE.sensors.imuAddr != 0; }
inline bool hasLeds() { return ACTIVE.leds.data != PIN_UNASSIGNED && ACTIVE.leds.count > 0; }

}  // namespace BoardConfig
