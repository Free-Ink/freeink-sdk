#include <SDCardManager.h>

#include <cassert>
#include <cstdio>
#include <limits>

class Sink : public Print {
 public:
  std::vector<uint8_t> data;
  size_t maxWrite = std::numeric_limits<size_t>::max();
  size_t stopAt = std::numeric_limits<size_t>::max();
  bool invalidCount = false;
  uint32_t writeMs = 0;
  unsigned writes = 0;

  size_t write(const uint8_t* bytes, size_t count) override {
    ++writes;
    nowMs += writeMs;
    if (invalidCount) return count + 1;
    if (data.size() >= stopAt) return 0;
    const size_t accepted = std::min({count, maxWrite, stopAt - data.size()});
    data.insert(data.end(), bytes, bytes + accepted);
    return accepted;
  }
};

static void reset(size_t size) {
  fakeSd::data.resize(size);
  for (size_t i = 0; i < size; ++i) fakeSd::data[i] = static_cast<uint8_t>(i);
  fakeSd::advertisedSize = size;
  fakeSd::openFails = false;
  fakeSd::failAt = std::numeric_limits<size_t>::max();
  fakeSd::maxRead = std::numeric_limits<size_t>::max();
  fakeSd::largestRequest = 0;
  fakeSd::reads = fakeSd::closes = 0;
  fakeSd::readMs = nowMs = yields = watchdogResets = 0;
  watchdogSubscribed = false;
}

int main() {
  {
    SDCardManager card;
    uint64_t used = 99;
    assert(!card.sdUsedBytes(used) && used == 0);
    nowMs = 0;
    fakeSd::clusters = 64;
    fakeSd::freeClusters = 64;
    fakeSd::clusterScans = 0;
    assert(card.begin());
    assert(card.sdUsedBytes(used) && used == 0);  // a genuinely empty card
    fakeSd::freeClusters = -1;
    assert(card.sdUsedBytes(used) && used == 0);  // cached valid zero
    nowMs += 20001;
    assert(!card.sdUsedBytes(used) && used == 0);
    const auto scansAfterFailure = fakeSd::clusterScans;
    assert(card.sdUsedBytes() == 0);  // preserve the legacy API
    assert(!card.sdUsedBytes(used));
    assert(fakeSd::clusterScans == scansAfterFailure);  // cache failure, too
    fakeSd::freeClusters = 16;
    nowMs += 20001;
    assert(card.sdUsedBytes(used) && used == 48 * 512);
    fakeSd::freeClusters = 100;
    nowMs += 20001;
    assert(card.sdUsedBytes(used) && used == 0);  // retain the existing clamp
    fakeSd::clusters = 1;
    fakeSd::freeClusters = 1;
  }

  SDCardManager manager;
  reset(8);
  Sink unopened;
  assert(!manager.readFileToStream("/test", unopened));
  assert(fakeSd::reads == 0 && fakeSd::closes == 0);
  assert(manager.begin());

  fakeSd::openFails = true;
  assert(!manager.readFileToStream("/test", unopened));
  assert(fakeSd::reads == 0 && fakeSd::closes == 0);

  for (const size_t size : {size_t(0), size_t(8), size_t(256), size_t(769)}) {
    for (const size_t chunk : {size_t(0), size_t(1), size_t(17), size_t(256), size_t(1024)}) {
      reset(size);
      Sink sink;
      assert(manager.readFileToStream("/test", sink, chunk));
      assert(sink.data == fakeSd::data);
      assert(fakeSd::closes == 1);
      assert(fakeSd::largestRequest <= (chunk == 0 ? 256 : std::min(chunk, size_t(256))));
      assert(yields == 0 && watchdogResets == 0);
    }
  }

  reset(769);
  Sink partial;
  partial.maxWrite = 3;
  assert(manager.readFileToStream("/test", partial));
  assert(partial.data == fakeSd::data);
  assert(fakeSd::closes == 1);

  for (const size_t stopAt : {size_t(0), size_t(3), size_t(259)}) {
    reset(769);
    Sink stalled;
    stalled.maxWrite = 3;
    stalled.stopAt = stopAt;
    assert(!manager.readFileToStream("/test", stalled));
    assert(stalled.data.size() == stopAt);
    assert(std::equal(stalled.data.begin(), stalled.data.end(), fakeSd::data.begin()));
    assert(fakeSd::closes == 1);
    assert(fakeSd::reads == (stopAt <= 256 ? 1u : 2u));
  }

  reset(8);
  Sink invalid;
  invalid.invalidCount = true;
  assert(!manager.readFileToStream("/test", invalid));
  assert(fakeSd::closes == 1);

  for (const size_t failAt : {size_t(0), size_t(256)}) {
    reset(769);
    fakeSd::failAt = failAt;
    Sink sink;
    assert(!manager.readFileToStream("/test", sink));
    assert(sink.data.size() == failAt);
    assert(fakeSd::closes == 1);
  }

  reset(8);
  fakeSd::advertisedSize = 12;
  Sink truncated;
  assert(!manager.readFileToStream("/test", truncated));
  assert(truncated.data == fakeSd::data && fakeSd::closes == 1);

  reset(8);
  fakeSd::maxRead = 0;
  Sink stalledRead;
  assert(!manager.readFileToStream("/test", stalledRead));
  assert(stalledRead.data.empty() && fakeSd::reads == 1 && fakeSd::closes == 1);

  reset(769);
  fakeSd::maxRead = 7;
  Sink partialRead;
  assert(manager.readFileToStream("/test", partialRead));
  assert(partialRead.data == fakeSd::data && fakeSd::closes == 1);

  reset(0);
  fakeSd::advertisedSize = uint64_t(1) << 32;
  fakeSd::failAt = 0;
  Sink largeFile;
  assert(!manager.readFileToStream("/test", largeFile));
  assert(fakeSd::reads == 1 && fakeSd::closes == 1);

  for (const bool subscribed : {false, true}) {
    reset(769);
    watchdogSubscribed = subscribed;
    fakeSd::readMs = 100;
    Sink sink;
    assert(manager.readFileToStream("/test", sink));
    assert(yields == 4 && watchdogResets == (subscribed ? 4u : 0u));
    assert(sink.data == fakeSd::data);
  }

  reset(8);
  watchdogSubscribed = true;
  nowMs = std::numeric_limits<uint32_t>::max() - 50;
  Sink slowPartial;
  slowPartial.maxWrite = 1;
  slowPartial.writeMs = 25;
  assert(manager.readFileToStream("/test", slowPartial));
  assert(yields == 2 && watchdogResets == 2);
  assert(slowPartial.data == fakeSd::data && fakeSd::closes == 1);

  std::puts("SD stream tests passed (short/stalled writes, read errors, EOF, chunk limits, watchdog)");
}
