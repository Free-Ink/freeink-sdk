#pragma once

#include "detail.h"
#include "../controls/button.h"

namespace freeink {
namespace ui {

// App-owned paging state, shared by horizontal shelves and vertical catalogs.
// Rendering sets count/visibleCount and clamps stale positions after feed changes.
struct CatalogWindow {
  uint16_t firstIndex = 0;
  uint16_t visibleCount = 0;
  uint16_t count = 0;
  void sync(uint16_t total, uint16_t visible) {
    count = total;
    visibleCount = visible;
    const uint16_t last = count > visibleCount ? count - visibleCount : 0;
    if (firstIndex > last) firstIndex = last;
  }
  bool canPrevious() const { return visibleCount > 0 && firstIndex > 0; }
  bool canNext() const { return visibleCount > 0 && static_cast<uint32_t>(firstIndex) + visibleCount < count; }
  void previous() { firstIndex = firstIndex > visibleCount ? firstIndex - visibleCount : 0; }
  void next() {
    if (!canNext()) return;
    const uint16_t last = count - visibleCount;
    const uint32_t next = static_cast<uint32_t>(firstIndex) + visibleCount;
    firstIndex = next < last ? static_cast<uint16_t>(next) : last;
  }
};

struct CatalogItem {
  const char* title = nullptr;
  const char* author = nullptr;
  BitmapRef cover{};
  AssetRef coverAsset{};
  int16_t value = 0;  // stable app identifier, not the visible slot
  State state = StateNormal;
  bool enabled = true;
};
using CatalogItemProvider = CatalogItem (*)(uint16_t index, void* userData);
using CatalogCoverPainter = bool (*)(DrawTarget&, Rect, const CatalogItem&, void* userData);

struct CatalogCoverProps {
  CatalogItem item{};
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault;
  int16_t minTouchSize = 44;
  State state = StateNormal;
  bool enabled = true;
  StyleSet styles{};
  uint8_t radius = 0;
  uint8_t borderEdges = EdgesAll;
  Insets padding{4, 4, 4, 4};
  Insets titlePadding{4, 6, 4, 6};
  TextStyle titleText{};
  TextStyle authorText{};
  uint8_t titleLines = 2;
  int16_t authorHeight = 18;
  int16_t gap = 4;
  // An opaque title band stays readable over real cover art.
  bool titleOnCover = true;
  Paint titleBackground = Paint::solid(Color::White);
  Paint titleForeground = Paint::solid(Color::Black);
  Paint placeholder = Paint::dither(Color::LightGray);
  BitmapMode coverMode = BitmapMode::Contain;
  CatalogCoverPainter coverPainter = nullptr;
  void* coverPainterUserData = nullptr;
};

template <size_t N>
void catalogCover(Frame<N>& frame, Rect rect, const CatalogCoverProps& props) {
  using namespace media_detail;
  if (rect.empty()) return;
  CatalogCoverProps card = props;
  card.state |= props.item.state;
  card.enabled = card.enabled && props.item.enabled;
  const BoxStyle style = surface(frame, rect, card, props.item.value);
  Rect content = rect.inset(props.padding);
  if (content.empty()) return;
  const int16_t gap = nonnegative(props.gap);
  Rect author{};
  if (present(props.item.author) && props.authorHeight > 0 && content.height > props.authorHeight + gap) {
    author = Rect{content.x, static_cast<int16_t>(content.bottom() - props.authorHeight), content.width, props.authorHeight};
    content.height -= props.authorHeight + gap;
  }
  const int16_t titleH = min(content.height, static_cast<int16_t>(
      frame.target().lineHeight(props.titleText.font) * props.titleLines +
      nonnegative(props.titlePadding.top) + nonnegative(props.titlePadding.bottom)));
  Rect label{content.x, static_cast<int16_t>(content.bottom() - titleH), content.width, titleH};
  Rect cover = content;
  if (!props.titleOnCover) cover.height = nonnegative(static_cast<int16_t>(cover.height - titleH - gap));
  if (!cover.empty()) {
    const bool painted = props.coverPainter && props.coverPainter(frame.target(), cover, props.item, props.coverPainterUserData);
    if (!painted) {
      frame.target().fill(cover, props.placeholder);
      const BitmapRef bitmap = props.item.cover ? props.item.cover : resolveBitmap(frame.assets(), props.item.coverAsset);
      if (bitmap) frame.target().bitmap(cover, bitmap, props.coverMode, style.foreground);
    }
  }
  if (!label.empty() && present(props.item.title)) {
    if (props.titleOnCover) frame.target().fill(label, props.titleBackground);
    label = label.inset(props.titlePadding);
    text(frame.target(), label, props.item.title,
         textStyleWithForeground(props.titleText, props.titleOnCover ? props.titleForeground : style.foreground), props.titleLines, 0);
  }
  text(frame.target(), author, props.item.author, textStyleWithForeground(props.authorText, style.foreground), 1, 0);
  // Keep focus visible even when cover art fills most of the card.
  const State state = frame.stateFor(card.action, props.item.value, card.state);
  if (card.enabled && !hasState(state, StateDisabled) &&
      (hasState(state, StateFocused) || hasState(state, StateSelected)))
    frame.target().stroke(rect, style.foreground, 2, props.radius ? props.radius : style.radius, style.corners);
}

struct CoverShelfProps {
  const char* title = nullptr;
  const char* emptyLabel = "No titles";
  const CatalogItem* items = nullptr;
  CatalogItemProvider itemProvider = nullptr;
  void* itemProviderUserData = nullptr;
  uint16_t count = 0;
  CatalogWindow* window = nullptr;  // keep one per group
  CatalogCoverProps card{};
  ButtonProps previous{};
  ButtonProps next{};
  ButtonProps seeAll{};
  TextStyle headingText{};
  StyleSet styles{};
  State state = StateNormal;
  bool enabled = true;
  uint8_t radius = 0;
  uint8_t borderEdges = EdgesAll;
  Insets padding{8, 8, 8, 8};
  int16_t cardWidth = 112;
  int16_t gap = 12;
  int16_t headerHeight = 44;
  int16_t headerGap = 8;
  int16_t navigationWidth = 44;
  int16_t seeAllWidth = 80;
  bool swipeNavigation = false;  // only the active shelf should claim global swipes
};

template <size_t N>
void coverShelf(Frame<N>& frame, Rect rect, const CoverShelfProps& props) {
  using namespace media_detail;
  if (rect.empty()) { if (props.window) props.window->sync(props.count, 0); return; }
  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  if (props.radius) setStyleRadius(styles, props.radius);
  const bool enabled = props.enabled && !hasState(props.state, StateDisabled);
  const BoxStyle style = styles.resolve(enabled ? props.state : static_cast<State>(props.state | StateDisabled));
  box(frame.target(), rect, style, props.borderEdges);
  Rect body = rect.inset(props.padding);
  const int16_t headerH = props.headerHeight > frame.device().minTouchSize ? props.headerHeight : frame.device().minTouchSize;
  Rect header = take(body, headerH, props.headerGap);
  const int16_t gap = nonnegative(props.gap);
  const int16_t width = props.cardWidth > 0 ? min(body.width, props.cardWidth) : 0;
  const uint16_t visible = !body.empty() && width > 0 ? (body.width + gap) / (width + gap) : 0;
  CatalogWindow local;
  CatalogWindow& window = props.window ? *props.window : local;
  window.sync(props.count, visible);
  auto control = [&](ButtonProps buttonProps, int16_t desiredWidth, bool available) {
    if (!hasButton(buttonProps) || header.empty()) return;
    const int16_t w = min(header.width, desiredWidth > buttonProps.minTouchSize ? desiredWidth : buttonProps.minTouchSize);
    if (w <= 0) return;
    Rect band{static_cast<int16_t>(header.right() - w), header.y, w, header.height};
    header.width -= min(header.width, static_cast<int16_t>(w + gap));
    buttonProps.enabled = enabled && buttonProps.enabled && available && !hasState(buttonProps.state, StateDisabled);
    button(frame, band, buttonProps);
  };
  if (enabled && props.swipeNavigation) {
    if (window.canNext() && props.next.enabled && !hasState(props.next.state, StateDisabled) && props.next.action != NO_ACTION)
      frame.hit(rect, props.next.action, props.next.value, InputSwipeLeft);
    if (window.canPrevious() && props.previous.enabled && !hasState(props.previous.state, StateDisabled) && props.previous.action != NO_ACTION)
      frame.hit(rect, props.previous.action, props.previous.value, InputSwipeRight);
  }
  control(props.next, props.navigationWidth, window.canNext());
  control(props.previous, props.navigationWidth, window.canPrevious());
  control(props.seeAll, props.seeAllWidth, true);
  TextStyle heading = textStyleWithForeground(props.headingText, style.foreground);
  heading.bold = true;
  text(frame.target(), header, props.title, heading, 1, 0);
  if (!props.count || (!props.items && !props.itemProvider)) {
    text(frame.target(), body, props.emptyLabel, heading, 2, 0);
    return;
  }
  for (uint16_t slot = 0; slot < visible && window.firstIndex + slot < props.count; ++slot) {
    const uint16_t index = window.firstIndex + slot;
    CatalogCoverProps card = props.card;
    card.item = props.itemProvider ? props.itemProvider(index, props.itemProviderUserData) : props.items[index];
    card.enabled = enabled && card.enabled;
    Rect cell{static_cast<int16_t>(body.x + slot * (width + gap)), body.y, width, body.height};
    catalogCover(frame, cell, card);
  }
}

struct CatalogPageProps {
  const CoverShelfProps* shelves = nullptr;
  uint16_t count = 0;
  CatalogWindow* window = nullptr;
  int16_t shelfHeight = 252;
  int16_t activeShelf = -1;  // optional swipe target; -1 leaves swipes unclaimed
  int16_t gap = 12;
  Insets padding{8, 8, 8, 8};
  ButtonProps previous{};
  ButtonProps next{};
  int16_t navigationHeight = 44;
  StyleSet styles{};
  State state = StateNormal;
  bool enabled = true;
  uint8_t radius = 0;
  uint8_t borderEdges = EdgesAll;
};

template <size_t N>
void catalogPage(Frame<N>& frame, Rect rect, const CatalogPageProps& props) {
  using namespace media_detail;
  CatalogWindow local;
  CatalogWindow& window = props.window ? *props.window : local;
  window.sync(props.count, 0);
  if (rect.empty()) return;
  const bool enabled = props.enabled && !hasState(props.state, StateDisabled);
  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  if (props.radius) setStyleRadius(styles, props.radius);
  box(frame.target(), rect, styles.resolve(enabled ? props.state : static_cast<State>(props.state | StateDisabled)), props.borderEdges);
  Rect body = rect.inset(props.padding);
  if (body.empty()) return;
  Rect navigation{};
  const int16_t gap = nonnegative(props.gap);
  int16_t navH = props.navigationHeight > frame.device().minTouchSize ? props.navigationHeight : frame.device().minTouchSize;
  if (hasButton(props.previous) || hasButton(props.next)) {
    navH = min(body.height, navH);
    navigation = Rect{body.x, static_cast<int16_t>(body.bottom() - navH), body.width, navH};
    body.height -= min(body.height, static_cast<int16_t>(navH + gap));
  }
  const int16_t shelfH = props.shelfHeight > 0 ? min(body.height, props.shelfHeight) : 0;
  const uint16_t visible = !body.empty() && shelfH > 0 ? (body.height + gap) / (shelfH + gap) : 0;
  window.sync(props.count, visible);
  if (props.shelves) for (uint16_t slot = 0; slot < visible && window.firstIndex + slot < props.count; ++slot) {
    CoverShelfProps shelf = props.shelves[window.firstIndex + slot];
    shelf.enabled = enabled && shelf.enabled;
    shelf.swipeNavigation = props.activeShelf == static_cast<int16_t>(window.firstIndex + slot);
    coverShelf(frame, take(body, shelfH, gap), shelf);
  }
  if (!navigation.empty()) {
    const int16_t w = nonnegative(static_cast<int16_t>((navigation.width - gap) / 2));
    ButtonProps previous = props.previous;
    ButtonProps next = props.next;
    previous.enabled = enabled && previous.enabled && !hasState(previous.state, StateDisabled) && window.canPrevious();
    next.enabled = enabled && next.enabled && !hasState(next.state, StateDisabled) && window.canNext();
    if (w > 0 && hasButton(previous)) button(frame, Rect{navigation.x, navigation.y, w, navigation.height}, previous);
    if (w > 0 && hasButton(next)) button(frame, Rect{static_cast<int16_t>(navigation.right() - w), navigation.y, w, navigation.height}, next);
  }
}

}  // namespace ui
}  // namespace freeink
