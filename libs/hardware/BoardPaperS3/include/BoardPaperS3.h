#pragma once

#include <Arduino.h>

#include "BoardPaperS3Pins.h"

namespace BoardPaperS3 {

// Software power-off: pulses the PWROFF line to the PMS150G latch controller.
// The PaperS3 has no firmware-held power latch — the PMS150G latches power by
// itself on the side button — so releasing power is this pulse train, not a
// latch-pin release. Does not return if the board is on battery; on USB the
// board browns back up, so treat it as best-effort there.
void powerOff();

}  // namespace BoardPaperS3
