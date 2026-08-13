#include <BoardPaperS3.h>
#include <LgfxEpdConfig.h>

namespace {

// The PaperS3's EPD rails are plain GPIOs sequenced by LovyanGFX's stock
// Bus_EPD::powerControl (OE -> PWR -> SPV with settling delays), so powerOn /
// powerOff stay null and the driver delegates to the base class. prepare() only
// parks the PWROFF pulse line LOW (its idle level — M5GFX's autodetect does the
// same at init) so the PMS150G latch never sees a stray edge.
bool prepareEpdPower() {
  pinMode(PAPERS3_PWROFF_PULSE, OUTPUT);
  digitalWrite(PAPERS3_PWROFF_PULSE, LOW);
  return true;
}

}  // namespace

namespace freeink {

const LgfxEpdConfig& m5PaperS3LgfxConfig() {
  static const LgfxEpdConfig cfg = {
      {PAPERS3_EP_D0, PAPERS3_EP_D1, PAPERS3_EP_D2, PAPERS3_EP_D3, PAPERS3_EP_D4, PAPERS3_EP_D5, PAPERS3_EP_D6,
       PAPERS3_EP_D7},
      PAPERS3_EP_SPH,
      PAPERS3_EP_SPV,
      PAPERS3_EP_OE,
      PAPERS3_EP_LE,
      PAPERS3_EP_CL,
      PAPERS3_EP_CKV,
      PAPERS3_EP_PWR,
      16000000,  // busHz — M5GFX's bus_speed for this board
      8,         // linePadding — M5GFX's line_padding for this board
      // rotation 0 = the SDK's native-landscape convention (960x540 framebuffer),
      // like the LilyGo T5 S3. M5GFX ships the device portrait (offset_rotation=3);
      // if the landscape "up" comes out inverted on hardware, set 2 here.
      0,
      {&prepareEpdPower, nullptr, nullptr},  // no power hooks: stock Bus_EPD sequence
  };
  return cfg;
}

}  // namespace freeink
