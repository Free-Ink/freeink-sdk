#pragma once

#include <cstddef>
#include <cstdint>

namespace freeink {

enum class FastRefreshProfile : uint8_t {
  PanelDefault,
  TerminalInteractive,
  TerminalSettle,
  // Source compatibility for the first knietty prototype.
  TerminalTurbo = TerminalInteractive,
};

struct RefreshTiming {
  uint32_t totalUs = 0;
  uint32_t waveformUs = 0;
  uint32_t transferUs = 0;
  uint32_t lutUs = 0;
  uint32_t planeUs = 0;
  uint32_t baselineUs = 0;
  uint32_t activationToBusyUs = 0;
  uint32_t powerOffUs = 0;
  uint32_t presentedAtUs = 0;
  uint32_t readyAtUs = 0;
  bool windowed = false;
};

struct DriverRefreshTiming {
  uint32_t lutUs = 0;
  uint32_t planeUs = 0;
  uint32_t baselineUs = 0;
  uint32_t activationToBusyUs = 0;
  uint32_t powerOffUs = 0;
  uint32_t presentedAtUs = 0;
};

// One byte-aligned panel-memory rectangle inside a caller-owned packed buffer.
// Multiple regions may be written before one MASTER_ACTIVATION, then replayed
// after BUSY to restore the controller's differential baseline. The packed
// bytes must remain immutable until the deferred refresh is finished.
struct PackedWindowRegion {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t offset = 0;
};

constexpr size_t MAX_PACKED_WINDOW_REGIONS = 32;

}  // namespace freeink
