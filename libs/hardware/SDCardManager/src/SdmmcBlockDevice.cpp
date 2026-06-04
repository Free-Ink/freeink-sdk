#include "SdmmcBlockDevice.h"

#if FREEINK_SD_SDMMC

#include <Arduino.h>
#include <stdlib.h>

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

namespace freeink {

bool SdmmcBlockDevice::begin(const BoardConfig::SdmmcPins& pins) {
  if (pins.busWidth == 0) return false;

  // Peripheral clock + bus width.
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20 MHz; raise per board if validated
  if (pins.busWidth == 1) {
    // Keep the default flags (function pointers, deinit-arg) but drop wide-bus.
    host.flags &= ~(SDMMC_HOST_FLAG_8BIT | SDMMC_HOST_FLAG_4BIT);
  }

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

  _card = static_cast<sdmmc_card_t*>(malloc(sizeof(sdmmc_card_t)));
  if (!_card) {
    sdmmc_host_deinit();
    return false;
  }
  if (sdmmc_card_init(&host, _card) != ESP_OK) {
    free(_card);
    _card = nullptr;
    sdmmc_host_deinit();
    return false;
  }
  return true;
}

void SdmmcBlockDevice::end() {
  if (_card) {
    free(_card);
    _card = nullptr;
    sdmmc_host_deinit();
  }
}

bool SdmmcBlockDevice::readSectors(Sector_t sector, uint8_t* dst, size_t ns) {
  return _card && sdmmc_read_sectors(_card, dst, sector, ns) == ESP_OK;
}

bool SdmmcBlockDevice::writeSectors(Sector_t sector, const uint8_t* src, size_t ns) {
  return _card && sdmmc_write_sectors(_card, src, sector, ns) == ESP_OK;
}

Sector_t SdmmcBlockDevice::sectorCount() {
  return _card ? static_cast<Sector_t>(_card->csd.capacity) : 0;
}

}  // namespace freeink

#endif  // FREEINK_SD_SDMMC
