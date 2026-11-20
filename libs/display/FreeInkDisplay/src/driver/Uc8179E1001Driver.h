#pragma once

// UC8179 panel driver — Seeed reTerminal E1001 (7.5" 800x480 B/W).
//
// Deliberately separate from Uc8179Driver (the Xteink X4 family): the E1001's glass is a
// plain 7.5" UC8179 module driven the GxEPD2 way — host programs PWR (0x01) and BTST,
// the panel is addressed at its visible size (no 600-gate padding), 1-bpp KW mode
// (DTM1 = OLD plane, DTM2 = NEW plane). The register sequence and refresh flow are the
// ones proven on retail units with a bring-up sketch (full refresh 1.6 s).
//
// Single-buffer only: after every refresh the OLD plane is re-synced with the frame that
// was just shown, so the next fast refresh diffs against it. No grayscale.
//
// BUSY_N: low while busy, high when idle (BusyPolarity::UcIdleHigh).

#include "PanelDriver.h"

namespace freeink {

PanelDriver& uc8179E1001Driver();

// Waveform for fast (partial) updates, set from the app's settings:
//   false (the default): the panel's built-in waveform, about 870 ms, clean.
//   true: a shortened waveform loaded into the registers, about 770 ms; it drives the ink less
//         and can leave faint traces.
void uc8179E1001SetFastWaveform(bool fast);

}  // namespace freeink
