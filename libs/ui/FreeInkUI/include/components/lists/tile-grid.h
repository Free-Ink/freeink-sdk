#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/button.h"

namespace freeink {
namespace ui {

// 1-bit tile styling: an outlined card, filled solid when the setting the
// tile carries is on (StateChecked), never a dithered gray.
inline StyleSet tileGridStyles(uint8_t radius) {
  StyleSet s{};
  s.explicitlySet = true;
  s.normal.background = Paint::solid(Color::White);
  s.normal.foreground = Paint::solid(Color::Black);
  s.normal.border = Paint::solid(Color::Black);
  s.normal.borderWidth = 2;
  s.normal.radius = radius;
  s.selected = s.normal;
  s.selected.background = Paint::solid(Color::Black);
  s.selected.foreground = Paint::solid(Color::White);
  s.focused = s.selected;
  s.active = s.selected;
  s.disabled = s.normal;
  return s;
}

struct TileGridItem {
  const char *label = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  // Dispatched as the action's value; keep it a stable id rather than the
  // grid position, so hiding tiles does not renumber the survivors.
  int16_t value = 0;
  // StateChecked draws the tile filled (the setting is on).
  State state = StateNormal;
  bool enabled = true;
};

// A grid of finger-sized quick-setting tiles (a control-center panel): fixed
// columns, one shared action, per-tile value and checked state. All tiles
// share one row height; size the rect with tileGridHeight() so rows come out
// at the intended height.
struct TileGridProps {
  const TileGridItem *items = nullptr;
  uint16_t count = 0;
  ActionId action = NO_ACTION;
  uint8_t columns = 2;
  int16_t gap = 12;
  // 0 = divide the rect's height evenly across the rows.
  int16_t tileHeight = 0;
  uint16_t inputMask = InputTouch;
  TextStyle text{};
  StyleSet styles{};
  int16_t iconSize = 0;
  // RADIUS_INHERIT: Screen::tileGrid() substitutes the theme's controlRadius;
  // on a bare Frame it resolves to the classic 18.
  uint8_t radius = RADIUS_INHERIT;
};

// Height the grid needs for count tiles — for sizing the band (or a whole
// panel) before laying it out.
inline int16_t tileGridHeight(uint16_t count, uint8_t columns, int16_t tileHeight, int16_t gap) {
  if (count == 0 || columns == 0) return 0;
  const int16_t rows = static_cast<int16_t>((count + columns - 1) / columns);
  return static_cast<int16_t>(rows * tileHeight + (rows - 1) * gap);
}

template <size_t MaxInteractions>
void tileGrid(Frame<MaxInteractions> &frame, Rect rect, const TileGridProps &props) {
  if (!props.items || props.count == 0 || rect.empty()) return;
  const uint8_t columns = props.columns == 0 ? 1 : props.columns;
  const int16_t rows = static_cast<int16_t>((props.count + columns - 1) / columns);
  const int16_t tileH =
      props.tileHeight > 0
          ? props.tileHeight
          : static_cast<int16_t>((rect.height - (rows - 1) * props.gap) / rows);
  if (tileH <= 0) return;
  const int16_t tileW =
      static_cast<int16_t>((rect.width - props.gap * (columns - 1)) / columns);
  if (tileW <= 0) return;

  const StyleSet styles =
      props.styles.unset() ? tileGridStyles(resolveRadius(props.radius, 18)) : props.styles;

  ButtonProps tile;
  tile.text = props.text;
  tile.styles = styles;
  tile.inputMask = props.inputMask;
  tile.iconSize = props.iconSize;

  for (uint16_t i = 0; i < props.count; ++i) {
    const int16_t r = static_cast<int16_t>(i / columns);
    const int16_t c = static_cast<int16_t>(i % columns);
    const Rect slot{static_cast<int16_t>(rect.x + c * (tileW + props.gap)),
                    static_cast<int16_t>(rect.y + r * (tileH + props.gap)), tileW, tileH};
    const TileGridItem &item = props.items[i];
    tile.label = item.label;
    tile.icon = item.icon;
    tile.iconAsset = item.iconAsset;
    tile.action = props.action;
    tile.value = item.value;
    tile.state = item.state;
    tile.enabled = item.enabled;
    button(frame, slot, tile);
  }
}

}  // namespace ui
}  // namespace freeink
