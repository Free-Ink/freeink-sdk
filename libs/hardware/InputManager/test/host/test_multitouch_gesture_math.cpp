#include <cmath>
#include <cstdio>

#include "../../src/MultiTouchGestureMath.h"

namespace {

using freeink::input_detail::classifyRotation;
using freeink::input_detail::GesturePoint;
using freeink::input_detail::hasRotationScale;
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

}  // namespace

int main() {
  testClockwiseAndCounterClockwise();
  testAngleWraparound();
  testContactOrderIndependence();
  testRejectionThresholds();
  testScaleEligibilityCanLatchIntermediatePinch();
  testRotationWinsWithTranslation();

  std::printf("%d checks, %d failures\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
