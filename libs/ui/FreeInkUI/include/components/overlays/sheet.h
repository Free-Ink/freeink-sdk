#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

// The edge of the screen the sheet hangs from. The rule and grabber sit on
// the opposite (free) edge — the edge the sheet is dragged from.
enum class SheetEdge : uint8_t { Top, Bottom };

// Chrome for a partial-height sheet pulled over the screen (an iOS-style
// control center or bottom sheet): the card body, a rule along its free edge
// so the sheet separates from the page behind it, and a grabber pill on that
// same edge. Content is the caller's: draw it inside sheetContentRect(rect).
struct SheetProps {
  SheetEdge anchor = SheetEdge::Top;
  Paint background = Paint::solid(Color::White);
  Paint rule = Paint::solid(Color::Black);
  int16_t ruleWidth = 2;
  Paint grabber = Paint::solid(Color::Black);
  int16_t grabberWidth = 72;
  int16_t grabberHeight = 5;
  // Air between the grabber and the sheet's free edge (the rule draws inside
  // this band), and between the content and the grabber.
  int16_t grabberInset = 16;
  int16_t grabberMargin = 8;
  // Rounded corners on the free edge, so the card reads as a sheet pulled
  // over the page. 0 = square (a plain full-width band). RADIUS_INHERIT:
  // Screen::sheet() substitutes the theme's sheetRadius; on a bare Frame it
  // resolves to square.
  uint8_t radius = RADIUS_INHERIT;
  // Optional: dispatch this action when the user taps the screen outside the
  // sheet (the customary way to dismiss one). Registered over the remaining
  // screen area on the sheet's free side.
  ActionId dismissAction = NO_ACTION;
};

// The part of the sheet its content may use: the rect minus the grabber band
// on the free edge.
inline Rect sheetContentRect(Rect rect, const SheetProps &props) {
  const int16_t band =
      static_cast<int16_t>(props.grabberMargin + props.grabberHeight + props.grabberInset);
  return props.anchor == SheetEdge::Top ? rect.inset(Insets{0, 0, band, 0})
                                        : rect.inset(Insets{band, 0, 0, 0});
}

template <size_t MaxInteractions>
void sheet(Frame<MaxInteractions> &frame, Rect rect, const SheetProps &props) {
  const bool fromTop = props.anchor == SheetEdge::Top;
  frame.target().fill(rect, props.background, resolveRadius(props.radius, 0),
                      fromTop ? CornersBottom : CornersTop);

  if (props.ruleWidth > 0) {
    const int16_t ruleY =
        fromTop ? static_cast<int16_t>(rect.bottom() - props.ruleWidth) : rect.y;
    frame.target().fill(Rect{rect.x, ruleY, rect.width, props.ruleWidth}, props.rule);
  }

  if (props.grabberWidth > 0 && props.grabberHeight > 0) {
    const int16_t grabberY =
        fromTop ? static_cast<int16_t>(rect.bottom() - props.grabberInset - props.grabberHeight)
                : static_cast<int16_t>(rect.y + props.grabberInset);
    const Rect grabber{static_cast<int16_t>(rect.x + (rect.width - props.grabberWidth) / 2),
                       grabberY, props.grabberWidth, props.grabberHeight};
    frame.target().fill(grabber, props.grabber, static_cast<uint8_t>(props.grabberHeight / 2));
  }

  if (props.dismissAction != NO_ACTION) {
    const Rect screen = frame.safeRect();
    const Rect outside =
        fromTop ? Rect{screen.x, rect.bottom(), screen.width,
                       static_cast<int16_t>(screen.bottom() - rect.bottom())}
                : Rect{screen.x, screen.y, screen.width, static_cast<int16_t>(rect.y - screen.y)};
    if (!outside.empty()) frame.hit(outside, props.dismissAction, 0, InputTouch);
  }
}

}  // namespace ui
}  // namespace freeink
