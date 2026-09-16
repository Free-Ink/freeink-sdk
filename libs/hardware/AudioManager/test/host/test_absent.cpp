#include <AudioManager.h>
#include <Microphone.h>
#include <cassert>
int main() {
  AudioManager audio;
  Microphone mic;
  assert(!audio.present() && !audio.begin());
  assert(!mic.begin() && !mic.present());
  int16_t sample = 0;
  assert(mic.read(&sample, 1) == -1);
  audio.stop(); audio.powerDown(); mic.end();
}
