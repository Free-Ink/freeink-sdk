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
// supply their own detection in the consumer. X3/X4 share the ESP32-C3 profile
// and are picked at runtime via EInkDisplay::setDisplayX3(); ACTIVE defaults to
// a compile-time default and only changes if the consumer calls selectDevice().

#include <Arduino.h>

// ============================================================================
// Build composition — devices x capabilities
// ============================================================================

// --- 1) Default the device set from legacy -DBOARD_* macros when unset -------
#if !defined(FREEINK_DEVICE_X4) && !defined(FREEINK_DEVICE_X3) &&            \
    !defined(FREEINK_DEVICE_M5) && !defined(FREEINK_DEVICE_MURPHY) &&        \
    !defined(FREEINK_DEVICE_DELINK)
#if defined(CROSSPOINT_BOARD_M5STACK_PAPERCOLOR) || defined(BOARD_M5STACK_PAPERCOLOR)
#define FREEINK_DEVICE_M5 1
#elif defined(CROSSPOINT_BOARD_MURPHY_M3) || defined(BOARD_MURPHY_M3)
#define FREEINK_DEVICE_MURPHY 1
#elif defined(CROSSPOINT_BOARD_DELINK) || defined(BOARD_DELINK)
#define FREEINK_DEVICE_DELINK 1
#else
#define FREEINK_DEVICE_X4 1
#define FREEINK_DEVICE_X3 1
#endif
#endif

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

// --- 2) MCU coherence: one binary runs on one MCU ----------------------------
#if (FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3) &&                              \
    (FREEINK_DEVICE_M5 || FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_DELINK)
#error "FreeInk: cannot combine ESP32-C3 devices (X3/X4) with ESP32-S3 devices (M5/Murphy/de-link) in one binary."
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
#if FREEINK_DEVICE_M5
#define FREEINK_DRIVER_ED2208 1
#else
#define FREEINK_DRIVER_ED2208 0
#endif
#if FREEINK_DEVICE_MURPHY
#define FREEINK_DRIVER_UC8253_MURPHY 1
#else
#define FREEINK_DRIVER_UC8253_MURPHY 0
#endif

// --- 4) Derive default capabilities (override with -DFREEINK_CAP_*=0/1) -------
#ifndef FREEINK_CAP_TOUCH
#define FREEINK_CAP_TOUCH (FREEINK_DEVICE_MURPHY)
#endif
#ifndef FREEINK_CAP_FRONTLIGHT
#define FREEINK_CAP_FRONTLIGHT (FREEINK_DEVICE_DELINK || FREEINK_DEVICE_MURPHY)
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

namespace BoardConfig {

// Physical device family. X3 is deliberately absent: it shares XTEINK_X4's
// profile and is selected at runtime, not via a distinct board macro.
enum class Board : uint8_t { XteinkX4, M5StackPaperColor, MurphyM3, DeLink };

// How the board reports button presses.
enum class InputStyle : uint8_t {
  XteinkAdcLadder,         // resistor ladder on two ADC pins (X3/X4)
  DigitalButtons,          // plain active-low GPIO buttons
  DigitalConfirmBackHold,  // confirm held > N ms synthesizes BACK (M5 PaperColor)
  DigitalFiveKey,          // 3 physical GPIO keys + synthesized events (Murphy M3)
};

// Panel controller silicon. Drivers are selected from this at begin().
enum class DisplayController : uint8_t { SSD1677, UC8253, ED2208 };

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
};

constexpr TouchConfig NO_TOUCH = {
    TouchController::None, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 0, 0, 0, 0, false};
constexpr FrontlightConfig NO_FRONTLIGHT = {PIN_UNASSIGNED, 0, 0, true};
constexpr AudioConfig NO_AUDIO = {AudioOutput::None, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, true};

// --- Xteink X4 (and X3, runtime-selected) — ESP32-C3, SSD1677 ----------------
constexpr BoardProfile XTEINK_X4 = {
    Board::XteinkX4,
    "xteink_x4",
    InputStyle::XteinkAdcLadder,
    DisplayController::SSD1677,
    800,
    480,
    {8, 10, 21, 4, 5, 6, PIN_UNASSIGNED},
    0,  // displaySpiHz: 0 -> SSD1677 driver default (40 MHz); X3 uses UC8253 default (10 MHz)
    {PIN_UNASSIGNED, 7, PIN_UNASSIGNED, 12, PIN_UNASSIGNED, false, 0},
    {0, 1, 2, 3, 4, 5, 3, false},
    0,
    20,
    NO_TOUCH,
    NO_FRONTLIGHT,
    NO_AUDIO};

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
    NO_AUDIO};

// --- Murphy M3 (CrowPanel 3.7") — UC8253, CHSC6x touch, PWM frontlight --------
constexpr BoardProfile MURPHY_M3 = {
    Board::MurphyM3,
    "murphy_m3",
    InputStyle::DigitalFiveKey,
    DisplayController::UC8253,
    240,
    416,
    {4, 3, 5, 6, 7, 8, PIN_UNASSIGNED},
    0,  // displaySpiHz: 0 -> UC8253 driver default (10 MHz)
    {39, 13, 40, 10, PIN_UNASSIGNED, true, 0},
    {PIN_UNASSIGNED, 0, PIN_UNASSIGNED, PIN_UNASSIGNED, 1, 2, 0, false},
    PIN_UNASSIGNED,
    PIN_UNASSIGNED,
    {TouchController::Chsc6x, 13, 12, 44, 45, 0x2e, 24, 224, 24, 392, true},
    {48, 25000, 10, true},
    NO_AUDIO};

// --- de-link (X4-class GDEQ0426T82 panel on ESP32-S3) — SSD1677 + frontlight ---
// Reuses the SSD1677 driver (same controller/panel as X4); differs at the board
// level: S3 MCU, SDMMC SD, MCP73832 charge-sense, warm/cool PWM frontlight,
// optional vertical flip (-DFREEINK_DISPLAY_FLIPPED).
constexpr BoardProfile DE_LINK = {
    Board::DeLink,
    "de_link",
    InputStyle::XteinkAdcLadder,
    DisplayController::SSD1677,
    800,
    480,
    {8, 10, 21, 4, 5, 6, PIN_UNASSIGNED},
    0,  // displaySpiHz: SSD1677 default (40 MHz)
    // SDMMC 4-bit (CLK39/CMD40/D0=38/D1=48/D2=42/D3=41). The SdFat/SPI SD backend
    // can't drive SDMMC yet — an SDMMC FsFile backend is a documented follow-up.
    {39, 38, 40, 41, PIN_UNASSIGNED, true, 0},
    {0, 1, 2, 3, 4, 5, 3, true},  // power button active-HIGH (INPUT_PULLDOWN) on de-link
    4,  // batteryAdc GPIO4 (charge-status GPIO8 is passed to BatteryMonitor by firmware)
    PIN_UNASSIGNED,
    NO_TOUCH,
    // Primary brightness PWM (GPIO5). Warm/cool/rail/fault pins (GPIO6/7/17/18)
    // need the richer FrontlightManager path (follow-up).
    {5, 20000, 8, true},
    NO_AUDIO};

// Compile-time default device — the profile ACTIVE starts as. With a single
// device in the build this is the only device; with several same-MCU devices it
// is the boot default until the consumer calls selectDevice().
#if FREEINK_DEVICE_M5
constexpr BoardProfile DEFAULT_DEVICE = M5STACK_PAPER_COLOR;
#elif FREEINK_DEVICE_MURPHY
constexpr BoardProfile DEFAULT_DEVICE = MURPHY_M3;
#elif FREEINK_DEVICE_DELINK
constexpr BoardProfile DEFAULT_DEVICE = DE_LINK;
#else
constexpr BoardProfile DEFAULT_DEVICE = XTEINK_X4;  // X3 and X4 share this profile
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
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
    case Board::XteinkX4: ACTIVE = XTEINK_X4; return true;
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
    default: break;
  }
  return false;
}

inline bool isM5StackPaperColor() { return ACTIVE.board == Board::M5StackPaperColor; }
inline bool isMurphyM3() { return ACTIVE.board == Board::MurphyM3; }
inline bool isDeLink() { return ACTIVE.board == Board::DeLink; }
inline bool hasTouch() { return ACTIVE.touch.controller != TouchController::None; }
inline bool hasPwmFrontlight() { return ACTIVE.frontlight.gpio != PIN_UNASSIGNED; }
inline bool hasAudio() { return ACTIVE.audio.output != AudioOutput::None; }

}  // namespace BoardConfig
