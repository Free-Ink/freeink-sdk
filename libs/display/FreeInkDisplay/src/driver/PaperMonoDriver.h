#pragma once

#include <atomic>

#include "PanelDriver.h"

namespace freeink {

// Paper Mono panel driver. The controller is an SSD1677; every waveform it
// runs here is a host-authored 111-byte LUT.
//
// Binary UI and reader Fast paints use the panel's internal, temperature-
// selected, non-flashing OTP waveform. Balanced book paints are one target-
// coded W/G/B activation. Ordinary pages leave unchanged white background on
// entry 0, which is not idle: it carries a short white-biased top-up (one +15 V
// frame, then the white dose) that erases a little residue on every page turn
// instead of letting ghosts accumulate until the corrective refresh. Changed
// pixels and every target gray/black pixel run the full driven classes.
struct PaperMonoGrayParams {
  // Retired endpoint-polish count, kept only so stored presets keep loading.
  // setGrayParams() ignores it: the driven classes are right-aligned and end
  // target-directed, and a 1+1 polish dither was measured to clean nothing.
  uint8_t cleanupPasses = 0;
  // Legacy preset fields. Only lightFrames still has an effect: it selects the
  // full-resync middle tone (see setGrayParams). The rest are retained so
  // existing callers and stored presets keep compiling.
  uint8_t revertBoost = 0;
  uint8_t baseDouble = 0;
  uint8_t grayStrong = 0;
  uint8_t scheme = 1;
  uint8_t darkFrames = 3;
  uint8_t lightFrames = 11;
  uint8_t pipeline = 0;
  uint8_t slamFrames = 0;
  uint8_t frameRate = 0;
};

class PaperMonoDriver final : public PanelDriver {
 public:
  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  // The complete 3-gray target is intentionally batched in host RAM before a
  // waveform starts. Generic displayStart/displayFinish therefore stays
  // synchronous; advertising an in-flight refresh here would make BUSY polling
  // lie while no controller activation exists yet.
  bool supportsAsyncDisplay() const override { return false; }
  bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;
  void seedPreviousFrame(EpdBus& bus, const uint8_t* buf) override;

  bool supportsStripGrayscale() const override { return true; }
  bool combinesGrayscaleBase() const override { return true; }
  void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  // Selector staging is host-RAM only, so the renderer may hand over strips at
  // any time without synchronising against the controller.
  bool supportsBusyGrayscaleStaging() const override { return true; }
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void prepareGrayscaleTarget(const uint8_t* bw) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void displayGrayCalibration(EpdBus& bus, const uint8_t* fb, uint16_t customX, uint16_t customY, uint16_t customW,
                              uint16_t customH) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;

  void requestResync(uint8_t settlePasses) override;
  // Paper Mono cuts EPD power in deep sleep, so the on-glass state is unknown
  // at the first paint. Drive every pixel once instead of diffing.
  void skipInitialResync() override { _needsFull = true; }
  void beginDisplayWork() override;
  void abortPostRefresh() override;
  bool postRefreshAborted() const override;
  bool displayCommitted() const override { return _displayCommitted; }
  void controllerIdle(EpdBus& bus) override;
  // Inverted (dark-background) content: runOtpUpdate() widens its drive set to
  // re-blacken the unchanged background every update. See the comment there.
  void setBackgroundHint(bool darkBackground) override { _darkBackground = darkBackground; }

  void setGrayParams(const PaperMonoGrayParams& params);
  void abortGray() { abortPostRefresh(); }
  void resetGray();

 private:
  // Timelines in 5 ms frames.
  struct TriParams {
    // One-shot W/G/B sweep. The three per-class white lengths are derived from
    // these so that each class's net V*frames is exactly zero; see makeTriLut().
    uint8_t preUp = 16;      // activation kick; also hosts the entry-0 top-up
    uint8_t tGray = 24;      // weak-rail (+5 V) frames that develop the middle tone
    uint8_t tBlack = 32;     // strong-rail (+15 V) frames that develop black
    uint8_t postCleanCycles = 0;  // retired; nonzero only for lab experiments
  };

  bool allocateBuffers();
  void initController(EpdBus& bus);
  // Re-runs the just-finished drive (planes rewritten, then re-trigger) while
  // the boot-clean budget lasts; see the definition for why.
  void runBootCleanPass(EpdBus& bus, const uint8_t* newPlane, const uint8_t* oldPlane);
  void resetRamCounter(EpdBus& bus);
  void writePlane(EpdBus& bus, uint8_t command, const uint8_t* data);
  void activate(EpdBus& bus, uint8_t control);
  void activateOtp(EpdBus& bus);
  void powerOffController(EpdBus& bus);
  void loadCustomLut(EpdBus& bus, const uint8_t lut[111]);
  // bgTopUp=false leaves LUT entry 0 completely idle (no background top-up)
  // for overlay passes, where entry 0 also holds undriven black text.
  uint16_t makeTriLut(uint8_t out[111], bool bgTopUp = true) const;
  uint16_t makePostCleanLut(uint8_t out[111]) const;
  bool runOtpUpdate(EpdBus& bus, const uint8_t* bwTarget, bool forceAll);
  // Encodes the source-to-target transition against the recorded glass state,
  // runs its required activations plus optional endpoint post-clean, and waits
  // them out. Returns true when a waveform actually ran. overlayOnly restricts
  // the drive set to changed pixels (the AA grays) for the follow-up pass after
  // a separately displayed B/W base — see displayGray().
  bool runUpdate(EpdBus& bus, const uint8_t* bwTarget, bool useGray, bool corrective, bool overlayOnly = false);
  void stashTarget(const uint8_t* fb, RefreshMode mode);
  bool commitPending(EpdBus& bus, bool useGray);
  void clearGrayStaging();
  bool markGrayRows(GrayPlane plane, uint16_t yStart, uint16_t numRows);
  bool displayWorkCancelled() const;

  bool _initialized = false;
  bool _needsFull = true;
  bool _darkBackground = false;
  // Paints remaining that double-drive to erase dwelled pre-boot residue.
  uint8_t _bootCleanPaints = 0;
  bool _lastBwValid = false;
  bool _preparingGray = false;
  bool _panelHasGray = false;
  bool _grayLsbReady = false;
  bool _grayMsbReady = false;
  bool _pendingTri = false;
  bool _pendingCorrective = false;
  bool _displayCommitted = false;
  bool _controllerPowered = false;
  enum class LutState : uint8_t { Unknown, OtpBw, Custom };
  LutState _lutState = LutState::Unknown;
  uint32_t _pendingGeneration = 0;
  uint32_t _grayLsbGeneration = 0;
  uint32_t _grayMsbGeneration = 0;
  uint32_t _renderGeneration = 0;
  std::atomic<uint32_t> _abortGeneration{0};
  uint32_t _displayWorkGeneration = 0;
  TriParams _tri{};
  PaperMonoGrayParams _grayParams{};

  // All logical raster order; writePlane() applies the 180-degree mount
  // transform on the way out.
  uint8_t* _lastBw = nullptr;    // last committed two-level target
  uint8_t* _pendingBw = nullptr; // stashed target awaiting its activation
  uint8_t* _grayLsb = nullptr;   // staged dark selector
  uint8_t* _grayMsb = nullptr;   // staged light selector
  uint8_t* _glassNonWhite = nullptr;  // on-glass q24: pixel is not white
  uint8_t* _glassBlack = nullptr;     // on-glass q26: pixel is black
  uint8_t* _sel24 = nullptr;     // LUT selector plane written to RAM 0x24
  uint8_t* _sel26 = nullptr;     // LUT selector plane written to RAM 0x26
  static constexpr uint16_t GRAY_ROWS = 480;
  static constexpr uint16_t GRAY_COVERAGE_BYTES = GRAY_ROWS / 8;
  uint8_t _grayLsbCoverage[GRAY_COVERAGE_BYTES]{};
  uint8_t _grayMsbCoverage[GRAY_COVERAGE_BYTES]{};
  uint16_t _grayLsbRowsCovered = 0;
  uint16_t _grayMsbRowsCovered = 0;
};

PaperMonoDriver& paperMonoDriver();
void paperMonoSetGrayParams(const PaperMonoGrayParams& params);
void paperMonoAbortGray();
void paperMonoResetGray();

}  // namespace freeink
