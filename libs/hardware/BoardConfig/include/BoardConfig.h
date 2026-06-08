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

// --- 2) Coherence: exactly one MCU family, at least one device ---------------
#if !(FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3 || FREEINK_DEVICE_M5 ||         \
      FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_DELINK || FREEINK_DEVICE_LILYGO || \
      FREEINK_DEVICE_M5PAPER)
#error "FreeInk: no device selected. Pass at least one -DFREEINK_DEVICE_<NAME> (X4, X3, M5, MURPHY, DELINK, LILYGO, M5PAPER) in your build env — see platformio.sample.ini."
#endif
// Each device belongs to one MCU family; a binary targets exactly one. X3/X4 are
// ESP32-C3; M5 PaperColor/Murphy/de-link/LilyGo are ESP32-S3; M5Paper v1.1 is the
// classic ESP32 (ESP32-D0WDQ6). The three families differ in deep-sleep wakeup,
// SPI peripheral count, and toolchain, so they never share a binary.
#define FREEINK_MCU_C3 (FREEINK_DEVICE_X3 || FREEINK_DEVICE_X4)
#define FREEINK_MCU_S3 (FREEINK_DEVICE_M5 || FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_DELINK || FREEINK_DEVICE_LILYGO)
#define FREEINK_MCU_ESP32 (FREEINK_DEVICE_M5PAPER)
#if (FREEINK_MCU_C3 + FREEINK_MCU_S3 + FREEINK_MCU_ESP32) != 1
#error "FreeInk: all selected devices must share one MCU family — ESP32-C3 (X3/X4), ESP32-S3 (M5/Murphy/de-link/LilyGo), or ESP32 (M5Paper). Build one binary per family."
#endif

// --- 3) Derive panel drivers from the device set -----------------------------
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_DELINK
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
#define FREEINK_CAP_TOUCH (FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_M5PAPER)
#endif
#ifndef FREEINK_CAP_FRONTLIGHT
#define FREEINK_CAP_FRONTLIGHT (FREEINK_DEVICE_DELINK || FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_LILYGO)
#endif

// I2C fuel-gauge battery backend. Compiled in when a build contains a gauge
// device (X3's BQ27220, or LilyGo's BQ27220+BQ25896). Selection is then *runtime*
// per active profile (BatteryMonitor uses the gauge only when
// ACTIVE.batteryGauge.gaugeAddr != 0) — required because X3 (gauge) and X4 (ADC)
// share one C3 binary.
#ifndef FREEINK_BATTERY_I2C_GAUGE
#define FREEINK_BATTERY_I2C_GAUGE (FREEINK_DEVICE_X3 || FREEINK_DEVICE_LILYGO)
#endif
#ifndef FREEINK_CAP_COLOR
#define FREEINK_CAP_COLOR (FREEINK_DEVICE_M5)
#endif
#ifndef FREEINK_CAP_AUDIO
#define FREEINK_CAP_AUDIO 0
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

// SD transport. de-link is wired for 4-bit SDMMC; SdFat can't drive SDIO, so it
// gets a native esp-idf SDMMC block device behind SDCardManager. Every other
// board stays on SdFat-over-SPI. Override with -DFREEINK_SD_SDMMC=0/1.
#ifndef FREEINK_SD_SDMMC
#define FREEINK_SD_SDMMC (FREEINK_DEVICE_DELINK)
#endif

namespace BoardConfig {

// Physical device family. X3 and X4 are sibling devices on the same ESP32-C3
// board (identical pinout, different panel/size): both profiles compile into the
// C3 binary and one is chosen at runtime (setDisplayX3() -> selectDevice).
enum class Board : uint8_t { XteinkX4, XteinkX3, M5StackPaperColor, MurphyM3, DeLink, LilyGoT5S3, M5PaperV11 };

// How the board reports button presses.
enum class InputStyle : uint8_t {
  XteinkAdcLadder,         // resistor ladder on two ADC pins (X3/X4)
  DigitalButtons,          // plain active-low GPIO buttons
  DigitalConfirmBackHold,  // confirm held > N ms synthesizes BACK (M5 PaperColor)
  DigitalFiveKey,          // 3 physical GPIO keys + synthesized events (Murphy M3)
};

// Panel controller silicon. Drivers are selected from this at begin().
// LgfxEpd = a raw-parallel EPD with no on-glass controller, driven via LovyanGFX
// (e.g. ED047TC1 on LilyGo T5 S3).
enum class DisplayController : uint8_t { SSD1677, UC8253, ED2208, LgfxEpd, IT8951 };

// Optional capacitive touch controller.
enum class TouchController : uint8_t { None, Chsc6x, Gt911 };

// Optional audio output path. No current board ships audio; this is scaffolding
// so a future device fills in pins and the AudioManager lights up.
enum class AudioOutput : uint8_t { None, I2sDac, PwmBuzzer };

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

// I2C fuel-gauge / charger wiring (e.g. BQ27220 + BQ25896 on LilyGo T5 S3). When
// gaugeAddr != 0 (and FREEINK_BATTERY_I2C_GAUGE is set), BatteryMonitor reads the
// gauge over I2C instead of an ADC pin. chargerAddr is optional (0 = none) and
// only used for charge status.
struct BatteryGaugeConfig {
  int8_t i2cSda;
  int8_t i2cScl;
  uint32_t i2cHz;
  uint8_t gaugeAddr;    // BQ27220 = 0x55; 0 = no I2C gauge (use ADC)
  uint8_t chargerAddr;  // BQ25896 = 0x6B; 0 = none
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
  bool synthesizeConfirm;     // emit a CONFIRM button event on tap
  uint8_t i2cAddressAlt;      // alternate I2C address to probe (GT911 0x14; 0 = none)
  bool irqActiveLow;          // touch IRQ asserted LOW (CHSC6x)
  // GT911 point-frame layout: false = datasheet standard (track-id at 0x8150, so
  // coords start at byte 1); true = coords start at byte 0 (no track-id), as seen
  // on M5Paper's GT911 which boots without a reset/config dance. Ignored (CHSC6x).
  bool gt911CoordsAtByte0;
};

// PWM frontlight description (gpio == PIN_UNASSIGNED disables it).
struct FrontlightConfig {
  int8_t gpio;
  uint32_t pwmFrequency;
  uint8_t pwmResolutionBits;
  bool activeHigh;
};

// Audio output description (AudioOutput::None disables it). Scaffolding only —
// no current board populates real pins.
struct AudioConfig {
  AudioOutput output;
  int8_t bclk;    // I2S bit clock (unused for PWM buzzer)
  int8_t lrclk;   // I2S word select (unused for PWM buzzer)
  int8_t dout;    // I2S data out, or the PWM pin for a buzzer
  int8_t enable;  // amplifier enable / shutdown pin
  bool enableActiveHigh;
};

// How the panel is mounted relative to the driver's native scan. Any board injects
// its own mirroring here; a 180° rotation is mirrorX && mirrorY. (90°/270° need a
// software transpose — they swap width/height and aren't expressible by panel RAM
// addressing alone — so they are not a flag here.)
struct DisplayOrientation {
  bool mirrorX;  // reverse source/column (X) order
  bool mirrorY;  // reverse gate/row (Y) order
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
  int8_t usbDetect;
  TouchConfig touch;
  FrontlightConfig frontlight;
  AudioConfig audio;
  DisplayOrientation orientation;  // panel mount transform (mirrorX/mirrorY)
  SdmmcPins sdmmc;                 // 4-bit SDMMC wiring (busWidth 0 = use SPI/SdFat)
  BatteryGaugeConfig batteryGauge;  // I2C fuel gauge (gaugeAddr 0 = use ADC pin)
};

constexpr TouchConfig NO_TOUCH = {
    TouchController::None, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 0, 0, 0, 0, false, 0, false,
    false};

// LilyGo T5 S3 Pro Lite GT911 touch (shared I2C bus, native portrait 540x960).
// A named config ready to drop into a LilyGo board profile once its display
// driver lands. GT911 reports pixel coords directly, so raw range == panel size.
constexpr TouchConfig LILYGO_T5_PRO_GT911 = {
    TouchController::Gt911, 39, 40, 3, 9, 0x5D, 0, 539, 0, 959, false, 0x14, false, false};
constexpr FrontlightConfig NO_FRONTLIGHT = {PIN_UNASSIGNED, 0, 0, true};
constexpr AudioConfig NO_AUDIO = {AudioOutput::None, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, true};
constexpr DisplayOrientation NO_FLIP = {false, false};            // native scan
constexpr DisplayOrientation ROTATE_180 = {true, true};           // upside-down mount
constexpr DisplayOrientation MIRROR_X = {true, false};            // horizontal mirror
constexpr DisplayOrientation MIRROR_Y = {false, true};            // vertical mirror
constexpr SdmmcPins NO_SDMMC = {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED,
                                PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0};
constexpr BatteryGaugeConfig NO_GAUGE = {PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 0, 0};  // ADC battery

// --- Xteink X4 — ESP32-C3, SSD1677 (800x480) ---------------------------------
constexpr BoardProfile XTEINK_X4 = {
    Board::XteinkX4,
    "xteink_x4",
    InputStyle::XteinkAdcLadder,
    DisplayController::SSD1677,
    800,
    480,
    {8, 10, 21, 4, 5, 6, PIN_UNASSIGNED},
    0,  // displaySpiHz: 0 -> SSD1677 driver default (40 MHz)
    {PIN_UNASSIGNED, 7, PIN_UNASSIGNED, 12, PIN_UNASSIGNED, false, 0},
    {0, 1, 2, 3, 4, 5, 3, false},
    0,
    20,
    NO_TOUCH,
    NO_FRONTLIGHT,
    NO_AUDIO,
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
    0,  // displaySpiHz: 0 -> UC8253 driver default (16 MHz)
    {PIN_UNASSIGNED, 7, PIN_UNASSIGNED, 12, PIN_UNASSIGNED, false, 0},
    {0, 1, 2, 3, 4, 5, 3, false},
    0,
    20,
    NO_TOUCH,
    NO_FRONTLIGHT,
    NO_AUDIO,
    NO_FLIP,
    NO_SDMMC,
    {20, 0, 400000, 0x55, 0}};  // BQ27220 fuel gauge (0x55) on SDA20/SCL0; no charger IC

// --- M5Stack PaperColor — ESP32-S3, ED2208 color panel, M5PM1 PMIC -----------
constexpr BoardProfile M5STACK_PAPER_COLOR = {
    Board::M5StackPaperColor,
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
    NO_TOUCH,
    NO_FRONTLIGHT,
    NO_AUDIO,
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
    PIN_UNASSIGNED,
    PIN_UNASSIGNED,
    {TouchController::Chsc6x, 13, 12, 44, 45, 0x2e, 24, 224, 24, 392, false, 0, true, false},
    {48, 25000, 10, true},
    NO_AUDIO,
    NO_FLIP,
    NO_SDMMC,
    NO_GAUGE};

// --- de-link (X4-class GDEQ0426T82 panel on ESP32-S3) — SSD1677 + frontlight ---
// Reuses the SSD1677 driver (same controller/panel as X4); differs at the board
// level: S3 MCU, SDMMC SD, MCP73832 charge-sense, warm/cool PWM frontlight.
//
// Orientation: this profile ships NO_FLIP (X4 orientation). A board that mounts
// the panel rotated sets `ROTATE_180` (or a mirror) here, and the SSD1677 driver
// applies it in hardware (mirrorX via RAM addressing, mirrorY via gate scan). Any
// board injects its own mount transform the same way.
constexpr BoardProfile DE_LINK = {
    Board::DeLink,
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
    4,  // batteryAdc GPIO4 (charge-status GPIO8 is passed to BatteryMonitor by firmware)
    PIN_UNASSIGNED,
    NO_TOUCH,
    // Primary brightness PWM (GPIO5). Warm/cool/rail/fault pins (GPIO6/7/17/18)
    // are not driven.
    {5, 20000, 8, true},
    NO_AUDIO,
    NO_FLIP,
    {39, 40, 38, 48, 42, 41, 4},  // SDMMC 4-bit: CLK39 CMD40 D0=38 D1=48 D2=42 D3=41
    NO_GAUGE};

// --- LilyGo T5 S3 4.7" (ED047TC1 raw-parallel EPD) — ESP32-S3 -----------------
// 960x540 16-gray raw parallel panel driven via LovyanGFX (FREEINK_DRIVER_LGFX_EPD);
// the panel can't power up without the board's PMIC (TPS65185) + PCA9535 expander
// sequence, which the board injects through LgfxEpdConfig::power (see the LilyGo
// support doc). Geometry is the physical scan size; the driver's rotation puts the
// reader UI in portrait. Display + GT911 touch + PWM backlight + the I2C fuel gauge
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
     PIN_UNASSIGNED},                   // no SPI display pins: parallel bus lives in LgfxEpdConfig
    0,                                  // displaySpiHz n/a (external bus)
    {14, 21, 13, 12, PIN_UNASSIGNED, false, 0},  // SD over SPI: SCLK14 MISO21 MOSI13 CS12
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0, false},  // power=BOOT (GPIO0), active-low
    PIN_UNASSIGNED,  // batteryAdc: none — uses the I2C fuel gauge below
    PIN_UNASSIGNED,
    LILYGO_T5_PRO_GT911,  // GT911 touch (SDA39 SCL40 INT3 RST9, 0x5D, 540x960)
    {11, 5000, 8, true},  // backlight: BL_EN GPIO11, PWM 5 kHz / 8-bit, active-high
    NO_AUDIO,
    NO_FLIP,
    NO_SDMMC,
    {39, 40, 400000, 0x55, 0x6B}};  // BQ27220 gauge (0x55) + BQ25896 charger (0x6B) on SDA39/SCL40

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
// System note (consumer/board init, not the SDK): M5Paper latches its own power
// through a MOSFET on GPIO2 — it must be driven HIGH at boot or the device powers
// off the moment USB is unplugged. EXT power (GPIO5) and EPD power (GPIO23) gate
// the peripheral and panel rails. The IT8951 driver asserts GPIO23 (the EPD rail);
// the main-power latch is the firmware's responsibility — see platformio.sample.ini.
constexpr BoardProfile M5PAPER_V11 = {
    Board::M5PaperV11,
    "m5paper_v11",
    InputStyle::DigitalButtons,
    DisplayController::IT8951,
    960,  // landscape framebuffer (byte-aligned); driver rotates onto the 540x960 panel
    540,
    {14, 12, 15, PIN_UNASSIGNED, PIN_UNASSIGNED, 27, 23},  // SCLK14 MOSI12 CS15, no DC/RST, HRDY27, EPD_PWR_EN23
    0,                                  // displaySpiHz: 0 -> IT8951 driver default (10 MHz)
    {14, 13, 12, 4, PIN_UNASSIGNED, false, 20000000},  // SD shares the SPI bus: SCLK14 MISO13 MOSI12, CS4. 20 MHz
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
    // GT911 touch: panel-native portrait raw range (540x960), shared I2C SDA21/SCL22,
    // INT36, address 0x5D (alt 0x14). No reset GPIO is exposed on M5Paper.
    {TouchController::Gt911, 21, 22, 36, PIN_UNASSIGNED, 0x5D, 0, 539, 0, 959, false, 0x14, false,
     true},  // GT911 reports coords at byte 0 (no track-id) on M5Paper
    NO_FRONTLIGHT,
    NO_AUDIO,
    NO_FLIP,
    NO_SDMMC,
    NO_GAUGE};

// Largest framebuffer (bytes) over the devices compiled into this build, derived
// from the profiles above. The display facade sizes its static framebuffer to
// this so one binary holds whichever panel is runtime-selected; a single-device
// build gets exactly that panel's size. Adding a device adds one term here — no
// device names leak into the display code.
constexpr uint32_t cmax(uint32_t a, uint32_t b) { return a > b ? a : b; }
constexpr uint32_t panelBytes(const BoardProfile& p) {
  return static_cast<uint32_t>(p.displayWidth / 8) * p.displayHeight;
}
constexpr uint32_t MAX_FRAMEBUFFER_BYTES =
    cmax(cmax(cmax(FREEINK_DEVICE_X4 ? panelBytes(XTEINK_X4) : 0u,
                   FREEINK_DEVICE_X3 ? panelBytes(XTEINK_X3) : 0u),
              cmax(FREEINK_DEVICE_M5 ? panelBytes(M5STACK_PAPER_COLOR) : 0u,
                   FREEINK_DEVICE_MURPHY ? panelBytes(MURPHY_M3) : 0u)),
         cmax(cmax(FREEINK_DEVICE_DELINK ? panelBytes(DE_LINK) : 0u,
                   FREEINK_DEVICE_LILYGO ? panelBytes(LILYGO_T5S3) : 0u),
              FREEINK_DEVICE_M5PAPER ? panelBytes(M5PAPER_V11) : 0u));

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
    case Board::XteinkX4: ACTIVE = XTEINK_X4; return true;
#endif
#if FREEINK_DEVICE_X3
    case Board::XteinkX3: ACTIVE = XTEINK_X3; return true;
#endif
#if FREEINK_DEVICE_M5
    case Board::M5StackPaperColor: ACTIVE = M5STACK_PAPER_COLOR; return true;
#endif
#if FREEINK_DEVICE_MURPHY
    case Board::MurphyM3: ACTIVE = MURPHY_M3; return true;
#endif
#if FREEINK_DEVICE_DELINK
    case Board::DeLink: ACTIVE = DE_LINK; return true;
#endif
#if FREEINK_DEVICE_LILYGO
    case Board::LilyGoT5S3: ACTIVE = LILYGO_T5S3; return true;
#endif
#if FREEINK_DEVICE_M5PAPER
    case Board::M5PaperV11: ACTIVE = M5PAPER_V11; return true;
#endif
    default: break;
  }
  return false;
}

inline bool isM5StackPaperColor() { return ACTIVE.board == Board::M5StackPaperColor; }
inline bool isMurphyM3() { return ACTIVE.board == Board::MurphyM3; }
inline bool isDeLink() { return ACTIVE.board == Board::DeLink; }
inline bool isM5PaperV11() { return ACTIVE.board == Board::M5PaperV11; }
inline bool hasTouch() { return ACTIVE.touch.controller != TouchController::None; }
inline bool hasPwmFrontlight() { return ACTIVE.frontlight.gpio != PIN_UNASSIGNED; }
inline bool hasAudio() { return ACTIVE.audio.output != AudioOutput::None; }

}  // namespace BoardConfig
