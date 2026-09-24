#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/button.h"
#include "detail.h"

namespace freeink {
namespace ui {

// All strings/assets are borrowed. Applications own acquisition policy,
// localization, HTML-to-text conversion, loading, and network operations.
struct PublicationHeaderProps {
  const char* title = nullptr;
  const char* author = nullptr;
  const char* format = nullptr;
  const char* series = nullptr;
  BitmapRef cover{};
  AssetRef coverAsset{};
  TextStyle titleText{};
  TextStyle detailText{};
  StyleSet styles{};
  State state = StateNormal;
  bool enabled = true;
  uint8_t radius = 0;  // positive values override every state; otherwise use styles
  uint8_t borderEdges = EdgesAll;
  ActionId action = NO_ACTION;  // optional whole-section action
  int16_t value = 0;
  uint16_t inputMask = InputDefault;
  int16_t minTouchSize = 44;
  Insets padding{};
  Size coverSize{128, 192};
  BitmapMode coverMode = BitmapMode::Contain;
  BoxStyle coverStyle{Paint::solid(Color::White), Paint::solid(Color::Black), Paint::solid(Color::Black), 1};
  Insets coverPadding{12, 8, 8, 8};
  int16_t gap = 16;
  int16_t textGap = 6;
  int16_t titleGap = 10;
  uint8_t titleLines = 3;
  uint8_t authorLines = 2;
  uint8_t seriesLines = 2;
  uint8_t coverTitleLines = 5;
  bool titleBold = true;
  bool (*coverPainter)(DrawTarget&, Rect, const PublicationHeaderProps&, void*) = nullptr;
  void* coverPainterUserData = nullptr;
};

struct PublicationAvailabilityProps {
  const char* status = nullptr;       // e.g. "Available to borrow"
  const char* copies = nullptr;       // absent data stays absent
  const char* holds = nullptr;        // e.g. "You are #3 in line"
  const char* loan = nullptr;         // e.g. "Borrow for 21 days"
  TextStyle headingText{};
  TextStyle detailText{};
  StyleSet styles{};
  State state = StateNormal;
  bool enabled = true;
  uint8_t radius = 0;  // positive values override every state; otherwise use styles
  uint8_t borderEdges = EdgesAll;
  ActionId action = NO_ACTION;  // optional whole-section action
  int16_t value = 0;
  uint16_t inputMask = InputDefault;
  int16_t minTouchSize = 44;
  Insets padding{10, 0, 0, 0};
  int16_t gap = 6;
  Paint divider = Paint::solid(Color::Black);
  int16_t dividerHeight = 1;
  bool headingBold = true;
};



template <size_t N>
void publicationHeader(Frame<N>& frame, Rect rect, const PublicationHeaderProps& props) {
  using namespace media_detail;
  if (rect.empty()) return;
  const BoxStyle style = surface(frame, rect, props, props.value);
  rect = rect.inset(props.padding);
  if (rect.empty()) return;
  // Reserve about a third of the width for a portrait cover; preserve its ratio.
  int16_t coverW = 0;
  if (props.coverSize.width > 0 && props.coverSize.height > 0) {
    coverW = min(props.coverSize.width, rect.width / 3);
    const int32_t heightLimit = static_cast<int32_t>(rect.height) * props.coverSize.width / props.coverSize.height;
    if (heightLimit < coverW) coverW = static_cast<int16_t>(heightLimit);
  }
  if (coverW > 0) {
    Rect coverRect{rect.x, rect.y, coverW, static_cast<int16_t>(static_cast<int32_t>(coverW) * props.coverSize.height / props.coverSize.width)};
    BitmapRef cover = props.cover ? props.cover : resolveBitmap(frame.assets(), props.coverAsset);
    const bool painted = props.coverPainter &&
        props.coverPainter(frame.target(), coverRect, props, props.coverPainterUserData);
    if (painted) {
      // The app painted the cover (e.g. a rounded or decoded asset).
    } else if (cover) {
      frame.target().bitmap(coverRect, cover, props.coverMode, props.coverStyle.foreground);
    } else {
      // A quiet typeset cover instead of a broken-image marker.
      box(frame.target(), coverRect, props.coverStyle, EdgesAll);
      Rect inset = coverRect.inset(props.coverPadding);
      TextStyle coverText = textStyleWithForeground(props.detailText, props.coverStyle.foreground);
      coverText.align = TextAlign::Center;
      text(frame.target(), inset, props.title, coverText, props.coverTitleLines);
    }
    const int16_t used = min(rect.width, static_cast<int16_t>(coverW + nonnegative(props.gap)));
    rect.x += used;
    rect.width -= used;
  }
  TextStyle heading = textStyleWithForeground(props.titleText, style.foreground);
  TextStyle detail = textStyleWithForeground(props.detailText, style.foreground);
  heading.bold = props.titleBold;
  text(frame.target(), rect, props.title, heading, props.titleLines, props.titleGap);
  text(frame.target(), rect, props.author, detail, props.authorLines, props.titleGap);
  text(frame.target(), rect, props.series, detail, props.seriesLines, props.textGap);
  text(frame.target(), rect, props.format, detail, 1, props.textGap);
}

template <size_t N>
void publicationAvailability(Frame<N>& frame, Rect rect, const PublicationAvailabilityProps& props) {
  using namespace media_detail;
  if (rect.empty()) return;
  const BoxStyle style = surface(frame, rect, props, props.value);
  if (props.dividerHeight > 0 && props.divider.kind != PaintKind::None)
    frame.target().fill(Rect{rect.x, rect.y, rect.width, min(rect.height, props.dividerHeight)}, props.divider);
  rect = rect.inset(props.padding);
  TextStyle heading = textStyleWithForeground(props.headingText, style.foreground);
  TextStyle detail = textStyleWithForeground(props.detailText, style.foreground);
  heading.bold = props.headingBold;
  text(frame.target(), rect, props.status, heading, 1, props.gap);
  text(frame.target(), rect, props.copies, detail, 1, props.gap);
  text(frame.target(), rect, props.holds, detail, 1, props.gap);
  text(frame.target(), rect, props.loan, detail, 1, props.gap);
}

struct PublicationPageProps {
  PublicationHeaderProps book{};
  PublicationAvailabilityProps availability{};
  const char* descriptionHeading = "About this book";
  const char* description = nullptr;  // plain text, not raw OPDS HTML
  const char* metadata = nullptr;     // localized publisher / date / language
  TextStyle headingText{};
  TextStyle bodyText{};
  ButtonProps primary{};              // Borrow / Place hold / Buy / Download
  ButtonProps secondary{};            // Read sample / Manage hold, optional
  ButtonProps more{};                 // app opens full description, optional
  Insets padding{16, 16, 16, 16};
  int16_t heroHeight = 192;
  int16_t actionHeight = 48;
  int16_t actionGap = 8;
  int16_t sectionGap = 12;
  int16_t descriptionGap = 8;
  uint8_t metadataLines = 2;
  uint8_t descriptionLines = 255;
  bool headingBold = true;
  StyleSet styles{};
  State state = StateNormal;
  bool enabled = true;  // disables every child action
  uint8_t radius = 0;
  uint8_t borderEdges = EdgesAll;
};

// Compose inside the app's header/footer. Acquisition controls are reserved
// first so a long description cannot push them off-screen. Small viewports
// omit lower-priority text; `more` can open the app's full-description screen.
template <size_t N>
void publicationPage(Frame<N>& frame, Rect rect, const PublicationPageProps& props) {
  using namespace media_detail;
  if (rect.empty()) return;
  const bool enabled = props.enabled && !hasState(props.state, StateDisabled);
  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  if (props.radius > 0) setStyleRadius(styles, props.radius);
  const BoxStyle style = styles.resolve(enabled ? props.state : static_cast<State>(props.state | StateDisabled));
  box(frame.target(), rect, style, props.borderEdges);
  Rect body = rect.inset(props.padding);
  if (body.empty()) return;
  auto action = [&](ButtonProps buttonProps, bool primary) {
    int16_t actionH = props.actionHeight > frame.device().minTouchSize
                         ? props.actionHeight : frame.device().minTouchSize;
    if (buttonProps.minTouchSize > actionH) actionH = buttonProps.minTouchSize;
    const int16_t textH = frame.target().lineHeight(buttonProps.text.font) +
                         nonnegative(buttonProps.padding.top) + nonnegative(buttonProps.padding.bottom);
    if (textH > actionH) actionH = textH;
    buttonProps.enabled = enabled && buttonProps.enabled && !hasState(buttonProps.state, StateDisabled);
    if (!hasButton(buttonProps) || actionH <= 0 || body.height < actionH) return;
    Rect band{body.x, static_cast<int16_t>(body.bottom() - actionH), body.width, actionH};
    body.height -= min(body.height, static_cast<int16_t>(actionH + nonnegative(props.actionGap)));
    if (buttonProps.styles.unset()) {
      buttonProps.styles = outlinedButtonStyles();
      if (primary) {
        buttonProps.styles.normal.background = Paint::solid(Color::Black);
        buttonProps.styles.normal.foreground = Paint::solid(Color::White);
      }
    }
    button(frame, band, buttonProps);
  };
  action(props.primary, true);
  action(props.secondary, false);
  action(props.more, false);
  if (body.empty()) return;

  const auto& av = props.availability;
  int16_t availabilityH = 0;
  if (present(av.status)) availabilityH += frame.target().lineHeight(av.headingText.font) + nonnegative(av.gap);
  const char* details[] = {av.copies, av.holds, av.loan};
  for (const char* label : details) {
    if (present(label)) availabilityH += frame.target().lineHeight(av.detailText.font) + nonnegative(av.gap);
  }
  if (availabilityH) availabilityH += nonnegative(av.padding.top) + nonnegative(av.padding.bottom);
  int16_t heroH = props.heroHeight > 0 ? props.heroHeight : 192;
  // Keep availability visible on compact displays, while retaining book identity.
  const int16_t budget = static_cast<int16_t>(body.height - availabilityH - nonnegative(props.sectionGap));
  heroH = min(heroH, budget > body.height / 2 ? budget : body.height / 2);
  PublicationHeaderProps book = props.book;
  PublicationAvailabilityProps availability = av;
  book.enabled = enabled && book.enabled;
  availability.enabled = enabled && availability.enabled;
  publicationHeader(frame, take(body, heroH, props.sectionGap), book);
  if (availabilityH) publicationAvailability(frame, take(body, availabilityH, props.sectionGap), availability);
  TextStyle bodyText = textStyleWithForeground(props.bodyText, style.foreground);
  text(frame.target(), body, props.metadata, bodyText, props.metadataLines, props.sectionGap);
  if (present(props.description)) {
    TextStyle heading = textStyleWithForeground(props.headingText, style.foreground);
    heading.bold = props.headingBold;
    text(frame.target(), body, props.descriptionHeading, heading, 1, props.descriptionGap);
    text(frame.target(), body, props.description, bodyText, props.descriptionLines, 0);
  }
}

}  // namespace ui
}  // namespace freeink
