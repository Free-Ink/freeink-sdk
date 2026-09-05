#include <cmath>
#include <cstdio>

#include "../../src/MultiTouchGestureMath.h"

namespace {

using freeink::input_detail::classifyPinch;
using freeink::input_detail::classifyRotation;
using freeink::input_detail::GesturePoint;
using freeink::input_detail::hasRotationScale;
using freeink::input_detail::PinchResult;
using freeink::input_detail::RotationResult;

int checksRun = 0;
int checksFailed = 0;

#define CHECK(condition)                                               \
  do {                                                                 \
    ++checksRun;                                                       \
    if (!(condition)) {                                                \
      ++checksFailed;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                  \
  } while (0)

bool near(const float actual, const float expected, const float tolerance = 0.1f) {
  return std::fabs(actual - expected) <= tolerance;
}

void testClockwiseAndCounterClockwise() {
  RotationResult result;
  CHECK(classifyRotation({0, 0}, {100, 0}, {50, -50}, {50, 50}, result));
  CHECK(near(result.degrees, 90.0f));
  CHECK(result.centerX == 50);
  CHECK(result.centerY == 0);

  CHECK(classifyRotation({0, 0}, {100, 0}, {50, 50}, {50, -50}, result));
  CHECK(near(result.degrees, -90.0f));
}

void testAngleWraparound() {
  RotationResult result;
  CHECK(!classifyRotation({200, 200}, {102, 217}, {200, 200}, {102, 183}, result));
  CHECK(classifyRotation({200, 200}, {102, 217}, {200, 200}, {106, 166}, result));
  CHECK(result.degrees > 20.0f && result.degrees < 30.0f);
}

void testContactOrderIndependence() {
  RotationResult forward;
  RotationResult reversed;
  CHECK(classifyRotation({0, 0}, {100, 0}, {50, -50}, {50, 50}, forward));
  CHECK(classifyRotation({100, 0}, {0, 0}, {50, 50}, {50, -50}, reversed));
  CHECK(near(forward.degrees, reversed.degrees));
}

void testRejectionThresholds() {
  RotationResult result;
  CHECK(!classifyRotation({0, 0}, {100, 0}, {2, -17}, {98, 17},
                          result));  // Below 20 degrees.
  CHECK(!classifyRotation({0, 0}, {50, 0}, {25, -25}, {25, 25},
                          result));  // Span below 60 px.
  CHECK(!classifyRotation({0, 0}, {100, 0}, {-10, -10}, {110, 10},
                          result));  // Pinch/spread over 20%.
  CHECK(!classifyRotation({0, 0}, {100, 0}, {80, 0}, {180, 0},
                          result));  // Translation only.
}

void testScaleEligibilityCanLatchIntermediatePinch() {
  CHECK(hasRotationScale({0, 0}, {100, 0}, {0, 0}, {80, 0}));
  CHECK(hasRotationScale({0, 0}, {100, 0}, {0, 0}, {120, 0}));
  CHECK(!hasRotationScale({0, 0}, {100, 0}, {0, 0}, {79, 0}));
  CHECK(!hasRotationScale({0, 0}, {100, 0}, {0, 0}, {121, 0}));
}

void testRotationWinsWithTranslation() {
  RotationResult result;
  CHECK(classifyRotation({0, 0}, {100, 0}, {130, 50}, {130, 150}, result));
  CHECK(near(result.degrees, 90.0f));
  CHECK(result.centerX == 90);
  CHECK(result.centerY == 50);
}

void testPinchInAndOut() {
  PinchResult result;
  CHECK(classifyPinch({0, 0}, {100, 0}, {10, 0}, {70, 0}, result));
  CHECK(near(result.scale, 0.6f, 0.01f));
  CHECK(result.centerX == 45);
  CHECK(result.centerY == 0);

  CHECK(classifyPinch({0, 0}, {100, 0}, {-20, 0}, {140, 0}, result));
  CHECK(near(result.scale, 1.6f, 0.01f));
  CHECK(result.centerX == 55);
}

void testPinchRejectionThresholds() {
  PinchResult result;
  CHECK(!classifyPinch({0, 0}, {100, 0}, {5, 0}, {95, 0}, result));      // Small scale change.
  CHECK(!classifyPinch({0, 0}, {50, 0}, {0, 0}, {90, 0}, result));       // Start span below 60 px.
  CHECK(!classifyPinch({0, 0}, {100, 0}, {50, -50}, {50, 90}, result));  // Rotation too large.
  CHECK(!classifyPinch({0, 0}, {100, 0}, {60, 80}, {160, 80}, result));  // Translation only.
}

// The two classifiers are gated on the same separation band from opposite
// sides, so no gesture can be accepted by both. Each of these is accepted by
// one and must be rejected by the other.
void testPinchAndRotationAreMutuallyExclusive() {
  RotationResult rotation;
  PinchResult pinch;

  // A pure 90-degree turn holds the separation, so the pinch gate rejects it.
  CHECK(classifyRotation({0, 0}, {100, 0}, {50, -50}, {50, 50}, rotation));
  CHECK(!classifyPinch({0, 0}, {100, 0}, {50, -50}, {50, 50}, pinch));

  // A pure 40% close leaves the band, so the rotation gate rejects it.
  CHECK(classifyPinch({0, 0}, {100, 0}, {10, 0}, {70, 0}, pinch));
  CHECK(!classifyRotation({0, 0}, {100, 0}, {10, 0}, {70, 0}, rotation));
}

// Contacts that converge by exactly the 20% threshold while both also travel
// 80 px. The translation path tolerates 45 px of separation change, so it would
// accept this as a two-finger swipe; finishMultiTouchGesture() tries pinch
// first, which is what makes it a pinch.
void testPinchWinsWithTranslation() {
  PinchResult result;
  CHECK(classifyPinch({0, 0}, {100, 0}, {80, 0}, {160, 0}, result));
  CHECK(near(result.scale, 0.8f, 0.01f));
  CHECK(result.centerX == 85);
  CHECK(result.centerY == 0);
}

}  // namespace

int main() {
  testClockwiseAndCounterClockwise();
  testAngleWraparound();
  testContactOrderIndependence();
  testRejectionThresholds();
  testScaleEligibilityCanLatchIntermediatePinch();
  testRotationWinsWithTranslation();
  testPinchInAndOut();
  testPinchRejectionThresholds();
  testPinchAndRotationAreMutuallyExclusive();
  testPinchWinsWithTranslation();

  std::printf("%d checks, %d failures\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
