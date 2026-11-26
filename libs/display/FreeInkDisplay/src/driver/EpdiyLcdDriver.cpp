/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */

// 中文：epdiy LCD 路径的 PanelDriver 适配。所有面板时序、LUT/波形与逐行供数都在
// EpdiyLcd 库里；这里只把 SDK 的 1bpp 帧缓冲与刷新档位翻译
// 成它的调用。
//
// English: PanelDriver adapter for the epdiy LCD path. All panel timing, LUTs
// and line feeding live in the EpdiyLcd library; this file translates the SDK's
// 1 bpp framebuffer and refresh
// profiles into its calls.

#include "EpdiyLcdDriver.h"

#include <BoardConfig.h>
#include <esp_heap_caps.h>

#include <cstring>

#if FREEINK_DRIVER_EPDIY_LCD

#ifndef FREEINK_EPDIY_LCD_CONFIG
#error                                                                         \
    "FREEINK_DRIVER_EPDIY_LCD requires a board config: define `const EpdiyLcdConfig& yourConfig();` in namespace freeink and build with -DFREEINK_EPDIY_LCD_CONFIG=yourConfig"
#endif

namespace freeink {

// 板子提供的配置函数，名字由 -DFREEINK_EPDIY_LCD_CONFIG 注入——与 LgfxEpd 的
// FREEINK_LGFX_EPD_CONFIG 同一套路，SDK 因此不需要 include 任何板子头文件。
// / Board-supplied config function, name injected by -DFREEINK_EPDIY_LCD_CONFIG
// — the same pattern as LgfxEpd's FREEINK_LGFX_EPD_CONFIG, so the SDK never
// includes a board header.
const EpdiyLcdConfig &FREEINK_EPDIY_LCD_CONFIG();

namespace {

// Preserve the established expansion convention: a set facade bit maps to
// panel white (15). This wrapper's historical parameter name is blackIsOne.
constexpr bool kBlackIsOne = true;

EpdiyLcdRefresh refreshFor(RefreshMode mode) {
  switch (mode) {
  case RefreshMode::Full:
    return EpdiyLcdRefresh::Full;
  case RefreshMode::Half:
    return EpdiyLcdRefresh::Half;
  case RefreshMode::Fast:
    return EpdiyLcdRefresh::Fast;
  }
  return EpdiyLcdRefresh::Fast;
}

} // namespace

EpdiyLcdDriver::EpdiyLcdDriver(const EpdiyLcdConfig &cfg) : _cfg(cfg) {}

PanelGeometry EpdiyLcdDriver::geometry() const {
  const uint16_t w = BoardConfig::ACTIVE.displayWidth;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight;
  const uint16_t wb = w / 8;
  return {w, h, wb, static_cast<uint32_t>(wb) * h};
}

GrayscaleCapabilities
EpdiyLcdDriver::grayscaleCapabilities(GrayscaleMode mode) const {
  if (mode != GrayscaleMode::Overlay)
    return {};
  // OverlayMasks: plane background 0 = black/white (taken from the B/W base the
  // host supplies first), LSB set = dark, MSB set = light. The host supplies
  // whole LSB/MSB planes; strip uploads are not implemented.
  // Combined: this driver defers the base so the grey commit presents the whole
  // page once. See displayGrayscaleBase() below.
  return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, false,
          false, false};
}

void EpdiyLcdDriver::begin(EpdBus &bus) {
  (void)bus;
  _basePending = false;
  _displayCommitted = false;
  _ready = epdiyLcdBegin(_cfg, BoardConfig::ACTIVE.displayWidth,
                         BoardConfig::ACTIVE.displayHeight, kBlackIsOne);
  if (!_ready)
    return;

  // 两个选择平面：只有宿主真的做抗锯齿时才会被写入，但分配是无条件的，因为它们
  // 必须在 copyGrayscaleLsb() 第一次被调用之前就位。
  // / The two selector planes: only written when the host actually
  // anti-aliases, but allocated unconditionally because they must exist before
  // the first copyGrayscaleLsb() call.
  const PanelGeometry g = geometry();
  if (_lsb == nullptr)
    _lsb = static_cast<uint8_t *>(
        heap_caps_malloc(g.bufferSize, MALLOC_CAP_SPIRAM));
  if (_msb == nullptr)
    _msb = static_cast<uint8_t *>(
        heap_caps_malloc(g.bufferSize, MALLOC_CAP_SPIRAM));
  if (_lsb == nullptr || _msb == nullptr) {
    Serial.printf(
        "[EpdiyLcd] grayscale plane allocation failed (%u bytes each)\n",
        static_cast<unsigned>(g.bufferSize));
    heap_caps_free(_lsb);
    heap_caps_free(_msb);
    _lsb = nullptr;
    _msb = nullptr;
    epdiyLcdEnd();
    _ready = false;
    return;
  }
  memset(_lsb, 0, g.bufferSize);
  memset(_msb, 0, g.bufferSize);
}

void EpdiyLcdDriver::deepSleep(EpdBus &bus) {
  (void)bus;
  epdiyLcdDeepSleep();
  heap_caps_free(_lsb);
  heap_caps_free(_msb);
  _lsb = nullptr;
  _msb = nullptr;
  _ready = false;
  _basePending = false;
  _displayCommitted = false;
}

void EpdiyLcdDriver::display(EpdBus &bus, const uint8_t *fb,
                             const uint8_t *prev, RefreshMode mode,
                             bool turnOff) {
  (void)bus;
  (void)prev; // epdiy's highlevel keeps its own previous frame / epdiy
              // 自己记上一帧
  _basePending = false;
  _displayCommitted = false;
  if (!_ready)
    return;

  // 帧缓冲极性见文件顶部的 kBlackIsOne。/ Framebuffer polarity: see kBlackIsOne
  // above.
  _lastBaseMode = refreshFor(_reading && mode == RefreshMode::Half ? RefreshMode::Full : mode);
  _displayCommitted = epdiyLcdDraw(fb, _lastBaseMode, turnOff);
}

void EpdiyLcdDriver::copyGrayscaleLsb(EpdBus &bus, const uint8_t *lsb) {
  (void)bus;
  if (_lsb == nullptr || lsb == nullptr)
    return;
  memcpy(_lsb, lsb, geometry().bufferSize);
}

void EpdiyLcdDriver::copyGrayscaleMsb(EpdBus &bus, const uint8_t *msb) {
  (void)bus;
  if (_msb == nullptr || msb == nullptr)
    return;
  memcpy(_msb, msb, geometry().bufferSize);
}

void EpdiyLcdDriver::displayGray(EpdBus &bus, const uint8_t *fb, bool turnOff,
                                 const unsigned char *lut, bool factoryMode) {
  (void)bus;
  (void)lut;
  (void)factoryMode;
  _displayCommitted = false;
  if (!_ready || _lsb == nullptr || _msb == nullptr)
    return;

  (void)fb;  // The drawing buffer holds the last selector plane at this point.
  // Image pages use GC16 for calibrated tones. Text turns skip unchanged pixels.
  const auto grayMode = _lastBaseMode == EpdiyLcdRefresh::Full || _fullTone
                            ? EpdiyLcdRefresh::Full
                            : (_reading ? EpdiyLcdRefresh::TextTurn : EpdiyLcdRefresh::Half);
  _displayCommitted = epdiyLcdDrawGray(_lsb, _msb, grayMode, turnOff);
  if (_displayCommitted)
    _basePending = false;
}

void EpdiyLcdDriver::displayGrayscaleBase(EpdBus &bus,
                                                     const uint8_t *fb,
                                                     RefreshMode fallback,
                                                     bool turnOff) {
  (void)bus;
  (void)turnOff;
  _displayCommitted = false;
  _basePending = false;
  if (!_ready)
    return;
  _lastBaseMode = refreshFor(_reading && fallback == RefreshMode::Half ? RefreshMode::Full : fallback);
  // 底图只暂存。宿主接下来会写 LSB/MSB 平面并调
  // displayGray()，由它合成整页后推一次。 / Defer: the host writes the LSB/MSB
  // planes next and calls displayGray(), which composes the whole page and
  // presents it once.
  epdiyLcdStashBase(fb);
  _basePending = true;
}

void EpdiyLcdDriver::requestResync(uint8_t) {
  if (_basePending)
    _displayCommitted = false;
  _basePending = false;
}

void EpdiyLcdDriver::cleanupGrayscaleBuffers(EpdBus &bus, const uint8_t *bw) {
  (void)bus;
  if (!_basePending)
    return;
  if (bw && _ready)
    _displayCommitted = epdiyLcdDraw(bw, _lastBaseMode, false);
  _basePending = false;
}

EpdiyLcdDriver &epdiyLcdDriver() {
  static EpdiyLcdDriver driver(FREEINK_EPDIY_LCD_CONFIG());
  return driver;
}

} // namespace freeink

#endif // FREEINK_DRIVER_EPDIY_LCD
