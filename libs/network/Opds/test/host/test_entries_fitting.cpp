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
  constexpr size_t kEntry = sizeof(OpdsEntry);
  const size_t kPsramBig = 7 * 1024 * 1024;
  const size_t full = ENTRY_STORAGE_CAPACITY;

  // No PSRAM: only internal DRAM counts, as before.
  expect("internal room", entriesFitting(full, 100000, 0, kHeadroom), full);
  expect("internal tight", entriesFitting(full, kHeadroom + 3 * kEntry, 0, kHeadroom), 3);
  expect("internal below headroom", entriesFitting(full, kHeadroom, 0, kHeadroom), 0);

  // X4 Pro with Wi-Fi and TLS up: a few KB of internal DRAM, PSRAM mostly free.
  // The full reserve fits in PSRAM.
  expect("psram rescues", entriesFitting(full, 5 * 1024, kPsramBig, kHeadroom), full);
  // A PSRAM block under the full reserve still counts; malloc falls back to it
  // whatever the request size.
  expect("small psram counts", entriesFitting(full, 2000, kHeadroom + 5 * kEntry, kHeadroom), 5);
  // The larger of the two blocks wins, and PSRAM never lowers the result.
  expect("internal beats psram", entriesFitting(full, kHeadroom + 40 * kEntry, kHeadroom + 5 * kEntry, kHeadroom), 40);
  expect("psram never lowers", entriesFitting(full, 100000, 5000, kHeadroom), full);
  // A PSRAM block that does not clear the headroom holds nothing.
  expect("psram below headroom", entriesFitting(full, 2000, kHeadroom - 1, kHeadroom), 0);

  if (failures) return EXIT_FAILURE;
  std::puts("entriesFitting tests passed");
  return EXIT_SUCCESS;
}
