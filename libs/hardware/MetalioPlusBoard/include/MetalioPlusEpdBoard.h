#pragma once

// Metalio E-Ink4-Plus panel board for the EPDiy core.
//
// The panel has no controller: the S31 LCD peripheral clocks the source data,
// and a TPS65185 makes the panel rails. The TPS65185 control lines and the
// screen rail hang off the board's TCA9555 (MetalioEInk4PlusBoard.h), so every
// rail change is an I2C write. Pins follow the vendor's rev 1.3/1.4 `config.h`
// (XSTL on GPIO4, MODE on GPIO19).

#include <MetalioEInk4PlusBoard.h>
#include <stdint.h>

extern "C" {
#include <epd_board.h>
}

namespace freeink {
namespace metalioplus {

constexpr uint8_t TPS65185_ADDR = 0x68;
constexpr int DEFAULT_VCOM_MV = 1200;

// EPDiy board definition. Pass it to epd_init() with the E0470A14_AF display.
const EpdBoardDefinition& epdBoard();

// True after the last epd_poweron() brought the TPS65185 rails up.
bool panelPowerGood();

}  // namespace metalioplus
}  // namespace freeink
