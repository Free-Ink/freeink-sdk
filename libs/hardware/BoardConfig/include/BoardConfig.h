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
#include <esp_rom_sys.h>  // esp_rom_printf in holdPowerRails()

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
#ifndef FREEINK_DEVICE_X4CLASSIC
#define FREEINK_DEVICE_X4CLASSIC 0
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
#ifndef FREEINK_DEVICE_PAPERMONO
#define FREEINK_DEVICE_PAPERMONO 0
#endif
#ifndef FREEINK_DEVICE_PAPERS3
#define FREEINK_DEVICE_PAPERS3 0
#endif
#ifndef FREEINK_DEVICE_MURPHY_M4
#define FREEINK_DEVICE_MURPHY_M4 0
#endif
#ifndef FREEINK_DEVICE_EEGO_A4
#define FREEINK_DEVICE_EEGO_A4 0
#endif

// --- 2) Coherence: exactly one MCU family, at least one device ---------------
#if !(FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3 || FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_M5 || \
      FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_DELINK || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_M5PAPER ||               \
      FREEINK_DEVICE_STICKY || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_PAPERS3 || FREEINK_DEVICE_MURPHY_M4 ||         \
      FREEINK_DEVICE_EEGO_A4)
#error \
    "FreeInk: no device selected. Pass at least one -DFREEINK_DEVICE_<NAME> (X4, X3, X4PRO, X4CLASSIC, M5, MURPHY, DELINK, LILYGO, M5PAPER, STICKY, PAPERMONO, PAPERS3, MURPHY_M4, EEGO_A4) in your build env — see platformio.sample.ini."
#endif
// Each device belongs to one MCU family; a binary targets exactly one. X3/X4 are
// ESP32-C3; M5 PaperColor/Murphy/de-link/LilyGo are ESP32-S3; M5Paper v1.1 is the
// classic ESP32 (ESP32-D0WDQ6). The three families differ in deep-sleep wakeup,
// SPI peripheral count, and toolchain, so they never share a binary.
#define FREEINK_MCU_C3 (FREEINK_DEVICE_X3 || FREEINK_DEVICE_X4)
#define FREEINK_MCU_S3                                                                                    \
  (FREEINK_DEVICE_M5 || FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_DELINK || FREEINK_DEVICE_LILYGO ||        \
   FREEINK_DEVICE_STICKY || FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO ||  \
   FREEINK_DEVICE_PAPERS3 || FREEINK_DEVICE_MURPHY_M4 || FREEINK_DEVICE_EEGO_A4)
#define FREEINK_MCU_ESP32 (FREEINK_DEVICE_M5PAPER)
#if (FREEINK_MCU_C3 + FREEINK_MCU_S3 + FREEINK_MCU_ESP32) != 1
#error \
    "FreeInk: all selected devices must share one MCU family — ESP32-C3 (X3/X4), ESP32-S3 (M5/Murphy/de-link/LilyGo/Sticky/X4Pro), or ESP32 (M5Paper). Build one binary per family."
#endif

// --- 3) Derive panel drivers from the device set -----------------------------
// Sticky reuses SSD1677: its 800x480 panel rides a 24-pin FPC whose GDR/RESE/BS1
// + dual VSH1/VSH2 + external VGH/VGL/VSL/VCOM charge pump is the SSD1677
// application circuit (same controller + resolution as X4 / de-link).
// X4 Pro is a distinct ESP32-S3 device (NOT the C3 X4): its 800x480 panel may
// use SSD1677, UC8179, or UC8279, recovered from OEM firmware and hardware
// references — see docs/xteink-x4pro-support.md.
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_DELINK || FREEINK_DEVICE_STICKY || FREEINK_DEVICE_X4PRO || \
    FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_MURPHY_M4
#define FREEINK_DRIVER_SSD1677 1
#else
#define FREEINK_DRIVER_SSD1677 0
#endif
#if FREEINK_DEVICE_X3
#define FREEINK_DRIVER_UC8253_X3 1
#else
#define FREEINK_DRIVER_UC8253_X3 0
#endif
// UltraChip controller variants. Newer batches of several Xteink panels ship an
// UltraChip controller in place of the original. Both are in the UC81xx KW
// command family but are separate drivers (different power/LUT bring-up):
//   * UC8279d — X3 (792x528), replaces the UC8253. Runs pure OTP waveforms.
//   * UC8179  — X4 / X4 Pro (800x480), replaces the SSD1677. Needs an explicit
//     PLL/booster/VCOM bring-up.
//   * UC8279 (800x480) — a second UltraChip variant of the X4 Pro panel
//     (LUT_VER 0x02/0x68); its own driver (different PSR/PLL init, 1-byte CDI,
//     gate offset, inverted AA planes).
// Which controller a given unit runs is resolved at boot by the display-bus
// probe (0x70 VER readback; NVS hw_calib/screenType is diagnostics-only) and the
// matching driver is selected before display begin(). Link each driver wherever
// a batch might carry it.
#if FREEINK_DEVICE_X3
#define FREEINK_DRIVER_UC8279 1
#else
#define FREEINK_DRIVER_UC8279 0
#endif
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC
#define FREEINK_DRIVER_UC8179 1
#define FREEINK_DRIVER_UC8279_X4 1
#else
#define FREEINK_DRIVER_UC8179 0
#define FREEINK_DRIVER_UC8279_X4 0
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
// LilyGo T5 S3 and M5Stack PaperS3: raw-parallel ED047TC1 via LovyanGFX (M5GFX).
// External-bus driver; each board injects its own bus pins/power in an
// LgfxEpdConfig (PaperS3's rails are plain GPIOs, LilyGo's ride a PMIC+expander).
#if FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_PAPERS3
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
#if FREEINK_DEVICE_PAPERMONO
#define FREEINK_DRIVER_PAPER_MONO 1
#else
#define FREEINK_DRIVER_PAPER_MONO 0
#endif
// EEGO Reader A4: UC8279C (768x552), UC81xx KW-family sibling of the UC8279 X4 driver.
#if FREEINK_DEVICE_EEGO_A4
#define FREEINK_DRIVER_UC8279C 1
#else
#define FREEINK_DRIVER_UC8279C 0
#endif

// --- 4) Derive default capabilities (override with -DFREEINK_CAP_*=0/1) -------
#ifndef FREEINK_CAP_TOUCH
#define FREEINK_CAP_TOUCH                                                                               \
  (FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_M5PAPER || FREEINK_DEVICE_STICKY || \
   FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_PAPERS3 || FREEINK_DEVICE_MURPHY_M4 || \
   FREEINK_DEVICE_EEGO_A4)
#endif
#ifndef FREEINK_CAP_FRONTLIGHT
// EEGO A4's frontlight is an I2C LED driver (viaI2cLed), not LEDC PWM — the
// FrontlightManager I2C backend drives it.
#define FREEINK_CAP_FRONTLIGHT                                                                        \
  (FREEINK_DEVICE_DELINK || FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_X4PRO || \
   FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_MURPHY_M4 || FREEINK_DEVICE_EEGO_A4)
#endif
// Warm/cool color-temperature frontlight: a second warm PWM channel on top of
// the brightness one (FrontlightConfig::gpioWarm). Sub-capability of
// FREEINK_CAP_FRONTLIGHT — gates the warmth UI/settings out of single-channel
// builds (Paper Mono, de-link, Murphy, LilyGo). Within a multi-device build
// the profile's gpioWarm stays the runtime truth (hasColorTemperature()).
#ifndef FREEINK_CAP_WARMLIGHT
#define FREEINK_CAP_WARMLIGHT (FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_MURPHY_M4 || FREEINK_DEVICE_EEGO_A4)
#endif
// USB Mass Storage ("USB Transfer" mode): exposes the SD card to a host over
// USB-MSC. OPT-IN (default off), NOT board-derived: it forces the build into
// USB-OTG mode (ARDUINO_USB_MODE=0 + CONFIG_TINYUSB_MSC_ENABLED), which changes
// how the USB serial console works — so a board enables it in its OWN env
// alongside those flags (e.g. X4 Pro adds -DFREEINK_CAP_USB_MSC=1
// -DARDUINO_USB_MODE=0 -DARDUINO_USB_CDC_ON_BOOT=1). Native-USB (ESP32-S3/C3
// OTG) targets only. When 0, UsbMassStorage links stub bodies and pulls in no
// TinyUSB/MSC code. Requires SDMMC/SPI storage exposing a block device.
#ifndef FREEINK_CAP_USB_MSC
#define FREEINK_CAP_USB_MSC 0
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
#define FREEINK_BATTERY_I2C_GAUGE                                                            \
  (FREEINK_DEVICE_X3 || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_STICKY || FREEINK_DEVICE_X4PRO || \
   FREEINK_DEVICE_X4CLASSIC)
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
#define FREEINK_CAP_MIC (FREEINK_DEVICE_STICKY || FREEINK_DEVICE_PAPERMONO)
#endif
// On-board I2C sensors. Each lib (Rtc / EnvironmentSensor / Imu) compiles its
// I2C driver only when its flag is set; otherwise it links stub bodies.
#ifndef FREEINK_CAP_RTC
#define FREEINK_CAP_RTC                                                                             \
  (FREEINK_DEVICE_X3 || FREEINK_DEVICE_STICKY || FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || \
   FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_PAPERS3 || FREEINK_DEVICE_LILYGO || FREEINK_DEVICE_EEGO_A4)
#endif
#ifndef FREEINK_CAP_TEMP_HUMIDITY
#define FREEINK_CAP_TEMP_HUMIDITY (FREEINK_DEVICE_STICKY)
#endif
#ifndef FREEINK_CAP_IMU
#define FREEINK_CAP_IMU (FREEINK_DEVICE_X3 || FREEINK_DEVICE_STICKY || FREEINK_DEVICE_X4CLASSIC)
#endif
// LEDC PWM buzzer (tone beeper). The Buzzer lib drives the AudioConfig.buzzer
// pin; on for boards that wire one (Sticky GPIO48, Murphy GPIO46, PaperS3
// GPIO21). Separate from FREEINK_CAP_AUDIO — a buzzer is a tone device, not a
// WAV/codec output.
#ifndef FREEINK_CAP_BUZZER
#define FREEINK_CAP_BUZZER \
  (FREEINK_DEVICE_STICKY || FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_PAPERS3)
#endif
#ifndef FREEINK_CAP_LED
#define FREEINK_CAP_LED (FREEINK_DEVICE_M5 || FREEINK_DEVICE_PAPERMONO)
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
#define FREEINK_FB_PSRAM (FREEINK_DEVICE_M5PAPER || FREEINK_DEVICE_PAPERMONO)
#endif

// SD transport. de-link (4-bit) and X4 Pro (1-bit) are wired for SDMMC; SdFat
// can't drive SDIO, so they get a native esp-idf SDMMC block device behind
// SDCardManager. Every other board stays on SdFat-over-SPI. The consumer's build
// must define USE_BLOCK_DEVICE_INTERFACE=1 for the SdFat FsVolume these mount on.
// Override with -DFREEINK_SD_SDMMC=0/1.
#ifndef FREEINK_SD_SDMMC
#define FREEINK_SD_SDMMC                                                                            \
  (FREEINK_DEVICE_DELINK || FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || \
   FREEINK_DEVICE_MURPHY_M4)
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
  XteinkX3Uc8279,  // newer X3 production run: same board/glass, UC8279d controller
  XteinkX4Pro,     // ESP32-S3 sibling of the C3 X4: SSD1677 + GT911 touch + warm/cold frontlight
  XteinkX4Classic,  // ESP32-S3 "X4 Classic" (X4C): same panel/glass as the X4 Pro but NO touch and
                    // NO frontlight — those pins become four extra discrete front keys
  M5StackPaperColor,
  MurphyM3,
  MurphyM4,
  DeLink,
  LilyGoT5S3,
  M5PaperV11,
  Sticky,
  PaperMono,
  M5PaperS3,  // ESP32-S3 sibling of M5Paper v1.1: same ED047TC1 glass, no IT8951 — raw parallel via LovyanGFX
  EegoA4,     // EEGO Reader A4: ESP32-S3, UC8279C 768x552 SPI panel, GSLX680 touch, PCF8563 RTC
};

// How the board reports button presses.
enum class InputStyle : uint8_t {
  XteinkAdcLadder,          // resistor ladder on two ADC pins (X3/X4)
  DigitalButtons,           // plain active-low GPIO buttons
  DigitalConfirmBackHold,   // confirm held > N ms synthesizes BACK (M5 PaperColor)
  DigitalConfirmPowerHold,  // confirm click, power hold on a shared GPIO
  DigitalFiveKey,           // 3 physical GPIO keys + synthesized events (Murphy M3)
  DigitalTwoButton,         // short up/down; holds synthesize back/confirm/power
};

// Panel controller silicon. Drivers are selected from this at begin().
// LgfxEpd = a raw-parallel EPD with no on-glass controller, driven via LovyanGFX
// (e.g. ED047TC1 on LilyGo T5 S3).
// UC8179 and UC8279 are the UltraChip siblings that newer batches ship in place
// of the original controller (UC8179/UC8279 for the X4 family's SSD1677, UC8279d
// for the X3's UC8253). Same UC81xx KW command family, separate drivers. Which
// one a unit carries is resolved at boot by the display-bus probe (0x70 VER /
// 0x71 FLG read, which SSD1677 lacks; VER byte2 LUT_VER tells UC8179 from
// UC8279). NVS hw_calib/screenType is read for diagnostics only. See
// XteinkDetect::applyXteinkDisplayController.
// Paper Mono uses SSD1677 silicon with a board-specific driver and host-authored
// waveforms, so its profile reports the actual controller here.
enum class DisplayController : uint8_t {
  SSD1677 = 0,
  UC8253 = 2,
  ED2208 = 3,
  LgfxEpd = 4,
  IT8951 = 5,
  UC8279 = 6,
  UC8179 = 7,
  UC8279C = 8  // EEGO A4 768x552 sibling of the UC8279 family (Uc8279cA4Driver)
};

// Optional capacitive touch controller.
enum class TouchController : uint8_t { None, Chsc6x, Gt911, Ft5x06, Ft6336u, Gslx680 };

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
  // Touch power-rail enable. PIN_UNASSIGNED on boards whose touch controller is always
  // powered; otherwise driven to its ON level before the reset/probe on boards that
  // gate it (e.g. Sticky's active-high TOUCH_EN, or the X4 Pro's active-low GPIO2).
  // Default keeps existing initializers valid.
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
  // Polarity of powerEnable. true (default) = active-high (drive HIGH to power the
  // controller). false = active-LOW (drive LOW to power it, e.g. X4 Pro's GPIO2). The
  // reset path drives the ON level; the sleep path drives the OFF level.
  bool powerEnableActiveHigh = true;
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
  // Paper Mono: the frontlight PWM is generated by the M5PM1 PMIC (its GPIO3
  // routed to alt-function PWM0 drives an AW9967 boost LED driver), not by an
  // ESP LEDC channel. `gpio` stays PIN_UNASSIGNED (ESP GPIO3 is a button
  // there!); FrontlightManager talks to the PMIC over I2C instead. The AW9967
  // is fed from the EPD rail, so the frontlight only lights while EPD power is
  // on. pwmFrequency still applies (PM1 PWM_FREQ register); resolution is the
  // PM1's fixed 12 bits.
  bool viaPm1Pwm = false;
};

// I2C frontlight controller (LM3630A on the EEGO A4). The controller is driven
// over a board I2C bus with a separate enable GPIO; the enable is also the
// hardware probe: an unpopulated optional circuit (some retail A4 units ship
// without a frontlight) never ACKs, so FrontlightManager only reports present()
// after a successful probe.
enum class I2cFrontlightController : uint8_t { None, Lm3630a };
struct I2cFrontlightConfig {
  I2cFrontlightController controller;
  int8_t sda;
  int8_t scl;
  uint32_t i2cHz;
  uint8_t address;
  int8_t enable;
};
constexpr I2cFrontlightConfig NO_I2C_FRONTLIGHT = {I2cFrontlightController::None, PIN_UNASSIGNED, PIN_UNASSIGNED, 0,
                                                   0, PIN_UNASSIGNED};

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
  // Paper Mono: one discrete RGB LED with no addressable controller — red is
  // the M5PM1's LED output (PWR_CFG bit 4), green/blue are M5IOE1 pins IO8/IO9.
  // On/off per channel (no per-channel intensity); `data` stays PIN_UNASSIGNED.
  bool paperMonoDiscrete = false;
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

enum class RtcType : uint8_t { None, Pcf8563, Ds3231, Rx8130 };
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
  // Battery-charger enable input (e.g. the Sticky's EN_BAT_CHGn -> BQ25616 /CE
  // on GPIO39). holdPowerRails() drives it to its active level and latches it
  // with gpio_hold_en so the charger stays enabled awake AND through deep sleep.
  // Left unmapped, an S3 JTAG-group pin like GPIO39 keeps its reset-default weak
  // pull-up while the firmware runs — /CE sits high and the device won't charge
  // until sleep isolates the pad and lets the line float back to enabled.
  int8_t chargeEnable = PIN_UNASSIGNED;
  bool chargeEnableActiveHigh = false;  // "n"-suffixed enables are active-low
};

// Panel rows/columns the device's bezel physically overlaps, in the panel's
// native portrait frame; firmware keeps content out of them. The default is
// the value CrossPoint historically hardcoded for every board (tuned on the
// X4 bezel) — override per profile as boards are measured.
struct ViewableInsets {
  uint8_t top = 9;
  uint8_t right = 3;
  uint8_t bottom = 3;
  uint8_t left = 3;
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
  // Panel-controller variant byte, filled in at boot by the display probe when it
  // matters (UC8279 800x480: VER byte2 LUT_VER, 0x02 vs 0x68 — selects which AA
  // waveform table the driver uploads). 0 = not probed / not applicable.
  uint8_t displayControllerVariant = 0;
  // Bezel-covered edge insets. Defaulted so existing profiles need no change;
  // a measured board overrides it.
  ViewableInsets viewableInsets = {};
  // Polarity of batteryChargeStatus. Default is the MCP73832-style /STAT that
  // every earlier board uses: open-drain, LOW = charging, read with the internal
  // pull-up. true = the line is push-pull driven HIGH while charging and carries
  // no pull (the X4 Pro's GPIO21, recovered from the stock Cw2017PowerHal —
  // stock configures it input/no-pull and reports the raw level).
  bool batteryChargeStatusActiveHigh = false;
  // I2C frontlight (LM3630A). Defaulted so existing profiles need no change;
  // a board with one sets it (EEGO A4).
  I2cFrontlightConfig i2cFrontlight = NO_I2C_FRONTLIGHT;
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
// hasHomeKey=true: the board HAS a capacitive home key below the panel. The vendor
// wiki's button list ("RST + BOOT + IO48 + PWR") omits it entirely, so it was found
// by tracing the GT911 status bit (0x10) on hardware. InputManager reads that bit
// unconditionally, so detection always worked -- it was the consumers gated on this
// flag (wasHomeGesture()/wasHomeKeyHold()) that discarded every press. On a board
// with one physical nav key that is a real loss.
constexpr TouchConfig LILYGO_T5_PRO_GT911 = {
    TouchController::Gt911, 39,   40,    3,    9, 0x5D, 0, 959, 0, 539, false, 0x14, false, true,
    PIN_UNASSIGNED,         true, false, true, true};  // powerEnable, swapXY, flipX, flipY, hasHomeKey
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
// M5Stack PaperS3 has no output codec — just the LEDC buzzer on GPIO21 (per the
// official pin map and M5Unified's buzzer speaker config). Same shape as Sticky.
constexpr AudioConfig M5_PAPERS3_AUDIO = {AudioOutput::None,    PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED,
                                          PIN_UNASSIGNED,       PIN_UNASSIGNED, true,           PIN_UNASSIGNED,
                                          PIN_UNASSIGNED,       PIN_UNASSIGNED, 0,              21};
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
                                    NO_GAUGE,
                                    NO_MIC,
                                    NO_SENSORS,
                                    1.0f,
                                    // GPIO13 gates the battery MOSFET. Known units self-latch through a
                                    // pull once the power button bridges the rail, so firmware never had
                                    // to assert it — but at least one hardware revision in the field does
                                    // not self-latch and stays powered only while the button is held.
                                    // Asserting the latch is a no-op on self-latching units. Driving it
                                    // LOW is the battery power-off (see consumers' deep-sleep path).
                                    {13, PIN_UNASSIGNED}};

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
    // powerEnable=GPIO13 = the X3 SD-rail power switch (active-high; HIGH at boot
    // powers the card, the sleep path drives it LOW). Confirmed by X3 factory-firmware
    // RE: setup() does digitalWrite(13,HIGH); every deep-sleep does digitalWrite(13,LOW).
    // Without declaring it, powerDownRailsForSleep() has no X3 SD enable to cut, so the
    // card stays powered through sleep -> battery drain. Shares the display SPI bus
    // (SCLK 8 / MOSI 10); MISO 7, CS 12. NOTE: X4 uses the same GPIO13 but keeps it as
    // power.latch0 (driven by the consumer's sleep path), so it is left unchanged.
    {PIN_UNASSIGNED, 7, PIN_UNASSIGNED, 12, 13, false, 0},
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

// --- Xteink X3 (UC8279d run) — ESP32-C3, UC8279d (792x528) -------------------
// Newer X3 production units swap the UC8253 for a UC8279d ("d_B" silicon; the
// TFT-module UltraChip BWR part driven in KW mode) on the same board, glass and
// pinout. Everything except the panel controller is inherited from XTEINK_X3;
// which sibling is running is fingerprinted at boot via the XteinkDetect
// display-controller probe (UC8279 VER/FLG readback). UC8279 serial write
// timing is also rated to 20 MHz ("Clock rate up to 20MHz").
constexpr BoardProfile XTEINK_X3_UC8279 = {
    Board::XteinkX3Uc8279,
    "xteink_x3_uc8279",
    InputStyle::XteinkAdcLadder,
    DisplayController::UC8279,
    792,
    528,
    {8, 10, 21, 4, 5, 6, PIN_UNASSIGNED},
    20000000,
    {PIN_UNASSIGNED, 7, PIN_UNASSIGNED, 12, 13, false, 0},  // SD powerEnable=GPIO13 (active-high) — see XTEINK_X3
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
    {20, 0, 400000, 0x55, 0},
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

// --- M5Stack Paper Mono / PaperS3 — ESP32-S3, 800x480 SSD1677 --------------
// EPD power/reset and the microSD rail are switched by the on-board M5IOE1;
// their direct GPIO fields stay unassigned and the consumer board-support
// library supplies the corresponding hooks (see M5Ioe1.h for the pin map).

// Passive beeper switched by an NMOS from the 3V3_L2 core rail, gate on ESP
// GPIO42 — a plain LEDC tone pin for the Buzzer lib, no codec on board.
constexpr AudioConfig PAPER_MONO_AUDIO = {AudioOutput::None,
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
                                          42};

// Frontlight: AW9967 boost LED driver whose CTRL input is the M5PM1's GPIO3
// routed to its PWM0 engine (5 kHz, 12-bit) — no ESP pin involved. The AW9967
// runs from the EPD rail, so it only lights while EPD power is on.
constexpr FrontlightConfig PAPER_MONO_FRONTLIGHT = {PIN_UNASSIGNED, 5000, 12, true, PIN_UNASSIGNED, true};

// One discrete RGB LED: red on the PM1's LED output, green/blue on IOE1
// IO8/IO9. LedManager's paperMonoDiscrete path drives it; colorOrder unused.
constexpr LedConfig PAPER_MONO_LEDS = {PIN_UNASSIGNED, 1, LedColorOrder::RGB, false, true};

// PDM MEMS mic on CLK=GPIO45 / DATA=GPIO46. Its power rail is IOE1 IO12, not
// an ESP GPIO — the consumer's board bring-up raises it (m5ioe1::PIN_MIC_POWER)
// before capture; `enable` stays unassigned.
constexpr MicConfig PAPER_MONO_MIC = {MicInput::Pdm, 45, 46, PIN_UNASSIGNED, true};

constexpr BoardProfile PAPER_MONO = {
    Board::PaperMono,
    "m5stack_paper_mono",
    InputStyle::DigitalTwoButton,
    DisplayController::SSD1677,
    800,
    480,
    {15, 14, 16, 17, PIN_UNASSIGNED, 18, PIN_UNASSIGNED},
    20000000,
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, false, 0},
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 2, 3, PIN_UNASSIGNED, false},
    PIN_UNASSIGNED,
    PIN_UNASSIGNED,
    2.0f,
    PIN_UNASSIGNED,
    // FT6336 is register-compatible with the FT5x06 family. It reports a
    // portrait 480x800 frame, so swap it into the panel-native 800x480 frame.
    // Paper Mono's EPD source is rotated 180 degrees by the board profile;
    // flip the post-swap Y axis so touch follows the displayed framebuffer.
    {TouchController::Ft5x06, 47, 48, 4, PIN_UNASSIGNED, 0x38, 0, 799, 0, 479, false, 0, true, false, PIN_UNASSIGNED,
     true, false, true},
    PAPER_MONO_FRONTLIGHT,
    PAPER_MONO_AUDIO,
    PAPER_MONO_LEDS,
    ROTATE_180,
    {13, 12, 11, 10, 9, 8, 4},
    NO_GAUGE,
    PAPER_MONO_MIC,
    {47, 48, 100000, 0x32, 0, 0, 0, RtcType::Rx8130, ImuType::None},
    1.0f,  // uiScale: unchanged (was the struct default)
    {},    // power: none
    0,     // displayControllerVariant
    // Bezel overlap: the recessed panel swallows an edge-hugging scroll
    // indicator on the sides. 7px sides as a starting value (mirroring the
    // X4 Pro's tuned inset) — adjust after on-device measurement.
    {9, 7, 3, 7}};

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

// --- Murphy M4 — ESP32-S3R8, SSD1677 (480x800 portrait), FT6336U touch, dual frontlight ---
// Touch wiring below follows the CrossPoint-derived Murphy Reader v1.2.16
// implementation recovered from its ESP-IDF image. GPIO10 is the independent
// SD power gate and GPIO4 is the display clock; neither is part of touch I2C.
// Confirmed pins: display MOSI=3 SCK=4 CS=5 DC=6 RST=7 BUSY=8, SD CLK=16 CMD=15
// D0=17 D1=18 D2=11 D3=14 power=10(active LOW), touch SDA=10 SCL=4 INT=44 power=45(active LOW).
constexpr BoardProfile MURPHY_M4 = {
    Board::MurphyM4,
    "murphy_m4",
    InputStyle::DigitalFiveKey,
    DisplayController::SSD1677,
    // The GDEQ0426T82 panel has 800 source lines and 480 gate lines — landscape
    // regardless of physical housing orientation. SSD1677 gate count = height-1 =
    // 479, matching the factory firmware. The M4 physically mounts this landscape
    // panel in a portrait housing; a software rotation will be added later.
    800,
    480,
    // SSD1677 SPI: SCLK=4 (shared with I2C SCL), MOSI=3, CS=5, DC=6, RST=7, BUSY=8
    {4, 3, 5, 6, 7, 8, PIN_UNASSIGNED},
    20000000,  // 20 MHz — conservative start; SSD1677 supports up to 40 MHz
    // SD power via GPIO10 active-LOW. SPI pins unused; SDMMC wiring below.
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 10, false, 0, false},
    // {back, confirm, left, right, up, down, power, powerActiveHigh}
    // top=GPIO1(up/prev), middle=GPIO2(down/next), bottom=GPIO0(confirm+power shared, same as M3)
    {PIN_UNASSIGNED, 0, PIN_UNASSIGNED, PIN_UNASSIGNED, 1, 2, 0, false},
    9,               // battery ADC, measured with a 2:1 divider on hardware
    PIN_UNASSIGNED,  // batteryChargeStatus
    2.0f,
    PIN_UNASSIGNED,  // usbDetect: native USB, no dedicated detect GPIO
    // Murphy Reader v1.2.16: SDA=GPIO13, SCL=GPIO12, address=0x2E.
    // INT=44 (active LOW), touch power=GPIO45 (active LOW, PMOS)
    // Physical sensor is portrait (X:0-479, Y:0-799), landscape panel: swapXY=true.
    // {ctrl,sda,scl,irq,rst,addr,minX,maxX,minY,maxY,synthConfirm,altAddr,irqActiveLow,gt911Byte0,
    //  powerEnable,swapXY,flipX,flipY,hasHomeKey,powerEnableActiveHigh}
    {TouchController::Ft6336u,
     13,
     12,
     44,
     7,
     0x2E,
     0,
     799,
     0,
     479,
     false,
     0,
     true,
     false,
     45,
     true,
     false,
     true,
     false,
     false},
    // Dual-channel frontlight: cool=GPIO47, warm=GPIO48, 25 kHz / 10-bit PWM, active-HIGH
    {47, 25000, 10, true, 48},
    NO_AUDIO,
    NO_LEDS,
    NO_FLIP,
    // 4-bit SDMMC: CLK=16, CMD=15, D0=17, D1=18, D2=11, D3=14
    {16, 15, 17, 18, 11, 14, 4},
    NO_GAUGE,
    NO_MIC,
    NO_SENSORS,
    1.2f,  // uiScale: touch device
};

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
                                  4,                            // batteryAdc GPIO4
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
    // Battery-backed RTC on the shared main I2C bus. The vendor schematic
    // (hardware/T5 E-paper S3 Pro V1.0 24-12-24.pdf, page 3 / U3) shows a
    // PCF8563TS at 0x51; the README's product table says PCF85063, and the
    // vendor's own docs/pinmap.md notes say to prefer the schematic and the
    // mounted part. Was NO_SENSORS, so the board kept time in software and lost
    // it whenever power was actually cut rather than merely deep-slept.
    {39, 40, 400000, 0x51, 0, 0, 0, RtcType::Pcf8563, ImuType::None},
    1.2f,  // uiScale: 4.7" 960x540 touch (~234 PPI) — finger-sized chrome, like Sticky
    // Power latch: main-power MOSFET on GPIO2, driven HIGH first thing in boot
    // via holdPowerRails() or the board powers off when USB is unplugged.
    {2},
    0,  // displayControllerVariant: not probed on this panel
    // Bezel: this case sits closer over the glass at the sides than the X4's, so
    // the default 3px leaves the first and last characters of a line hard to
    // read. Measured by eye on hardware in two passes (3 -> 6 -> 8); top/bottom
    // are correct at the defaults. Compare the X4 Pro's 7px sides.
    {9, 8, 3, 8}};

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
     true,                                // gt911CoordsAtByte0: reports coords at byte 0 (no track-id) on M5Paper
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

// --- M5Stack PaperS3 4.7" (ED047TC1 raw-parallel EPD) — ESP32-S3 ---------------
// The S3 successor to M5Paper v1.1: the same 960x540 16-gray ED047TC1 glass, but
// with NO IT8951 — the S3 drives the panel directly over the 8-bit parallel bus,
// exactly the LilyGo T5 S3 display class, so it shares FREEINK_DRIVER_LGFX_EPD
// (LovyanGFX Panel_EPD/Bus_EPD via m5stack/M5GFX). Unlike the LilyGo there is no
// PMIC/expander: the EPD rails are plain GPIOs (OE=45, PWR=46) that Bus_EPD's
// stock power sequence drives itself, so the board's LgfxEpdConfig
// (m5PaperS3LgfxConfig, libs/hardware/BoardPaperS3) carries real pins and no
// power hooks. Pin map cross-checked M5GFX autodetect (board_M5PaperS3) +
// M5Unified + the official docs; where the docs pinmap disagrees (it labels G45
// "PWR" and omits G16/G46), M5GFX — the working vendor driver — wins.
//
// Inputs: there are NO firmware-readable buttons. The single side button feeds
// the PMS150G power-latch chip (press = on, 2 s hold = hard off), so paging and
// all navigation MUST come from the GT911 touch (tap zones/gestures are firmware
// policy). Power-off is not a latch release either: firmware pulses GPIO44
// (BoardPaperS3::powerOff()); there is no latch pin to hold.
//
// Pending hardware validation: panel rotation (M5GFX defaults the device to
// portrait via offset_rotation=3; this profile keeps the SDK's native-landscape
// convention with rotation 0 in the LgfxEpdConfig), touch flipX/flipY +
// gt911CoordsAtByte0 (no RST wired -> self-configured, like M5Paper v1.1), and
// the battery divider. IMU is a BMI270 at 0x68 on the internal bus — not a
// supported ImuType yet, so it is left out of sensors.
constexpr BoardProfile M5PAPER_S3 = {
    Board::M5PaperS3,
    "m5paper_s3",
    InputStyle::DigitalButtons,  // vacuous: no GPIO buttons exist (see note above)
    DisplayController::LgfxEpd,
    960,
    540,
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED,
     PIN_UNASSIGNED},                            // no SPI display pins: parallel bus lives in LgfxEpdConfig
    0,                                           // displaySpiHz n/a (external bus)
    {39, 40, 38, 47, PIN_UNASSIGNED, false, 0},  // SD over SPI: SCLK39 MISO40 MOSI38 CS47, no power gate
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED,
     PIN_UNASSIGNED, false},  // no GPIO buttons — touch-only navigation
    3,     // batteryAdc GPIO3 (ADC1_CH2)
    4,     // batteryChargeStatus GPIO4, LOW = charging (LGS4056H STAT)
    2.0f,  // divider 2:1 (M5Unified's _adc_ratio; pending hardware validation)
    5,     // usbDetect GPIO5, HIGH = USB present
    // GT911 on the internal I2C bus SDA41/SCL42 (shared with the BM8563 RTC +
    // BMI270 IMU), INT=GPIO48, no reset wired (self-loads its config), 0x5D alt
    // 0x14. Portrait digitizer (540x960) on the landscape panel -> swapXY, rawMax
    // in post-swap panel order; flips mirror the LilyGo/M5Paper defaults pending
    // corner-tap validation. NOTE: GPIO48 is not an RTC IO on the S3, so touch
    // cannot be a deep-sleep EXT wake source (light-sleep GPIO wake only).
    {TouchController::Gt911, 41, 42, 48, PIN_UNASSIGNED, 0x5D, 0, 959, 0, 539, false, 0x14, false,
     true,  // gt911CoordsAtByte0: no reset/config dance, like M5Paper v1.1 (pending validation)
     PIN_UNASSIGNED, true, false, true},  // powerEnable none, swapXY=true, flipX=false, flipY=true
    NO_FRONTLIGHT,     // e-paper, no frontlight (the GPIO0 status LED is board-support)
    M5_PAPERS3_AUDIO,  // no output codec; LEDC buzzer on GPIO21 (Buzzer lib)
    NO_LEDS,           // single PWM LED on GPIO0 is not an addressable strip — board-support
    NO_FLIP,
    NO_SDMMC,  // SD is SPI, not SDMMC
    NO_GAUGE,  // ADC battery (GPIO3 above), no I2C fuel gauge
    NO_MIC,
    // BM8563 RTC (PCF8563 register-compatible) at 0x51 on the internal bus
    // SDA41/SCL42, shared with the GT911 — bus 0 (Wire), like the X4 Pro's shared
    // touch/RTC bus. The RTC INT line feeds the PMS150G power latch (wake-from-off
    // via Rtc alarm), not an ESP32 GPIO. BMI270 IMU (0x68) unsupported — omitted.
    {41, 42, 400000, 0x51, 0, 0, 0, RtcType::Pcf8563, ImuType::None},
    1.2f,  // uiScale: 4.7" 960x540 touch (~234 PPI) — finger-sized chrome, like LilyGo/M5Paper
    {}};   // no power latch: the PMS150G self-latches; off = GPIO44 pulse (BoardPaperS3::powerOff)

// --- EEGO Reader A4 — ESP32-S3 N16R8, UC8279C (768x552) + GSLX680 touch -------
// CrossPoint fork device; pins/resolution/calibration from stock-firmware RE,
// none hardware-confirmed. See docs/eego-a4-support.md.
constexpr BoardProfile EEGO_A4 = {
    Board::EegoA4,
    "eego_a4",
    InputStyle::DigitalButtons,
    DisplayController::UC8279C,
    768,
    552,
    {42, 45, 21, 14, 13, 41, 6},                       // SCLK MOSI CS DC RST BUSY PWR-EN
    20000000,
    {39, 40, 38, 47, PIN_UNASSIGNED, true, 20000000},  // dedicated HSPI SD: SCLK MISO MOSI CS
    // UP DOWN POWER. Power is active-HIGH (press drives 3V3) with a weak
    // external pull-down; INPUT_PULLDOWN required (InputManager honors the
    // polarity for pinMode). An internal pull-up leaves the pin mid-rail and
    // phantom-pressed.
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 5, 7, 8, true},
    10,      // batteryAdc
    11,      // batteryChargeStatus
    1.559f,  // divider
    PIN_UNASSIGNED,
    // GSLX680: pollGslx680 applies the 1.2.7 calibration and returns panel-native
    // x=0..767, y=0..551, so no raw-range/swap/flip mapping is needed here.
    {TouchController::Gslx680, 2, 1, PIN_UNASSIGNED, 3, 0x40, 0, 767, 0, 551, false, 0, false, false,
     PIN_UNASSIGNED, false, false, false, true, true},  // powerEnable, swapXY, flipX, flipY, hasHomeKey, pwrEnActiveHigh
    // The frontlight is an LM3630A driven via the i2cFrontlight field below, not
    // this LEDC/PWM FrontlightConfig — the frontlit variant's light is I2C only.
    NO_FRONTLIGHT,
    NO_AUDIO,
    NO_LEDS,
    NO_FLIP,
    NO_SDMMC,
    NO_GAUGE,
    NO_MIC,
    {2, 1, 400000, 0x51, 0, 0, 0, RtcType::Pcf8563, ImuType::None},  // PCF8563 on the shared touch bus
    1.2f,
    {4},  // power latch GPIO4
    0,    // displayControllerVariant (UC8279C, not probed)
    // Rounded-corner panel: pull the sides (left/right in the portrait frame) in a
    // lot so text/status icons aren't clipped by the curved bezel. Tune on hardware.
    // {top, right, bottom, left}
    {24, 24, 8, 24},
    // Charger STAT GPIO11 is driven HIGH while charging, like the X4 Pro
    // (hardware verified: with the active-low default the charging indicator
    // lit only when USB was unplugged).
    true,
    // LM3630A on the shared I2C bus (SDA2/SCL1), enable GPIO12.
    // Probed at runtime: some retail A4 units have no frontlight.
    {I2cFrontlightController::Lm3630a, 2, 1, 400000, 0x36, 12}};

static_assert(EEGO_A4.displayWidth / 8 * EEGO_A4.displayHeight == 52992,
              "EEGO A4 framebuffer must be 52,992 bytes (768/8 x 552)");

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
    // chargeEnable: EN_BAT_CHGn GPIO39 -> BQ25616 /CE, active-low (confirmed by
    // Seeed's firmware team; native firmware drives it). Without it the pin's
    // JTAG reset-default pull-up disables charging the whole time we're awake
    // (~0.06 A USB input awake vs ~0.5 A once sleep isolates the pad).
    {45, 46, 39, false}};

// --- Xteink X4 Pro — ESP32-S3, 800x480 EPD + GT911 touch + warm/cold frontlight ---
// Recovered from the OEM flash dump (x4pro_flash_dump.bin); full evidence and confidence
// levels in docs/xteink-x4pro-support.md. This is a DISTINCT device from the C3
// `XTEINK_X4` above: the same display size, but an ESP32-S3 with 8 MB PSRAM, a
// GT911 capacitive digitizer, and a dual warm/cold color-temperature frontlight.
//
// Hardware-confirmed profile: display pins, GT911 pins/orientation, digital buttons,
// GPIO8/GPIO9 frontlight channels, 1-bit SDMMC with GPIO5 enable, BM8563 RTC, and
// CW2017 gauge. Panel mount orientation and USB/VBUS detection remain unconfirmed.
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
    20000000,  // displaySpiHz: 20 MHz, matching the X4's default. The OEM clocks the panel at only 5 MHz
               // (SPISettings 0x4C4B40), but the SSD1677 handles far more (X4 runs 20, de-link 40), so 20 MHz
               // is well in spec and gives noticeably faster RAM writes. Drop back to 5 MHz if artifacts appear.
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
    // Charger STAT on GPIO21, ACTIVE-HIGH (batteryChargeStatusActiveHigh at the
    // profile tail): stock's Cw2017PowerHal configures GPIO21 input/no-pull and
    // reports the raw level as "charging" (vtable slot 3 -> FUN_4214f67c;
    // gpio object configured pin=0x15 in board init FUN_4214eeb0). This is the
    // OEM battery-icon source — the CW2017 itself cannot observe charging.
    21,
    2.0f,
    PIN_UNASSIGNED,  // usbDetect: USB-MSC/VBUS-detect present; GPIO10 is a candidate (unconfirmed)
    // GT911 touch on the SHARED I2C bus SDA39/SCL38 (with RTC 0x51 + CW2017 gauge 0x63), addr 0x5D
    // (alt 0x14), 400 kHz. CONFIRMED ON HARDWARE: **INT=GPIO10, RST=GPIO4** (a first RE had these
    // reversed), and the controller is on an **active-LOW power rail: GPIO2** (powerEnable=2,
    // powerEnableActiveHigh=false) — the GT911 stays unpowered/silent until GPIO2 is driven LOW
    // (GPIO1, power.latch0, must also be HIGH). Like the Sticky panel, the GT911 SELF-LOADS its
    // internal config on the standard reset dance — no host config upload needed. Mounted PORTRAIT
    // (reports X:0..480, Y:0..800) on the 800x480 landscape panel → swapXY=true; rawMax describe the
    // post-swap panel axes. Coords start at byte 0 of the 0x8150 read → gt911CoordsAtByte0=true.
    // flipX/flipY pending a corner-tap test. {ctrl,sda,scl,irq,rst,addr,rawMinX,rawMaxX,rawMinY,rawMaxY,
    //  synthConfirm,altAddr,irqActiveLow,coordsAtByte0,powerEnable,swapXY,flipX,flipY,hasHomeKey,pwrActiveHigh}
    {TouchController::Gt911,
     39,
     38,
     10,
     4,
     0x5D,
     0,
     799,
     0,
     479,
     false,
     0x14,
     false,
     true,
     2,
     true,
     false,
     true,
     true,
     false},  // swapXY + flipY (confirmed by corner-tap); powerEnable=GPIO2 active-LOW; hasHomeKey
    // Frontlight: dual warm/cold LEDC PWM with color temperature (NVS lightWarmValue/
    // lightColdValue/lightCT/lightBri/lightOn). Recovered from the OEM LEDC init (IROM
    // 0x420a2130 → helper 0x420a20c0): two channels — GPIO8 on LEDC ch4 and GPIO9 on ch5 —
    // The X4 Pro frontlight runs through the FREEINK_FRONTLIGHT_LS path, which clocks the
    // LEDC timer from RC_FAST (~17.5 MHz) so the PWM survives light sleep. RC_FAST can only
    // reach ~17 kHz at 10-bit resolution (divisor must exceed 256; 25 kHz/10-bit yields div
    // 175 -> ledc_timer_config fails -> both channels unconfigured -> light stays dark). Use
    // 10 kHz / 10-bit (the pre-regression value that worked): div ~438, valid, full dim floor.
    // Both channels are active-HIGH (init drives the pin LOW = off, brightness raises duty).
    // GPIO8 is the hardware-confirmed cool channel and GPIO9 the warm channel;
    // FrontlightManager mixes them for color-temperature control.
    // NOTE: if light-sleep LEDC support is ever dropped (driver uses XTAL/AUTO clock) this
    // can safely return to the OEM 25 kHz.
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
    {1},
    0,  // displayControllerVariant: filled by the boot probe
    // Bezel overlap: the panel sits recessed, and 7px is the empirically-tuned
    // side inset that keeps an edge-hugging scroll indicator visible (was the
    // firmware's hardcoded X4 Pro scrollbar inset); top/bottom keep the X4
    // historical values pending measurement.
    {9, 7, 3, 7},
    true};  // batteryChargeStatusActiveHigh: GPIO21 STAT is driven HIGH while charging

// --- Xteink X4 Classic (X4C) — ESP32-S3, 800x480 EPD, NO touch, NO frontlight ---
// The X4C shares the X4 Pro's ESP32-S3 board, glass, and display stack (SSD1677 /
// UC8179 / UC8279, selected the same way at boot), but DROPS the GT911 touchscreen
// and the warm/cool frontlight. The GPIOs those freed up (touch-power GPIO2,
// SD-power GPIO5, frontlight GPIO8/GPIO9) are reused as four extra discrete front
// ("Bottom Key") buttons.
//
// Evidence: reverse-engineered from the stock flash dump (flashx4c.bin, app0 =
// xteink_app v7.0.11). Board tag `ESP32S3_X4_CLA` (rev `ESP32S3_X4R2_CLA`), default
// panel string `ESP32S3_X4_CLA_SSD1677`. The single board hardware-init function
// FUN_4214c798 constructs the SPI display bus (FUN_4214d260(bus,11,12,13,14) ->
// MOSI11/SCLK12/CS13/DC14), the BM8563 RTC and the Cw2017 gauge on I2C SDA39/SCL38
// @400kHz, and registers every GPIO. The GPIO config call's mode arg is decoded from
// unambiguous pins: SPI drive lines + the GPIO1 rail use mode 1 = OUTPUT, the STAT
// line uses mode 0 = plain INPUT, buttons use mode 2 = input+ISR. Confidence:
//   HIGH   : display MOSI11/SCLK12/CS13/DC14/RST10/BUSY18. FUN_4214d260 stores
//            GPIO14 as the derived bus's D/C pin. The boot path binds the remaining
//            display tuple as bus/GPIO10/GPIO18; FUN_42148418 pulses tuple[1]
//            HIGH->LOW->HIGH as RESET, while tuple[2] is the BUSY input. GPIO6 is
//            attached
//            to the storage object; FUN_4214d4a8 pulses it HIGH for 80 ms, LOW for
//            120 ms, then runs SDMMC with the active-low gate held LOW. Also confirmed:
//            GPIO1 power rail, the discrete button GPIOs (0/7/3 primary cluster +
//            2/5/8/9 front keys), charge-STAT=GPIO21, touch ABSENT, frontlight ABSENT,
//            and the SSD1677/UC8179/UC8279 controller set + hw_calib/screenType.
// Physical button layout is the X4 Pro's two SIDE keys (page turn) plus the C3
// X4's four BOTTOM keys, so the mapping mirrors both boards (see the input field).
// As a buttons-only device (DigitalButtons + NO_TOUCH) with all six nav actions
// mapped, the UI shows side-bezel button hints, like the X4/X3.
//   PENDING hardware: the role of GPIO4 (configured as an input but NOT
//            interrupt-attached, so not a button), plus confirmation of the
//            charge-STAT polarity and panel orientation. See
//            docs/xteink-x4c-support.md.
constexpr BoardProfile XTEINK_X4_CLASSIC = {
    Board::XteinkX4Classic,
    "xteink_x4_classic",
    InputStyle::DigitalButtons,
    DisplayController::SSD1677,  // boot default; NVS screenType selects UC8179/UC8279
    800,
    480,
    // Display SPI, recovered from the stock X4C image. No MISO; the controller comes
    // from NVS screenType. GPIO1 supplies the panel/peripheral rail.
    // {SCLK, MOSI, CS, DC, RST, BUSY, powerEnable}
    {12, 11, 13, 14, 10, 18, PIN_UNASSIGNED},
    20000000,  // displaySpiHz: UC8279 serial-write maximum; avoids throttling full-plane AA uploads
    // SD SPI view retained only for consistency; the card mounts via the native SDMMC
    // block device (sdmmc field below). GPIO6 is its active-low enable; the stock mount
    // pulses HIGH->LOW and leaves it LOW while the card is in use.
    {41, 40, 42, 45, 6, true, 0, false},
    // Seven discrete active-low GPIO buttons (each an input+ISR in the board init;
    // NOT an ADC ladder): six navigation keys plus power. Physical layout = the two
    // side keys of the X4 Pro PLUS the
    // four bottom keys of the C3 X4, so the logical mapping mirrors both:
    //   * Two SIDE keys (page turn, like the X4 Pro): Left=GPIO0 -> up/prev,
    //     Right=GPIO7 -> down/next.
    //   * Four BOTTOM keys (like the normal X4's cluster): left/right/confirm/back on
    //     GPIO2/5/8/9 (stock "Bottom Key 1-4", array order 2,5,8,9). Because there is
    //     no touchscreen, these physical keys are how the reader gets back/confirm, and
    //     with all six nav actions mapped the UI renders side-bezel button hints (the
    //     device is InputStyle::DigitalButtons + NO_TOUCH, like the X4/X3). This order
    //     is confirmed by hardware testing: the old GPIO8/9 left/right assignment made
    //     the physical Confirm/Back keys act as Up/Down.
    //   * Power=GPIO3. (GPIO4 is a non-interrupt input, role PENDING — not a button.)
    // {back, confirm, left, right, up, down, power, powerActiveHigh}
    // back=GPIO9, confirm=GPIO8, left=GPIO5, right=GPIO2 (all confirmed on hardware).
    {9, 8, 5, 2, 0, 7, 3, false},
    PIN_UNASSIGNED,  // batteryAdc: uses the CW2017 gauge, not an ADC pin
    21,              // batteryChargeStatus: GPIO21 STAT (carried from X4 Pro), active-HIGH (tail)
    2.0f,
    PIN_UNASSIGNED,  // usbDetect: native S3 USB detects the cable
    NO_TOUCH,        // no touchscreen — stock instantiates NoTouchDriver
    NO_FRONTLIGHT,   // no frontlight — stock reports "Frontlight hardware is disabled on this board"
    NO_AUDIO,
    NO_LEDS,
    NO_FLIP,  // panel mount transform pending hardware; native SSD1677 scan is 800x480 landscape
    // Native SDMMC 1-bit: CLK=41 CMD=42 DAT0=40, slot 1. D1/D2/D3 are unused.
    // Its GPIO6 gate is carried by sd.powerEnable above.
    {41, 42, 40, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 1},
    // CW2017 fuel gauge at I2C 0x63 on the shared bus SDA39/SCL38 400 kHz (same as X4 Pro).
    {39, 38, 400000, 0x63, 0, 0, GaugeType::Cw2017},
    NO_MIC,
    // Shared I2C bus SDA39/SCL38 @400k: BM8563 RTC (PCF8563-compatible) @0x51, plus a
    // QMI8658 6-axis IMU @0x6B (RE-confirmed: WHO_AM_I reg0==0x05, CTRL1/2/3/7 init on
    // addr 0x6B, no INT pin). {sda,scl,hz,rtcAddr,tempHumAddr,imuAddr,bus,rtcType,imuType}
    {39, 38, 400000, 0x51, 0, 0x6B, 0, RtcType::Pcf8563, ImuType::Qmi8658},
    1.0f,  // uiScale: button-navigated device (no touch) — original pixel-era chrome sizes
    // GPIO1 is the board's master peripheral/panel rail. GPIO10 is display RESET,
    // not a power latch, and is initialized by EpdBus.
    {1, PIN_UNASSIGNED},
    0,  // displayControllerVariant: filled by the boot probe
    {9, 7, 3, 7},  // bezel insets: carried from X4 Pro (same glass), pending measurement
    true};  // batteryChargeStatusActiveHigh: GPIO21 STAT driven HIGH while charging

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
                   cmax(FREEINK_DEVICE_X4PRO ? panelBytes(XTEINK_X4_PRO) : 0u,
                        FREEINK_DEVICE_X4CLASSIC ? panelBytes(XTEINK_X4_CLASSIC) : 0u))),
         cmax(cmax(FREEINK_DEVICE_STICKY ? panelBytes(STICKY) : 0u,
                   FREEINK_DEVICE_PAPERMONO ? panelBytes(PAPER_MONO) : 0u),
              cmax(cmax(FREEINK_DEVICE_PAPERS3 ? panelBytes(M5PAPER_S3) : 0u,
                        FREEINK_DEVICE_MURPHY_M4 ? panelBytes(MURPHY_M4) : 0u),
                   FREEINK_DEVICE_EEGO_A4 ? panelBytes(EEGO_A4) : 0u))));

// Compile-time default device — the profile ACTIVE starts as. With a single
// device in the build this is the only device; with several same-MCU devices it
// is the boot default until the consumer calls selectDevice().
#if FREEINK_DEVICE_PAPERMONO
constexpr BoardProfile DEFAULT_DEVICE = PAPER_MONO;
#elif FREEINK_DEVICE_EEGO_A4
constexpr BoardProfile DEFAULT_DEVICE = EEGO_A4;
#elif FREEINK_DEVICE_M5
constexpr BoardProfile DEFAULT_DEVICE = M5STACK_PAPER_COLOR;
#elif FREEINK_DEVICE_MURPHY_M4
constexpr BoardProfile DEFAULT_DEVICE = MURPHY_M4;
#elif FREEINK_DEVICE_MURPHY
constexpr BoardProfile DEFAULT_DEVICE = MURPHY_M3;
#elif FREEINK_DEVICE_DELINK
constexpr BoardProfile DEFAULT_DEVICE = DE_LINK;
#elif FREEINK_DEVICE_LILYGO
constexpr BoardProfile DEFAULT_DEVICE = LILYGO_T5S3;
#elif FREEINK_DEVICE_M5PAPER
constexpr BoardProfile DEFAULT_DEVICE = M5PAPER_V11;
#elif FREEINK_DEVICE_PAPERS3
constexpr BoardProfile DEFAULT_DEVICE = M5PAPER_S3;
#elif FREEINK_DEVICE_STICKY
constexpr BoardProfile DEFAULT_DEVICE = STICKY;
#elif FREEINK_DEVICE_X4PRO
constexpr BoardProfile DEFAULT_DEVICE = XTEINK_X4_PRO;
#elif FREEINK_DEVICE_X4CLASSIC
constexpr BoardProfile DEFAULT_DEVICE = XTEINK_X4_CLASSIC;
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

inline void holdPowerRails();  // defined below; used by selectDevice()

// Set ACTIVE to one of the devices compiled into this build. Returns false (and
// leaves ACTIVE unchanged) if `which` was not included via -DFREEINK_DEVICE_*.
inline bool selectDevice(Board which) {
  switch (which) {
#if FREEINK_DEVICE_X4
    case Board::XteinkX4:
      ACTIVE = XTEINK_X4;
      break;
#endif
#if FREEINK_DEVICE_X3
    case Board::XteinkX3:
      ACTIVE = XTEINK_X3;
      break;
    case Board::XteinkX3Uc8279:
      ACTIVE = XTEINK_X3_UC8279;
      break;
#endif
#if FREEINK_DEVICE_M5
    case Board::M5StackPaperColor:
      ACTIVE = M5STACK_PAPER_COLOR;
      break;
#endif
#if FREEINK_DEVICE_MURPHY
    case Board::MurphyM3:
      ACTIVE = MURPHY_M3;
      break;
#endif
#if FREEINK_DEVICE_MURPHY_M4
    case Board::MurphyM4:
      ACTIVE = MURPHY_M4;
      break;
#endif
#if FREEINK_DEVICE_DELINK
    case Board::DeLink:
      ACTIVE = DE_LINK;
      break;
#endif
#if FREEINK_DEVICE_LILYGO
    case Board::LilyGoT5S3:
      ACTIVE = LILYGO_T5S3;
      break;
#endif
#if FREEINK_DEVICE_M5PAPER
    case Board::M5PaperV11:
      ACTIVE = M5PAPER_V11;
      break;
#endif
#if FREEINK_DEVICE_STICKY
    case Board::Sticky:
      ACTIVE = STICKY;
      break;
#endif
#if FREEINK_DEVICE_X4PRO
    case Board::XteinkX4Pro:
      ACTIVE = XTEINK_X4_PRO;
      break;
#endif
#if FREEINK_DEVICE_X4CLASSIC
    case Board::XteinkX4Classic:
      ACTIVE = XTEINK_X4_CLASSIC;
      break;
#endif
#if FREEINK_DEVICE_PAPERMONO
    case Board::PaperMono:
      ACTIVE = PAPER_MONO;
      return true;
#endif
#if FREEINK_DEVICE_PAPERS3
    case Board::M5PaperS3:
      ACTIVE = M5PAPER_S3;
      break;
#endif
#if FREEINK_DEVICE_EEGO_A4
    case Board::EegoA4:
      ACTIVE = EEGO_A4;
      break;
#endif
    default:
      return false;
  }
  // Runtime-selected boards resolve after the consumer's first-thing-in-setup()
  // holdPowerRails() call (the dual X3+X4 binary boots with the X4 profile and
  // detects the real board here), so re-assert the selected board's latch pins
  // now that they are known.
  holdPowerRails();
  return true;
}

inline bool isM5StackPaperColor() { return ACTIVE.board == Board::M5StackPaperColor; }
inline bool isMurphyM3() { return ACTIVE.board == Board::MurphyM3; }
inline bool isDeLink() { return ACTIVE.board == Board::DeLink; }
inline bool isM5PaperV11() { return ACTIVE.board == Board::M5PaperV11; }
inline bool isM5PaperS3() { return ACTIVE.board == Board::M5PaperS3; }
inline bool isSticky() { return ACTIVE.board == Board::Sticky; }
inline bool isX4Pro() { return ACTIVE.board == Board::XteinkX4Pro; }
inline bool isX4Classic() { return ACTIVE.board == Board::XteinkX4Classic; }
inline bool isPaperMono() { return ACTIVE.board == Board::PaperMono; }
inline bool isEegoA4() { return ACTIVE.board == Board::EegoA4; }
inline bool hasTouch() { return ACTIVE.touch.controller != TouchController::None; }
inline bool hasHomeKey() { return ACTIVE.touch.hasHomeKey; }
inline bool hasPwmFrontlight() { return ACTIVE.frontlight.gpio != PIN_UNASSIGNED || ACTIVE.frontlight.viaPm1Pwm; }
inline bool hasI2cFrontlight() { return ACTIVE.i2cFrontlight.controller != I2cFrontlightController::None; }
inline bool hasColorTemperatureFrontlight() {
  return (ACTIVE.frontlight.gpio != PIN_UNASSIGNED && ACTIVE.frontlight.gpioWarm != PIN_UNASSIGNED) ||
         ACTIVE.i2cFrontlight.controller == I2cFrontlightController::Lm3630a;
}
inline bool hasAudio() { return ACTIVE.audio.output != AudioOutput::None; }

// Safety guard: a power-latch pin must never coincide with a display or SDMMC
// bus pin. A latch is driven hard HIGH (asserted) or LOW (power-off) and held
// across sleep — if that pin is really, say, the display CS (GPIO13 on the X4
// Pro) or an SDMMC line, asserting the "latch" would clobber the bus. This
// catches a mis-set profile or an X4-vs-X4Pro config mixup before it drives the
// wrong pin. (The GPIO13 battery latch is correct on the C3 X4, where 13 is not
// a bus pin; on the X4 Pro 13 is the display CS, so a latch there is rejected.)
inline bool latchConflictsWithBus(int8_t pin) {
  if (pin < 0) return false;
  const DisplayPins& d = ACTIVE.display;
  if (pin == d.sclk || pin == d.mosi || pin == d.cs || pin == d.dc || pin == d.rst || pin == d.busy) return true;
  const SdmmcPins& s = ACTIVE.sdmmc;
  if (s.busWidth != 0 && (pin == s.clk || pin == s.cmd || pin == s.d0 || pin == s.d1 || pin == s.d2 || pin == s.d3)) {
    return true;
  }
  return false;
}

// Assert the board's power-rail latch pins. Battery-latched boards (e.g. the
// Sticky) must call this first thing in setup() or the board powers off when
// the user releases the power button. Releasing the pins (driving them LOW)
// is a software power-off. No-op on boards without a latch.
inline void holdPowerRails() {
  for (const int8_t pin : {ACTIVE.power.latch0, ACTIVE.power.latch1}) {
    if (pin < 0) continue;
    if (latchConflictsWithBus(pin)) {
      // Refuse to drive a bus pin as a latch — see latchConflictsWithBus().
#if defined(ENABLE_SERIAL_LOG)
      // esp_rom_printf, not Serial: this runs first thing in setup(), before
      // USB CDC enumerates, and consumers deprecate Serial.printf in headers.
      esp_rom_printf("[BOARD] power latch pin %d collides with a display/SD bus pin; skipping\n", pin);
#endif
      continue;
    }
    // A previous power-off may have latched the pin LOW with gpio_hold_en —
    // a state that survives a reset and a USB-powered deep-sleep wake, and
    // silently defeats the digitalWrite below. Release it first.
    gpio_hold_dis(static_cast<gpio_num_t>(pin));
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  // Charger enable (see PowerConfig::chargeEnable). Held with gpio_hold_en so the
  // level survives esp_sleep_config_gpio_isolate() and deep sleep (PowerManager
  // calls gpio_deep_sleep_hold_en() before sleeping) — the charger must stay
  // enabled whether the firmware is awake or asleep.
  if (const int8_t ce = ACTIVE.power.chargeEnable; ce >= 0 && !latchConflictsWithBus(ce)) {
    const auto g = static_cast<gpio_num_t>(ce);
    gpio_hold_dis(g);
    pinMode(ce, OUTPUT);
    digitalWrite(ce, ACTIVE.power.chargeEnableActiveHigh ? HIGH : LOW);
    gpio_hold_en(g);
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
