#include "RecoveryBoot.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <InputManager.h>
#include <SDCardManager.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <spi_flash_mmap.h>
#include <string.h>

namespace freeink {
namespace recovery {
namespace {

// --- Recovery combo: Back + Up (default) ------------------------------------
// The two buttons sit on different ADC ladders (Back on GPIO1, Up on GPIO2),
// which is the only kind of two-button combo the ladder can report at once —
// buttons sharing a pin (e.g. Back+Right) collapse to a single reading. Reading
// through InputManager keeps the board's calibrated ranges (and works on the
// digital-button boards too) instead of hard-coding ADC thresholds here.
//
// We don't need BoardConfig::selectDevice() to have run first: the Xteink X3 and
// X4 share an identical input config, so the default-active profile already reads
// the ladder correctly. That lets this run as the first line of setup().
constexpr int kSettleSamples = 16;  // max polls while debounce warms up
constexpr int kConfirmSamples = 5;  // consecutive holds required (~30 ms)

bool comboHeld(int8_t button1, int8_t button2) {
  if (button1 < 0) return false;
  InputManager input;
  input.begin();
  int consecutive = 0;
  for (int i = 0; i < kSettleSamples; ++i) {
    input.update();  // applies debounce; currentState lags the first poll or two
    const bool held = input.isPressed(static_cast<uint8_t>(button1)) &&
                      (button2 < 0 || input.isPressed(static_cast<uint8_t>(button2)));
    consecutive = held ? consecutive + 1 : 0;
    if (consecutive >= kConfirmSamples) return true;
    delay(6);
  }
  return false;
}

// --- otadata switch ---------------------------------------------------------
// Point the bootloader at `dest` by writing a fresh otadata entry into the
// inactive slot. Bypasses esp_ota_set_boot_partition's esp_image_verify (which
// rejects patched Xteink images). Layout per esp_flash_partitions.h; CRC covers
// ota_seq only.
struct __attribute__((packed)) SelectEntry {
  uint32_t ota_seq;
  uint8_t seq_label[20];
  uint32_t ota_state;
  uint32_t crc;
};
static_assert(sizeof(SelectEntry) == 32, "SelectEntry must be 32 bytes");

constexpr uint32_t kOtaImgNew = 0;      // ESP_OTA_IMG_NEW
constexpr uint32_t kOtaImgInvalid = 3;  // ESP_OTA_IMG_INVALID
constexpr uint32_t kOtaImgAborted = 4;  // ESP_OTA_IMG_ABORTED

uint32_t seqCrc(uint32_t seq) {
  return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&seq), sizeof(uint32_t));
}

// A partition begins with a plausible app image (magic 0xE9). Excludes an erased
// (0xFF) / empty slot. Deliberately not esp_image_verify (see above).
bool hasApp(const esp_partition_t* p) {
  if (!p) return false;
  uint8_t magic = 0;
  return esp_partition_read(p, 0, &magic, sizeof(magic)) == ESP_OK && magic == 0xE9;
}

// The plain hatch: repoint otadata at slot 0 and reboot into it. No-op (returns)
// when slot 0 is empty or is already the running slot.
void switchToSlot0AndReboot() {
  // Recovery firmware lives in ota_0 (default upload offset 0x10000).
  const esp_partition_t* hatch =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (!hatch) return;

  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && running->address == hatch->address) return;  // already the recovery slot
  if (!hasApp(hatch)) return;                                 // nothing bootable there

  if (switchBootPartition(hatch)) {
    delay(50);
    esp_restart();
  }
}

void serialProgress(size_t written, size_t total, void*) {
  static size_t lastDecile = SIZE_MAX;
  const size_t decile = total ? (written * 10) / total : 0;
  if (decile == lastDecile) return;
  lastDecile = decile;
  if (Serial) {
    Serial.printf("[%lu] [FLASH] %u%% (%u / %u bytes)\n", millis(),
                  static_cast<unsigned>(decile * 10), static_cast<unsigned>(written),
                  static_cast<unsigned>(total));
  }
}

// Mount the SD card and, if the update image is present, flash it and reboot
// into it. Returns (false) when there is no card, no update file, or the flash
// failed — the caller then falls back to the slot-0 hatch.
bool trySdUpdateAndReboot(const SdUpdateOptions& options) {
  // A flash takes tens of seconds; on battery-latched boards the user will have
  // released the power button long before it finishes, so latch the rails first.
  BoardConfig::holdPowerRails();
  BoardConfig::releaseSdRail();
  if (!SdMan.begin()) return false;
  if (!SdMan.exists(options.path)) return false;

  if (Serial) Serial.printf("[%lu] [FLASH] combo held, flashing %s\n", millis(), options.path);
  const firmware::ProgressCb cb = options.onProgress ? options.onProgress : serialProgress;
  void* ctx = options.onProgress ? options.progressCtx : nullptr;
  if (firmware::flashFromSdPath(options.path, cb, ctx) != firmware::Result::OK) return false;

  if (options.renameOnSuccess) {
    const String flashed = String(options.path) + ".flashed";
    SdMan.remove(flashed.c_str());
    SdMan.rename(options.path, flashed.c_str());
  }
  delay(50);
  esp_restart();
  return true;  // unreachable
}

}  // namespace

bool switchBootPartition(const esp_partition_t* dest) {
  if (!dest) return false;
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata || otadata->size < 2 * SPI_FLASH_SEC_SIZE) return false;

  SelectEntry slots[2] = {};
  if (esp_partition_read(otadata, 0, &slots[0], sizeof(SelectEntry)) != ESP_OK ||
      esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, &slots[1], sizeof(SelectEntry)) != ESP_OK) {
    return false;
  }

  // Active slot = valid CRC, highest seq, not INVALID/ABORTED.
  int activeIdx = -1;
  uint32_t activeSeq = 0;
  for (int i = 0; i < 2; ++i) {
    if (slots[i].ota_seq == 0xFFFFFFFFu) continue;
    if (slots[i].crc != seqCrc(slots[i].ota_seq)) continue;
    if (slots[i].ota_state == kOtaImgInvalid || slots[i].ota_state == kOtaImgAborted) continue;
    if (activeIdx < 0 || slots[i].ota_seq > activeSeq) {
      activeIdx = i;
      activeSeq = slots[i].ota_seq;
    }
  }

  // ota_seq encoding: (seq - 1) % NUM_OTA_PARTITIONS selects the partition.
  const uint32_t destIdx =
      static_cast<uint32_t>(dest->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (destIdx > 15) return false;

  // Smallest seq > activeSeq landing on `dest` (2 OTA partitions).
  uint32_t newSeq = activeSeq + 1;
  while (((newSeq - 1u) % 2u) != (destIdx % 2u)) ++newSeq;

  SelectEntry next = {};
  next.ota_seq = newSeq;
  memset(next.seq_label, 0xFF, sizeof(next.seq_label));
  next.ota_state = kOtaImgNew;
  next.crc = seqCrc(next.ota_seq);

  // Write the OTHER otadata slot so the bootloader sees the higher seq there.
  const int targetSlot = (activeIdx == 0) ? 1 : 0;
  const size_t targetOff = static_cast<size_t>(targetSlot) * SPI_FLASH_SEC_SIZE;
  if (esp_partition_erase_range(otadata, targetOff, SPI_FLASH_SEC_SIZE) != ESP_OK) return false;
  if (esp_partition_write(otadata, targetOff, &next, sizeof(next)) != ESP_OK) return false;
  return true;
}

void checkBootCombo() {
  if (!comboHeld(InputManager::BTN_BACK, InputManager::BTN_UP)) return;
  switchToSlot0AndReboot();
}

void checkBootCombo(const SdUpdateOptions& options) {
  if (!comboHeld(options.button1, options.button2)) return;
  if (trySdUpdateAndReboot(options)) return;  // reboots on success
  switchToSlot0AndReboot();
}

}  // namespace recovery
}  // namespace freeink
