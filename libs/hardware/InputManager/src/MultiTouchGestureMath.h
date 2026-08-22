#pragma once

#include <cmath>
#include <cstdint>

namespace freeink::input_detail {

struct GesturePoint {
  int32_t x;
  int32_t y;
};

struct RotationResult {
  float degrees = 0.0f;
  uint16_t centerX = 0;
  uint16_t centerY = 0;
};

inline int64_t separationSquared(const GesturePoint& first, const GesturePoint& second) {
  const int64_t dx = static_cast<int64_t>(second.x) - first.x;
  const int64_t dy = static_cast<int64_t>(second.y) - first.y;
  return dx * dx + dy * dy;
}

inline bool hasRotationScale(const GesturePoint& startFirst, const GesturePoint& startSecond,
                             const GesturePoint& currentFirst, const GesturePoint& currentSecond) {
  constexpr int64_t scalePercentSq = 100LL * 100LL;
  constexpr int64_t minScalePercentSq = 80LL * 80LL;
  constexpr int64_t maxScalePercentSq = 120LL * 120LL;
  const int64_t startSeparationSq = separationSquared(startFirst, startSecond);
  const int64_t currentSeparationSq = separationSquared(currentFirst, currentSecond);
  return currentSeparationSq * scalePercentSq >= startSeparationSq * minScalePercentSq &&
         currentSeparationSq * scalePercentSq <= startSeparationSq * maxScalePercentSq;
}

inline bool classifyRotation(const GesturePoint& startFirst, const GesturePoint& startSecond,
                             const GesturePoint& endFirst, const GesturePoint& endSecond, RotationResult& result) {
  constexpr int64_t minSeparationPxSq = 60LL * 60LL;
  constexpr float minRotationDegrees = 20.0f;
  constexpr double radiansToDegrees = 57.295779513082320876;

  const int64_t startDx = static_cast<int64_t>(startSecond.x) - startFirst.x;
  const int64_t startDy = static_cast<int64_t>(startSecond.y) - startFirst.y;
  const int64_t endDx = static_cast<int64_t>(endSecond.x) - endFirst.x;
  const int64_t endDy = static_cast<int64_t>(endSecond.y) - endFirst.y;
  const int64_t startSeparationSq = separationSquared(startFirst, startSecond);
  const int64_t endSeparationSq = separationSquared(endFirst, endSecond);

  if (startSeparationSq < minSeparationPxSq || endSeparationSq < minSeparationPxSq) return false;
  if (!hasRotationScale(startFirst, startSecond, endFirst, endSecond)) return false;

  // Panel coordinates use a downward-positive Y axis, so a positive cross
  // product is a clockwise visual rotation.
  const int64_t cross = startDx * endDy - startDy * endDx;
  const int64_t dot = startDx * endDx + startDy * endDy;
  const float degrees =
      static_cast<float>(std::atan2(static_cast<double>(cross), static_cast<double>(dot)) * radiansToDegrees);
  if (std::fabs(degrees) < minRotationDegrees) return false;

  result.degrees = degrees;
  result.centerX =
      static_cast<uint16_t>((static_cast<int64_t>(startFirst.x) + startSecond.x + endFirst.x + endSecond.x) / 4);
  result.centerY =
      static_cast<uint16_t>((static_cast<int64_t>(startFirst.y) + startSecond.y + endFirst.y + endSecond.y) / 4);
  return true;
}

}  // namespace freeink::input_detail
