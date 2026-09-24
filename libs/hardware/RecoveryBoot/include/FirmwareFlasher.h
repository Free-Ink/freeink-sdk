#pragma once

#include <cstddef>
#include <cstdint>

// FreeInk SDK — SD-card firmware flasher.
//
// Streams an ESP32 app image from an SD-card path into the next OTA app
// partition using raw esp_partition_erase_range + esp_partition_write, then
// repoints otadata via recovery::switchBootPartition(). Deliberately avoids
// the Arduino Update class and esp_image_verify, which reject patched vendor
// images; the full integrity pass below provides the same protection.
//
// Requires the SD card to be mounted (SDCardManager::begin()) before any call.
// The caller is responsible for restarting after a successful flash.

namespace freeink {
namespace firmware {

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_SEGMENTS,  // segment table malformed or runs past EOF
  BAD_CHECKSUM,  // ESP image XOR checksum mismatch
  BAD_SHA,       // SHA256 trailer mismatch (hash_appended images)
  BAD_CHIP,      // image chip_id doesn't match the running MCU family
  BAD_SIZE,      // body+pad+sha length doesn't match file size
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  OTADATA_FAIL,
};

const char* resultName(Result r);

// Progress callback: called after every chunk write. `written`/`total` are bytes.
using ProgressCb = void (*)(size_t written, size_t total, void* ctx);

// Full-image integrity check that mirrors the bootloader's verification:
// header magic, chip_id vs the running image, segment table walk, XOR
// checksum, and SHA256 trailer (when hash_appended == 1). Run this before
// flashing a candidate so a truncated/corrupted/wrong-chip .bin never
// reaches otadata.
//
// `partitionSize` is the size of the destination OTA partition; pass 0 to
// skip the size-fits-partition check. Streams the file in small chunks, so
// it works within ESP32-C3 RAM budgets.
Result validateImageFile(const char* sdPath, size_t partitionSize);

// Open `sdPath`, validate it looks like an ESP32 image for this MCU, then
// stream it into the next OTA app partition with interleaved 64 KiB erase +
// sector writes (so progress advances smoothly instead of stalling on one
// up-front erase). On success switches otadata so the bootloader picks the
// new image up on the next reset; the caller restarts.
//
// `alreadyValidated` lets a caller that just ran validateImageFile() itself
// skip the redundant second pass. Defaults to false so entry points without
// prior validation keep the defense-in-depth check.
Result flashFromSdPath(const char* sdPath, ProgressCb onProgress = nullptr, void* ctx = nullptr,
                       bool alreadyValidated = false);

// Returns the chip_id (esp_image_header_t offset 12) of the currently-running
// image, or 0xFFFF if it cannot be read. Because the running slot booted
// successfully, its chip_id is authoritative for the current CPU, so a
// candidate image must match it to be safe to flash.
uint16_t runningPartitionChipId();

}  // namespace firmware
}  // namespace freeink
