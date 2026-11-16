// A build keeps only the IMU drivers its selected devices' profiles name (FREEINK_IMU_<PART>). Built with the Metalio
// EInk4 alone, only the SC7A20H driver is in it: a QMI8658, LSM6DS3 or BMI270 that answers on the bus is refused, and
// not one of its registers is written.
#include <BoardConfig.h>
#include <Imu.h>
#include <Wire.h>
#include <cassert>
#include <cstdio>

static_assert(FREEINK_IMU_SC7A20H, "the Metalio's part is built");
static_assert(!FREEINK_IMU_QMI8658 && !FREEINK_IMU_LSM6DS3 && !FREEINK_IMU_BMI270, "parts no selected profile names are not");
static_assert(BoardConfig::imuDriverCompiled(BoardConfig::ImuType::Sc7a20h));
static_assert(BoardConfig::imuDriverCompiled(BoardConfig::ImuType::None));
static_assert(!BoardConfig::imuDriverCompiled(BoardConfig::ImuType::Qmi8658));
static_assert(!BoardConfig::imuDriverCompiled(BoardConfig::ImuType::Bmi270));

int main() {
  Imu imu;
  Imu::Sample sample{};

  // Each part identifies correctly, as its own driver would check.
  Wire.devices[0x6A] = true;
  Wire.regs[0x6A][0x00] = 0x05;  // QMI8658 WHO_AM_I
  Wire.regs[0x6A][0x0F] = 0x6A;  // LSM6DS3 WHO_AM_I
  Wire.devices[0x68] = true;
  Wire.regs[0x68][0x00] = 0x24;  // BMI270 CHIP_ID

  BoardConfig::ACTIVE.sensors.imuType = BoardConfig::ImuType::Qmi8658;
  BoardConfig::ACTIVE.sensors.imuAddr = 0x6A;
  assert(!imu.begin() && !imu.present() && !imu.read(sample) && !imu.sleep() && !imu.wake());
  BoardConfig::ACTIVE.sensors.imuType = BoardConfig::ImuType::Lsm6ds3;
  assert(!imu.begin() && !imu.present());
  BoardConfig::ACTIVE.sensors.imuType = BoardConfig::ImuType::Bmi270;
  BoardConfig::ACTIVE.sensors.imuAddr = 0x68;
  assert(!imu.begin() && !imu.present());
  // The Metalio's IO expander (0x20) is brought up before any IMU access; the refused parts get nothing.
  for (const auto& w : Wire.writes) assert(w.addr != 0x6A && w.addr != 0x68);

  // The Metalio's own part still comes up.
  BoardConfig::ACTIVE = BoardConfig::METALIO_EINK4;
  Wire.devices[0x19] = true;
  Wire.regs[0x19][0x0F] = 0x11;
  Wire.regs[0x19][0x70] = 0x28;
  assert(imu.begin() && imu.present());
  std::puts("IMU driver gating: a build drives only the parts its selected profiles name");
  return 0;
}
