#pragma once

// UC8279d panel driver — Xteink X3, newer production run (792x528 B/W).
// UltraChip UC8279d ("d_B" silicon, TFT-module variant), driven in KW mode
// with EXTERNAL/custom LUTs (PSR REG=1): 1-bpp differential, DTM1 (0x10) = OLD
// plane, DTM2 (0x13) = NEW plane.
//
// These modules ship a BLANK MTP (address 0x000 != 0xA5, no factory command
// defaults, no per-temperature waveforms), so the host must drive EVERYTHING:
// PSR, the drive voltages (PWR/VDCS), booster, PLL, the full-panel PTL window
// (used instead of TRES), and the waveform LUT banks. An OTP-mode driver runs
// the panel with no drive rails and leaves it completely dark — the failure
// seen on the first UC8279d field units.
//
// The entire register recipe and every waveform bank were reverse-engineered
// from the stock X3 firmware (update.bin) and live in lut/Uc8279X3Luts.h:
//   - kUc8279X3_Init  : PSR 3F 4A, PTL 792x528 window, PFS 20,
//                       PWR 43 00 78 78 17, VDCS 24, BTST 25 25 3C, PLL 0F, E1 02
//   - kUc8279X3_BwGc  : B/W full (GC) waveform  (5 x 43, command-prefixed)
//   - kUc8279X3_BwDu  : B/W fast (DU) waveform  (5 x 43, command-prefixed)
//   - CDI 0x97 first refresh / 0xD7 later; DU adds E0=02, E5=5A.
// (XTF_AA / XTH4 grayscale banks are also captured there for a later AA path.)
//
// BUSY_N: low while busy (PON/DRF/POF all flag), same two-phase shape as the
// UC8253 X3 — reuses BusyPolarity::X3TwoPhase and the async start/finish split.

#include "PanelDriver.h"

namespace freeink {

class Uc8279Driver : public PanelDriver {
 public:
  Uc8279Driver();

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::X3TwoPhase; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;
  bool supportsAsyncDisplay() const override { return true; }

  void requestResync(uint8_t settlePasses) override;
  void skipInitialResync() override;

  // --- 4-level grayscale / anti-aliasing (mirrors the UC8253 X3 sibling) ---
  // Two 1bpp planes (LSB -> DTM1/old, MSB -> DTM2/new) encode 4 levels; the
  // external XTF_AA LUT bank resolves them. displayGrayscaleBase / precondition
  // run the OEM XTF_PRE_BW_MID "AA-pre-BW(mid)" settle before the gray planes.
  bool supportsStripGrayscale() const override { return true; }
  void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) override;
  void preconditionGrayscale(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;
  void grayscaleRevert(EpdBus& bus, const uint8_t* fb) override;

 private:
  void initController(EpdBus& bus);
  // Replay a {cmd, len, data...} register script (kUc8279X3_Init).
  void sendScript(EpdBus& bus, const uint8_t* script, uint16_t len);
  // Load a 5-table command-prefixed waveform bank (BW_GC/BW_DU/XTF_PRE_BW_MID)
  // into LUT registers 0x20-0x24: byte 0 of each table is the register id.
  void loadBank(EpdBus& bus, const uint8_t (*bank)[43]);
  // Load the raw (non-prefixed) 49-byte XTF_AA grayscale bank: 0x20+i then table.
  void loadXtfAa(EpdBus& bus);
  // Blocking PON -> DRF -> wait (-> POF) used by the grayscale paths.
  void triggerGrayRefresh(EpdBus& bus, bool turnOff);
  // Enter the full 792x528 PTL partial window (PTIN + PTL). ALL RAM plane writes
  // and refreshes must run in this window: normal mode addresses the controller's
  // 800x600 frame (100-byte rows) and misaligns our 99-byte-row planes. The B/W
  // path is implicitly windowed (init leaves this PTL set + displayStart's PTIN);
  // the grayscale methods must re-assert it explicitly.
  void grayWindowIn(EpdBus& bus);

  uint16_t _w;   // visible width  (792)
  uint16_t _h;   // visible height (528)
  uint16_t _wb;  // width in bytes (99)
  uint32_t _bufferSize;

  bool _isScreenOn = false;
  bool _firstRefresh = true;   // CDI 0x97 on the first refresh after init, 0xD7 after
  bool _oldPlaneValid = false; // DTM1 holds a real previous frame (differential baseline)
  bool _forceFullSyncNext = false;
  // Boot initial-full budget: force GC (strong clear) for this many content
  // paints after begin(), so the first screen after the splash is a real clear
  // even though CrossPoint requests it as FAST. Matches the UC8253 X3 sibling.
  uint8_t _initialFullsRemaining = 0;

  // Grayscale state (mirrors the UC8253 sibling): _inGrayscaleMode means the
  // gray bank/planes are loaded so the next B/W turn must revert; _lsbValid
  // means gray planes were written over DTM1/DTM2 (no valid B/W baseline).
  bool _inGrayscaleMode = false;
  bool _lsbValid = false;

  // Async split state (see Uc8253X3Driver for the contract).
  bool _pendingRefresh = false;
  bool _pendingTurnOff = false;
  bool _pendingUsedGc = false;  // this refresh ran the GC bank (spends the budget)
};

PanelDriver& uc8279Driver();

}  // namespace freeink
