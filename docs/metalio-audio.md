# Metalio speaker and microphone

Select `FREEINK_DEVICE_METALIO_EINK4=1` and include the `AudioManager`,
`Microphone` and `BoardConfig` libraries. Audio and microphone capabilities are
enabled by default; either can be disabled with its `FREEINK_CAP_*` flag.

## Consumer API

```cpp
#include <AudioManager.h>
#include <Microphone.h>

AudioManager speaker;
Microphone microphone;

bool startRecording() {
  return microphone.begin(16000);
}

// Pump this from a recording task. Positive results are samples, zero is a
// timeout without samples, and negative results indicate an error.
int readRecording(int16_t* samples, size_t capacity) {
  return microphone.read(samples, capacity, 100);
}

bool playSound(const uint8_t* wav, size_t bytes) {
  if (!speaker.begin()) return false;
  speaker.setVolume(50);
  // Keep wav alive until playback finishes or stop() returns.
  return speaker.playBuffer(wav, bytes, false);
}

void stopAudioBeforeSleep() {
  // First stop/join any application task calling microphone.read().
  speaker.powerDown();
  microphone.end();
}
```

- Playback accepts **16-bit PCM WAV at 16 kHz**, mono or stereo. Other sample
  rates are rejected because the module supplies the clock. `play(source, true)`
  loops a caller-supplied byte source until `stop()`; source callbacks must return
  promptly. Source callbacks execute in the playback task, so consumers must
  serialize access to shared storage.
- `setVolume(0..100)` applies the demo's squared software gain and can update
  during playback. The amplifier is enabled only during playback and disabled
  on completion, stop, or I²S failure.
- Capture returns 16-bit mono PCM, taking the left I²S slot with the demo's
  shift-by-12 gain and saturation. Each `read()` returns at most **32 samples**;
  call repeatedly to fill a larger buffer. A timed-out read still returns any
  complete samples already received.
- One speaker instance and one microphone instance can own the bus. A second
  instance of either fails initialization until the first releases ownership.
  Playback and capture can run simultaneously. Stopping/releasing one preserves
  the other. The channel pair is deleted when both owners release it.
- Serialize lifecycle calls (`begin`, `play`, `stop`, `powerDown`, `end`) on one
  control task. Recording can run on another task; stop that task before `end()`.
  Streaming uses fixed scratch buffers. The I²S driver allocates DMA resources
  once per bus lifetime; playback uses the existing AudioManager worker stack.
- `begin()` can block for the vendor's 700 ms module setup delay. It confirms
  local resource initialization, not a module response or the presence of its
  external clock. Missing clocks cause bounded I/O timeouts. No Bluetooth pairing
  API or undocumented module shutdown command is added.

## Hardware and implementation evidence

The supplied `metalio-hw-test` sources establish:

- `main/hal/metalio-e-ink-4/config.h`: BCLK6, WS43, ESP32 DOUT7, DIN17;
  UART2 TX48/RX47 at 115200 baud; 16 kHz input/output.
- `metalio_e_ink_4_board.cc`, `BtDefaultModeTask`: `AT+RX=2\r\n`, 700 ms delay,
  then `AT+MODE=1\r\n` to make the module supply I²S clocks.
- `main/hal/common/bt_audio_codec.cc`: slave Philips I²S with two 32-bit slots,
  software output gain and left-slot microphone conversion.
- TCA9555 P0.1 low selects the ESP32 path; P0.4 enables the amplifier.

The backend allocates TX and RX together, following ESP-IDF's
[duplex I²S configuration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html).
It is shared internally through `MetalioAudio.h`; consumers use the managers.
The board helper serializes expander output read/modify/write operations so
amplifier changes cannot overwrite another helper's output update.

## Verification

```sh
sh libs/hardware/AudioManager/test/host/run_metalio.sh
```

Host tests cover slot conversion and clipping, ownership, both shutdown orders,
initialization failures/retries, partial WAV reads and I²S writes, odd-length mono
audio, stereo preservation, volume, rate rejection, microphone partial timeout,
task creation failure, missing-clock timeout and stop while capture is active.
An ESP32-S3 firmware linking both managers has also been built.

Physical validation is pending: verify actual module clocks, speaker volume,
microphone gain, simultaneous playback/capture, repeated start/stop and standby
current. The module remains in its configured mode after SDK resources are
released; the supplied code does not establish a module sleep command.
