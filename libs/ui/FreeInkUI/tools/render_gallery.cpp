#include <FreeInkUI.h>
#include <FreeInkUIDisplayTarget.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace freeink::ui;

namespace {

struct Canvas {
  int16_t width;
  int16_t height;
  int16_t widthBytes;
  std::vector<uint8_t> fb;
  DisplayTarget target;

  Canvas(int16_t w, int16_t h) : width(w), height(h), widthBytes((w + 7) / 8), fb(widthBytes * h, 0xFF),
                                 target(fb.data(), w, h, widthBytes) {}

  void clear() {
    for (uint8_t& b : fb) b = 0xFF;
  }

  bool inkAt(int16_t x, int16_t y) const {
    const uint8_t byte = fb[static_cast<int32_t>(y) * widthBytes + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
    return (byte & mask) == 0;
  }
};

void writeSvg(const Canvas& c, const char* path) {
  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "failed to open %s\n", path);
    std::exit(1);
  }

  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << c.width << " " << c.height
      << "\" width=\"" << c.width << "\" height=\"" << c.height << "\" shape-rendering=\"crispEdges\">\n";
  out << "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/>\n";
  out << "<g fill=\"#111\">\n";
  for (int16_t y = 0; y < c.height; ++y) {
    int16_t x = 0;
    while (x < c.width) {
      while (x < c.width && !c.inkAt(x, y)) ++x;
      const int16_t start = x;
      while (x < c.width && c.inkAt(x, y)) ++x;
      if (x > start) out << "<rect x=\"" << start << "\" y=\"" << y << "\" width=\"" << (x - start)
                         << "\" height=\"1\"/>\n";
    }
  }
  out << "</g>\n</svg>\n";
}

void writeManifest(const char* path) {
  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "failed to open %s\n", path);
    std::exit(1);
  }

  out <<
      "{\n"
      "  \"schema\": 1,\n"
      "  \"generatedBy\": \"libs/ui/FreeInkUI/tools/render_gallery.cpp\",\n"
      "  \"images\": [\n"
      "    {\n"
      "      \"file\": \"freeinkui-settings.svg\",\n"
      "      \"category\": \"settings\",\n"
      "      \"title\": \"Settings and controls\",\n"
      "      \"components\": [\"settingRow\", \"toggleRow\", \"stepperRow\", \"radioGroup\", \"textField\", "
      "\"tabBar\", \"list\"]\n"
      "    },\n"
      "    {\n"
      "      \"file\": \"freeinkui-reader.svg\",\n"
      "      \"category\": \"reader\",\n"
      "      \"title\": \"Reader screen controls\",\n"
      "      \"components\": [\"tapZones\", \"readerChrome\", \"statusBar\", \"progressBar\", \"toast\"]\n"
      "    },\n"
      "    {\n"
      "      \"file\": \"freeinkui-library.svg\",\n"
      "      \"category\": \"library\",\n"
      "      \"title\": \"Library and book surfaces\",\n"
      "      \"components\": [\"bookCard\", \"coverGrid\", \"coverCarousel\", \"metricCard\", "
      "\"batteryIndicator\"]\n"
      "    },\n"
      "    {\n"
      "      \"file\": \"freeinkui-overlays.svg\",\n"
      "      \"category\": \"overlays\",\n"
      "      \"title\": \"Overlays, dialogs, keyboard, and actions\",\n"
      "      \"components\": [\"contextMenu\", \"optionDialog\", \"messagePanel\", \"textField\", \"qwertyKeyboard\", \"keyGrid\", "
      "\"gestureBar\", \"toast\"]\n"
      "    }\n"
      "  ],\n"
      "  \"palette\": [\n"
      "    {\"component\": \"button\", \"category\": \"controls\", \"file\": \"freeinkui-components/button.svg\"},\n"
      "    {\"component\": \"settingRow\", \"category\": \"settings\", \"file\": \"freeinkui-components/setting-row.svg\"},\n"
      "    {\"component\": \"toggleRow\", \"category\": \"settings\", \"file\": \"freeinkui-components/toggle-row.svg\"},\n"
      "    {\"component\": \"stepperRow\", \"category\": \"settings\", \"file\": \"freeinkui-components/stepper-row.svg\"},\n"
      "    {\"component\": \"radioGroup\", \"category\": \"settings\", \"file\": \"freeinkui-components/radio-group.svg\"},\n"
      "    {\"component\": \"list\", \"category\": \"settings\", \"file\": \"freeinkui-components/list.svg\"},\n"
      "    {\"component\": \"tabBar\", \"category\": \"navigation\", \"file\": \"freeinkui-components/tab-bar.svg\"},\n"
      "    {\"component\": \"textField\", \"category\": \"input\", \"file\": \"freeinkui-components/text-field.svg\"},\n"
      "    {\"component\": \"keyGrid\", \"category\": \"input\", \"file\": \"freeinkui-components/key-grid.svg\"},\n"
      "    {\"component\": \"qwertyKeyboard\", \"category\": \"input\", \"file\": \"freeinkui-components/qwerty-keyboard.svg\"},\n"
      "    {\"component\": \"gestureBar\", \"category\": \"navigation\", \"file\": \"freeinkui-components/gesture-bar.svg\"},\n"
      "    {\"component\": \"statusBar\", \"category\": \"reader\", \"file\": \"freeinkui-components/status-bar.svg\"},\n"
      "    {\"component\": \"progressBar\", \"category\": \"reader\", \"file\": \"freeinkui-components/progress-bar.svg\"},\n"
      "    {\"component\": \"readerChrome\", \"category\": \"reader\", \"file\": \"freeinkui-components/reader-chrome.svg\"},\n"
      "    {\"component\": \"tapZones\", \"category\": \"reader\", \"file\": \"freeinkui-components/tap-zones.svg\"},\n"
      "    {\"component\": \"bookCard\", \"category\": \"library\", \"file\": \"freeinkui-components/book-card.svg\"},\n"
      "    {\"component\": \"coverGrid\", \"category\": \"library\", \"file\": \"freeinkui-components/cover-grid.svg\"},\n"
      "    {\"component\": \"coverCarousel\", \"category\": \"library\", \"file\": \"freeinkui-components/cover-carousel.svg\"},\n"
      "    {\"component\": \"metricCard\", \"category\": \"library\", \"file\": \"freeinkui-components/metric-card.svg\"},\n"
      "    {\"component\": \"batteryIndicator\", \"category\": \"status\", \"file\": \"freeinkui-components/battery-indicator.svg\"},\n"
      "    {\"component\": \"contextMenu\", \"category\": \"overlays\", \"file\": \"freeinkui-components/context-menu.svg\"},\n"
      "    {\"component\": \"optionDialog\", \"category\": \"overlays\", \"file\": \"freeinkui-components/option-dialog.svg\"},\n"
      "    {\"component\": \"messagePanel\", \"category\": \"overlays\", \"file\": \"freeinkui-components/message-panel.svg\"},\n"
      "    {\"component\": \"toast\", \"category\": \"overlays\", \"file\": \"freeinkui-components/toast.svg\"},\n"
      "    {\"component\": \"popup\", \"category\": \"overlays\", \"file\": \"freeinkui-components/popup.svg\"}\n"
      "  ]\n"
      "}\n";
}

DeviceContext deviceFor(const Canvas& c) {
  DeviceContext device;
  device.width = c.width;
  device.height = c.height;
  device.hasTouch = true;
  device.hasButtons = true;
  device.minTouchSize = 44;
  return device;
}

template <size_t N>
Frame<N> makeFrame(Canvas& c, InteractionBuffer<N>& interactions) {
  static InputSnapshot input;
  return Frame<N>(c.target, deviceFor(c), input, interactions);
}

TextStyle text(FontId font = 0, TextAlign align = TextAlign::Left, uint8_t maxLines = 1) {
  TextStyle s;
  s.font = font;
  s.align = align;
  s.maxLines = maxLines;
  return s;
}

void title(DrawTarget& target, Rect rect, const char* label) {
  TextStyle s = text(0, TextAlign::Left, 1);
  s.bold = true;
  target.text(rect, label, s);
  target.fill(Rect{rect.x, static_cast<int16_t>(rect.bottom() - 2), rect.width, 1}, Paint::solid(Color::Black));
}

template <typename Draw>
void renderComponent(const std::string& dir, const char* file, const char* label, Draw&& draw) {
  Canvas c(360, 180);
  InteractionBuffer<64> interactions;
  auto frame = makeFrame(c, interactions);
  title(c.target, Rect{16, 10, 328, 22}, label);
  draw(frame, Rect{24, 46, 312, 112});
  writeSvg(c, (dir + "/freeinkui-components/" + file).c_str());
}

void renderPalette(const std::string& dir) {
  renderComponent(dir, "button.svg", "button", [](auto& frame, Rect rect) {
    ButtonProps props;
    props.label = "Open book";
    props.action = 1;
    props.text = text(0, TextAlign::Center);
    button(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 24), rect.width, 44}, props);
  });

  renderComponent(dir, "setting-row.svg", "settingRow", [](auto& frame, Rect rect) {
    SettingRowProps props;
    props.label = "Wi-Fi";
    props.subtitle = "Connected";
    props.value = "On";
    props.drawChevron = true;
    props.labelText = text();
    props.subtitleText = text();
    props.valueText = text();
    settingRow(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 20), rect.width, 54}, props);
  });

  renderComponent(dir, "toggle-row.svg", "toggleRow", [](auto& frame, Rect rect) {
    ToggleRowProps props;
    props.row.label = "Dark mode";
    props.row.labelText = text();
    props.checked = true;
    toggleRow(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 24), rect.width, 44}, props);
  });

  renderComponent(dir, "stepper-row.svg", "stepperRow", [](auto& frame, Rect rect) {
    StepperRowProps props;
    props.row.label = "Font size";
    props.row.labelText = text();
    props.row.valueText = text();
    props.value = "12";
    props.decrement = 1;
    props.increment = 2;
    stepperRow(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 24), rect.width, 44}, props);
  });

  renderComponent(dir, "radio-group.svg", "radioGroup", [](auto& frame, Rect rect) {
    const RadioOption options[3] = {{"Small", 1, true}, {"Medium", 2, true}, {"Large", 3, true}};
    RadioGroupProps props;
    props.options = options;
    props.count = 3;
    props.selectedValue = 2;
    props.action = 1;
    props.text = text(0, TextAlign::Center);
    radioGroup(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 28), rect.width, 42}, props);
  });

  renderComponent(dir, "list.svg", "list", [](auto& frame, Rect rect) {
    const ListItem items[3] = {{"Display", nullptr, "42%", {}, {}, StateNormal, 1, true, false},
                               {"Reading", nullptr, nullptr, {}, {}, StateNormal, 2, true, false},
                               {"Storage", nullptr, "8GB", {}, {}, StateNormal, 3, true, false}};
    ListProps props;
    props.items = items;
    props.count = 3;
    props.selectedIndex = 1;
    props.action = 1;
    props.labelText = text();
    props.valueText = text();
    props.rowHeight = 34;
    list(frame, rect, props);
  });

  renderComponent(dir, "tab-bar.svg", "tabBar", [](auto& frame, Rect rect) {
    const TabItem tabs[3] = {{"Books", 1, true}, {"Authors", 2, false}, {"Tags", 3, false}};
    TabBarProps props;
    props.tabs = tabs;
    props.count = 3;
    props.action = 1;
    props.text = text(0, TextAlign::Center);
    props.divider = true;
    tabBar(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 24), rect.width, 50}, props);
  });

  renderComponent(dir, "text-field.svg", "textField", [](auto& frame, Rect rect) {
    TextFieldProps props;
    props.text = "Search";
    props.cursorVisible = true;
    props.cursor = 6;
    props.textStyle = text();
    textField(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 26), rect.width, 42}, props);
  });

  renderComponent(dir, "key-grid.svg", "keyGrid", [](auto& frame, Rect rect) {
    const KeyGridKey keys[9] = {{"Q"}, {"W"}, {"E"}, {"A"}, {"S"}, {"D"}, {"Space", nullptr, {}, {}, KeyKind::Space},
                                {"Del", nullptr, {}, {}, KeyKind::Delete}, {"OK", nullptr, {}, {}, KeyKind::Ok}};
    KeyGridProps props;
    props.keys = keys;
    props.rows = 3;
    props.cols = 3;
    props.action = 1;
    props.selectedIndex = 4;
    props.labelText = text(0, TextAlign::Center);
    keyGrid(frame, rect, props);
  });

  renderComponent(dir, "qwerty-keyboard.svg", "qwertyKeyboard", [](auto& frame, Rect rect) {
    QwertyKeyboardProps props;
    props.keyAction = 1;
    props.shiftAction = 2;
    props.modeAction = 3;
    props.deleteAction = 4;
    props.okAction = 5;
    props.selectedIndex = 5;
    props.labelText = text(0, TextAlign::Center);
    qwertyKeyboard(frame, rect, props);
  });

  renderComponent(dir, "gesture-bar.svg", "gestureBar", [](auto& frame, Rect rect) {
    GestureBarProps props;
    props.left = GestureBarButton{"Back", {}, {}, 1};
    props.center = GestureBarButton{"Select", {}, {}, 2};
    props.right = GestureBarButton{"Menu", {}, {}, 3};
    props.text = text(0, TextAlign::Center);
    gestureBar(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 24), rect.width, 48}, props);
  });

  renderComponent(dir, "status-bar.svg", "statusBar", [](auto& frame, Rect rect) {
    StatusBarProps props;
    props.leading = "10:42";
    props.title = "Chapter";
    props.trailing = "82%";
    props.text = text();
    props.showProgress = true;
    props.progress.value = 42;
    props.progress.max = 100;
    props.progress.track = Paint::dither(Color::LightGray);
    statusBar(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 26), rect.width, 40}, props);
  });

  renderComponent(dir, "progress-bar.svg", "progressBar", [](auto& frame, Rect rect) {
    ProgressBarProps props;
    props.value = 42;
    props.max = 100;
    props.track = Paint::dither(Color::LightGray);
    props.border = Paint::solid(Color::Black);
    props.borderWidth = 1;
    progressBar(frame, Rect{rect.x, static_cast<int16_t>(rect.y + 50), rect.width, 12}, props);
  });

  renderComponent(dir, "reader-chrome.svg", "readerChrome", [](auto& frame, Rect rect) {
    frame.target().stroke(rect, Paint::solid(Color::Black), 1);
    ReaderChromeProps props;
    props.top.title = "Book title";
    props.top.trailing = "82%";
    props.top.text = text();
    props.bottom.trailing = "42%";
    props.bottom.text = text();
    props.bottom.showProgress = true;
    props.bottom.progress.value = 42;
    props.bottom.progress.max = 100;
    props.bottom.progress.track = Paint::dither(Color::LightGray);
    readerChrome(frame, rect, props);
  });

  renderComponent(dir, "tap-zones.svg", "tapZones", [](auto& frame, Rect rect) {
    const int16_t third = static_cast<int16_t>(rect.width / 3);
    const TapZone zones[3] = {{Rect{rect.x, rect.y, third, rect.height}, 1},
                              {Rect{static_cast<int16_t>(rect.x + third), rect.y, third, rect.height}, 2},
                              {Rect{static_cast<int16_t>(rect.x + third * 2), rect.y, third, rect.height}, 3}};
    TapZonesProps props;
    props.zones = zones;
    props.count = 3;
    tapZones(frame, rect, props);
    for (const TapZone& zone : zones) frame.target().stroke(zone.rect, Paint::dither(Color::LightGray), 1);
    TextStyle label = text(0, TextAlign::Center);
    frame.target().text(zones[0].rect, "Prev", label);
    frame.target().text(zones[1].rect, "Menu", label);
    frame.target().text(zones[2].rect, "Next", label);
  });

  renderComponent(dir, "book-card.svg", "bookCard", [](auto& frame, Rect rect) {
    BookCardProps props;
    props.title = "A Long Book Title";
    props.author = "Ada Reader";
    props.meta = "42% read";
    props.progress = 42;
    props.titleText = text(0, TextAlign::Left, 2);
    props.authorText = text();
    props.metaText = text();
    bookCard(frame, rect, props);
  });

  renderComponent(dir, "cover-grid.svg", "coverGrid", [](auto& frame, Rect rect) {
    const CoverGridItem items[3] = {{"One", {}, {}, StateNormal, 1, true},
                                    {"Two", {}, {}, StateNormal, 2, true},
                                    {"Three", {}, {}, StateNormal, 3, true}};
    CoverGridProps props;
    props.items = items;
    props.count = 3;
    props.columns = 3;
    props.coverSize = Size{42, 62};
    props.rowHeight = 92;
    props.titleText = text(0, TextAlign::Center);
    coverGrid(frame, rect, props);
  });

  renderComponent(dir, "cover-carousel.svg", "coverCarousel", [](auto& frame, Rect rect) {
    CarouselProps props;
    props.count = 5;
    props.selectedIndex = 2;
    props.centerSize = Size{76, 104};
    props.sideSize = Size{46, 66};
    props.wrap = true;
    CarouselSlot slots[3];
    coverCarousel(frame, rect, props, slots);
  });

  renderComponent(dir, "metric-card.svg", "metricCard", [](auto& frame, Rect rect) {
    MetricCardProps props;
    props.label = "Streak";
    props.value = "12";
    props.unit = "days";
    props.caption = "this month";
    props.labelText = text();
    props.valueText = text(0, TextAlign::Center);
    props.captionText = text();
    metricCard(frame, centeredRect(rect, Size{140, 92}), props);
  });

  renderComponent(dir, "battery-indicator.svg", "batteryIndicator", [](auto& frame, Rect rect) {
    BatteryIndicatorProps props;
    props.percent = 82;
    props.label = "82%";
    props.text = text();
    batteryIndicator(frame, Rect{static_cast<int16_t>(rect.x + 70), static_cast<int16_t>(rect.y + 42), 160, 28}, props);
  });

  renderComponent(dir, "context-menu.svg", "contextMenu", [](auto& frame, Rect rect) {
    const DialogOption options[3] = {{"Open", 1, 0, StateNormal, true},
                                     {"Book info", 2, 0, StateNormal, true},
                                     {"Delete", 3, 0, StateNormal, true}};
    ContextMenuProps props;
    props.title = "Actions";
    props.options = options;
    props.optionCount = 3;
    props.titleText = text();
    props.itemText = text();
    props.dimBackground = false;
    contextMenu(frame, centeredRect(rect, Size{180, 106}), props);
  });

  renderComponent(dir, "option-dialog.svg", "optionDialog", [](auto& frame, Rect rect) {
    const DialogOption options[2] = {{"Delete", 1, 0, StateNormal, true}, {"Cancel", 2, 0, StateNormal, true}};
    OptionDialogProps props;
    props.title = "Delete book?";
    props.message = "Remove local file.";
    props.options = options;
    props.optionCount = 2;
    props.titleText = text();
    props.messageText = text();
    props.buttonText = text(0, TextAlign::Center);
    optionDialog(frame, centeredRect(rect, Size{230, 118}), props);
  });

  renderComponent(dir, "message-panel.svg", "messagePanel", [](auto& frame, Rect rect) {
    MessagePanelProps props;
    props.title = "No books";
    props.message = "Add EPUB files to the SD card.";
    props.actionLabel = "Retry";
    props.titleText = text(0, TextAlign::Center);
    props.messageText = text(0, TextAlign::Center, 2);
    props.buttonText = text(0, TextAlign::Center);
    messagePanel(frame, centeredRect(rect, Size{220, 110}), props);
  });

  renderComponent(dir, "toast.svg", "toast", [](auto& frame, Rect rect) {
    ToastProps props;
    props.message = "Bookmark saved";
    props.text = text(0, TextAlign::Center);
    props.anchor = ToastAnchor::Center;
    toast(frame, rect, props);
  });

  renderComponent(dir, "popup.svg", "popup", [](auto& frame, Rect rect) {
    PopupProps props;
    props.message = "Syncing...";
    props.text = text(0, TextAlign::Center);
    props.showProgress = true;
    props.progress.value = 60;
    props.progress.max = 100;
    props.progress.track = Paint::dither(Color::LightGray);
    popup(frame, centeredRect(rect, Size{180, 80}), props);
  });
}

void renderSettings(const char* path) {
  Canvas c(640, 420);
  InteractionBuffer<64> interactions;
  auto frame = makeFrame(c, interactions);
  title(c.target, Rect{16, 12, 608, 24}, "Settings and controls");

  SettingRowProps wifi;
  wifi.label = "Wi-Fi";
  wifi.subtitle = "Network connected";
  wifi.value = "On";
  wifi.drawChevron = true;
  wifi.action = 1;
  wifi.labelText = text();
  wifi.subtitleText = text();
  wifi.valueText = text();
  settingRow(frame, Rect{20, 50, 300, 48}, wifi);

  ToggleRowProps dark;
  dark.row.label = "Dark mode";
  dark.row.action = 2;
  dark.row.labelText = text();
  dark.checked = true;
  toggleRow(frame, Rect{20, 106, 300, 44}, dark);

  StepperRowProps font;
  font.row.label = "Font size";
  font.row.labelText = text();
  font.row.valueText = text();
  font.value = "12";
  font.decrement = 3;
  font.increment = 4;
  stepperRow(frame, Rect{20, 158, 300, 44}, font);

  const RadioOption radios[3] = {{"Small", 1, true}, {"Medium", 2, true}, {"Large", 3, true}};
  RadioGroupProps radio;
  radio.options = radios;
  radio.count = 3;
  radio.selectedValue = 2;
  radio.action = 5;
  radio.text = text(0, TextAlign::Center);
  radioGroup(frame, Rect{20, 214, 300, 42}, radio);

  TextFieldProps field;
  field.text = "Search library";
  field.cursorVisible = true;
  field.cursor = 6;
  field.textStyle = text();
  textField(frame, Rect{20, 274, 300, 42}, field);

  const TabItem tabs[3] = {{"Books", 1, true}, {"Authors", 2, false}, {"Tags", 3, false}};
  TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = 3;
  tabProps.action = 6;
  tabProps.text = text(0, TextAlign::Center);
  tabProps.divider = true;
  tabBar(frame, Rect{340, 50, 280, 50}, tabProps);

  const ListItem items[5] = {
      {"Display", "Brightness, theme", "42%", {}, {}, StateNormal, 1, true, false},
      {"Reading", "Margins, font, spacing", nullptr, {}, {}, StateNormal, 2, true, false},
      {"Sync", "Wi-Fi and cloud", "Idle", {}, {}, StateNormal, 3, true, false},
      {"Storage", "SD card", "8.1 GB", {}, {}, StateNormal, 4, true, false},
      {"About", "Firmware version", "0.1", {}, {}, StateNormal, 5, true, false},
  };
  ListProps listProps;
  listProps.items = items;
  listProps.count = 5;
  listProps.selectedIndex = 1;
  listProps.action = 7;
  listProps.labelText = text();
  listProps.subtitleText = text();
  listProps.valueText = text();
  listProps.rowHeight = 54;
  list(frame, Rect{340, 114, 280, 286}, listProps);

  writeSvg(c, path);
}

void renderReader(const char* path) {
  Canvas c(640, 420);
  InteractionBuffer<64> interactions;
  auto frame = makeFrame(c, interactions);
  title(c.target, Rect{16, 12, 608, 24}, "Reader screen controls");

  Rect page{30, 56, 580, 320};
  c.target.stroke(page, Paint::solid(Color::Black), 1);
  TextStyle body = text(0, TextAlign::Left, 8);
  c.target.text(page.inset(Insets{42, 48, 42, 48}),
                "Reader content is app-owned. FreeInkUI provides invisible tap zones, page chrome, progress, "
                "status bars, and e-paper-safe overlays without forcing a retained widget tree.",
                body);

  const TapZone zones[3] = {
      {Rect{page.x, page.y, static_cast<int16_t>(page.width / 3), page.height}, 10, -1, InputTouch, StateNormal, true},
      {Rect{static_cast<int16_t>(page.x + page.width / 3), page.y, static_cast<int16_t>(page.width / 3), page.height},
       11, 0, InputTouch, StateNormal, true},
      {Rect{static_cast<int16_t>(page.x + page.width * 2 / 3), page.y, static_cast<int16_t>(page.width / 3), page.height},
       12, 1, InputTouch, StateNormal, true},
  };
  TapZonesProps taps;
  taps.zones = zones;
  taps.count = 3;
  taps.swipeLeft = 12;
  taps.swipeRight = 10;
  tapZones(frame, page, taps);

  for (const TapZone& zone : zones) c.target.stroke(zone.rect, Paint::dither(Color::LightGray), 1);
  TextStyle zoneLabel = text(0, TextAlign::Center);
  c.target.text(Rect{page.x, static_cast<int16_t>(page.bottom() + 4), static_cast<int16_t>(page.width / 3), 18},
                "prev", zoneLabel);
  c.target.text(Rect{static_cast<int16_t>(page.x + page.width / 3), static_cast<int16_t>(page.bottom() + 4),
                     static_cast<int16_t>(page.width / 3), 18},
                "menu", zoneLabel);
  c.target.text(Rect{static_cast<int16_t>(page.x + page.width * 2 / 3), static_cast<int16_t>(page.bottom() + 4),
                     static_cast<int16_t>(page.width / 3), 18},
                "next", zoneLabel);

  ReaderChromeProps chrome;
  chrome.top.title = "The Example Book";
  chrome.top.leading = "10:42";
  chrome.top.trailing = "82%";
  chrome.top.text = text(0, TextAlign::Left);
  chrome.top.fillBackground = true;
  chrome.bottom.trailing = "42%";
  chrome.bottom.text = text();
  chrome.bottom.showProgress = true;
  chrome.bottom.progress.value = 42;
  chrome.bottom.progress.max = 100;
  chrome.bottom.progress.track = Paint::dither(Color::LightGray);
  readerChrome(frame, page, chrome);

  ToastProps toastProps;
  toastProps.message = "Bookmark saved";
  toastProps.text = text(0, TextAlign::Center);
  toast(frame, page, toastProps);

  writeSvg(c, path);
}

void renderLibrary(const char* path) {
  Canvas c(640, 460);
  InteractionBuffer<64> interactions;
  auto frame = makeFrame(c, interactions);
  title(c.target, Rect{16, 12, 608, 24}, "Library and book surfaces");

  BookCardProps card;
  card.title = "A Long Book Title";
  card.author = "Ada Reader";
  card.meta = "42% read · opened today";
  card.progress = 42;
  card.action = 20;
  card.value = 1;
  card.titleText = text(0, TextAlign::Left, 2);
  card.authorText = text();
  card.metaText = text();
  bookCard(frame, Rect{20, 52, 300, 104}, card);

  MetricCardProps metric;
  metric.label = "Reading streak";
  metric.value = "12";
  metric.unit = "days";
  metric.caption = "this month";
  metric.labelText = text();
  metric.valueText = text(0, TextAlign::Center);
  metric.captionText = text();
  metricCard(frame, Rect{20, 174, 140, 92}, metric);

  BatteryIndicatorProps battery;
  battery.percent = 82;
  battery.label = "82%";
  battery.text = text();
  batteryIndicator(frame, Rect{176, 190, 128, 32}, battery);

  const CoverGridItem gridItems[6] = {
      {"One", {}, {}, StateNormal, 1, true},   {"Two", {}, {}, StateNormal, 2, true},
      {"Three", {}, {}, StateNormal, 3, true}, {"Four", {}, {}, StateNormal, 4, true},
      {"Five", {}, {}, StateNormal, 5, true},  {"Six", {}, {}, StateNormal, 6, true},
  };
  CoverGridProps grid;
  grid.items = gridItems;
  grid.count = 6;
  grid.selectedIndex = 1;
  grid.action = 21;
  grid.columns = 3;
  grid.coverSize = Size{54, 78};
  grid.rowHeight = 110;
  grid.titleText = text(0, TextAlign::Center);
  coverGrid(frame, Rect{340, 52, 280, 238}, grid);

  CarouselProps carousel;
  carousel.count = 5;
  carousel.selectedIndex = 2;
  carousel.action = 22;
  carousel.centerSize = Size{96, 138};
  carousel.sideSize = Size{58, 84};
  carousel.wrap = true;
  CarouselSlot slots[3];
  coverCarousel(frame, Rect{40, 292, 560, 148}, carousel, slots);

  writeSvg(c, path);
}

void renderOverlays(const char* path) {
  Canvas c(640, 460);
  InteractionBuffer<96> interactions;
  auto frame = makeFrame(c, interactions);
  title(c.target, Rect{16, 12, 608, 24}, "Overlays, dialogs, keyboard, and actions");

  const DialogOption actions[4] = {
      {"Open", 30, 0, StateNormal, true},
      {"Book info", 31, 0, StateNormal, true},
      {"Mark read", 32, 0, StateNormal, true},
      {"Delete", 33, 0, StateNormal, true},
  };
  ContextMenuProps menu;
  menu.title = "Book actions";
  menu.options = actions;
  menu.optionCount = 4;
  menu.titleText = text();
  menu.itemText = text();
  menu.dimBackground = false;
  contextMenu(frame, Rect{20, 54, 190, 210}, menu);

  OptionDialogProps dialog;
  dialog.title = "Delete book?";
  dialog.headline = "A Long Book Title";
  dialog.message = "This removes the local file.";
  const DialogOption dialogButtons[2] = {{"Delete", 34, 0, StateNormal, true}, {"Cancel", 35, 0, StateNormal, true}};
  dialog.options = dialogButtons;
  dialog.optionCount = 2;
  dialog.titleText = text();
  dialog.headlineText = text();
  dialog.messageText = text();
  dialog.buttonText = text(0, TextAlign::Center);
  optionDialog(frame, Rect{230, 54, 230, 160}, dialog);

  MessagePanelProps panel;
  panel.title = "No books";
  panel.message = "Add EPUB files to the SD card.";
  panel.actionLabel = "Retry";
  panel.action = 36;
  panel.titleText = text(0, TextAlign::Center);
  panel.messageText = text(0, TextAlign::Center, 2);
  panel.buttonText = text(0, TextAlign::Center);
  messagePanel(frame, Rect{480, 54, 140, 160}, panel);

  TextFieldProps field;
  field.text = "freeink";
  field.cursorVisible = true;
  field.cursor = 7;
  field.textStyle = text();
  textField(frame, Rect{230, 232, 230, 38}, field);

  QwertyKeyboardProps keyboard;
  keyboard.keyAction = 37;
  keyboard.shiftAction = 38;
  keyboard.modeAction = 39;
  keyboard.deleteAction = 40;
  keyboard.okAction = 41;
  keyboard.selectedIndex = 2;
  keyboard.labelText = text(0, TextAlign::Center);
  qwertyKeyboard(frame, Rect{20, 286, 440, 146}, keyboard);

  GestureBarProps gestures;
  gestures.left = GestureBarButton{"Back", {}, {}, 42};
  gestures.center = GestureBarButton{"Select", {}, {}, 43};
  gestures.right = GestureBarButton{"Menu", {}, {}, 44};
  gestures.text = text(0, TextAlign::Center);
  gestureBar(frame, Rect{480, 286, 140, 146}, gestures);

  ToastProps toastProps;
  toastProps.message = "Sync failed";
  toastProps.anchor = ToastAnchor::Top;
  toastProps.text = text(0, TextAlign::Center);
  toast(frame, Rect{480, 220, 140, 60}, toastProps);

  writeSvg(c, path);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s OUTPUT_DIR\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  renderSettings((dir + "/freeinkui-settings.svg").c_str());
  renderReader((dir + "/freeinkui-reader.svg").c_str());
  renderLibrary((dir + "/freeinkui-library.svg").c_str());
  renderOverlays((dir + "/freeinkui-overlays.svg").c_str());
  renderPalette(dir);
  writeManifest((dir + "/freeinkui-gallery.json").c_str());
  return 0;
}
