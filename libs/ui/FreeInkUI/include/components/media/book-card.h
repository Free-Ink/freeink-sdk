#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/progress-bar.h"

namespace freeink {
namespace ui {

enum class BookCardSelectionIndicator {
  Card,
  CoverFrame,
};

struct BookCardProps {
  const char* title = nullptr;
  const char* author = nullptr;
  const char* meta = nullptr;
  const char* progressLabel = nullptr;
  BitmapRef cover{};
  AssetRef coverAsset{};
  int32_t progress = 0;
  int32_t progressMax = 100;
  ActionId action = NO_ACTION;
  int16_t value = 0;
  TextStyle titleText{};
  TextStyle authorText{};
  TextStyle metaText{};
  TextStyle progressText{};
  StyleSet styles{};
  State state = StateNormal;
  bool enabled = true;
  Size coverSize{62, 84};
  Insets padding{6, 8, 6, 8};
  int16_t gap = 14;
  int16_t textGap = 4;
  int16_t textProgressGap = 8;
  int16_t progressHeight = 4;
  int16_t progressLabelGap = 8;
  bool centerTextVertically = false;
  // Center against the cover, constrained above the progress row.
  bool centerTextOnCover = false;
  BookCardSelectionIndicator selectionIndicator = BookCardSelectionIndicator::Card;
  int16_t selectedCoverFrameGap = 3;
  int16_t selectedCoverFrameWidth = 2;
  uint8_t selectedCoverFrameRadius = 0;
  bool (*coverPainter)(DrawTarget& target, Rect rect, const BookCardProps& props, void* userData) = nullptr;
  void* coverPainterUserData = nullptr;
};

template <size_t MaxInteractions>
void bookCard(Frame<MaxInteractions>& frame, Rect rect, const BookCardProps& props) {
  State state = props.enabled ? props.state : static_cast<State>(props.state | StateDisabled);
  if (props.action != NO_ACTION && props.enabled) {
    frame.hit(ensureMinTouchRect(rect, frame.device().minTouchSize, frame.screen()), props.action, props.value,
              InputDefault, state);
  }
  state = frame.stateFor(props.action, props.value, state);
  const bool selected = (state & StateSelected) != 0;
  State cardState = state;
  if (selected && props.selectionIndicator == BookCardSelectionIndicator::CoverFrame) {
    cardState = static_cast<State>(static_cast<uint8_t>(cardState) & ~static_cast<uint8_t>(StateSelected));
  }
  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  const BoxStyle& style = styles.resolve(cardState);
  const BoxStyle& selectedStyle = styles.resolve(state);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(props.padding);
  const BitmapRef cover = props.cover ? props.cover : resolveBitmap(frame.assets(), props.coverAsset);
  const int16_t coverW = props.coverSize.width > content.width ? content.width : props.coverSize.width;
  const int16_t coverH = props.coverSize.height > content.height ? content.height : props.coverSize.height;
  Rect coverRect{content.x, static_cast<int16_t>(content.y + (content.height - coverH) / 2), coverW, coverH};
  bool coverDrawn = false;
  if (props.coverPainter) {
    coverDrawn = props.coverPainter(frame.target(), coverRect, props, props.coverPainterUserData);
  }
  if (!coverDrawn) {
    frame.target().fill(coverRect, Paint::dither(Color::LightGray));
    if (cover) frame.target().bitmap(coverRect, cover, BitmapMode::Cover, style.foreground);
  }
  if (selected && props.selectionIndicator == BookCardSelectionIndicator::CoverFrame) {
    const int16_t gap = props.selectedCoverFrameGap;
    Rect frameRect{static_cast<int16_t>(coverRect.x - gap), static_cast<int16_t>(coverRect.y - gap),
                   static_cast<int16_t>(coverRect.width + gap * 2),
                   static_cast<int16_t>(coverRect.height + gap * 2)};
    frame.target().stroke(frameRect,
                          selectedStyle.border.kind != PaintKind::None ? selectedStyle.border
                                                                        : Paint::solid(Color::Black),
                          props.selectedCoverFrameWidth, props.selectedCoverFrameRadius);
  }
  content.x = static_cast<int16_t>(coverRect.right() + props.gap);
  content.width = static_cast<int16_t>(rect.right() - props.padding.right - content.x);
  const int16_t progressH = props.progressHeight < 1 ? 1 : props.progressHeight;
  const bool hasProgressLabel = props.progressMax > 0 && props.progressLabel && props.progressLabel[0] != '\0';
  const int16_t labelH = hasProgressLabel ? frame.target().lineHeight(props.progressText.font) : 0;
  const int16_t progressRowH = labelH > progressH ? labelH : progressH;
  const int16_t progressRowY = static_cast<int16_t>(coverRect.bottom() - progressRowH);
  const int16_t progressY = static_cast<int16_t>(progressRowY + (progressRowH - progressH) / 2);
  const int16_t textBottom = static_cast<int16_t>(progressRowY - props.textProgressGap);

  int16_t textBlockH = 0;
  if (props.title) {
    TextStyle title = textStyleWithForeground(props.titleText, style.foreground);
    title.maxLines = title.maxLines ? title.maxLines : 2;
    textBlockH = static_cast<int16_t>(textBlockH + measureWrappedText(frame.target(), props.title, title, content.width).height);
  }
  if (props.author) {
    if (textBlockH > 0) textBlockH = static_cast<int16_t>(textBlockH + props.textGap);
    textBlockH = static_cast<int16_t>(textBlockH + frame.target().lineHeight(props.authorText.font));
  }
  if (props.meta) {
    if (textBlockH > 0) textBlockH = static_cast<int16_t>(textBlockH + props.textGap);
    textBlockH = static_cast<int16_t>(textBlockH + frame.target().lineHeight(props.metaText.font));
  }

  int16_t y = content.y;
  if (props.centerTextOnCover && textBlockH > 0) {
    y = static_cast<int16_t>(coverRect.y + (coverRect.height - textBlockH) / 2);
    if (y + textBlockH > textBottom) y = static_cast<int16_t>(textBottom - textBlockH);
    if (y < content.y) y = content.y;
  } else if (props.centerTextVertically && textBlockH > 0) {
    const int16_t textAreaH = static_cast<int16_t>(textBottom - content.y);
    if (textAreaH > textBlockH) y = static_cast<int16_t>(content.y + (textAreaH - textBlockH) / 2);
  }
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
    int16_t barX = content.x;
    int16_t barWidth = content.width;
    if (hasProgressLabel && content.width > 0) {
      TextStyle labelStyle = textStyleWithForeground(props.progressText, style.foreground);
      labelStyle.maxLines = 1;
      labelStyle.align = TextAlign::Left;
      int16_t labelWidth = frame.target().measureText(labelStyle.font, props.progressLabel, labelStyle).width;
      if (labelWidth > content.width / 2) labelWidth = static_cast<int16_t>(content.width / 2);
      frame.target().text(Rect{content.x, static_cast<int16_t>(progressRowY + (progressRowH - labelH) / 2),
                               labelWidth, labelH}, props.progressLabel, labelStyle);
      const int16_t gap = props.progressLabelGap > 0 ? props.progressLabelGap : 0;
      barX = static_cast<int16_t>(content.x + labelWidth + gap);
      barWidth = static_cast<int16_t>(content.width - labelWidth - gap);
    }
    if (barWidth > 0) progressBar(frame, Rect{barX, progressY, barWidth, progressH}, progress);
  }
}

}  // namespace ui
}  // namespace freeink
