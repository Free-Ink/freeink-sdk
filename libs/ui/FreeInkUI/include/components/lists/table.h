#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct TableProps {
  const char* const* cells = nullptr;
  uint8_t rows = 0;
  uint8_t cols = 0;
  TextStyle text{};
  StyleSet cellStyles{};
  Paint grid = Paint::solid(Color::Black);
  uint8_t gridWidth = 1;
  uint8_t cellRadius = 0;
  int16_t rowHeight = 24;
  int16_t padding = 4;
  bool headerRow = false;
};

template <size_t MaxInteractions>
void table(Frame<MaxInteractions>& frame, Rect rect, const TableProps& props) {
  if (!props.cells || props.rows == 0 || props.cols == 0) return;
  StyleSet styles = props.cellStyles.unset() ? defaultListRowStyles() : props.cellStyles;
  if (props.cellRadius > 0) setStyleRadius(styles, props.cellRadius);
  const int16_t cellW = static_cast<int16_t>(rect.width / props.cols);
  const int16_t rowH = props.rowHeight < 1 ? static_cast<int16_t>(rect.height / props.rows) : props.rowHeight;
  for (uint8_t row = 0; row < props.rows; ++row) {
    int16_t x = rect.x;
    for (uint8_t col = 0; col < props.cols; ++col) {
      const bool lastCol = col == props.cols - 1;
      Rect cell{x, static_cast<int16_t>(rect.y + row * rowH),
                lastCol ? static_cast<int16_t>(rect.right() - x) : cellW, rowH};
      State state = props.headerRow && row == 0 ? StateSelected : StateNormal;
      const BoxStyle& style = styles.resolve(state);
      frame.target().fill(cell, style.background, style.radius, style.corners);
      if (props.grid.kind != PaintKind::None && props.gridWidth > 0) {
        frame.target().stroke(cell, props.grid, props.gridWidth, style.radius, style.corners);
      }
      const char* value = props.cells[static_cast<uint16_t>(row) * props.cols + col];
      if (value) {
        Rect textRect = cell.inset(Insets{0, props.padding, 0, props.padding});
        frame.target().text(textRect, value, textStyleWithForeground(props.text, style.foreground));
      }
      x = static_cast<int16_t>(x + cell.width);
    }
  }
}

}  // namespace ui
}  // namespace freeink
