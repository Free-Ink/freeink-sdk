#include <MetalioAudio.h>
#include <cassert>
#include <climits>
#include <cstdio>
using namespace freeink::metalio;
int main() {
  static_assert(FREEINK_CAP_AUDIO && FREEINK_CAP_MIC && FREEINK_CAP_IMU);
  assert(outputSample(INT16_MIN, 100) == INT32_MIN);
  assert(outputSample(INT16_MAX, 100) == 2147418112);
  assert(outputSample(12345, 0) == 0);
  assert(outputSample(-1000, 50) == -16384000);
  assert(outputSample(1000, 255) == outputSample(1000, 100));
  assert(inputSample(INT32_MIN) == INT16_MIN);
  assert(inputSample(INT32_MAX) == INT16_MAX);
  assert(inputSample(-4096) == -1 && inputSample(4096) == 1);
  int speaker = 0, microphone = 0, other = 0;
  uartFail = true;
  assert(!acquireAudio(&speaker, false));
  assert(!audioBus().speaker && !audioBus().tx);
  uartFail = false;
  failAllocation = true;
  assert(!acquireAudio(&speaker, false));
  assert(!audioBus().speaker && !audioBus().tx);
  failAllocation = false;
  for (int failedChannel = 1; failedChannel <= 2; ++failedChannel) {
    failInitAt = initCalls + failedChannel;
    assert(!acquireAudio(&speaker, false));
    assert(txFake.deleted && rxFake.deleted && !audioBus().tx && !audioBus().rx);
  }
  failInitAt = 0;
  uartCommands.clear();
  assert(acquireAudio(&microphone, true));
  assert(uartCommands == "AT+RX=2\r\nAT+MODE=1\r\n");
  assert(acquireAudio(&microphone, true));
  assert(!acquireAudio(&other, true));
  failEnable = true;
  assert(!startAudio(&microphone, true));
  failEnable = false;
  assert(startAudio(&microphone, true));
  const int before = allocations;
  assert(acquireAudio(&speaker, false) && allocations == before);
  assert(startAudio(&speaker, false));
  assert(rxFake.enabled && txFake.enabled);
  releaseAudio(&other, false); // A non-owner cannot stop another stream.
  assert(txFake.enabled);
  releaseAudio(&speaker, false);
  assert(rxFake.enabled && !txFake.enabled && !txFake.deleted);
  assert(acquireAudio(&speaker, false) && startAudio(&speaker, false));
  releaseAudio(&microphone, true);
  assert(txFake.enabled && !rxFake.enabled);
  releaseAudio(&speaker, false);
  assert(txFake.deleted && rxFake.deleted && !audioBus().tx && !audioBus().rx);
  // Fresh begin after full shutdown sends the mode commands again.
  assert(acquireAudio(&speaker, false));
  releaseAudio(&speaker, false);
  assert(txFake.deleted && rxFake.deleted);
  puts("Metalio audio format, ownership, duplex lifecycle, cleanup and retries passed");
}
