#pragma once

// FreeInk SDK — shared e-paper SPI/GPIO bus helper.
//
// Every panel driver talks to its controller through one EpdBus, configured
// once with the controller's SPI clock and BUSY polarity. This factors out the
// command/data framing, reset timing, busy-wait variants, and the vertical
// plane-mirror streamer that the UC8253 (X3) differential paths reuse.

#include <Arduino.h>
#include <SPI.h>

namespace freeink {

// BUSY line conventions differ by controller family.
enum class BusyPolarity : uint8_t {
  ActiveHigh,  // SSD1677 (X4 / de-link): busy while HIGH
  ActiveLow,   // ED2208 (M5 PaperColor): busy while LOW
  X3TwoPhase,  // UC8253 (X3): wait for the LOW edge, then wait back to HIGH
};

struct EpdPins {
  int8_t sclk;
  int8_t mosi;
  int8_t cs;
  int8_t dc;
  int8_t rst;
  int8_t busy;
  // EPD power-rail enable (active-high). PIN_UNASSIGNED (-1) on boards whose panel
  // is always powered; driven HIGH in begin() on boards that gate it (e.g. Sticky's
  // EP_PWR_EN GPIO47). Default -1 keeps existing 6-field aggregate initializers valid.
  int8_t powerEnable = -1;
};

class EpdBus {
 public:
  // coCs: a co-resident chip-select (e.g. the SD card sharing the SPI bus on
  // M5 PaperColor) that must be held de-asserted during panel transactions.
  void begin(const EpdPins& pins, uint32_t spiHz, BusyPolarity busy, int8_t spiMiso = -1, int8_t coCs = -1);

  // Hardware reset pulse; extraSettleMs adds a post-reset settle (X3 needs 50 ms).
  void reset(uint16_t extraSettleMs = 0);

  // Standalone command / data (each its own CS-framed transaction) — X4 style.
  void cmd(uint8_t c);
  void data(uint8_t d);
  void data(const uint8_t* d, uint16_t len);

  // Command followed by payload inside a single CS-low transaction — X3 style.
  void cmdData(uint8_t c, const uint8_t* d, uint16_t len);
  void cmdData2(uint8_t c, uint8_t d0, uint8_t d1);

  // Grouped transaction primitives (used by multi-step sequences, e.g. M5).
  void beginTxn();
  void endTxn();
  void rawCmd(uint8_t c);                            // assumes a transaction is open
  void rawData(uint8_t d);                           // assumes a transaction is open
  void rawWriteBytes(const uint8_t* d, uint16_t len);  // bulk data, transaction open

  // Wait for a refresh/operation to finish using the configured (or given) polarity.
  void waitBusy(const char* tag = nullptr);
  void waitBusy(BusyPolarity p, const char* tag = nullptr);

  // Optional hooks fired around long BUSY waits. A refresh takes ~0.3-2 s during
  // which the CPU only polls the BUSY pin; these let host firmware save power in
  // that window (e.g. reduce the CPU clock) without the SDK knowing the policy.
  // The begin hook fires once a wait exceeds BUSY_WAIT_HOOK_THRESHOLD_MS (so
  // short command waits never pay for it); the matching end hook fires when the
  // wait completes. Plain function pointers; both default to disabled.
  void setBusyWaitHooks(void (*beginHook)(), void (*endHook)()) {
    _busyWaitBeginHook = beginHook;
    _busyWaitEndHook = endHook;
  }

  // Optional slice hook replacing the poll delay once a wait has proven long
  // (i.e. after the begin hook fired). Receives the BUSY pin and the level that
  // means "still busy"; returns true if it already waited (e.g. host light-slept
  // until the pin left that level or a timer slice elapsed), false to fall back
  // to the plain delay. Lets host firmware sleep through the 0.3-2 s refresh
  // instead of polling, without the SDK knowing the wake mechanics.
  void setBusyWaitSliceHook(bool (*sliceHook)(int8_t busyPin, uint8_t busyLevel)) { _busyWaitSliceHook = sliceHook; }

  // Stream `plane` bottom-to-top (gates are physically reversed), widthBytes per
  // row, optionally bit-inverting. Replaces the per-driver mirror lambdas.
  void writeMirroredPlane(const uint8_t* plane, uint16_t height, uint16_t widthBytes, bool invert);

  // Send `ramCmd` then `plane` Y-flipped (gate order, bottom row first) as ONE
  // CS-low data burst — required by UC8253 DTM writes which must not toggle CS
  // mid-stream. (cmd uses its own CS pulse, matching the OEM sequence.)
  void sendPlaneFlipped(uint8_t ramCmd, const uint8_t* plane, uint16_t height, uint16_t widthBytes);

  // Send `ramCmd` then fill an entire RAM plane with `fillByte` (height rows of
  // widthBytes), as one CS-low burst. No framebuffer touched.
  void fillPlane(uint8_t ramCmd, uint8_t fillByte, uint16_t height, uint16_t widthBytes);

  const EpdPins& pins() const { return _pins; }
  uint32_t spiHz() const { return _spiHz; }
  BusyPolarity busyPolarity() const { return _busy; }

 private:
  // Busy-wait hooks (see setBusyWaitHooks / setBusyWaitSliceHook)
  static constexpr unsigned long BUSY_WAIT_HOOK_THRESHOLD_MS = 20;
  void (*_busyWaitBeginHook)() = nullptr;
  void (*_busyWaitEndHook)() = nullptr;
  bool (*_busyWaitSliceHook)(int8_t busyPin, uint8_t busyLevel) = nullptr;

  // One idle step of a long BUSY wait: defer to the slice hook once the wait
  // has proven long, otherwise (or if the hook declines) plain-delay.
  void busyIdle(bool longWait, uint8_t busyLevel, uint8_t fallbackDelayMs) {
    if (longWait && _busyWaitSliceHook != nullptr && _busyWaitSliceHook(_pins.busy, busyLevel)) {
      return;
    }
    delay(fallbackDelayMs);
  }

  EpdPins _pins{-1, -1, -1, -1, -1, -1};
  SPISettings _spi;
  BusyPolarity _busy = BusyPolarity::ActiveHigh;
  uint32_t _spiHz = 40000000;
  int8_t _coCs = -1;
};

}  // namespace freeink
