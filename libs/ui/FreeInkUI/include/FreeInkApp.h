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

struct FooterAction {
  const char* label = nullptr;
  ActionId action = NO_ACTION;
  int16_t value = 0;
  State state = StateNormal;
  bool enabled = true;
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

  void spacer(int16_t height) { takeTop(height); }

  Rect body() const { return content_; }

  void header(const char* title, const char* subtitle = nullptr, const char* rightLabel = nullptr) {
    HeaderProps props;
    props.title = title;
    props.subtitle = subtitle;
    props.rightLabel = rightLabel;
    props.titleText = theme_.titleText;
    props.subtitleText = theme_.smallText;
    props.styles = theme_.popup;
    ui::header(frame_, takeTop(theme_.headerHeight), props);
  }

  void status(const StatusBarProps& props) { ui::statusBar(frame_, takeTop(theme_.headerHeight), props); }

  void button(const char* label, ActionId action, int16_t value = 0, State state = StateNormal) {
    ButtonProps props;
    props.label = label;
    props.action = action;
    props.value = value;
    props.state = state;
    props.text = theme_.bodyText;
    props.styles = theme_.button;
    props.minTouchSize = theme_.minTouchSize;
    ui::button(frame_, takeTop(theme_.rowHeight, theme_.spaceSm), props);
  }

  void list(const ListItem* items, uint16_t count, int16_t selectedIndex, ActionId action, uint16_t topIndex = 0) {
    ListProps props;
    props.items = items;
    props.count = count;
    props.selectedIndex = selectedIndex;
    props.topIndex = topIndex;
    props.action = action;
    props.labelText = theme_.bodyText;
    props.subtitleText = theme_.smallText;
    props.valueText = theme_.smallText;
    props.headerText = theme_.smallText;
    props.rowStyles = theme_.listRow;
    props.rowHeight = theme_.rowHeight;
    ui::list(frame_, content_, props);
  }

  void list(const ListProps& props) { ui::list(frame_, content_, props); }

  void settingRow(const SettingRowProps& props) { ui::settingRow(frame_, takeTop(theme_.rowHeight), props); }

  void toggleRow(const ToggleRowProps& props) { ui::toggleRow(frame_, takeTop(theme_.rowHeight), props); }

  void stepperRow(const StepperRowProps& props) { ui::stepperRow(frame_, takeTop(theme_.rowHeight), props); }

  void radioGroup(const RadioGroupProps& props, int16_t height = 0) {
    ui::radioGroup(frame_, takeTop(height > 0 ? height : theme_.rowHeight), props);
  }

  void qwertyKeyboard(const QwertyKeyboardProps& props, int16_t height = 0) {
    ui::qwertyKeyboard(frame_, takeTop(height > 0 ? height : static_cast<int16_t>(theme_.rowHeight * 4)), props);
  }

  void bookCard(const BookCardProps& props, int16_t height = 0) {
    ui::bookCard(frame_, takeTop(height > 0 ? height : static_cast<int16_t>(theme_.rowHeight * 2)), props);
  }

  void footer(const FooterAction* actions, uint8_t count) {
    Rect rect = takeBottom(theme_.footerHeight);
    if (!actions || count == 0 || rect.empty()) return;
    const int16_t gap = theme_.spaceSm;
    const int16_t totalGap = static_cast<int16_t>(count > 1 ? (count - 1) * gap : 0);
    const int16_t slotW = static_cast<int16_t>((rect.width - totalGap) / count);
    int16_t x = rect.x;
    for (uint8_t i = 0; i < count; ++i) {
      Rect slot{x, rect.y, i == count - 1 ? static_cast<int16_t>(rect.right() - x) : slotW, rect.height};
      ButtonProps props;
      props.label = actions[i].label;
      props.action = actions[i].action;
      props.value = actions[i].value;
      props.state = actions[i].state;
      props.enabled = actions[i].enabled;
      props.text = theme_.bodyText;
      props.styles = theme_.button;
      props.minTouchSize = theme_.minTouchSize;
      ui::button(frame_, slot, props);
      x = static_cast<int16_t>(x + slot.width + gap);
    }
  }

  void popup(const char* message) {
    PopupProps props;
    props.message = message;
    props.text = theme_.bodyText;
    props.styles = theme_.popup;
    const Rect rect = centeredRect(frame_.safeRect(), Size{static_cast<int16_t>(frame_.safeRect().width * 3 / 4),
                                                           static_cast<int16_t>(theme_.rowHeight * 3)});
    ui::popup(frame_, rect, props);
  }

  void dialog(const OptionDialogProps& props, int16_t width = 0) {
    if (width <= 0) width = static_cast<int16_t>(frame_.safeRect().width * 4 / 5);
    const int16_t height = optionDialogHeight(frame_.target(), props, width);
    ui::optionDialog(frame_, centeredRect(frame_.safeRect(), Size{width, height}), props);
  }

 private:
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
      : target_(target), device_(device), assets_(assets) {}

  void setDevice(DeviceContext device) { device_ = device; }
  const DeviceContext& device() const { return device_; }

  void setTheme(const ThemeTokens& theme) { theme_ = theme; }
  const ThemeTokens& theme() const { return theme_; }

  void setAssets(AssetResolver* assets) { assets_ = assets; }
  AssetResolver* assets() const { return assets_; }

  void setScreen(ScreenFn screen, void* user = nullptr) {
    screen_ = screen;
    screenUser_ = user;
    interactions_.setFocusedIndex(-1);
    invalidate(RefreshHint::Full);
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

    Frame<MaxInteractions> frame(target_, device_, input, interactions_, assets_);
    ScreenType screen(frame, theme_);
    if (screen_) screen_(screen, screenUser_);
    lastEvent_ = frame.finish();
    if (lastEvent_) {
      dispatch(lastEvent_);
      invalidate(RefreshHint::Fast);
    }
    return lastEvent_;
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
};

}  // namespace ui
}  // namespace freeink
