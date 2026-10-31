#pragma once

// UC8279C panel driver for EEGO A4. The panel controller is 768x600; the host
// framebuffer is 768x552 and every transfer appends 48 white padding rows.
// Transfers scan framebuffer rows bottom-to-top and BUSY is active-low.

#include "PanelDriver.h"

namespace freeink {

class Uc8279cA4Driver : public PanelDriver {
 public:
  const char* controllerName() const override { return "UC8279C"; }
  Uc8279cA4Driver();

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  // Overlay (anti-aliased text) uses stock's reader "Gray3" path: the B/W base
  // and its gray masks go out as one three-level (white / middle / black)
  // transition from the previous Gray3 page, so a page turn drives only the
  // pixels that change. Absolute/direct planes (full images) use the stock
  // four-gray waveform, which drives every pixel from any prior state.
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode == GrayscaleMode::Overlay) return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, true, false, false};
    return {GrayscaleEncoding::AbsolutePlanes, GrayscaleBase::Combined, true, false, false};
  }
  void beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback, bool turnOff) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;

 private:
  void hardwareReset(EpdBus& bus);
  enum class Session : uint8_t { Bw, Gray3, FourGray };
  void initController(EpdBus& bus, Session session);
  void ensurePowerOn(EpdBus& bus);
  void loadFullLut(EpdBus& bus);
  void loadHalfFullLut(EpdBus& bus);
  void loadFastLut(EpdBus& bus);
  void loadGrayLut(EpdBus& bus);
  void writeFrame(EpdBus& bus, uint8_t command, const uint8_t* fb, bool invert = false);
  void showGray3(EpdBus& bus);
  void writeGray3Codes(EpdBus& bus, bool dtm1, bool& anyBlack, bool& anyWhiteClear);
  void loadGray3Lut(EpdBus& bus, bool anyBlack, bool anyWhiteClear);
  void fillControllerRam(EpdBus& bus, uint8_t command, uint8_t fill);
  void refresh(EpdBus& bus, const uint8_t* bwBaseline = nullptr);
  uint8_t* allocateGrayBuffer();
  bool allocateGray3();

  uint16_t _width;
  uint16_t _height;
  uint16_t _widthBytes;
  bool _screenOn = false;
  bool _grayControllerMode = false;
  bool _redriveAfterGray = false;  // re-drive all target pixels after AA, using the requested waveform
  // begin() seeds DTM1 white, but after a reflash/reset the glass holds the
  // previous UI, so a white-assumed diff leaves it ghosting through the first
  // paint. The first frame is driven to its inverse, then to the target with
  // DTM1 = inverse, so every pixel swings both ways under the full LUT.
  bool _firstRefreshPending = false;
  uint8_t* _grayLsb = nullptr;
  uint8_t* _grayMsb = nullptr;
  // The overlay base (bit 1 = white): stashed undisplayed by beginGrayscale(),
  // or the frame a plain display() just showed.
  uint8_t* _grayBase = nullptr;
  // A gray pass is an overlay unless beginGrayscale() started it as
  // absolute/direct: callers may display the base with a plain display() and
  // never announce the overlay.
  bool _overlayPass = true;
  bool _basePending = false;   // a stashed base has not reached the panel yet
  bool _cleanupPage = false;   // the stashed base asked for a cleanup (non-FAST) refresh
  // Gray3 frame currently on the panel, as white / middle masks (bit 1 = that
  // level); black is neither. Invalid after any other refresh.
  uint8_t* _gray3White = nullptr;
  uint8_t* _gray3Middle = nullptr;
  bool _gray3HistoryValid = false;
  bool _gray3Session = false;  // controller is already set up and powered for Gray3
};

PanelDriver& uc8279cA4Driver();

}  // namespace freeink
