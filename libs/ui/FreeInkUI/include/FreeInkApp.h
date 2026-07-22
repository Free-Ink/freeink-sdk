#pragma once

// FreeInkApp: small runtime + screen builder for FreeInkUI.
//
// This is the ergonomic layer over the immediate-mode primitives in FreeInkUI.h:
// it owns the interaction buffer, dispatches semantic actions to callbacks, and
// gives screens a simple top-to-bottom builder API. It stays freestanding and
// allocation-free, so firmware can use it directly and design-time tools can
// generate ordinary C++ against it.

#include <FreeInkUI.h>

namespace freeink {
namespace ui {

enum class RefreshHint : uint8_t {
  None,
  Fast,
  Full,
  Clean,
};

enum class LayoutAnchor : uint8_t {
  Top,
  Bottom,
};

struct FooterAction {
  const char* label = nullptr;
  ActionId action = NO_ACTION;
  int16_t value = 0;
  State state = StateNormal;
  bool enabled = true;
};

struct FooterProps {
  const FooterAction* actions = nullptr;
  uint8_t count = 0;
  int16_t sidePadding = 8;
  int16_t gap = 4;
  uint8_t buttonBorderEdges = EdgesNone;
  // Style for passive slots: a FooterAction with NO_ACTION renders as plain
  // text in its slot instead of a button — version lines, status notes.
  // font 0 = the theme's small text; align is honored either way.
  TextStyle passiveText{};
};

template <size_t MaxInteractions>
class Screen {
 public:
  using FrameType = Frame<MaxInteractions>;

  Screen(FrameType& frame, const ThemeTokens& theme) : frame_(frame), theme_(theme), content_(frame.safeRect()) {}

  FrameType& frame() { return frame_; }
  DrawTarget& target() { return frame_.target(); }
  const DeviceContext& device() const { return frame_.device(); }
  const ThemeTokens& theme() const { return theme_; }
  Rect contentRect() const { return content_; }

  void setContentMargin(Insets margin) { content_ = insetClamped(frame_.safeRect(), margin); }
  void insetContent(Insets margin) { content_ = insetClamped(content_, margin); }

  Rect takeTop(int16_t height, int16_t gap = 0) {
    if (height < 0) height = 0;
    if (height > content_.height) height = content_.height;
    Rect rect{content_.x, content_.y, content_.width, height};
    const int16_t consumed = static_cast<int16_t>(height + (gap > 0 ? gap : 0));
    content_.y = static_cast<int16_t>(content_.y + consumed);
    content_.height = static_cast<int16_t>(content_.height > consumed ? content_.height - consumed : 0);
    return rect;
  }

  Rect takeBottom(int16_t height, int16_t gap = 0) {
    if (height < 0) height = 0;
    if (height > content_.height) height = content_.height;
    Rect rect{content_.x, static_cast<int16_t>(content_.bottom() - height), content_.width, height};
    const int16_t consumed = static_cast<int16_t>(height + (gap > 0 ? gap : 0));
    content_.height = static_cast<int16_t>(content_.height > consumed ? content_.height - consumed : 0);
    return rect;
  }

  Rect take(LayoutAnchor anchor, int16_t height, int16_t gap = 0) {
    return anchor == LayoutAnchor::Bottom ? takeBottom(height, gap) : takeTop(height, gap);
  }

  void spacer(int16_t height, LayoutAnchor anchor = LayoutAnchor::Top) { take(anchor, height); }

  Rect body() const { return content_; }

 private:
  // Row band + gap for the row builders: theme cadence by default; a custom
  // height scales the inter-row gap proportionally so taller rows also get
  // more air between them.
  Rect takeRow(LayoutAnchor anchor, int16_t height) {
    if (height <= 0) return take(anchor, theme_.rowHeight, theme_.spaceSm);
    const int16_t gap = static_cast<int16_t>(theme_.spaceMd * height / theme_.rowHeight);
    return take(anchor, height, gap);
  }

 public:

  void header(const char* title, const char* subtitle = nullptr, const char* rightLabel = nullptr,
              LayoutAnchor anchor = LayoutAnchor::Top) {
    HeaderProps props;
    props.title = title;
    props.subtitle = subtitle;
    props.rightLabel = rightLabel;
    props.borderEdges = EdgeBottom;
    header(props, anchor);
  }

  void header(const HeaderProps& props, LayoutAnchor anchor = LayoutAnchor::Top) {
    HeaderProps themed = props;
    if (themed.titleText.font == 0) {
      themed.titleText = theme_.titleText;
      themed.titleText.align = theme_.headerTitleAlign;
    }
    if (themed.subtitleText.font == 0) themed.subtitleText = theme_.smallText;
    if (themed.styles.unset()) themed.styles = theme_.popup;
    if (themed.leadingStyles.unset()) themed.leadingStyles = theme_.button;
    if (themed.trailingStyles.unset()) themed.trailingStyles = plainStyles(Paint::solid(Color::Black));
    if (themed.trailingText.font == 0) themed.trailingText = theme_.bodyText;
    if (themed.sidePadding < 0) themed.sidePadding = theme_.headerSidePadding;
    // Divider: the theme's headerUnderline sets the rule thickness when the
    // popup style ships without a border of its own (0 = no rule).
    if (themed.styles.normal.border.kind == PaintKind::None && theme_.headerUnderline > 0) {
      themed.styles.normal.border = Paint::solid(Color::Black);
      themed.styles.normal.borderWidth = theme_.headerUnderline;
    }
    themed.minTouchSize = theme_.minTouchSize;
    ui::header(frame_, take(anchor, theme_.headerHeight), themed);
  }

  // Sub-screen chrome: leading back button + centered title, with an optional
  // right-aligned label (a live clock, a count). borderEdges is HeaderProps'
  // divider setting: EdgeBottom (default) draws the rule, EdgesNone drops it.
  // trailingLabel/trailingAction put an action button (a "Save"/"Done") on the
  // right edge instead of the passive rightLabel — set one or the other.
  void navHeader(const char* title, ActionId backAction, BitmapRef backIcon,
                 const char* rightLabel = nullptr, uint8_t borderEdges = EdgeBottom,
                 const char* trailingLabel = nullptr, ActionId trailingAction = NO_ACTION,
                 bool trailingEnabled = true, LayoutAnchor anchor = LayoutAnchor::Top) {
    HeaderProps props;
    props.title = title;
    props.centered = true;
    props.borderEdges = borderEdges;
    props.leadingIcon = backIcon;
    props.leadingAction = backAction;
    props.leadingRadius = 8;
    props.rightLabel = rightLabel;
    props.trailingLabel = trailingLabel;
    props.trailingAction = trailingAction;
    props.trailingEnabled = trailingEnabled;
    props.trailingRadius = 8;
    header(props, anchor);
  }

  void status(const StatusBarProps& props, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::statusBar(frame_, take(anchor, theme_.headerHeight), props);
  }

  void button(const char* label, ActionId action, int16_t value = 0, State state = StateNormal,
              LayoutAnchor anchor = LayoutAnchor::Top) {
    ButtonProps props;
    props.label = label;
    props.action = action;
    props.value = value;
    props.state = state;
    props.text = theme_.bodyText;
    props.styles = theme_.button;
    props.minTouchSize = theme_.minTouchSize;
    ui::button(frame_, take(anchor, theme_.rowHeight, theme_.spaceSm), props);
  }

  void button(const ButtonProps& props, LayoutAnchor anchor = LayoutAnchor::Top) {
    ButtonProps themed = props;
    if (themed.text.font == 0) themed.text = theme_.bodyText;
    if (themed.styles.unset()) themed.styles = theme_.button;
    themed.minTouchSize = theme_.minTouchSize;
    ui::button(frame_, take(anchor, theme_.rowHeight, theme_.spaceSm), themed);
  }

  // Themed button at an explicit rect, for layouts the row cadence can't
  // express (bottom action bars, centered blocks). Does not consume body
  // space — pair with takeTop/takeBottom when the band should be reserved.
  void button(const ButtonProps& props, Rect rect) {
    ButtonProps themed = props;
    if (themed.text.font == 0) themed.text = theme_.bodyText;
    if (themed.styles.unset()) themed.styles = theme_.button;
    themed.minTouchSize = theme_.minTouchSize;
    ui::button(frame_, rect, themed);
  }

  void list(const ListItem* items, uint16_t count, int16_t selectedIndex, ActionId action, uint16_t topIndex = 0,
            int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ListProps props;
    props.items = items;
    props.count = count;
    props.selectedIndex = selectedIndex;
    props.topIndex = topIndex;
    props.action = action;
    list(props, height, anchor);
  }

  void list(const ListProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ListProps themed = props;
    if (themed.labelText.font == 0) themed.labelText = theme_.bodyText;
    if (themed.subtitleText.font == 0) themed.subtitleText = theme_.smallText;
    if (themed.valueText.font == 0) themed.valueText = theme_.smallText;
    if (themed.headerText.font == 0) themed.headerText = theme_.smallText;
    if (themed.rowStyles.unset()) {
      // Expand the theme's selection style over its base row styles; explicit
      // rowStyles or a caller-set marker win.
      StyleSet styles = theme_.listRow.unset() ? defaultListRowStyles() : theme_.listRow;
      switch (theme_.listSelectionStyle) {
        case SelectionStyle::LightPill:
          styles.selected.background = Paint::dither(Color::LightGray);
          styles.selected.foreground = Paint::solid(Color::Black);
          styles.active = styles.selected;
          break;
        case SelectionStyle::Underline:
        case SelectionStyle::Triangle:
          styles.selected = styles.normal;  // the marker shows the selection
          if (themed.selectionMarker == SelectionMarker::None) {
            themed.selectionMarker = theme_.listSelectionStyle == SelectionStyle::Underline
                                         ? SelectionMarker::Underline
                                         : SelectionMarker::Triangle;
          }
          break;
        case SelectionStyle::InvertFill:
        default:
          break;
      }
      themed.rowStyles = styles;
    }
    if (themed.rowHeight <= 0) themed.rowHeight = theme_.rowHeight;
    if (themed.rowGap < 0) themed.rowGap = theme_.listRowGap;
    if (themed.rowRadius == 0) themed.rowRadius = theme_.listRowRadius;
    if (themed.sidePadding < 0) themed.sidePadding = theme_.listSidePadding;
    if (themed.scrollIndicatorWidth < 0) themed.scrollIndicatorWidth = theme_.listScrollWidth;
    if (themed.scrollIndicatorSide == 0xFF) themed.scrollIndicatorSide = theme_.listScrollSide;
    Rect rect = height > 0 ? take(anchor, height) : content_;
    if (theme_.listInset > 0) rect = rect.inset(Insets{0, theme_.listInset, 0, theme_.listInset});
    ui::list(frame_, rect, themed);
  }

  void checkbox(const CheckboxProps& props, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::checkbox(frame_, take(anchor, theme_.rowHeight, theme_.spaceSm), props);
  }

  void slider(const SliderProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::slider(frame_, take(anchor, height > 0 ? height : theme_.rowHeight, theme_.spaceSm), props);
  }

  void dropdown(const DropdownProps& props, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::dropdown(frame_, take(anchor, theme_.rowHeight, theme_.spaceSm), props);
  }

  void table(const TableProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::table(frame_, take(anchor, height > 0 ? height : static_cast<int16_t>(props.rowHeight * props.rows),
                           theme_.spaceSm),
              props);
  }

  // Rows default to the theme's row cadence; pass a height for larger rows
  // (a roomy settings list) — the gap grows with the row.
  void settingRow(const SettingRowProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::settingRow(frame_, takeRow(anchor, height), props);
  }

  void toggleRow(const ToggleRowProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::toggleRow(frame_, takeRow(anchor, height), props);
  }

  void stepperRow(const StepperRowProps& props, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::stepperRow(frame_, take(anchor, theme_.rowHeight, theme_.spaceSm), props);
  }

  void radioGroup(const RadioGroupProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::radioGroup(frame_, take(anchor, height > 0 ? height : theme_.rowHeight, theme_.spaceSm), props);
  }

  void qwertyKeyboard(const QwertyKeyboardProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::qwertyKeyboard(frame_, take(anchor, height > 0 ? height : defaultKeyboardHeight()), props);
  }

  void keyboard(const KeyboardProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::keyboard(frame_, take(anchor, height > 0 ? height : defaultKeyboardHeight()), props);
  }

  void bookCard(const BookCardProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    ui::bookCard(frame_, take(anchor, height > 0 ? height : static_cast<int16_t>(theme_.rowHeight * 2)), props);
  }

  // Multi-line writing canvas. Fills the remaining body by default; pass a height
  // to reserve a band. Text defaults to the theme body style.
  void textArea(const TextAreaProps& props, int16_t height = 0, LayoutAnchor anchor = LayoutAnchor::Top) {
    TextAreaProps themed = props;
    if (themed.style.font == 0) themed.style = theme_.bodyText;
    ui::textArea(frame_, height > 0 ? take(anchor, height) : content_, themed);
  }

  // A one-line message centered in the remaining body — empty states,
  // "Scanning…" notes — instead of hand-rolling lineHeight + centeredRect.
  void centeredText(const char* message, TextStyle style) {
    if (!message) return;
    style.align = TextAlign::Center;
    const int16_t lh = frame_.target().lineHeight(style.font);
    frame_.target().text(centeredRect(content_, Size{content_.width, lh}), message, style);
  }
  void centeredText(const char* message) { centeredText(message, theme_.smallText); }

  void footer(const FooterAction* actions, uint8_t count, LayoutAnchor anchor = LayoutAnchor::Bottom) {
    FooterProps props;
    props.actions = actions;
    props.count = count;
    footer(props, anchor);
  }

  void footer(const FooterProps& footer, LayoutAnchor anchor = LayoutAnchor::Bottom) {
    Rect rect = take(anchor, theme_.footerHeight);
    if (!footer.actions || footer.count == 0 || rect.empty()) return;
    const int16_t sidePadding = footer.sidePadding < 0 ? 0 : footer.sidePadding;
    const int16_t gap = footer.gap < 0 ? 0 : footer.gap;
    Rect content = insetClamped(rect, Insets{0, sidePadding, 0, sidePadding});
    if (content.empty()) return;
    const int16_t totalGap = static_cast<int16_t>(footer.count > 1 ? (footer.count - 1) * gap : 0);
    const int16_t slotW = static_cast<int16_t>((content.width - totalGap) / footer.count);
    int16_t x = content.x;
    for (uint8_t i = 0; i < footer.count; ++i) {
      Rect slot{x, content.y, i == footer.count - 1 ? static_cast<int16_t>(content.right() - x) : slotW,
                content.height};
      if (footer.actions[i].action == NO_ACTION) {
        // Passive slot: plain text, no button chrome, no hit rect.
        TextStyle style = footer.passiveText;
        if (style.font == 0) {
          const TextAlign align = style.align;
          style = theme_.smallText;
          style.align = align;
        }
        if (footer.actions[i].label) frame_.target().text(slot, footer.actions[i].label, style);
        x = static_cast<int16_t>(x + slot.width + gap);
        continue;
      }
      ButtonProps props;
      props.label = footer.actions[i].label;
      props.action = footer.actions[i].action;
      props.value = footer.actions[i].value;
      props.state = footer.actions[i].state;
      props.enabled = footer.actions[i].enabled;
      props.text = theme_.bodyText;
      props.styles = theme_.button;
      if (footer.buttonBorderEdges != EdgesNone) {
        props.styles.normal.border = Paint::solid(Color::Black);
        props.styles.normal.borderWidth = 1;
        props.styles.selected.border = Paint::solid(Color::Black);
        props.styles.selected.borderWidth = 1;
        props.styles.focused.border = Paint::solid(Color::Black);
        props.styles.focused.borderWidth = 1;
        props.styles.active.border = Paint::solid(Color::Black);
        props.styles.active.borderWidth = 1;
        props.styles.disabled.border = Paint::solid(Color::Black);
        props.styles.disabled.borderWidth = 1;
      }
      props.minTouchSize = theme_.minTouchSize;
      props.borderEdges = footer.buttonBorderEdges;
      ui::button(frame_, slot, props);
      x = static_cast<int16_t>(x + slot.width + gap);
    }
  }

  void popup(const char* message) {
    PopupProps props;
    props.message = message;
    props.text = theme_.bodyText;
    props.styles = theme_.popup;
    popup(props);
  }

  void popup(const PopupProps& props) {
    PopupProps themed = props;
    if (themed.text.font == 0) themed.text = theme_.bodyText;
    if (themed.styles.unset()) themed.styles = theme_.popup;
    const Rect bounds = frame_.safeRect();
    const int16_t maxW = themed.maxWidth > 0 ? themed.maxWidth : static_cast<int16_t>(bounds.width * 3 / 4);
    const int16_t contentW = static_cast<int16_t>(maxW - themed.padding.left - themed.padding.right);
    const Size textSize = measureWrappedText(frame_.target(), themed.message, themed.text, contentW > 1 ? contentW : 1);
    int16_t height = static_cast<int16_t>(textSize.height + themed.padding.top + themed.padding.bottom);
    if (themed.showProgress) {
      const int16_t barH = themed.progressHeight > 0 ? themed.progressHeight : 4;
      height = static_cast<int16_t>(height + barH + theme_.spaceSm);
    }
    const Size panelSize{static_cast<int16_t>(textSize.width + themed.padding.left + themed.padding.right), height};
    ui::popup(frame_, centeredRect(bounds, panelSize), themed);
  }

  void dialog(const OptionDialogProps& props, int16_t width = 0) {
    if (width <= 0) width = static_cast<int16_t>(frame_.safeRect().width * 4 / 5);
    const int16_t height = optionDialogHeight(frame_.target(), props, width);
    ui::optionDialog(frame_, centeredRect(frame_.safeRect(), Size{width, height}), props);
  }

 private:
  int16_t defaultKeyboardHeight() const {
    const Rect safe = frame_.safeRect();
    int16_t height = static_cast<int16_t>(theme_.rowHeight * 3 + theme_.spaceSm * 3);
    const int16_t widthBased = static_cast<int16_t>(safe.width / 4);
    if (widthBased > height) height = widthBased;
    const int16_t maxHeight = static_cast<int16_t>(safe.height * 45 / 100);
    if (height > maxHeight) height = maxHeight;
    if (height > safe.height) height = safe.height;
    return height < 1 ? 1 : height;
  }

  static Rect insetClamped(Rect rect, Insets margin) {
    int32_t x = static_cast<int32_t>(rect.x) + margin.left;
    int32_t y = static_cast<int32_t>(rect.y) + margin.top;
    int32_t right = static_cast<int32_t>(rect.right()) - margin.right;
    int32_t bottom = static_cast<int32_t>(rect.bottom()) - margin.bottom;
    if (right < x) right = x;
    if (bottom < y) bottom = y;
    return Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(right - x),
                static_cast<int16_t>(bottom - y)};
  }

  FrameType& frame_;
  const ThemeTokens& theme_;
  Rect content_{};
};

template <size_t MaxInteractions = 32, size_t MaxHandlers = 16>
class FreeInkApp {
 public:
  using ScreenType = Screen<MaxInteractions>;
  using ScreenFn = void (*)(ScreenType& screen, void* user);
  using ActionHandler = void (*)(const ActionEvent& event, void* user);

  FreeInkApp(DrawTarget& target, DeviceContext device, AssetResolver* assets = nullptr)
      : target_(target), device_(device), assets_(assets) {
    // Size the default metric tokens to the target's actual body font: the
    // static 44px defaults fit ~18px UI fonts but clip label+subtitle rows
    // with larger fonts (the bundled Noto Sans is 34px/line). setTheme()
    // still replaces everything.
    theme_ = themeTokensForLineHeight(target.lineHeight(theme_.bodyText.font));
  }

  void setDevice(DeviceContext device) { device_ = device; }
  const DeviceContext& device() const { return device_; }

  void setTheme(const ThemeTokens& theme) { theme_ = theme; }
  const ThemeTokens& theme() const { return theme_; }

  void setAssets(AssetResolver* assets) { assets_ = assets; }
  AssetResolver* assets() const { return assets_; }

  // Switch the active screen. `hint` is the refresh requested for the redraw:
  // Full (default) gives a clean full refresh — good for the first paint or to
  // clear ghosting — but on e-paper a full refresh on every screen change is
  // slow and, on some panels, prone to a one-frame lag. Pass RefreshHint::Fast
  // for snappy partial-refresh transitions between screens.
  void setScreen(ScreenFn screen, void* user = nullptr, RefreshHint hint = RefreshHint::Full) {
    screen_ = screen;
    screenUser_ = user;
    interactions_.setFocusedIndex(-1);
    invalidate(hint);
  }

  void on(ActionId action, ActionHandler handler, void* user = nullptr) {
    for (size_t i = 0; i < handlerCount_; ++i) {
      if (handlers_[i].action == action) {
        handlers_[i].handler = handler;
        handlers_[i].user = user;
        return;
      }
    }
    if (handlerCount_ >= MaxHandlers) {
      handlerOverflowed_ = true;
      return;
    }
    handlers_[handlerCount_++] = Handler{action, handler, user};
  }

  void invalidate(RefreshHint hint = RefreshHint::Fast) {
    invalidated_ = true;
    if (static_cast<uint8_t>(hint) > static_cast<uint8_t>(refreshHint_)) refreshHint_ = hint;
  }

  // Screen-transition redraw with a ghosting policy: fast partial refreshes
  // keep transitions snappy, with a full refresh every Nth transition to clear
  // accumulated ghosting (see setTransitionFullEvery).
  void invalidateTransition() {
    const bool full = transitionFullEvery_ > 0 && ++transitions_ % transitionFullEvery_ == 0;
    invalidate(full ? RefreshHint::Full : RefreshHint::Fast);
  }

  // How often invalidateTransition() promotes to a full refresh (0 = never).
  void setTransitionFullEvery(uint8_t n) { transitionFullEvery_ = n; }

  // Fill the whole target with this color before each paint. Frames do not
  // clear the target on their own — without this (or an app-side clear) the
  // previous screen shows through wherever the new one doesn't draw.
  void setClearColor(Color color) {
    clearPaint_ = Paint::solid(color);
    clearBeforePaint_ = true;
  }

  bool invalidated() const { return invalidated_; }
  RefreshHint refreshHint() const { return refreshHint_; }
  RefreshHint lastRenderRefreshHint() const { return lastRenderHint_; }
  bool interactionOverflowed() const { return interactions_.overflowed(); }
  bool handlerOverflowed() const { return handlerOverflowed_; }
  ActionEvent lastEvent() const { return lastEvent_; }

  ActionEvent render(const InputSnapshot& input = InputSnapshot{}) {
    lastRenderHint_ = refreshHint_;
    invalidated_ = false;
    refreshHint_ = RefreshHint::None;

    // Tap flash bookkeeping: a flash armed by the previous frame's dispatch
    // stays visible for exactly the repaint that shows the tap's result, then
    // clears (the panel keeps showing it until the next real refresh).
    if (flashTicks_ > 0 && --flashTicks_ == 0) interactions_.clearFlash();
    flashSuppressed_ = false;

    if (clearBeforePaint_) target_.fill(device_.screen(), clearPaint_);

    Frame<MaxInteractions> frame(target_, device_, input, interactions_, assets_);
    ScreenType screen(frame, theme_);
    if (screen_) screen_(screen, screenUser_);
    lastEvent_ = frame.finish();
    if (lastEvent_) {
      dispatch(lastEvent_);
      invalidate(RefreshHint::Fast);
      // Tap feedback: paint the tapped element with its focused style in the
      // same refresh that shows the tap's result — visual confirmation with
      // no extra panel refresh. Skipped when the handler called
      // clearTapFlash() (screen transitions).
      if (!flashSuppressed_) {
        interactions_.setFlash(lastEvent_.action, lastEvent_.value);
        flashTicks_ = 2;  // this frame + the invalidated repaint that gets pushed
      }
    }
    return lastEvent_;
  }

  // Route input against the interactions the LAST rendered frame registered
  // and dispatch the resulting action — without drawing. A full render costs
  // real time (~100-200 ms on large panels), so firmware that buffers input
  // (e.g. InputManager::beginAsync tap queues) drains the whole burst through
  // route() and repaints once. Only valid while the screen content still
  // matches the last render (a dispatched handler that navigates makes the
  // remaining queued taps route against the old screen — same as taps landing
  // just before a transition).
  ActionEvent route(const InputSnapshot& input) {
    lastEvent_ = interactions_.route(input);
    if (lastEvent_) {
      flashSuppressed_ = false;
      dispatch(lastEvent_);
      invalidate(RefreshHint::Fast);
      if (!flashSuppressed_) {
        interactions_.setFlash(lastEvent_.action, lastEvent_.value);
        flashTicks_ = 2;
      }
    }
    return lastEvent_;
  }

  // True while a held touch sits on an interactive element (the routing marks
  // it active and it renders with its StateActive style).
  bool touchActive() const { return interactions_.activeIndex() >= 0; }

  // Drop a pending tap flash. Call from handlers that navigate to a different
  // screen: the tapped element no longer exists there, and an element on the
  // NEW screen with the same action/value would inherit the gray instead
  // (e.g. a back button graying the next screen's back button).
  void clearTapFlash() {
    interactions_.clearFlash();
    flashTicks_ = 0;
    flashSuppressed_ = true;  // also skip the arm that follows this dispatch
  }

 private:
  struct Handler {
    ActionId action = NO_ACTION;
    ActionHandler handler = nullptr;
    void* user = nullptr;
  };

  void dispatch(const ActionEvent& event) {
    for (size_t i = 0; i < handlerCount_; ++i) {
      if (handlers_[i].action == event.action && handlers_[i].handler) {
        handlers_[i].handler(event, handlers_[i].user);
      }
    }
  }

  DrawTarget& target_;
  DeviceContext device_{};
  ThemeTokens theme_ = defaultThemeTokens();
  AssetResolver* assets_ = nullptr;
  InteractionBuffer<MaxInteractions> interactions_{};
  ScreenFn screen_ = nullptr;
  void* screenUser_ = nullptr;
  Handler handlers_[MaxHandlers]{};
  size_t handlerCount_ = 0;
  bool handlerOverflowed_ = false;
  bool invalidated_ = true;
  RefreshHint refreshHint_ = RefreshHint::Full;
  RefreshHint lastRenderHint_ = RefreshHint::None;
  ActionEvent lastEvent_{};
  uint8_t flashTicks_ = 0;      // frames the current tap flash stays armed
  bool flashSuppressed_ = false;  // set by clearTapFlash() during dispatch
  uint8_t transitions_ = 0;
  uint8_t transitionFullEvery_ = 6;
  Paint clearPaint_{};
  bool clearBeforePaint_ = false;
};

}  // namespace ui
}  // namespace freeink
