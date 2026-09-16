#include <BoardConfig.h>
#include <Imu.h>
#include <Wire.h>
#include <cassert>
#include <cmath>
#include <cstdio>
static void chip(uint8_t address, uint8_t who = 0x11, uint8_t version = 0x28) {
  Wire.devices[address] = true;
  Wire.regs[address][0x0F] = who;
  Wire.regs[address][0x70] = version;
}
static void xyz(uint8_t address, uint8_t reg, int16_t x, int16_t y, int16_t z) {
  for (int16_t v : {x,y,z}) {
    Wire.regs[address][reg++] = uint8_t(v);
    Wire.regs[address][reg++] = uint16_t(v) >> 8;
  }
}
static bool close(float a, float b) { return std::fabs(a-b) < 0.00001f; }
int main() {
  Imu imu;
  Imu::Sample sample = {42,42,42,42,42,42};
#if !FREEINK_CAP_IMU
  assert(!imu.begin() && !imu.present() && !imu.hasGyroscope());
  assert(!imu.read(sample) && !imu.sleep() && !imu.wake());
  return 0;
#else
  static_assert(BoardConfig::METALIO_EINK4.sensors.imuType == BoardConfig::ImuType::Sc7a20h);
  static_assert(BoardConfig::METALIO_EINK4.sensors.imuAddr == 0x19);
  assert(!imu.read(sample) && !imu.sleep() && !imu.wake() && !imu.hasGyroscope());
  assert(!imu.begin());
  chip(0x19, 0x33); // A LIS3DH-compatible layout isn't sufficient identity.
  assert(!imu.begin());
  chip(0x19, 0x11, 0); // Older SC7A20 is not the H version.
  assert(!imu.begin());
  chip(0x19);
  Wire.failReadReg = 0x70;
  assert(!imu.begin());
  Wire.failReadReg = -1;
  assert(imu.begin() && imu.present() && !imu.hasGyroscope());
  assert(Wire.regs[0x19][0x1F] == 1 && Wire.regs[0x19][0x20] == 0x57);
  assert(Wire.regs[0x19][0x23] == 0x80); // BDU, little endian, ±2g; HR is in 0x1F!
  assert(Wire.regs[0x19][0x22] == 0 && Wire.regs[0x19][0x2E] == 0);
  assert(!imu.read(sample) && sample.ax == 42); // No data-ready yet.
  Wire.regs[0x19][0x27] = 0x08;
  xyz(0x19, 0x28, 0x400F, -16384, 8192);
  assert(imu.read(sample) && Wire.lastRead == 0xA8);
  assert(sample.ax == 1 && sample.ay == -1 && sample.az == 0.5f);
  assert(sample.gx == 0 && sample.gy == 0 && sample.gz == 0);
  Wire.shortRead = true;
  assert(!imu.read(sample) && sample.ax == 1);
  Wire.shortRead = false;
  Wire.failReadReg = 0x28;
  assert(!imu.read(sample) && sample.ax == 1);
  Wire.failReadReg = -1;
  Wire.failWriteReg = 0x20;
  assert(!imu.sleep());
  Wire.failWriteReg = -1;
  assert(imu.sleep() && Wire.regs[0x19][0x20] == 7 && !imu.read(sample));
  Wire.failWriteReg = 0x20;
  assert(!imu.wake() && !imu.read(sample));
  Wire.failWriteReg = -1;
  assert(imu.wake() && Wire.regs[0x19][0x20] == 0x57 && imu.read(sample));
  xyz(0x19, 0x28, -32768, 32752, -16);
  assert(imu.read(sample));
  assert(sample.ax == -2 && close(sample.ay, 2047.0f/1024) && sample.az == -1.0f/1024);
  assert(imu.sleep());
  // Each initialization write can fail; a retry must recover and never report present on failure.
  for (int reg : {0x20,0x1F,0x21,0x22,0x23,0x24,0x25,0x2E}) {
    Wire.failWriteReg = reg;
    assert(!imu.begin() && !imu.present() && !imu.read(sample));
    if (reg != 0x20) assert(Wire.regs[0x19][0x20] == 7);
    Wire.failWriteReg = -1;
    assert(imu.begin() && imu.sleep());
  }
  // Wrong primary identity must not receive sensor writes; alternate SA0 address works.
  chip(0x19, 0x33); chip(0x18);
  Wire.regs[0x18][0x57] = 0x04;
  Wire.writes.clear();
  assert(imu.begin() && Wire.regs[0x18][0x57] == 0x0C);
  for (auto w : Wire.writes) assert(w.addr != 0x19);
  Wire.regs[0x18][0x27] = 8;
  xyz(0x18, 0x28, 0, 16384, 0);
  assert(imu.read(sample) && sample.ay == 1);
  assert(imu.sleep() && Wire.regs[0x18][0x20] == 7);
  assert(imu.wake() && Wire.regs[0x18][0x20] == 0x57);
  Wire.failWriteReg = 0x57;
  assert(!imu.begin() && Wire.regs[0x18][0x20] == 7);
  Wire.failWriteReg = -1;
  assert(imu.begin());
  // Existing gyro backends retain their scaling and lifecycle behavior.
  BoardConfig::ACTIVE.sensors.imuType = BoardConfig::ImuType::Qmi8658;
  BoardConfig::ACTIVE.sensors.imuAddr = 0x6B;
  Wire.devices[0x6A] = true; Wire.regs[0x6A][0] = 5;
  assert(imu.begin() && imu.hasGyroscope());
  xyz(0x6A, 0x35, 16384, -16384, 0); xyz(0x6A, 0x3B, 64, -64, 0);
  assert(imu.read(sample) && sample.ax == 1 && sample.gx == 1 && sample.gy == -1);
  assert(imu.sleep() && imu.wake());
  BoardConfig::ACTIVE.sensors.imuType = BoardConfig::ImuType::Lsm6ds3;
  BoardConfig::ACTIVE.sensors.imuAddr = 0x6A;
  Wire.regs[0x6A][0x0F] = 0x6A;
  assert(imu.begin() && imu.hasGyroscope());
  xyz(0x6A, 0x28, 1000, -1000, 0); xyz(0x6A, 0x22, 1000, -1000, 0);
  assert(imu.read(sample) && close(sample.ax, 0.061f) && close(sample.gx, 8.75f));
  assert(imu.sleep() && imu.wake());
  puts("SC7A20H identity/address, setup, signed scaling, data readiness, failures and sleep/wake passed; gyro regressions passed");
#endif
}
