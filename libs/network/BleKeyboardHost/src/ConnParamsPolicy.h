#pragma once

// The host's answer to a peripheral's connection-parameter request
// (BleKeyboardHost.cpp, onConnParamsUpdateRequest). Pure and NimBLE-free so the host
// tests can run it. Units are BLE's: interval 1.25 ms, supervision timeout 10 ms.

#include <stdint.h>

namespace freeink {

struct ConnParamsRequest {
  uint16_t itvlMin;
  uint16_t itvlMax;
  uint16_t latency;
  uint16_t supervisionTimeout;
};

// The host's own parameters (begin()). 8 s of supervision timeout: on a single-core
// C3 a chapter render blocks the host for seconds, and the default 2.56 s dropped
// links there (ea75c718).
inline constexpr uint16_t kHostItvlMin = 12;
inline constexpr uint16_t kHostItvlMax = 24;
inline constexpr uint16_t kHostLatency = 0;
inline constexpr uint16_t kHostSupervisionTimeout = 800;

// Bounds of an acceptable request: a key waits at most one interval for the next
// connection event, so a page turner's interval stays at 100 ms or less; peripheral
// latency up to 30 events; the spec caps the timeout at 32 s.
inline constexpr uint16_t kMinInterval = 6;
inline constexpr uint16_t kMaxAcceptedInterval = 80;
inline constexpr uint16_t kMaxAcceptedLatency = 30;
inline constexpr uint16_t kMaxSupervisionTimeout = 3200;

// The spec's floor, timeout (10 ms) > (1 + latency) * itvlMax (1.25 ms) * 2, that is
// 4 * timeout > (1 + latency) * itvlMax, holds for every request inside the bounds
// once its timeout is at least the host's.
static_assert(4u * kHostSupervisionTimeout > (1u + kMaxAcceptedLatency) * kMaxAcceptedInterval,
              "an accepted request would break the supervision timeout's floor");

// True accepts the request's interval and latency, with its supervision timeout
// raised to at least the host's own (written back into `req`); false rejects it. A
// remote that naps between presses asks for peripheral latency, and refusing it left
// one (a Free2) unanswered under latency 0 until the link timed out, about 8 s into
// every idle.
constexpr bool acceptPeerConnParams(ConnParamsRequest& req) {
  if (req.itvlMin < kMinInterval || req.itvlMin > req.itvlMax ||
      req.itvlMax > kMaxAcceptedInterval || req.latency > kMaxAcceptedLatency) {
    return false;
  }
  uint16_t timeout = req.supervisionTimeout < kHostSupervisionTimeout
                         ? kHostSupervisionTimeout
                         : req.supervisionTimeout;
  if (timeout > kMaxSupervisionTimeout) timeout = kMaxSupervisionTimeout;
  req.supervisionTimeout = timeout;
  return true;
}

}  // namespace freeink
