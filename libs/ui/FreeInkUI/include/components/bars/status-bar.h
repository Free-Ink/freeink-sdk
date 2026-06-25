#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/progress-bar.h"

namespace freeink {
namespace ui {

struct StatusBarProps {
  const char* title = nullptr;
  const char* leading = nullptr;
  // Second left-cluster item drawn after `leading` (e.g. estimated time left
  // next to the clock/battery in a reader status bar).
  const char* leadingSecondary = nullptr;
  const char* trailing = nullptr;
  // Second right-cluster item drawn left of `trailing` (e.g. a clock next to
  // the page-progress text).
  const char* trailingSecondary = nullptr;
  // Width at the left edge occupied by app-drawn chrome (e.g. a theme's own
  // battery glyph); the clusters and title stay clear of it.
  int16_t leadingReserve = 0;
  // Shifts the title upward (legacy reader chrome fine-tuning).
  int16_t titleOffsetY = 0;
  BitmapRef leadingIcon{};
  AssetRef leadingIconAsset{};
  ProgressBarProps progress{};
  TextStyle text{};
  int16_t horizontalPadding = 6;
  int16_t gap = 8;
  int16_t progressHeight = 4;
  bool showProgress = false;
  bool fillBackground = false;
  Paint background = Paint::solid(Color::White);
};

template <size_t MaxInteractions>
void statusBar(Frame<MaxInteractions>& frame, Rect rect, const StatusBarProps& props) {
  if (props.fillBackground) frame.target().fill(rect, props.background);
  Rect textRect = rect.inset(Insets{0, props.horizontalPadding, 0, props.horizontalPadding});

  // Left cluster: icon, leading, leadingSecondary — measured and laid out in
  // sequence so the title never draws over them.
  const Paint ink = Paint::solid(props.text.color);  // icon matches text color in dark mode
  int16_t leftX = static_cast<int16_t>(textRect.x + props.leadingReserve);
  BitmapRef leadingIcon = props.leadingIcon ? props.leadingIcon : resolveBitmap(frame.assets(), props.leadingIconAsset);
  if (leadingIcon) {
    const int16_t iconSize = static_cast<int16_t>(leadingIcon.height > 0 ? leadingIcon.height : leadingIcon.width);
    Rect iconRect{leftX, static_cast<int16_t>(textRect.y + (textRect.height - iconSize) / 2), iconSize, iconSize};
    frame.target().bitmap(iconRect, leadingIcon, BitmapMode::Contain, ink);
    leftX = static_cast<int16_t>(leftX + iconSize + props.gap);
  }
  const char* leftItems[2] = {props.leading, props.leadingSecondary};
  for (const char* item : leftItems) {
    if (item == nullptr) continue;
    const int16_t itemW = frame.target().measureText(props.text.font, item, props.text).width;
    TextStyle left = props.text;
    left.align = TextAlign::Left;
    frame.target().text(Rect{leftX, textRect.y, itemW, textRect.height}, item, left);
    leftX = static_cast<int16_t>(leftX + itemW + props.gap);
  }

  int16_t trailingX = textRect.right();
  const char* rightItems[2] = {props.trailing, props.trailingSecondary};
  for (const char* item : rightItems) {
    if (item == nullptr) continue;
    const int16_t itemW = frame.target().measureText(props.text.font, item, props.text).width;
    TextStyle right = props.text;
    right.align = TextAlign::Right;
    trailingX = static_cast<int16_t>(trailingX - itemW);
    frame.target().text(Rect{trailingX, textRect.y, itemW, textRect.height}, item, right);
    trailingX = static_cast<int16_t>(trailingX - props.gap);
  }

  if (props.title) {
    // Prefer centering on the whole bar; fall back to centering between the
    // clusters when a wide cluster would collide with the title.
    TextStyle center = props.text;
    center.align = TextAlign::Center;
    const int16_t titleY = static_cast<int16_t>(textRect.y - props.titleOffsetY);
    const int16_t titleW = frame.target().measureText(props.text.font, props.title, props.text).width;
    int16_t titleX = static_cast<int16_t>(textRect.x + (textRect.width - titleW) / 2);
    if (titleX < leftX || static_cast<int16_t>(titleX + titleW) > trailingX) {
      Rect between{leftX, titleY, static_cast<int16_t>(trailingX - leftX), textRect.height};
      if (!between.empty()) frame.target().text(between, props.title, center);
    } else {
      frame.target().text(Rect{titleX, titleY, titleW, textRect.height}, props.title, center);
    }
  }
  if (props.showProgress) {
    Rect bar{rect.x, static_cast<int16_t>(rect.bottom() - props.progressHeight), rect.width, props.progressHeight};
    progressBar(frame, bar, props.progress);
  }
}

}  // namespace ui
}  // namespace freeink
