#include "FreeInkDisplay.h"

#include <BoardConfig.h>

#include <cstring>
#ifndef ARDUINO
#include <fstream>
#include <vector>
#endif

#include "driver/PanelDriver.h"

// Which panel drivers link is derived from the device set (-DFREEINK_DEVICE_*)
// in BoardConfig.h, included above, which defines each FREEINK_DRIVER_* to 0/1.
// A build links the drivers it can reach and selects among them at runtime
// (X3 + X4 both link in the generic C3 build; setDisplayX3() picks at runtime).

#if FREEINK_DRIVER_SSD1677
#include "driver/Ssd1677Driver.h"
#endif
#if FREEINK_DRIVER_UC8253_X3
#include "driver/Uc8253X3Driver.h"
#endif
#if FREEINK_DRIVER_ED2208
#include "driver/Ed2208M5Driver.h"
#endif
#if FREEINK_DRIVER_M5_OFFICIAL
#include "driver/M5OfficialDriver.h"
#endif
#if FREEINK_DRIVER_UC8253_MURPHY
#include "driver/Uc8253MurphyDriver.h"
#endif

namespace freeink {
namespace {
RefreshMode toInternal(FreeInkDisplay::RefreshMode m) {
  switch (m) {
    case FreeInkDisplay::FULL_REFRESH: return RefreshMode::Full;
    case FreeInkDisplay::HALF_REFRESH: return RefreshMode::Half;
    default: return RefreshMode::Fast;
  }
}
}  // namespace

FreeInkDisplay::FreeInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy)
    : _pins{sclk, mosi, cs, dc, rst, busy}, frameBuffer(nullptr)
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
      ,
      frameBufferActive(nullptr)
#endif
{
}

void FreeInkDisplay::setDisplayX3() {
  _panelSel = PanelSel::X3;
  displayWidth = X3_DISPLAY_WIDTH;
  displayHeight = X3_DISPLAY_HEIGHT;
  displayWidthBytes = X3_DISPLAY_WIDTH_BYTES;
  bufferSize = X3_BUFFER_SIZE;
}

void FreeInkDisplay::setDisplayM5PaperColor() {
  _panelSel = PanelSel::M5;
  // Landscape memory layout (panel is physically 600x400).
  displayWidth = 600;
  displayHeight = 400;
  displayWidthBytes = 600 / 8;
  bufferSize = static_cast<uint32_t>(displayWidthBytes) * displayHeight;
}

void FreeInkDisplay::selectDriver() {
  if (BoardConfig::isM5StackPaperColor()) {
    _panelSel = PanelSel::M5;
  }

  switch (_panelSel) {
#if FREEINK_DRIVER_M5_OFFICIAL || FREEINK_DRIVER_ED2208
    case PanelSel::M5:
#if FREEINK_DRIVER_M5_OFFICIAL
      _driver = &m5OfficialDriver();  // M5 official M5GFX backend
#else
      _driver = &ed2208M5Driver();    // fast hand-rolled ED2208 backend
#endif
      break;
#endif
#if FREEINK_DRIVER_UC8253_X3
    case PanelSel::X3: _driver = &uc8253X3Driver(); break;
#endif
    case PanelSel::X4:
    default:
#if FREEINK_DRIVER_SSD1677
      _driver = &ssd1677Driver();
#elif FREEINK_DRIVER_UC8253_MURPHY
      _driver = &uc8253MurphyDriver();
#elif FREEINK_DRIVER_M5_OFFICIAL
      _driver = &m5OfficialDriver();
#elif FREEINK_DRIVER_ED2208
      _driver = &ed2208M5Driver();
#elif FREEINK_DRIVER_UC8253_X3
      _driver = &uc8253X3Driver();
#endif
      break;
  }
}

void FreeInkDisplay::begin() {
  selectDriver();

  // External-library drivers (e.g. M5GFX) own the SPI/display hardware; only
  // bring up FreeInk's bus for native controller drivers.
  if (!_driver->usesExternalBus()) {
    _bus.begin(_pins, _driver->spiHz(), _driver->busyPolarity(), _driver->spiMiso(), _driver->coCs());
  }

  const PanelGeometry geom = _driver->geometry();
  displayWidth = geom.width;
  displayHeight = geom.height;
  displayWidthBytes = geom.widthBytes;
  bufferSize = geom.bufferSize;

  frameBuffer = frameBuffer0;
  memset(frameBuffer0, 0xFF, bufferSize);
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  frameBufferActive = frameBuffer1;
  memset(frameBuffer1, 0xFF, bufferSize);
#endif

  _driver->begin(_bus);
}

// ============================================================================
// Framebuffer composition (facade-owned; no driver involvement)
// ============================================================================

void FreeInkDisplay::clearScreen(uint8_t color) const { memset(frameBuffer, color, bufferSize); }

void FreeInkDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               bool fromProgmem) const {
  if (!frameBuffer) return;
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= displayHeight) break;
    const uint32_t destOffset = static_cast<uint32_t>(destY) * displayWidthBytes + (x / 8);
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= displayWidthBytes) break;
      frameBuffer[destOffset + col] =
          fromProgmem ? pgm_read_byte(&imageData[srcOffset + col]) : imageData[srcOffset + col];
    }
  }
}

void FreeInkDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                          bool fromProgmem) const {
  if (!frameBuffer) return;
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= displayHeight) break;
    const uint32_t destOffset = static_cast<uint32_t>(destY) * displayWidthBytes + (x / 8);
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= displayWidthBytes) break;
      const uint8_t srcByte = fromProgmem ? pgm_read_byte(&imageData[srcOffset + col]) : imageData[srcOffset + col];
      frameBuffer[destOffset + col] &= srcByte;  // only black pixels are drawn
    }
  }
}

void FreeInkDisplay::setFramebuffer(const uint8_t* bwBuffer) const { memcpy(frameBuffer, bwBuffer, bufferSize); }

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
void FreeInkDisplay::swapBuffers() {
  uint8_t* temp = frameBuffer;
  frameBuffer = frameBufferActive;
  frameBufferActive = temp;
}
#endif

// ============================================================================
// Panel operations (delegated to the active driver)
// ============================================================================

void FreeInkDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  _driver->display(_bus, frameBuffer, nullptr, toInternal(mode), turnOffScreen);
#else
  _driver->display(_bus, frameBuffer, frameBufferActive, toInternal(mode), turnOffScreen);
  swapBuffers();
#endif
}

void FreeInkDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  _driver->displayWindow(_bus, frameBuffer, nullptr, x, y, w, h, turnOffScreen);
#else
  _driver->displayWindow(_bus, frameBuffer, frameBufferActive, x, y, w, h, turnOffScreen);
#endif
}

void FreeInkDisplay::displayGrayBuffer(bool turnOffScreen) {
  _driver->displayGray(_bus, frameBuffer, turnOffScreen);
}

void FreeInkDisplay::refreshDisplay(RefreshMode mode, bool turnOffScreen) { displayBuffer(mode, turnOffScreen); }

void FreeInkDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  _driver->copyGrayscaleLsb(_bus, lsbBuffer);
  _driver->copyGrayscaleMsb(_bus, msbBuffer);
}

void FreeInkDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { _driver->copyGrayscaleLsb(_bus, lsbBuffer); }

void FreeInkDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { _driver->copyGrayscaleMsb(_bus, msbBuffer); }

void FreeInkDisplay::writeGrayscalePlaneStrip(GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                              uint16_t numRows) {
  _driver->writeGrayscalePlaneStrip(_bus, plane == GRAY_PLANE_LSB ? freeink::GrayPlane::Lsb : freeink::GrayPlane::Msb,
                                    rows, yStart, numRows);
}

bool FreeInkDisplay::supportsStripGrayscale() const { return _driver && _driver->supportsStripGrayscale(); }

#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
void FreeInkDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  _driver->cleanupGrayscaleBuffers(_bus, bwBuffer);
}
#endif

void FreeInkDisplay::requestResync(uint8_t settlePasses) {
  if (_driver) _driver->requestResync(settlePasses);
}

void FreeInkDisplay::skipInitialResync() {
  if (_driver) _driver->skipInitialResync();
}

void FreeInkDisplay::requestCompleteWaveformNextRefresh() {
  if (_driver) _driver->requestCompleteWaveformNextRefresh();
}

void FreeInkDisplay::grayscaleRevert() {
  if (_driver) _driver->grayscaleRevert(_bus, frameBuffer);
}

void FreeInkDisplay::setCustomLUT(bool enabled, const unsigned char* lutData) {
  if (_driver) _driver->setCustomLut(_bus, enabled, lutData);
}

void FreeInkDisplay::deepSleep() {
  if (_driver) _driver->deepSleep(_bus);
}

// ============================================================================
// Desktop/test helper
// ============================================================================

void FreeInkDisplay::saveFrameBufferAsPBM(const char* filename) {
#ifndef ARDUINO
  const uint8_t* buffer = getFrameBuffer();
  std::ofstream file(filename, std::ios::binary);
  if (!file) return;

  // Rotate 90 degrees counterclockwise: 800x480 landscape -> 480x800 portrait.
  const int W = DISPLAY_WIDTH;
  const int H = DISPLAY_HEIGHT;
  const int WB = W / 8;

  file << "P4\n" << H << " " << W << "\n";
  std::vector<uint8_t> rotated((H / 8) * W, 0);
  for (int outY = 0; outY < W; outY++) {
    for (int outX = 0; outX < H; outX++) {
      const int inX = outY;
      const int inY = H - 1 - outX;
      const int inByte = inY * WB + (inX / 8);
      const int inBit = 7 - (inX % 8);
      const bool isWhite = (buffer[inByte] >> inBit) & 1;
      if (!isWhite) {
        const int outByte = outY * (H / 8) + (outX / 8);
        const int outBit = 7 - (outX % 8);
        rotated[outByte] |= (1 << outBit);
      }
    }
  }
  file.write(reinterpret_cast<const char*>(rotated.data()), rotated.size());
#else
  (void)filename;
#endif
}

}  // namespace freeink
