#pragma once

#include "../../FreeInkUICore.h"
#include "../bars/status-bar.h"

namespace freeink {
namespace ui {

struct ReaderChromeProps {
  StatusBarProps top{};
  StatusBarProps bottom{};
  int16_t topHeight = 28;
  int16_t bottomHeight = 32;
  bool showTop = true;
  bool showBottom = true;
  bool dimContent = false;
};

template <size_t MaxInteractions>
void readerChrome(Frame<MaxInteractions>& frame, Rect rect, const ReaderChromeProps& props) {
  if (props.dimContent) frame.target().fill(rect, Paint::dither(Color::LightGray));
  if (props.showTop && props.topHeight > 0) {
    statusBar(frame, Rect{rect.x, rect.y, rect.width, props.topHeight}, props.top);
  }
  if (props.showBottom && props.bottomHeight > 0) {
    statusBar(frame,
              Rect{rect.x, static_cast<int16_t>(rect.bottom() - props.bottomHeight), rect.width, props.bottomHeight},
              props.bottom);
  }
}

}  // namespace ui
}  // namespace freeink
