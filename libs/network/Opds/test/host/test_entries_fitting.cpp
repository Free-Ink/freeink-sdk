#include <cstdio>
#include <cstdlib>

#include "OpdsEntry.h"

using namespace OpdsLimits;

static int failures = 0;
static void expect(const char* what, size_t got, size_t want) {
  if (got == want) return;
  std::printf("FAIL %s: got %zu, want %zu\n", what, got, want);
  ++failures;
}

int main() {
  constexpr size_t kHeadroom = 4096;
  constexpr size_t kMin = 4096;  // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
  constexpr size_t kEntry = sizeof(OpdsEntry);
  const size_t kPsramBig = 7 * 1024 * 1024;

  // No PSRAM malloc: only internal DRAM counts, as before.
  expect("internal room", entriesFitting(64, 100000, 0, NO_PSRAM_MALLOC, kHeadroom), 64);
  expect("internal tight", entriesFitting(64, kHeadroom + 3 * kEntry, 0, NO_PSRAM_MALLOC, kHeadroom), 3);
  expect("internal below headroom", entriesFitting(64, kHeadroom, 0, NO_PSRAM_MALLOC, kHeadroom), 0);
  // PSRAM ignored when malloc does not use it, however large.
  expect("psram ignored without malloc", entriesFitting(64, 2000, kPsramBig, NO_PSRAM_MALLOC, kHeadroom), 0);

  // TLS has split internal DRAM, PSRAM is free: the full reserve fits.
  expect("psram rescues", entriesFitting(64, 2000, kPsramBig, kMin, kHeadroom), 64);
  expect("psram never lowers", entriesFitting(64, 100000, 5000, kMin, kHeadroom), 64);

  // A reserve at or under the malloc threshold stays in internal DRAM, so a
  // PSRAM block that only holds that few entries must not count.
  const size_t smallPsram = kHeadroom + (kMin / kEntry) * kEntry;  // holds kMin/kEntry entries
  expect("small psram request stays internal", entriesFitting(64, 2000, smallPsram, kMin, kHeadroom), 0);
  const size_t bigEnough = kHeadroom + (kMin / kEntry + 2) * kEntry;  // clears the threshold
  expect("psram above threshold counts", entriesFitting(64, 2000, bigEnough, kMin, kHeadroom),
         kMin / kEntry + 2);

  // A threshold of 0 (every malloc may use PSRAM) is a valid setting, not "off".
  expect("threshold zero uses psram", entriesFitting(64, 2000, kPsramBig, 0, kHeadroom), 64);

  // Exact boundary, pinned with a threshold that is a whole number of entries:
  // a reserve of exactly `psramMinBytes` stays internal, one entry more counts.
  constexpr size_t kBoundary = 10 * kEntry;
  expect("boundary equal stays internal", entriesFitting(64, 2000, kHeadroom + kBoundary, kBoundary, kHeadroom), 0);
  expect("boundary plus one counts", entriesFitting(64, 2000, kHeadroom + kBoundary + kEntry, kBoundary, kHeadroom),
         11);

  // Internal larger than what PSRAM offers: the larger of the two wins.
  expect("internal beats psram credit", entriesFitting(64, kHeadroom + 40 * kEntry, bigEnough, kMin, kHeadroom), 40);
  // A PSRAM block that does not clear the headroom holds nothing.
  expect("psram below headroom", entriesFitting(64, 2000, kHeadroom - 1, kMin, kHeadroom), 0);

  if (failures) return EXIT_FAILURE;
  std::puts("entriesFitting tests passed");
  return EXIT_SUCCESS;
}
