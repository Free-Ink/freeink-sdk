#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/progress-bar.h"

namespace freeink {
namespace ui {

struct BookCardProps {
  const char* title = nullptr;
  const char* author = nullptr;
  const char* meta = nullptr;
  BitmapRef cover{};
  AssetRef coverAsset{};
  int32_t progress = 0;
  int32_t progressMax = 100;
  ActionId action = NO_ACTION;
  int16_t value = 0;
  TextStyle titleText{};
  TextStyle authorText{};
  TextStyle metaText{};
  StyleSet styles{};
  State state = StateNormal;
  bool enabled = true;
  Size coverSize{62, 84};
  Insets padding{6, 8, 6, 8};
  int16_t gap = 14;
  int16_t textGap = 4;
  int16_t textProgressGap = 8;
  int16_t progressHeight = 4;
};

template <size_t MaxInteractions>
void bookCard(Frame<MaxInteractions>& frame, Rect rect, const BookCardProps& props) {
  State state = props.enabled ? props.state : static_cast<State>(props.state | StateDisabled);
  if (props.action != NO_ACTION && props.enabled) {
    frame.hit(ensureMinTouchRect(rect, frame.device().minTouchSize, frame.screen()), props.action, props.value,
              InputDefault, state);
  }
  state = frame.stateFor(props.action, props.value, state);
  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  const BoxStyle& style = styles.resolve(state);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(props.padding);
  const BitmapRef cover = props.cover ? props.cover : resolveBitmap(frame.assets(), props.coverAsset);
  const int16_t coverW = props.coverSize.width > content.width ? content.width : props.coverSize.width;
  const int16_t coverH = props.coverSize.height > content.height ? content.height : props.coverSize.height;
  Rect coverRect{content.x, static_cast<int16_t>(content.y + (content.height - coverH) / 2), coverW, coverH};
  frame.target().fill(coverRect, Paint::dither(Color::LightGray));
  if (cover) frame.target().bitmap(coverRect, cover, BitmapMode::Cover, style.foreground);
  content.x = static_cast<int16_t>(coverRect.right() + props.gap);
  content.width = static_cast<int16_t>(rect.right() - props.padding.right - content.x);
  const int16_t progressH = props.progressHeight < 1 ? 1 : props.progressHeight;
  const int16_t progressY = static_cast<int16_t>(coverRect.bottom() - progressH);
  const int16_t textBottom = static_cast<int16_t>(progressY - props.textProgressGap);

  int16_t y = content.y;
  if (props.title) {
    TextStyle title = textStyleWithForeground(props.titleText, style.foreground);
    title.maxLines = title.maxLines ? title.maxLines : 2;
    const int16_t h = measureWrappedText(frame.target(), props.title, title, content.width).height;
    frame.target().text(Rect{content.x, y, content.width, h}, props.title, title);
    y = static_cast<int16_t>(y + h + props.textGap);
  }
  if (props.author && y < textBottom) {
    const int16_t h = frame.target().lineHeight(props.authorText.font);
    frame.target().text(Rect{content.x, y, content.width, h}, props.author,
                        textStyleWithForeground(props.authorText, style.foreground));
    y = static_cast<int16_t>(y + h + props.textGap);
  }
  if (props.meta && y < textBottom) {
    const int16_t h = frame.target().lineHeight(props.metaText.font);
    frame.target().text(Rect{content.x, y, content.width, h}, props.meta,
                        textStyleWithForeground(props.metaText, style.foreground));
  }
  if (props.progressMax > 0) {
    ProgressBarProps progress;
    progress.value = props.progress;
    progress.max = props.progressMax;
    progress.track = Paint::dither(Color::LightGray);
    progress.fill = Paint::solid(Color::Black);
    progressBar(frame, Rect{content.x, progressY, content.width, progressH}, progress);
  }
}

}  // namespace ui
}  // namespace freeink
