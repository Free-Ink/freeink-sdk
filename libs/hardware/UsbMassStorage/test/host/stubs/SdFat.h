#pragma once
#include <cstddef>
#include <cstdint>
using Sector_t = uint32_t;
struct FsBlockDeviceInterface {
  virtual ~FsBlockDeviceInterface() = default;
  virtual bool readSector(Sector_t sector, uint8_t* buffer) = 0;
  virtual bool readSectors(Sector_t sector, uint8_t* buffer, size_t count) = 0;
  virtual bool writeSector(Sector_t sector, const uint8_t* buffer) = 0;
  virtual bool writeSectors(Sector_t sector, const uint8_t* buffer, size_t count) = 0;
  virtual Sector_t sectorCount() = 0;
  virtual bool syncDevice() = 0;
};
