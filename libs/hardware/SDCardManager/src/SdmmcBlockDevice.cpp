#include "SdmmcBlockDevice.h"

#if FREEINK_SD_SDMMC

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

namespace freeink {

bool SdmmcBlockDevice::begin(const BoardConfig::SdmmcPins& pins) {
  if (pins.busWidth == 0) return false;

  // Host config matches the OEM (recovered from app1's mountSD via Ghidra): full
  // default capability flags (0x37) with the actual width selected via slot.width
  // only, and the data clock at 40 MHz. The read timeouts we chased earlier were a
  // mount-sequencing problem, not a clock-margin one — see the retry loop below.
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 40 MHz

  // Slot pin map. The ESP32-S3 routes SDMMC through the GPIO matrix, so the data
  // and clock/command lines are assignable (unlike the classic ESP32's fixed slot).
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = pins.busWidth;
  slot.clk = static_cast<gpio_num_t>(pins.clk);
  slot.cmd = static_cast<gpio_num_t>(pins.cmd);
  slot.d0 = static_cast<gpio_num_t>(pins.d0);
  slot.d1 = static_cast<gpio_num_t>(pins.d1);
  slot.d2 = static_cast<gpio_num_t>(pins.d2);
  slot.d3 = static_cast<gpio_num_t>(pins.d3);
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  if (sdmmc_host_init() != ESP_OK) return false;
  if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK) {
    sdmmc_host_deinit();
    return false;
  }

  // The slot's INTERNAL_PULLUP flag frequently does NOT engage on ESP32-S3 SDMMC
  // pins routed through the GPIO matrix, leaving CMD/DAT floating — the card's
  // command line works but the first data read (ACMD13/SSR) times out. Force the
  // pull-ups explicitly after the driver has claimed the pins.
  gpio_pullup_en(static_cast<gpio_num_t>(pins.cmd));
  gpio_pullup_en(static_cast<gpio_num_t>(pins.d0));

  auto* card = static_cast<sdmmc_card_t*>(malloc(sizeof(sdmmc_card_t)));
  if (!card) {
    sdmmc_host_deinit();
    return false;
  }
  // SD power/enable pin (sd.powerEnable, GPIO5 on X4 Pro). The OEM mountSD pulses it
  // HIGH→LOW before each attempt and runs the card with the pin held LOW; it is an
  // active-LOW enable that gates the card's data path, not a one-shot rail. Confirmed
  // on hardware: pulsing then holding LOW mounts reliably, whereas driving it HIGH
  // after init breaks every block read with 0x107.
  const int8_t sdPwr = BoardConfig::ACTIVE.sd.powerEnable;
  if (sdPwr >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(sdPwr));
    pinMode(sdPwr, OUTPUT);
  }
  // DMA-capable bounce buffer for the sector-0 validation read. SdFat's own cache may
  // live in PSRAM / at an unaligned address; a known-good internal-RAM buffer proves
  // the data path on its own terms.
  auto* probe = static_cast<uint8_t*>(heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!probe) {
    free(card);
    sdmmc_host_deinit();
    return false;
  }

  // Retry the WHOLE mount — init AND a real sector-0 read — power-cycling GPIO5 before
  // each attempt, mirroring the OEM mountSD. Retrying only card_init leaves SdFat's
  // first (un-retried) block read to hit a still-marginal data path and fail with
  // 0x107; validating a real read here means _card is only published once genuine
  // block I/O works, and GPIO5 is left in the exact LOW state that read succeeded under.
  esp_err_t mountErr = ESP_FAIL;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (sdPwr >= 0) {
      digitalWrite(sdPwr, HIGH);
      delay(80);
      digitalWrite(sdPwr, LOW);  // run with the enable held LOW
      delay(120);
    }
    esp_err_t e = sdmmc_card_init(&host, card);
    if (e != ESP_OK && card->csd.capacity == 0) {
      mountErr = e;  // failed before CSD — nothing to read; power-cycle and retry
      continue;
    }
    // CSD is valid (capacity known); prove real block I/O before committing.
    e = sdmmc_read_sectors(card, probe, 0, 1);
    mountErr = e;
    if (e == ESP_OK) break;
  }
  heap_caps_free(probe);

  if (mountErr != ESP_OK) {
    if (Serial)
      Serial.printf("[%lu] [SD] SDMMC mount failed after retries: %s\n", millis(), esp_err_to_name(mountErr));
    free(card);
    sdmmc_host_deinit();
    return false;
  }
  _card = card;
  return true;
}

void SdmmcBlockDevice::end() {
  if (_card) {
    free(_card);
    _card = nullptr;
    sdmmc_host_deinit();
  }
}

// esp-idf SDMMC uses DMA, which requires the transfer buffer to be in DMA-capable
// internal RAM and word-aligned. SdFat's cache buffers aren't guaranteed to be
// (PSRAM / arbitrary alignment), which makes sdmmc_read/write_sectors fail. Bounce
// through a DMA-capable buffer. (heap_caps_aligned_alloc via MALLOC_CAP_DMA.)
bool SdmmcBlockDevice::readSectors(Sector_t sector, uint8_t* dst, size_t ns) {
  if (!_card) return false;
  const size_t bytes = ns * 512u;
  auto* bounce = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!bounce) return false;
  const esp_err_t e = sdmmc_read_sectors(static_cast<sdmmc_card_t*>(_card), bounce, sector, ns);
  if (e == ESP_OK) memcpy(dst, bounce, bytes);
  heap_caps_free(bounce);
  return e == ESP_OK;
}

bool SdmmcBlockDevice::writeSectors(Sector_t sector, const uint8_t* src, size_t ns) {
  if (!_card) return false;
  const size_t bytes = ns * 512u;
  auto* bounce = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!bounce) return false;
  memcpy(bounce, src, bytes);
  const esp_err_t e = sdmmc_write_sectors(static_cast<sdmmc_card_t*>(_card), bounce, sector, ns);
  heap_caps_free(bounce);
  return e == ESP_OK;
}

Sector_t SdmmcBlockDevice::sectorCount() {
  return _card ? static_cast<Sector_t>(static_cast<sdmmc_card_t*>(_card)->csd.capacity) : 0;
}

}  // namespace freeink

#endif  // FREEINK_SD_SDMMC
