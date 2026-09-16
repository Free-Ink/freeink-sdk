#include <AudioManager.h>
#include <Microphone.h>
#include <MetalioAudio.h>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace freeink::metalio;
static std::vector<uint8_t> wav(uint32_t rate, uint16_t channels, std::initializer_list<int16_t> pcm) {
  std::vector<uint8_t> b;
  auto u16 = [&](uint16_t v) { b.push_back(v); b.push_back(v >> 8); };
  auto u32 = [&](uint32_t v) { u16(v); u16(v >> 16); };
  auto text = [&](const char* s) { b.insert(b.end(), s, s + 4); };
  text("RIFF"); u32(36 + pcm.size() * 2); text("WAVE"); text("fmt "); u32(16);
  u16(1); u16(channels); u32(rate); u32(rate * channels * 2); u16(channels * 2); u16(16);
  text("data"); u32(pcm.size() * 2); for (auto v : pcm) u16(v);
  return b;
}
int main() {
  AudioManager speaker, duplicate;
  Microphone mic, duplicateMic;
  assert(!speaker.play({}, false));
  assert(!speaker.playBuffer(nullptr, 64, false));
  assert(!mic.begin(48000));
  assert(speaker.begin() && !duplicate.begin());
  assert(mic.begin() && !duplicateMic.begin());
  assert(!mic.begin(8000));
  int16_t pcm[4] = {};
  incomingSlots = {4096, 123, -4096, 456, INT32_MAX, 0, INT32_MIN, 0};
  readTimeout = true; // Partial samples survive a timeout.
  assert(mic.read(pcm, 4, 10) == 4);
  assert(pcm[0] == 1 && pcm[1] == -1 && pcm[2] == INT16_MAX && pcm[3] == INT16_MIN);
  incomingSlots.clear(); assert(mic.read(pcm, 4, 10) == 0);
  assert(mic.read(nullptr, 4) == -1);
  auto data = wav(16000, 1, {1000, -1000, 32767}); // odd mono frame count
  speaker.setVolume(50);
  size_t pos = 0;
  AudioManager::WavSource source;
  source.seek = [&](size_t at) { if (at > data.size()) return false; pos = at; return true; };
  source.read = [&](uint8_t* dst, size_t n) {
    if (pos >= 44 && n > 1) n = 1; // tiny partial PCM reads
    if (n > data.size() - pos) n = data.size() - pos;
    memcpy(dst, data.data() + pos, n); pos += n; return static_cast<int>(n);
  };
  partialWrite = true;
  assert(speaker.play(source, false)); runTask();
  assert(!speaker.isPlaying() && rxFake.enabled && !txFake.enabled);
  assert(writtenSlots[256] == outputSample(1000, 50) && writtenSlots[257] == outputSample(1000, 50));
  assert(writtenSlots[258] == outputSample(-1000, 50));
  assert(writtenSlots[260] == outputSample(32767, 50));
  assert(!(Wire.expander[2] & (1 << PIN_AMP_ENABLE)));
  auto wrongRate = wav(48000, 1, {123});
  assert(!speaker.playBuffer(wrongRate.data(), wrongRate.size(), false));
  auto stereo = wav(16000, 2, {100, -200});
  writtenSlots.clear();
  assert(speaker.playBuffer(stereo.data(), stereo.size(), false)); runTask();
  assert(writtenSlots[256] == outputSample(100, 50) && writtenSlots[257] == outputSample(-200, 50));
  failTask = true;
  assert(!speaker.playBuffer(data.data(), data.size(), false) && !txFake.enabled);
  failTask = false;
  failWrite = true; // Missing module clocks: worker exits and drops amp.
  assert(speaker.playBuffer(data.data(), data.size(), false)); runTask();
  assert(!speaker.isPlaying() && !txFake.enabled && rxFake.enabled);
  failWrite = false;
  // stop() waits for worker cleanup; it must preserve the active microphone.
  assert(speaker.playBuffer(data.data(), data.size(), true)); speaker.stop();
  assert(!speaker.isPlaying() && rxFake.enabled);
  speaker.powerDown();
  assert(rxFake.enabled && !rxFake.deleted);
  mic.end(); assert(rxFake.deleted && txFake.deleted);
  assert(mic.read(pcm, 4) == -1);
  assert(mic.begin()); mic.end();
  puts("Metalio public playback/capture, partial I/O, rate rejection and stop tests passed");
}
