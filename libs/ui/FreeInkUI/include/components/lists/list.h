#pragma once

#include "../../FreeInkUICore.h"

#include <atomic>

namespace freeink {
namespace ui {

struct ListItem {
  const char *label = nullptr;
  const char *subtitle = nullptr;
  const char *value = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  State state = StateNormal;
  int16_t actionValue = 0;
  bool enabled = true;
  // Section header row: shorter, non-interactive, drawn with headerText and
  // an underline; never selected or focused.
  bool isHeader = false;
  // On/off row: a switch (toggle-row visuals) replaces the value slot; the
  // value string is ignored when set. Activation stays row-level via action.
  bool toggle = false;
  bool toggleChecked = false;
  // Optional section heading drawn immediately before this selectable row.
  // It shares the row's logical index and interaction value. Kept last so
  // existing aggregate initializers remain source-compatible.
  const char *sectionHeading = nullptr;
};

struct ListNav;

enum class SelectionMarker : uint8_t {
  None,      // selection shown by the row's selected BoxStyle
  Underline, // thin line under the selected row's content
  Triangle,  // right-pointing triangle at the selected row's left edge
  Bitmap, // caller-supplied glyph (markerBitmap/markerAsset) at the left edge
};

struct ListProps {
  const ListItem *items = nullptr;
  uint16_t count = 0;
  // First absolute index items[0] corresponds to. Lets `items` be a small
  // window around the viewport instead of an array of all `count` entries —
  // a several-hundred-row list (an EPUB table of contents) would otherwise
  // pin tens of KB of ListItems + label strings for rows that are never
  // drawn. list() stops at the viewport edge or itemsWindowCount, so keep
  // the supplied window large enough for the rows and an optional preview
  // (refresh it after viewport changes, before list()). Set props.nav for a window whose top may
  // enter the last fixed-height page; otherwise list() may clamp top below the
  // supplied window. 0 = items is the full array.
  uint16_t itemsWindowFirst = 0;
  // Number of ListItems supplied in `items` when it is a virtual window.
  // Set this whenever itemsWindowFirst is non-zero (or the supplied array is
  // otherwise shorter than count), so optional previews can stay within the
  // materialized data. 0 preserves the full-array behavior for existing
  // callers.
  uint16_t itemsWindowCount = 0;
  // Pull-based row source: when set, list() resolves each row it lays out by
  // calling rowProvider(rowProviderCtx, index, item) into a loop-local
  // scratch ListItem, and `items` may stay null. Nothing is materialized up
  // front: the caller formats row `index` on demand (typically into small
  // scratch buffers it owns) and points the item's strings there. Pointers
  // written into the item are read within that row's layout/draw pass only,
  // so they need to stay valid just until the next provider call. The
  // provider runs on the render task for the handful of rows that fit the
  // viewport, never for the rest of `count`.
  // itemsWindowFirst/itemsWindowCount apply to the array path only and are
  // ignored when a provider is set.
  void (*rowProvider)(void *ctx, uint16_t index, ListItem &item) = nullptr;
  void *rowProviderCtx = nullptr;
  // First item index drawn at the top of the rect. The list is virtualized:
  // only the rows that fully fit inside the rect are laid out, drawn, and
  // registered for interaction. Use listVisibleRows()/listTopIndexFor() to
  // keep a selection in view while scrolling.
  uint16_t topIndex = 0;
  int16_t selectedIndex = -1;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle labelText{};
  TextStyle subtitleText{};
  TextStyle valueText{};
  StyleSet rowStyles{};
  // Screen::list() resolves rowHeight <= 0 from the label font, list padding
  // and device touch minimum; each row grows for its actual content. Positive
  // rowHeight is an explicit minimum. Other sentinels inherit theme geometry.
  // Raw list() retains its 36px minimum when rowHeight is unset.
  int16_t rowHeight = 0;
  int16_t rowGap = -1;
  uint8_t rowRadius = 0;
  int16_t sidePadding = -1;
  int16_t textGap = 10;
  int16_t iconSize = 0;
  // Extra inset for the right-aligned value slot beyond sidePadding, so a
  // trailing chevron/value keeps air from the row edge on themes with tight
  // row padding.
  int16_t valueInset = 0;
  // When a multi-line label would otherwise overlap its trailing value, keep
  // the wrapped title band visually balanced with that value. Callers with a
  // short, secondary value (such as a file extension) can disable this to
  // use the full width remaining before the value.
  bool balanceWrappedLabelWithValue = true;
  // Switch geometry for ListItem::toggle rows (mirrors ToggleRowProps).
  // Colors derive from the row style's foreground so the switch stays legible
  // on inverted (selected) rows.
  int16_t toggleWidth = 38;
  int16_t toggleHeight = 18;
  uint8_t toggleRadius = 0;
  uint8_t toggleKnobRadius = 0;
  int16_t toggleKnobInset = 3;
  uint8_t toggleBorderWidth = 1;
  // Horizontal inset of the ROWS within the rect (the Lyra pill band). The
  // scroll indicator stays at the rect's edge, in the inset margin.
  // -1 = inherit: Screen::list() substitutes the theme's listInset.
  int16_t rowInset = -1;
  // Inherit sentinels like rowGap/sidePadding: width -1 = theme (raw list()
  // falls back to 3); side 0xFF = theme (raw falls back to right).
  int16_t scrollIndicatorWidth = -1;
  uint8_t scrollIndicatorSide = 0xFF; // 0 = right edge, 1 = left edge
  // Inward offset of the scroll track from the band edge (for panels recessed
  // behind a bezel). -1 = inherit the theme's listScrollInset.
  int16_t scrollIndicatorInset = -1;
  bool centerSingleLine = false;
  // Mirrors row layout for RTL languages: icon and label move to the
  // trailing (right) edge, value/toggle move to the leading (left) edge --
  // matching BaseTheme::drawList()'s "title right, value left" convention.
  // Callers must set this themselves (I18N.isRtl()); list() has no built-in
  // language awareness. Off by default so every existing caller is
  // unaffected.
  bool rtl = false;
  // Shrink each row's background/hit area to its label width plus side
  // padding instead of the full rect width (hug-content menu rows).
  bool hugContents = false;
  // Draws a thin position indicator along the right edge when the list
  // overflows the rect.
  bool scrollIndicator = true;
  // Draw a non-interactive preview of the next row when there is
  // leftover space after the fully visible rows. Uses normal row layout and
  // pixel clipping; omitted on targets without clipping. Only full rows count
  // towards navigation and interaction.
  bool partialTrailingRow = false;
  int16_t partialTrailingMinHeight = 18;
  // Additional marker drawn on the selected row (the v1 theme Underline and
  // Triangle selection styles).
  SelectionMarker selectionMarker = SelectionMarker::None;
  Paint markerPaint = Paint::solid(Color::Black);
  int16_t markerInset = 0;     // x offset of the marker / underline start
  int16_t markerThickness = 2; // underline thickness
  // Glyph for SelectionMarker::Bitmap, drawn vertically centered at the
  // selected row's left edge (markerInset offset). Direct bitmap wins;
  // otherwise the asset resolves through the frame's AssetResolver.
  BitmapRef markerBitmap{};
  AssetRef markerAsset{};
  // Section header rows (ListItem::isHeader).
  TextStyle headerText{};
  int16_t headerRowHeight = 0; // 0 = headerText line height + underline gap
  int16_t sectionGap = 16;     // extra padding above a non-first header
  bool headerUnderline = true;
  // Optional viewport-feedback channel: when set, list() reports the laid-out
  // viewport back to the nav (effective top, indexes that actually fit,
  // whether the selected row was drawn). Variable-height rows (wrapped
  // labels, subtitles) can fit fewer rows than the fixed-height
  // listVisibleRows() estimate; without feedback the selection can sit on a
  // never-drawn row and page jumps can skip rows entirely.
  // ListNav::syncToProps() wires this automatically.
  ListNav *nav = nullptr;
  // Explicit vertical content padding. -1 preserves legacy row-height-derived
  // padding; non-negative values make rowHeight a minimum, growing to content.
  int16_t rowPaddingY = -1;
};

// Stateful companion to the immediate-mode list helpers in FreeInkUICore.h:
// the selection-vs-viewport protocol most list screens want on e-paper.
// Drag/swipe scrolling moves the viewport (top) WITHOUT moving the selection —
// the selection may scroll off-screen; key/button navigation moves the
// selection and the caller re-follows (follow()) so the viewport is pulled the
// minimal amount to keep it visible. The first syncToProps() after reset()
// snaps the viewport to the selection (a list can open on an entry past the
// first page). How the selection index itself moves (wrap, paging) stays with
// the caller.
struct ListNav {
  // Logical selection; rendering snapshots it and conditionally clamps stale
  // indexes when the data shrinks.
  // Use .load() when passing to templates that deduce their argument type.
  std::atomic<int> selected{0};
  // Render-owned state below: use requests/inputPageRows() from input tasks.
  int top = 0;
  int visibleRows = 1; // measured by syncToProps(); 1 until the first build
  std::atomic<bool> followOnBuild{true};
  // Indexes the last list() build actually laid out from top (0 = no build
  // yet). With variable-height rows this is the real page size, unlike the
  // fixed-height visibleRows estimate.
  int drawnRows = 0;
  // props.count that drawnRows was measured against. A caller that reloads
  // its data (a File Browser folder, a filtered list) keeps the same nav, so
  // a measurement from a different row set must not be trusted as this list's
  // page size. list() sets it alongside the feedback below. It only catches a
  // reload that CHANGES the count: same-count new content (a rename, a
  // re-sort) still inherits the old measurement, which is close enough
  // because the next layout corrects it.
  int drawnCount = 0;
  // A follow() is awaiting confirmation from onListRendered() that the
  // selection was actually drawn. Swipe scrolling (scrollBy) never sets this:
  // the selection is allowed off-screen there by design.
  bool followPending = false;
  // onListRendered() advanced the viewport after layout; the caller should
  // rebuild the screen (consumeRebuildNeeded()) before displaying.
  bool rebuildNeeded = false;

  // Copies are for quiescent state (for example, initializing per-tab storage).
  ListNav() = default;
  ListNav(const ListNav &other) { *this = other; }
  ListNav &operator=(const ListNav &other) {
    selected.store(other.selected.load());
    top = other.top;
    visibleRows = other.visibleRows;
    followOnBuild.store(other.followOnBuild.load());
    drawnRows = other.drawnRows;
    drawnCount = other.drawnCount;
    followPending = other.followPending;
    rebuildNeeded = other.rebuildNeeded;
    pendingScroll_.store(other.pendingScroll_.load());
    publishedPageRows_.store(other.publishedPageRows_.load());
    layoutSelected_ = other.layoutSelected_;
    return *this;
  }

  // Input-task API. Selection changes are immediate for Confirm/rapid Next;
  // only the render task changes the viewport. Notify the renderer afterwards.
  void requestSelection(const int index) {
    selected.store(index);
    pendingScroll_.store(0);
    followOnBuild.store(true);
  }

  void requestScroll(const int deltaRows) {
    int pending = pendingScroll_.load();
    int next;
    do {
      const int64_t sum = static_cast<int64_t>(pending) + deltaRows;
      next = sum > 65535 ? 65535 : (sum < -65535 ? -65535 : static_cast<int>(sum));
    } while (!pendingScroll_.compare_exchange_weak(pending, next));
  }

  // Safe to read while a render is measuring a new page.
  int inputPageRows() const { return publishedPageRows_.load(); }

  void reset(const int selectedIndex = 0) {
    selected = selectedIndex;
    top = 0;
    visibleRows = 1;
    followOnBuild = true;
    drawnRows = 0;
    drawnCount = 0;
    followPending = false;
    rebuildNeeded = false;
    pendingScroll_.store(0);
    publishedPageRows_.store(1);
    layoutSelected_ = selectedIndex;
  }

  // Whether drawnRows describes a layout of `count` rows.
  bool trusts(const int count) const {
    return drawnRows > 0 && (drawnCount == 0 || drawnCount == count);
  }

  // Real rows per page once a build has run; the fixed-height estimate before.
  // Unchecked: it trusts the last measurement whatever list it was taken on.
  // Callers that know their current count should use pageRowsFor(count).
  int pageRows() const { return drawnRows > 0 ? drawnRows : visibleRows; }

  // pageRows() for a caller that knows the list's current count: a
  // measurement taken on a different count belongs to another row set, so it
  // falls back to the estimate. Every clamp and page step (here, in list(),
  // and in callers) must agree on this rule, or one of them scrolls to a
  // viewport another one refuses to draw. drawnCount 0 means "not recorded"
  // (a caller driving onListRendered() itself), which stays trusted.
  int pageRowsFor(const int count) const {
    return trusts(count) ? drawnRows : visibleRows;
  }


  // Layout feedback from list(): the effective top it drew from, how many
  // indexes fit, and whether the frame's selection was among them. A pending
  // follow advances top towards that selection and requests a rebuild. Stop
  // if even a viewport starting at the selection cannot fit the row.
  void onListRendered(const uint16_t effectiveTop, const int drawn,
                      const bool selectedDrawn) {
    top = effectiveTop;
    publishedPageRows_.store(drawn > 0 ? drawn : 1);
    if (drawn > 0)
      drawnRows = drawn;
    if (!followPending)
      return;
    if (selectedDrawn || layoutSelected_ < top || (drawn == 0 && layoutSelected_ == top)) {
      followPending = false;
      return;
    }
    int next = layoutSelected_ - (drawn > 0 ? drawn : 1) + 1;
    if (next <= top)
      next = top + 1;
    if (next > layoutSelected_)
      next = layoutSelected_;
    top = next;
    rebuildNeeded = true;
  }

  bool consumeRebuildNeeded() {
    const bool needed = rebuildNeeded;
    rebuildNeeded = false;
    return needed;
  }

  // Scroll the viewport by deltaRows, clamped to the valid range; the
  // selection stays put. Returns true when the viewport actually moved.
  // The clamp uses the measured page size when it is smaller than the
  // fixed-height estimate: with variable-height rows the true last page
  // holds fewer rows, and clamping to count - visibleRows would make the
  // tail rows unreachable (and fight onListRendered's follow correction).
  bool scrollBy(const int deltaRows, const int count) {
    const int pageSize =
        trusts(count) && drawnRows < visibleRows ? drawnRows : visibleRows;
    int maxTop = count - pageSize;
    if (maxTop < 0)
      maxTop = 0;
    int next = top + deltaRows;
    if (next > maxTop)
      next = maxTop;
    if (next < 0)
      next = 0;
    if (next == top)
      return false;
    top = next;
    return true;
  }

  // Pull the viewport the minimal amount so the selection is visible.
  // selected and top must both use absolute row indexes; callers that keep a
  // focus sentinel in selected must translate before calling follow().
  void follow(const int count) {
    layoutSelected_ = selected.load();
    followPending = true; // confirmed (or corrected) by onListRendered()
    const uint16_t rows =
        visibleRows > 0 ? static_cast<uint16_t>(visibleRows) : 1;
    top = listTopIndexFor(static_cast<int16_t>(layoutSelected_),
                          static_cast<uint16_t>(top < 0 ? 0 : top), rows,
                          static_cast<uint16_t>(count < 0 ? 0 : count));
  }

  // Screen-build sync: measure the rows that fit the band, apply the one-shot
  // follow-on-build, clamp the viewport, and write selection/viewport into the
  // props. Call from the screen builder right before list(). selectionOffset
  // maps an input ring to rows (1 reserves index 0 for a tab bar).
  void syncToProps(const Rect body, const int16_t rowHeight,
                   const int16_t rowGap, const int count, ListProps &props,
                   const int selectionOffset = 0) {
    const uint16_t rows = listVisibleRows(body, rowHeight, rowGap);
    visibleRows = rows > 0 ? rows : 1;
    const bool shouldFollow = followOnBuild.exchange(false);
    int logicalSelection = selected.load();
    layoutSelected_ = logicalSelection - selectionOffset;
    if (layoutSelected_ >= count) {
      layoutSelected_ = count - 1;
      // A reload may shrink the list. Clamp only the value we sampled, never
      // overwrite an input request that arrived while this build was starting.
      const int clamped = count > 0 ? layoutSelected_ + selectionOffset : 0;
      selected.compare_exchange_strong(logicalSelection, clamped);
    }
    if (shouldFollow) {
      followPending = layoutSelected_ >= 0;
      if (layoutSelected_ < 0) {
        top = 0;
      } else {
        top = listTopIndexFor(static_cast<int16_t>(layoutSelected_),
                             static_cast<uint16_t>(top < 0 ? 0 : top), rows,
                             static_cast<uint16_t>(count < 0 ? 0 : count));
      }
    }
    const int scroll = pendingScroll_.exchange(0);
    if (scroll != 0)
      followPending = false;
    scrollBy(scroll, count);
    props.selectedIndex = static_cast<int16_t>(layoutSelected_);
    props.topIndex = static_cast<uint16_t>(top);
    props.nav = this;
  }
private:
  std::atomic<int> pendingScroll_{0};
  std::atomic<int> publishedPageRows_{1};
  int layoutSelected_ = 0;
};

inline void drawListScrollIndicator(DrawTarget &target, const Rect rect,
                                    const uint32_t count,
                                    const uint32_t visible,
                                    const uint32_t top,
                                    const int16_t width = 3,
                                    const uint8_t side = 0,
                                    const int16_t inset = 0) {
  if (count <= visible || visible == 0 || width <= 0)
    return;

  const bool left = side == 1;
  const Rect track{left ? static_cast<int16_t>(rect.x + inset)
                        : static_cast<int16_t>(rect.right() - width - inset),
                   rect.y, width, rect.height};
  target.fill(track, Paint::dither(Color::LightGray));
  int16_t thumbH = static_cast<int16_t>(
      (static_cast<int32_t>(rect.height) * visible) / count);
  if (thumbH < 12)
    thumbH = 12;
  const uint32_t scrollRange = count - visible;
  const uint32_t clampedTop = top < scrollRange ? top : scrollRange;
  const int16_t thumbY = static_cast<int16_t>(
      track.y + (static_cast<int32_t>(track.height - thumbH) * clampedTop) /
                    scrollRange);
  target.fill(Rect{track.x, thumbY, track.width, thumbH},
              Paint::solid(Color::Black));
}

// Geometry shared by full rows and previews. Widths are resolved before
// wrapping, including both the trailing slot and optional balanced wrap cap.
struct ListRowLayout {
  int16_t height = 0;
  int16_t labelWidth = 0;
  int16_t labelHeight = 0;
  int16_t subtitleHeight = 0;
  int16_t valueWidth = 0;
  uint8_t labelLines = 1;
};

inline ListRowLayout measureListRow(const DrawTarget &target, AssetResolver *assets,
                                    const int16_t width, const ListProps &props,
                                    const ListItem &item) {
  ListRowLayout result;
  const int16_t rowH = props.rowHeight > 0 ? props.rowHeight : 36;
  const int16_t sidePad = props.sidePadding < 0 ? 8 : props.sidePadding;
  const int16_t labelLh = target.lineHeight(props.labelText.font);
  const BitmapRef icon = item.icon ? item.icon : resolveBitmap(assets, item.iconAsset);
  const int16_t iconSize = icon ? (props.iconSize > 0 ? props.iconSize : icon.width) : 0;
  const int16_t contentWidth = static_cast<int16_t>(width - sidePad * 2 -
                                                   (icon ? iconSize + props.textGap : 0));
  result.labelWidth = contentWidth;
  if (item.toggle) {
    result.valueWidth = props.toggleWidth < 18 ? 18 : props.toggleWidth;
  } else if (item.value) {
    result.valueWidth = target.measureText(props.valueText.font, item.value, props.valueText).width;
  }
  if (item.toggle || item.value)
    result.labelWidth = static_cast<int16_t>(result.labelWidth - result.valueWidth -
                                            props.valueInset - props.textGap);
  if (props.balanceWrappedLabelWithValue && props.labelText.maxLines > 1 &&
      (item.toggle || item.value) && item.label &&
      target.measureText(props.labelText.font, item.label, props.labelText).width > result.labelWidth) {
    const int16_t cap = static_cast<int16_t>(contentWidth * 3 / 5);
    if (result.labelWidth > cap) result.labelWidth = cap;
  }
  if (result.labelWidth < 0) result.labelWidth = 0;
  if (item.label && props.labelText.maxLines > 1 && labelLh > 0 && result.labelWidth > 0) {
    const int16_t lines = static_cast<int16_t>(measureWrappedText(
        target, item.label, props.labelText, result.labelWidth).height / labelLh);
    if (lines > 1) result.labelLines = static_cast<uint8_t>(lines);
  }
  const int16_t subLh = item.subtitle ? target.lineHeight(props.subtitleText.font) : 0;
  if (item.subtitle) {
    result.subtitleHeight = props.subtitleText.maxLines > 1
        ? measureWrappedText(target, item.subtitle, props.subtitleText, contentWidth).height : subLh;
  }
  const int16_t valueHeight = item.toggle ? (props.toggleHeight < 12 ? 12 : props.toggleHeight)
      : (item.value ? target.lineHeight(props.valueText.font) : 0);
  const int16_t labelHeight = static_cast<int16_t>(labelLh * result.labelLines);
  result.labelHeight = labelHeight > valueHeight ? labelHeight : valueHeight;
  int16_t needed = rowH;
  if (props.rowPaddingY >= 0) {
    const int16_t textHeight = static_cast<int16_t>(result.labelHeight + result.subtitleHeight);
    needed = static_cast<int16_t>((textHeight > iconSize ? textHeight : iconSize) + props.rowPaddingY * 2);
  } else if (item.subtitle) {
    const int16_t padding = static_cast<int16_t>(rowH - labelLh - subLh);
    needed = static_cast<int16_t>(result.labelHeight + result.subtitleHeight + (padding > 0 ? padding : 0));
  } else if (labelHeight > rowH) {
    needed = static_cast<int16_t>(rowH + labelLh * (result.labelLines - 1));
  }
  result.height = needed > rowH ? needed : rowH;
  const int16_t textHeight = static_cast<int16_t>(result.labelHeight + result.subtitleHeight);
  const int16_t contentHeight = iconSize > textHeight ? iconSize : textHeight;
  if (result.height < contentHeight) result.height = contentHeight;
  return result;
}

template <size_t MaxInteractions>
void list(Frame<MaxInteractions> &frame, Rect rect, const ListProps &props) {
  if ((!props.items && !props.rowProvider) || props.count == 0)
    return;
  const int16_t rowH = props.rowHeight > 0 ? props.rowHeight : 36;
  const int16_t rowGap = props.rowGap < 0 ? 0 : props.rowGap;
  const int16_t sidePad = props.sidePadding < 0 ? 8 : props.sidePadding;
  const int16_t scrollW =
      props.scrollIndicatorWidth < 0 ? 3 : props.scrollIndicatorWidth;
  const bool scrollLeft = props.scrollIndicatorSide == 1;
  const int16_t scrollInset =
      props.scrollIndicatorInset < 0 ? 0 : props.scrollIndicatorInset;
  const int16_t rowInset = props.rowInset < 0 ? 0 : props.rowInset;
  const uint16_t visible = listVisibleRows(rect, rowH, rowGap);
  // A nav-managed list reports how many indexes its last layout actually laid
  // out. Variable-height rows (wrapped labels/subtitles) routinely fit fewer
  // than the fixed-height `visible` estimate, so a list whose count is within
  // that estimate can still be clipped. Treating it as non-overflowing would
  // pin `top` at 0 below, discarding both swipe scrolling and the nav's
  // follow correction: the clipped tail rows would be unreachable by any
  // input, and the scroll indicator would never appear.
  // The measurement only speaks for the row set it was taken on, so a caller
  // that reloaded its data (same nav, new items) falls back to the estimate.
  const bool measured =
      props.nav != nullptr && props.nav->trusts(props.count);
  const bool measuredClip = measured && props.nav->drawnRows < props.count;
  const bool overflows = props.count > visible || measuredClip;
  // A nav-managed list always keeps the indicator's strip clear, whether or
  // not this pass draws one. Its overflow state is discovered by measuring,
  // so a strip that came and went would (a) leave the widened rows of an
  // earlier pass on screen, because callers repaint the rebuild pass over the
  // first one without clearing, and (b) measure the layout at a width the
  // list does not always draw with, which can report a clip that full-width
  // rows would not have had - and measuredClip then latches it.
  const bool reserveScrollStrip = overflows || props.nav != nullptr;
  uint16_t top = props.topIndex;
  if (top > props.count - 1)
    top = props.count - 1;
  // Nav-managed lists (props.nav set) clamp their own viewport with the
  // MEASURED page size (ListNav::scrollBy / onListRendered). The fixed-height
  // clamp below would undo the nav's follow correction when wrapped rows fit
  // fewer than `visible`: the nav advances top, this clamp pulls it back, and
  // the last row(s) can never be drawn (the rebuild loop oscillates instead
  // of converging).
  if (overflows && !props.nav && top > props.count - visible)
    top = static_cast<uint16_t>(props.count - visible);
  if (!overflows && !props.nav)
    top = 0;

  Rect rowArea = rect;
  // Width the reserved strip took from the rows, 0 when none was taken.
  int16_t stripCut = 0;
  if (rowInset > 0) {
    rowArea.x = static_cast<int16_t>(rowArea.x + rowInset);
    rowArea.width = static_cast<int16_t>(rowArea.width - rowInset * 2);
  }
  if (props.scrollIndicator && reserveScrollStrip && scrollW > 0) {
    // Rows only give up width when the row inset margin doesn't already
    // clear the track (plus its bezel inset) plus 2px of air.
    const int16_t needed = static_cast<int16_t>(scrollW + scrollInset + 2);
    if (rowInset < needed) {
      stripCut = static_cast<int16_t>(needed - rowInset);
      rowArea.width = static_cast<int16_t>(rowArea.width - stripCut);
      if (scrollLeft)
        rowArea.x = static_cast<int16_t>(rowArea.x + stripCut);
    }

  }

  // Cursor-based layout: section header rows are shorter than item rows, so
  // positions accumulate instead of multiplying a fixed stride.
  const int16_t headerLh = frame.target().lineHeight(props.headerText.font);
  const int16_t headerH = props.headerRowHeight > 0
                              ? props.headerRowHeight
                              : static_cast<int16_t>(headerLh + 4);
  int16_t cursorY = rowArea.y;
  uint16_t consumedIndexes = 0; // item AND header indexes laid out from top
  bool selectedDrawn = false;
  for (uint16_t i = top; i < props.count; ++i) {
    // Stop before reading the next window entry. The size/layout work below
    // dereferences `item`, so checking after it would require callers that
    // virtualize their data to provide one extra, otherwise out-of-window row.
    if (cursorY >= rowArea.bottom() ||
        (!props.rowProvider &&
         (i < props.itemsWindowFirst ||
          (props.itemsWindowCount > 0 &&
           i - props.itemsWindowFirst >= props.itemsWindowCount))))
      break;
    // One reused stack slot on the provider path; the array path keeps its
    // zero-copy reference.
    ListItem scratch;
    if (props.rowProvider)
      props.rowProvider(props.rowProviderCtx, i, scratch);
    const ListItem &item =
        props.rowProvider ? scratch : props.items[i - props.itemsWindowFirst];
    if (item.isHeader) {
      const int16_t pad = i != top ? props.sectionGap : 0;
      if (static_cast<int16_t>(cursorY + pad + headerH) > rowArea.bottom())
        break;
      ++consumedIndexes;
      cursorY = static_cast<int16_t>(cursorY + pad);
      Rect headerRow{static_cast<int16_t>(rowArea.x + sidePad), cursorY,
                     static_cast<int16_t>(rowArea.width - sidePad * 2),
                     headerLh};
      frame.target().text(headerRow, item.label, props.headerText);
      if (props.headerUnderline) {
        frame.target().fill(Rect{headerRow.x,
                                 static_cast<int16_t>(cursorY + headerLh + 2),
                                 headerRow.width, 1},
                            Paint::solid(props.headerText.color));
      }
      cursorY = static_cast<int16_t>(cursorY + headerH + rowGap);
      continue;
    }
    const ListRowLayout layout = measureListRow(frame.target(), frame.assets(),
                                                 rowArea.width, props, item);
    const int16_t itemH = layout.height;
    const int16_t subH = layout.subtitleHeight;
    const bool hasSectionHeading = item.sectionHeading != nullptr && item.sectionHeading[0] != '\0';
    const int16_t sectionPad = hasSectionHeading && i != top ? props.sectionGap : 0;
    const int16_t sectionH =
        hasSectionHeading ? static_cast<int16_t>(sectionPad + headerH + rowGap) : 0;
    const bool partial = cursorY + sectionH + itemH > rowArea.bottom();
    const Rect previousClip = frame.target().clipRect();
    if (partial) {
      // Clip the entire next section block, including its heading. Requiring
      // book text below that heading can hide all of the available preview.
      // Exclude decorative section padding from the minimum visible content.
      if (!props.partialTrailingRow ||
          rowArea.bottom() - cursorY - sectionPad < props.partialTrailingMinHeight)
        break;
      const int16_t left = rowArea.x > previousClip.x ? rowArea.x : previousClip.x;
      const int16_t upper = rowArea.y > previousClip.y ? rowArea.y : previousClip.y;
      const int16_t right = rowArea.right() < previousClip.right() ? rowArea.right() : previousClip.right();
      const int16_t bottom = rowArea.bottom() < previousClip.bottom() ? rowArea.bottom() : previousClip.bottom();
      if (!frame.target().setClipRect(Rect{left, upper, static_cast<int16_t>(right - left),
                                          static_cast<int16_t>(bottom - upper)}))
        break;
    }
    if (hasSectionHeading) {
      cursorY = static_cast<int16_t>(cursorY + sectionPad);
      Rect headerRow{static_cast<int16_t>(rowArea.x + sidePad), cursorY,
                     static_cast<int16_t>(rowArea.width - sidePad * 2),
                     headerLh};
      frame.target().text(headerRow, item.sectionHeading, props.headerText);
      if (props.headerUnderline) {
        frame.target().fill(Rect{headerRow.x,
                                 static_cast<int16_t>(cursorY + headerLh + 2),
                                 headerRow.width, 1},
                            Paint::solid(props.headerText.color));
      }
      cursorY = static_cast<int16_t>(cursorY + headerH + rowGap);
    }
    if (!partial) {
      ++consumedIndexes;
      if (props.selectedIndex == static_cast<int16_t>(i))
        selectedDrawn = true;
    }
    Rect row{rowArea.x, cursorY, rowArea.width, itemH};
    cursorY = static_cast<int16_t>(cursorY + itemH + rowGap);
    if (props.hugContents && item.label) {
      // Hug-content rows shrink to the label width plus padding so the
      // selection pill wraps the text instead of spanning the rect.
      const int16_t labelW =
          frame.target()
              .measureText(props.labelText.font, item.label, props.labelText)
              .width;
      const int16_t hugW = static_cast<int16_t>(labelW + sidePad * 2);
      if (hugW < row.width)
        row.width = hugW;
    }
    State state = partial ? static_cast<State>(item.state & ~(StateSelected | StateFocused | StateActive))
                          : item.state;
    if (!partial && props.selectedIndex == static_cast<int16_t>(i))
      state |= StateSelected;
    if (!item.enabled)
      state |= StateDisabled;
    if (!partial && props.action != NO_ACTION && item.enabled) {
      // item.enabled controls interactivity; a StateDisabled carried in
      // item.state is visual-only dimming and must not block touch routing
      // (findTouch skips disabled interactions).
      const State hitState = static_cast<State>(
          static_cast<int>(state) & ~static_cast<int>(StateDisabled));
      // A reserved strip with no indicator in it is empty screen, so the row
      // stays touchable across it: a nav list that fits would otherwise lose
      // a band at the edge that no longer snaps back (ensureMinTouchRect only
      // closes gaps under EDGE_SNAP_PX). Hugging rows keep their own width,
      // and so does a drag-masked row, whose position maps through this rect.
      Rect hitRow = row;
      if (!overflows && stripCut > 0 && !props.hugContents &&
          !acceptsInput(props.inputMask, InputDrag)) {
        hitRow.width = static_cast<int16_t>(hitRow.width + stripCut);
        if (scrollLeft)
          hitRow.x = static_cast<int16_t>(hitRow.x - stripCut);
      }
      Rect hit = ensureMinTouchRect(hitRow, frame.device().minTouchSize, frame.screen());
      // Vertical expansion must not turn the next row's preview into a hit on
      // this row. Horizontal expansion still covers the reserved scroll strip.
      if (props.partialTrailingRow) {
        hit.y = row.y;
        hit.height = row.height;
      }
      frame.hit(hit, props.action, item.actionValue, props.inputMask, hitState);
    }
    if (!partial)
      state = frame.stateFor(props.action, item.actionValue, state);
    StyleSet styles =
        props.rowStyles.unset() ? defaultListRowStyles() : props.rowStyles;
    if (props.rowRadius > 0)
      setStyleRadius(styles, props.rowRadius);
    const BoxStyle &style = styles.resolve(state);
    frame.target().fill(row, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(row, style.border, style.borderWidth, style.radius,
                            style.corners);
    }

    Rect content = row.inset(Insets{0, sidePad, 0, sidePad});

    // Slot layout (mirrors settingRow): the label owns a "title band" and the
    // icon and value align to it; the subtitle spans the full content width
    // under the band so it never collides with the value.
    TextStyle labelStyle =
        textStyleWithForeground(props.labelText, style.foreground);
    // The band holds every label line the height pre-pass measured (usually
    // one); the subtitle and the row's growth both follow it.
    const int16_t labelBlockH = layout.labelHeight;
    // subH carries over from the sizing pass: the subtitle's wrapped height,
    // or its single line height, or 0 without a subtitle.
    Rect band = content;
    if (item.subtitle) {
      int16_t bandTop = static_cast<int16_t>(
          content.y + (content.height - labelBlockH - subH) / 2);
      if (bandTop < content.y)
        bandTop = content.y;
      band = Rect{content.x, bandTop, content.width, labelBlockH};
    }

    const BitmapRef icon =
        item.icon ? item.icon : resolveBitmap(frame.assets(), item.iconAsset);
    if (icon) {
      const int16_t iconSize = props.iconSize > 0
                                   ? props.iconSize
                                   : static_cast<int16_t>(icon.width);
      // Centered on the full row content, not the title band: with a subtitle
      // the icon belongs to the label+subtitle block as a whole. RTL mirrors
      // the icon to the row's trailing (right) edge, matching
      // BaseTheme::drawList's "title anchored right" convention, and shrinks
      // content from that side instead so the label/value slots below
      // reflow to the left of it.
      const int16_t iconX = props.rtl
          ? static_cast<int16_t>(content.x + content.width - iconSize)
          : content.x;
      Rect iconRect{
          iconX,
          static_cast<int16_t>(content.y + (content.height - iconSize) / 2),
          iconSize, iconSize};
      frame.target().bitmap(iconRect, icon, BitmapMode::Contain,
                            style.foreground);
      if (!props.rtl)
        content.x = static_cast<int16_t>(content.x + iconSize + props.textGap);
      content.width =
          static_cast<int16_t>(content.width - iconSize - props.textGap);
      band.x = content.x;
      band.width = content.width;
    }

    // RTL: the value/toggle slot moves to the band's leading (left) edge and
    // the label fills whatever remains on the trailing (right) edge, next to
    // the icon -- computed below via labelX once the slot width is known.
    int16_t labelX = band.x;
    int16_t availW = band.width;
    if (item.toggle) {
      const int16_t togW = props.toggleWidth < 18 ? 18 : props.toggleWidth;
      const int16_t togH = props.toggleHeight < 12 ? 12 : props.toggleHeight;
      const int16_t togX = props.rtl
          ? static_cast<int16_t>(band.x + props.valueInset)
          : static_cast<int16_t>(band.x + band.width - togW - props.valueInset);
      Rect toggleRect{
          togX,
          static_cast<int16_t>(band.y + (band.height - togH) / 2), togW, togH};
      // The switch draws in row-foreground ink with the foreground's opposite
      // as "paper", so it inverts along with the row when selected.
      const Paint fg = style.foreground;
      const bool fgWhite =
          fg.kind == PaintKind::Solid && fg.color == Color::White;
      const Paint paper = Paint::solid(fgWhite ? Color::Black : Color::White);
      const uint8_t trackRadius = static_cast<uint8_t>(
          props.toggleRadius > togH / 2 ? togH / 2 : props.toggleRadius);
      frame.target().fill(toggleRect, item.toggleChecked ? fg : paper,
                          trackRadius);
      if (props.toggleBorderWidth > 0) {
        frame.target().stroke(toggleRect, fg, props.toggleBorderWidth,
                              trackRadius);
      }
      const int16_t knobInset =
          props.toggleKnobInset < 0 ? 0 : props.toggleKnobInset;
      const int16_t knobH = static_cast<int16_t>(togH - knobInset * 2);
      if (knobH > 0) {
        Rect knob{
            static_cast<int16_t>(item.toggleChecked
                                     ? toggleRect.right() - knobInset - knobH
                                     : toggleRect.x + knobInset),
            static_cast<int16_t>(toggleRect.y + knobInset), knobH, knobH};
        const uint8_t knobRadius = static_cast<uint8_t>(
            props.toggleKnobRadius > knobH / 2 ? knobH / 2
                                               : props.toggleKnobRadius);
        frame.target().fill(knob, item.toggleChecked ? paper : fg, knobRadius);
      }
      availW = static_cast<int16_t>(availW - togW - props.valueInset -
                                    props.textGap);
      if (props.rtl)
        labelX = static_cast<int16_t>(band.x + band.width - availW);
    } else if (item.value) {
      TextStyle valueStyle =
          textStyleWithForeground(props.valueText, style.foreground);
      valueStyle.align = props.rtl ? TextAlign::Left : TextAlign::Right;
      const int16_t valueW = layout.valueWidth;
      const int16_t valueX = props.rtl
          ? static_cast<int16_t>(band.x + props.valueInset)
          : static_cast<int16_t>(band.x + availW - valueW - props.valueInset);
      Rect valueRect{valueX, band.y, valueW, band.height};
      frame.target().text(valueRect, item.value, valueStyle);
      availW = static_cast<int16_t>(availW - valueW - props.valueInset -
                                    props.textGap);
      if (props.rtl)
        labelX = static_cast<int16_t>(band.x + band.width - availW);
    }

    availW = layout.labelWidth;
    if (props.rtl)
      labelX = static_cast<int16_t>(band.right() - availW);

    if (props.rtl && !props.centerSingleLine)
      labelStyle.align = TextAlign::Right;
    if (item.subtitle) {
      frame.target().text(Rect{labelX, band.y, availW, band.height}, item.label,
                          labelStyle);
      frame.target().text(
          Rect{content.x, static_cast<int16_t>(band.y + band.height),
               content.width, subH},
          item.subtitle,
          textStyleWithForeground(props.subtitleText, style.foreground));
    } else {
      if (props.centerSingleLine)
        labelStyle.align = TextAlign::Center;
      frame.target().text(Rect{labelX, band.y, availW, band.height}, item.label,
                          labelStyle);
    }

    if (!partial && props.selectedIndex == static_cast<int16_t>(i) &&
        props.selectionMarker != SelectionMarker::None) {
      if (props.selectionMarker == SelectionMarker::Underline) {
        // RTL mirrors which edge carries markerInset's extra gap, matching
        // the label's own trailing/leading swap above.
        const int16_t underlineX = props.rtl
            ? static_cast<int16_t>(row.x + sidePad)
            : static_cast<int16_t>(row.x + sidePad + props.markerInset);
        frame.target().fill(
            Rect{underlineX,
                 static_cast<int16_t>(row.bottom() - props.markerThickness),
                 static_cast<int16_t>(row.width - sidePad * 2 -
                                      props.markerInset),
                 props.markerThickness},
            props.markerPaint);
      } else if (props.selectionMarker == SelectionMarker::Bitmap) {
        const BitmapRef marker =
            props.markerBitmap
                ? props.markerBitmap
                : resolveBitmap(frame.assets(), props.markerAsset);
        if (marker) {
          frame.target().bitmap(
              Rect{static_cast<int16_t>(row.x + props.markerInset),
                   static_cast<int16_t>(row.y +
                                        (row.height - marker.height) / 2),
                   static_cast<int16_t>(marker.width),
                   static_cast<int16_t>(marker.height)},
              marker, BitmapMode::Contain, props.markerPaint);
        }
      } else {
        // 12x18 right-pointing triangle, vertically centered — the v1 theme
        // Triangle selection marker geometry.
        const int16_t tx = static_cast<int16_t>(row.x + props.markerInset);
        const int16_t cy = static_cast<int16_t>(row.y + row.height / 2);
        frame.target().triangle(Point{tx, static_cast<int16_t>(cy - 9)},
                                Point{tx, static_cast<int16_t>(cy + 9)},
                                Point{static_cast<int16_t>(tx + 12), cy},
                                props.markerPaint);
      }
    }
    // A preview uses the same row geometry and paint path, clipped at the fold.
    if (partial) {
      frame.target().setClipRect(previousClip);
      break;
    }
  }

  if (props.scrollIndicator && consumedIndexes > 0 &&
      (top > 0 || consumedIndexes < props.count)) {
    drawListScrollIndicator(frame.target(), rect, props.count, consumedIndexes, top,
                            scrollW, scrollLeft ? 1 : 0, scrollInset);
  }

  if (props.nav) {
    props.nav->onListRendered(top, consumedIndexes, selectedDrawn);
    if (consumedIndexes > 0)
      props.nav->drawnCount = props.count;
  }

}

} // namespace ui
} // namespace freeink
