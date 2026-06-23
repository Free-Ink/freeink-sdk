#include "HidKeymap.h"

namespace freeink {

namespace {

// Base (unshifted) ASCII for HID usages 0x00..0x38; 0 = not a printable key
// here (specials handled separately). 0x04='a' .. 0x1D='z', 0x1E='1'.. 0x27='0'.
constexpr char kBase[0x39] = {
    /*00*/ 0,   0,   0,   0,
    /*04*/ 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    /*11*/ 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    /*1E*/ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    /*28*/ 0,   0,   0,   0,   ' ',  // 0x2C = Space
    /*2D*/ '-', '=', '[', ']', '\\', 0 /*0x32 non-US #*/, ';', '\'', '`', ',', '.', '/',
};

// Shifted ASCII for the same range.
constexpr char kShift[0x39] = {
    /*00*/ 0,   0,   0,   0,
    /*04*/ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    /*11*/ 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    /*1E*/ '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    /*28*/ 0,   0,   0,   0,   ' ',
    /*2D*/ '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?',
};

}  // namespace

bool hidTranslate(uint8_t usage, uint8_t mods, char& ch, SpecialKey& special) {
  ch = 0;
  special = SpecialKey::None;

  switch (usage) {
    case 0x28:  // Enter
    case 0x58:  // Keypad Enter
      special = SpecialKey::Enter;
      return true;
    case 0x29:
      special = SpecialKey::Escape;
      return true;
    case 0x2A:
      special = SpecialKey::Backspace;
      return true;
    case 0x2B:
      special = SpecialKey::Tab;
      return true;
    case 0x4C:
      special = SpecialKey::Delete;
      return true;
    case 0x4A:
      special = SpecialKey::Home;
      return true;
    case 0x4B:
      special = SpecialKey::PageUp;
      return true;
    case 0x4D:
      special = SpecialKey::End;
      return true;
    case 0x4E:
      special = SpecialKey::PageDown;
      return true;
    case 0x4F:
      special = SpecialKey::Right;
      return true;
    case 0x50:
      special = SpecialKey::Left;
      return true;
    case 0x51:
      special = SpecialKey::Down;
      return true;
    case 0x52:
      special = SpecialKey::Up;
      return true;
    default:
      break;
  }

  if (usage < sizeof(kBase)) {
    const char c = hidShift(mods) ? kShift[usage] : kBase[usage];
    if (c != 0) {
      ch = c;
      return true;
    }
  }
  return false;
}

}  // namespace freeink
