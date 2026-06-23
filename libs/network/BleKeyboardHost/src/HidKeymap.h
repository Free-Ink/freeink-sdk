#pragma once

// HID keyboard usage -> character / SpecialKey translation (US QWERTY).
// Pure logic, no NimBLE — compiled regardless of FREEINK_CAP_BLE_HID_HOST so it
// can be host-tested and reused. See the USB HID Usage Tables (page 0x07).

#include <stdint.h>

#include "BleKeyboardHost.h"

namespace freeink {

// HID keyboard modifier bitmask (byte 0 of a boot/report-protocol report).
enum HidMod : uint8_t {
  HID_LCTRL = 0x01,
  HID_LSHIFT = 0x02,
  HID_LALT = 0x04,
  HID_LGUI = 0x08,
  HID_RCTRL = 0x10,
  HID_RSHIFT = 0x20,
  HID_RALT = 0x40,
  HID_RGUI = 0x80,
};

inline bool hidShift(uint8_t mods) { return (mods & (HID_LSHIFT | HID_RSHIFT)) != 0; }
inline bool hidCtrl(uint8_t mods) { return (mods & (HID_LCTRL | HID_RCTRL)) != 0; }

// Translate a HID usage id (+ modifier byte) into a KeyEvent payload.
// Writes `ch` (printable ASCII, else 0) and `special`. Returns false for keys
// that map to nothing meaningful (modifier-only, F-keys, etc.).
bool hidTranslate(uint8_t usage, uint8_t mods, char& ch, SpecialKey& special);

}  // namespace freeink
