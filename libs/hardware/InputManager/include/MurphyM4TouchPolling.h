#pragma once

#include <cstdint>

namespace freeink::murphy_m4_touch {

enum class FrameState : uint8_t { Contact, Released, Invalid };

struct RawPoint {
  uint16_t x = 0;
  uint16_t y = 0;
  uint32_t timestamp = 0;
};

struct PollState {
  bool contact = false;
  RawPoint down{};
  RawPoint latest{};
  bool completedPending = false;
  RawPoint completedDown{};
  RawPoint completedUp{};
};

using Snapshot = PollState;

constexpr uint16_t axis(const uint8_t high, const uint8_t low) { return static_cast<uint16_t>(high & 0x0F) << 8 | low; }

constexpr FrameState classifyFrame(const uint8_t status, const uint8_t xHigh, const uint8_t xLow, const uint8_t yHigh,
                                   const uint8_t yLow) {
  const uint8_t pointCount = status & 0x0F;
  if (status >= 0x10 || pointCount > 2) return FrameState::Invalid;
  if (pointCount == 0) return FrameState::Released;

  const uint8_t event = xHigh >> 6;
  if (event == 3) return FrameState::Invalid;
  if (status == 0x01 && xHigh == 0x01 && xLow == 0x01 && yHigh == 0x01 && yLow == 0x01) {
    return FrameState::Invalid;
  }
  return event == 1 ? FrameState::Released : FrameState::Contact;
}

constexpr bool pointInBounds(const uint8_t xHigh, const uint8_t xLow, const uint8_t yHigh, const uint8_t yLow,
                             const uint16_t maxX, const uint16_t maxY, const bool swapXY) {
  const uint16_t rawX = axis(xHigh, xLow);
  const uint16_t rawY = axis(yHigh, yLow);
  return (swapXY ? rawY : rawX) <= maxX && (swapXY ? rawX : rawY) <= maxY;
}

inline void recordContact(PollState& state, const uint16_t rawX, const uint16_t rawY, const uint32_t now) {
  const RawPoint point{rawX, rawY, now};
  if (!state.contact) state.down = point;
  state.contact = true;
  state.latest = point;
}

inline void recordRelease(PollState& state, const uint32_t now) {
  if (!state.contact) return;
  if (!state.completedPending) {
    state.completedPending = true;
    state.completedDown = state.down;
    state.completedUp = {state.latest.x, state.latest.y, now};
  }
  state.contact = false;
}

inline bool releaseIfStale(PollState& state, const uint32_t now, const uint32_t staleMs) {
  if (!state.contact || now - state.latest.timestamp < staleMs) return false;
  recordRelease(state, now);
  return true;
}

inline Snapshot takeSnapshot(PollState& state) {
  const Snapshot snapshot = state;
  state.completedPending = false;
  return snapshot;
}

static_assert(axis(0x41, 0x23) == 0x0123, "FT6336U coordinate decoding must ignore event/id bits");
static_assert(classifyFrame(1, 0x01, 0x23, 0x00, 0x42) == FrameState::Contact);
static_assert(classifyFrame(1, 0x81, 0x23, 0x00, 0x42) == FrameState::Contact);
static_assert(classifyFrame(1, 0x41, 0x23, 0x00, 0x42) == FrameState::Released);
static_assert(classifyFrame(0, 0, 0, 0, 0) == FrameState::Released);
static_assert(classifyFrame(0x10, 0, 0, 0, 0) == FrameState::Invalid);
static_assert(pointInBounds(1, 0xDF, 3, 0x1F, 799, 479, true));
static_assert(!pointInBounds(1, 0xE0, 3, 0x1F, 799, 479, true));

}  // namespace freeink::murphy_m4_touch
