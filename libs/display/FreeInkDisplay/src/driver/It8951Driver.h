#pragma once

// IT8951E controller driver — M5Paper v1.1 (ED047TC1, 540x960, 16-gray, ESP32).
//
// Unlike the panel-direct controllers (SSD1677/UC8253), the IT8951E is a timing
// controller with its own framebuffer SRAM. The host doesn't push waveforms: it
// loads an image into the controller's memory, then asks it to display a region
// with one of the IT8951's built-in waveform modes (INIT/GC16/DU/A2...). The
// IT8951 owns all LUT/VCOM/temperature handling internally.
//
// The protocol is 16-bit-word SPI with preamble words and MISO reads (device
// info, register reads, HRDY flow control) — a poor fit for the byte-oriented,
// write-only EpdBus — so this driver reports usesExternalBus() == true and drives
// its own SPIClass end to end (pins from BoardConfig::ACTIVE.display; MISO,
// rotation, VCOM, and clock from the injectable It8951Config).
//
// Selection: linked only when -DFREEINK_DRIVER_IT8951 (M5Paper env).

#include <It8951Config.h>
#include <SPI.h>

#include "PanelDriver.h"

namespace freeink {

const It8951Config& it8951DefaultConfig();

class It8951Driver : public PanelDriver {
 public:
  explicit It8951Driver(const It8951Config& cfg = it8951DefaultConfig());

  uint32_t spiHz() const override { return 0; }                                    // owns its own SPI
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }  // HRDY high = ready
  bool usesExternalBus() const override { return true; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

  // --- grayscale (16-gray native; reconstruct base + LSB/MSB planes -> 4bpp) ---
  // Strip support is advertised so the consumer keeps the B/W frame intact and
  // hands displayGray() the true base buffer (the no-strip fallback overwrites the
  // framebuffer with the MSB plane, which would paint a near-black, inverted page).
  //
  // Combined base: the controller takes a whole 4bpp frame per load, so the B/W
  // base and the gray planes go up together in ONE load and ONE refresh.
  // Advertising a Separate base made the host display() the B/W page first and
  // then re-load and re-refresh the whole panel for the AA edges -- two 1.3 MB
  // loads and two waveforms per page turn, seen as a second "flash" ~1.5 s after
  // the text appeared. displayGrayscaleBase() now only stages the base;
  // displayGray() commits it.
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, true, false, false};
  }
  void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) override;
  void repeatLastRefresh(EpdBus& bus) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;

 private:
  // --- low-level SPI framing ---
  void waitReady();                                    // block until HRDY high
  void writeWord(uint16_t preamble, uint16_t value);   // one preamble+word CS-framed write
  void writeCommand(uint16_t cmd);
  void writeData(uint16_t data);
  uint16_t readData();
  void readWords(uint16_t* buf, uint16_t count);       // bulk read in one CS-low frame
  void writeReg(uint16_t reg, uint16_t value);
  uint16_t readReg(uint16_t reg);

  // --- IT8951 operations ---
  uint32_t loadClockHz() const { return _cfg.loadSpiHz ? _cfg.loadSpiHz : _cfg.spiHz; }
  void systemRun();
  void getDeviceInfo();
  void setTargetMemoryAddr(uint32_t addr);
  void setVcom(uint16_t mv);
  void loadImageFull(const uint8_t* fb);  // expand 1bpp framebuffer -> 4bpp into controller SRAM
  // The same expansion for the framebuffer rectangle spanning byte columns
  // xb0..xb1 (8-px units) and rows y0..y1, inclusive, loaded at the matching
  // place in controller SRAM. loadImageFull() is the whole-frame case.
  void loadImageArea(const uint8_t* fb, uint16_t xb0, uint16_t xb1, uint16_t y0, uint16_t y1);
  // Rows y0..y1 at full width. Partial display() loads use this rather than the
  // exact box: a load with a non-zero x offset scattered a centred cover image
  // along the right edge on the eMinimal bench (2026-09-18), so the horizontal
  // extent of a box is trusted for the refresh area only, never for the load.
  void loadImageBand(const uint8_t* fb, uint16_t y0, uint16_t y1) {
    loadImageArea(fb, 0, static_cast<uint16_t>(_fbWb - 1), y0, y1);
  }
  // Rows y0..y1 at full width as a 1bpp bitmap (It8951LoadDepth::Bpp1): the
  // framebuffer bytes go up unexpanded under an 8bpp header a panel-width/8
  // wide, and the display engine is told to read the frame memory as one bit
  // per pixel (see setBitmapMode). A quarter of the 4bpp bytes. Whether the
  // bitmap format and the 4bpp/8bpp levels format can share the frame memory
  // is tracked in _memBitmap; a load in the other format widens itself to the
  // whole frame so the memory is never mixed.
  void loadImageBitmap(const uint8_t* fb, uint16_t y0, uint16_t y1);
  bool bitmapLoads() const { return _cfg.loadDepth == It8951LoadDepth::Bpp1 && _rotation == 0; }
  // Flip the display engine between reading the frame memory as 8-bit levels
  // and as a 1bpp bitmap painted with the BGVR colour table. `force` writes
  // the registers even when the cached state matches (begin(): the controller
  // keeps its registers across a host reset).
  void setBitmapMode(bool on, bool force = false);
  // Bounding box of the bytes that differ between fb and _base, in the units
  // loadImageArea() takes. False when nothing differs.
  bool diffBox(const uint8_t* fb, uint16_t& xb0, uint16_t& xb1, uint16_t& y0, uint16_t& y1) const;
  void loadImageGray(const uint8_t* base);  // combine base + LSB/MSB planes -> 4bpp into controller SRAM
  void displayArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode);
  void waitDisplayReady();                // poll LUT-busy register
  // Pick the waveform for a refresh the host asked for in `mode`: a clearing
  // GC16 for Full/Half, the first paint, a wake, or the periodic ghost-clear;
  // otherwise `differential` (DU for B/W, DU4 for gray). Updates the counters;
  // `weight` is the refresh's share of a whole frame in 1/256ths.
  static constexpr uint16_t FRAME_WEIGHT = 256;
  uint16_t resolveMode(RefreshMode mode, uint16_t differential, uint16_t weight = FRAME_WEIGHT, bool force = false);
  // Grow the region left with DU residue by partial-area moves since the last
  // clear (framebuffer byte columns and rows, inclusive); a screen change with
  // that region non-empty is promoted to a clear.
  void dirtyUnion(uint16_t xb0, uint16_t xb1, uint16_t y0, uint16_t y1);
  // Whether the frame memory shows anti-aliased gray inside a box (from the
  // buffered planes), and forgetting the planes once a box is loaded as B/W.
  bool grayWithin(uint16_t xb0, uint16_t xb1, uint16_t y0, uint16_t y1) const;
  void clearPlanesWithin(uint16_t xb0, uint16_t xb1, uint16_t y0, uint16_t y1);
  // Build the base/LSB/MSB -> 4bpp combine table (see loadImageGray).
  void buildGrayTable();

  const It8951Config& _cfg;
  // Reference to the Arduino global SPI bus (VSPI on ESP32) — the SAME object the
  // SDCardManager uses. On M5Paper the SD card and the IT8951 share one physical
  // SPI bus; two separate SPIClass instances bound to one VSPI peripheral corrupt
  // each other's transfers, so both must drive the one global bus object (manual
  // CS selects the device).
  SPIClass& _spi;

  uint16_t _fbW;   // landscape framebuffer width (960)
  uint16_t _fbH;   // landscape framebuffer height (540)
  uint16_t _fbWb;  // framebuffer width bytes (120)

  // Resolved at begin().
  int8_t _sclk = -1, _mosi = -1, _miso = -1, _cs = -1, _busy = -1, _pwrEn = -1, _sdCs = -1;
  uint16_t _panelW = 0, _panelH = 0;
  uint32_t _imgBufAddr = 0;
  uint16_t _rotation = 0;
  bool _running = false;

  // Automatic ghost-clear: count differential (DU/DU4) refreshes and promote to a
  // GC16 clear every ghostClearInterval. _lastClear lets the gray pass match the
  // B/W pass on a clearing page.
  uint32_t _partialsSinceClear = 0;  // in FRAME_WEIGHT units
  bool _lastClear = true;
  // Region driven differentially since the last clear, see dirtyUnion().
  bool _dirtyValid = false;
  uint16_t _dirtyXb0 = 0, _dirtyXb1 = 0, _dirtyY0 = 0, _dirtyY1 = 0;
  // The frame memory holds anti-aliased gray from the buffered planes (set by
  // displayGray, cleared by a whole-frame B/W load). Gates grayWithin().
  bool _memHasGray = false;
  // The frame memory holds a 1bpp bitmap (loadImageBitmap) rather than 8-bit
  // levels (every other load). The two cannot share a frame, so a load in the
  // other format goes whole-frame, and displayArea() sets the engine to match.
  bool _memBitmap = false;
  bool _bitmapModeOn = false;  // what the UP1SR bitmap bit was last set to
#ifdef IT8951_TIMING_LOG
  // Bench instrument (-DIT8951_TIMING_LOG): one serial line per display() /
  // displayGray() with what the push and the refresh cost. The load functions
  // add their bytes and microseconds here; display() zeroes them on entry.
  uint32_t _loadUs = 0;
  uint32_t _loadBytes = 0;
  void printTiming(const char* what, uint32_t t0Us, uint16_t dpyMode);
#endif
  // Combine table for loadImageGray(): index (base nibble << 8 | lsb nibble << 4
  // | msb nibble) -> the two 4bpp output bytes for those four pixels, first
  // byte in the high half. 8 KB, internal RAM, built once in begin(). Replaces
  // eight gray4() calls per framebuffer byte (2.6 M per page).
  uint16_t* _grayTab = nullptr;
  // begin() power-cycles the rail and INIT-wipes the glass, so there is no
  // retained frame for a differential first paint (the consumer's seamless
  // fast-wake assumes one). Promote the first content refresh to GC16.
  bool _firstPaintPending = true;

  // Buffered grayscale planes (PSRAM, _fbWb*_fbH each), combined with the B/W base
  // in displayGray(). Allocated in begin().
  uint8_t* _gLsb = nullptr;
  uint8_t* _gMsb = nullptr;

  // One 4bpp output row (_fbW / 2 bytes) for the load loops. Allocated in
  // begin() from the profile width, so any IT8951 panel geometry streams whole
  // rows — a fixed 960-px buffer would have truncated wider panels while the
  // LD_IMG_AREA header still promised the full width, scrambling the image.
  uint8_t* _rowBuf = nullptr;

  // Snapshot of the last B/W frame from display() or displayGrayscaleBase(). The
  // consumer's strip-grayscale pass clears the live framebuffer to 0x00 while
  // rendering the planes to a scratch buffer, so by displayGray() the passed
  // buffer is black — we use this snapshot (captured before the clear) as the
  // true base instead.
  uint8_t* _base = nullptr;
  // True while the controller's frame memory holds what _base describes (the
  // B/W content of every pixel; gray pages differ only in the AA edges, which
  // is what the glass shows anyway). Then a differential display() can diff the
  // new frame against _base and load and refresh only the rectangle that
  // changed -- a menu highlight is ~100 rows of 1404, so the 1.1 s whole-frame
  // push becomes ~80 ms. False after begin() (INIT wiped the glass, memory is
  // stale or unknown) and while a base is staged but not yet loaded.
  bool _memMirrorsBase = false;
  // A base staged by displayGrayscaleBase() and not yet on the glass. displayGray()
  // commits it with the mode the host asked for; cleanupGrayscaleBuffers() flushes
  // it as plain B/W if the gray pass never arrived, so a page is never lost.
  bool _baseStaged = false;
  RefreshMode _stagedMode = RefreshMode::Fast;
  bool _stagedTurnOff = false;
};

PanelDriver& it8951Driver();

}  // namespace freeink
