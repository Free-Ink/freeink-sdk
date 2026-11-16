#pragma once

namespace freeink::papermono {
inline bool powerButtonClicked = false;
inline bool pollPowerButtonClicked(unsigned long) {
  const bool clicked = powerButtonClicked;
  powerButtonClicked = false;
  return clicked;
}
}  // namespace freeink::papermono
