#include <cstdio>

#include "MurphyM4Batch.h"
#include "MurphyM4TouchPolling.h"

namespace {

namespace touch = freeink::murphy_m4_touch;

int checks = 0;
int failed = 0;

void want(const bool condition, const char* message) {
  ++checks;
  if (!condition) {
    ++failed;
    std::printf("FAIL murphy-m4  %s\n", message);
  }
}

void batchCalibration() {
  using freeink::mapMurphyM4TouchShortAxis;
  using freeink::MurphyM4Batch;

#if FREEINK_MURPHY_M4_BATCH1
  want(freeink::defaultMurphyM4Batch() == MurphyM4Batch::First, "batch-1 build selects batch 1 by default");
#else
  want(freeink::defaultMurphyM4Batch() == MurphyM4Batch::Second, "default build selects batch 2");
#endif
  want(freeink::murphyM4TouchRange(MurphyM4Batch::First).min == -52, "batch 1 minimum calibration");
  want(freeink::murphyM4TouchRange(MurphyM4Batch::First).max == 553, "batch 1 maximum calibration");
  want(freeink::murphyM4TouchRange(MurphyM4Batch::Second).min == -47, "batch 2 minimum calibration");
  want(freeink::murphyM4TouchRange(MurphyM4Batch::Second).max == 514, "batch 2 maximum calibration");
  want(mapMurphyM4TouchShortAxis(40, MurphyM4Batch::First, 479) == 72, "batch 1 reference point 40");
  want(mapMurphyM4TouchShortAxis(330, MurphyM4Batch::First, 479) == 302, "batch 1 reference point 330");
  want(mapMurphyM4TouchShortAxis(389, MurphyM4Batch::First, 479) == 349, "batch 1 reference point 389");
  want(mapMurphyM4TouchShortAxis(553, MurphyM4Batch::First, 479) == 479, "batch 1 maximum maps to last row");
  want(mapMurphyM4TouchShortAxis(514, MurphyM4Batch::Second, 479) == 479, "batch 2 maximum maps to last row");
  want(mapMurphyM4TouchShortAxis(2000, MurphyM4Batch::Second, 479) == 479, "above-range raw coordinate clamps");
}

void frameValidation() {
  want(touch::axis(0x41, 0x23) == 0x0123, "coordinate decoding ignores event and ID bits");
  want(touch::classifyFrame(1, 0x01, 0x23, 0x00, 0x42) == touch::FrameState::Contact, "press frame");
  want(touch::classifyFrame(1, 0x81, 0x23, 0x00, 0x42) == touch::FrameState::Contact, "contact frame");
  want(touch::classifyFrame(1, 0x41, 0x23, 0x00, 0x42) == touch::FrameState::Released, "lift frame");
  want(touch::classifyFrame(0, 0, 0, 0, 0) == touch::FrameState::Released, "zero points means release");
  want(touch::classifyFrame(0x10, 0, 0, 0, 0) == touch::FrameState::Invalid, "invalid status is rejected");
  want(touch::classifyFrame(1, 0xC1, 0x23, 0, 0x42) == touch::FrameState::Invalid, "reserved event is rejected");
  want(touch::classifyFrame(3, 0x01, 0x23, 0, 0x42) == touch::FrameState::Invalid, "excess points are rejected");
  want(touch::classifyFrame(1, 1, 1, 1, 1) == touch::FrameState::Invalid, "stuck all-ones frame is rejected");
  want(touch::pointInBounds(1, 0xDF, 3, 0x1F, 799, 479, true), "last valid swapped point");
  want(!touch::pointInBounds(1, 0xE0, 3, 0x1F, 799, 479, true), "long-axis overflow is rejected");
  want(!touch::pointInBounds(1, 0xDF, 3, 0x20, 799, 479, true), "short-axis overflow is rejected");
}

void pollingState() {
  touch::PollState state;
  touch::recordContact(state, 40, 100, 1000);
  touch::recordContact(state, 330, 150, 1040);
  touch::recordRelease(state, 1080);

  const auto snapshot = touch::takeSnapshot(state);
  want(!snapshot.contact, "released contact is no longer down");
  want(snapshot.completedPending, "completed gesture is pending");
  want(snapshot.completedDown.x == 40 && snapshot.completedDown.y == 100 && snapshot.completedDown.timestamp == 1000,
       "gesture retains its first point");
  want(snapshot.completedUp.x == 330 && snapshot.completedUp.y == 150 && snapshot.completedUp.timestamp == 1080,
       "gesture retains its last point and release time");
  want(!touch::takeSnapshot(state).completedPending, "consumed gesture is delivered once");

  touch::recordContact(state, 10, 20, 200);
  touch::recordRelease(state, 220);
  touch::recordContact(state, 30, 40, 230);
  touch::recordRelease(state, 250);
  const auto first = touch::takeSnapshot(state);
  want(first.completedPending && first.completedDown.x == 10 && first.completedUp.timestamp == 220,
       "first unconsumed gesture is not overwritten");

  touch::PollState stale;
  touch::recordContact(stale, 70, 80, 500);
  want(!touch::releaseIfStale(stale, 599, 100), "contact remains before stale threshold");
  want(touch::releaseIfStale(stale, 600, 100), "contact releases at stale threshold");
  const auto released = touch::takeSnapshot(stale);
  want(released.completedPending && released.completedUp.timestamp == 600, "stale release completes the gesture");
}

}  // namespace

int main() {
  batchCalibration();
  frameValidation();
  pollingState();
  std::printf("%d checks, %d failed\n", checks, failed);
  return failed == 0 ? 0 : 1;
}