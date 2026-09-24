# FreeInkUI

FreeInkUI is a lightweight UI layer for e-paper firmware. It sits above a
display driver and an input source while staying independent of any one
application: apps plug in their own renderer, fonts, icons, themes,
localization strings, and screen state through small adapter interfaces.

The design goal is "Tailwind for e-ink" without a web-style runtime:

- no heap allocation by default
- fixed-capacity interaction tables
- borrowed strings and asset pointers
- app-owned state and props
- virtualized lists/grids instead of one node per item
- reusable component functions such as `statusBar`, `tabBar`, `list`,
  `button`, `keyGrid`, and `batteryIndicator`
- touch, GPIO buttons, focus navigation, and gestures all route to semantic
  action IDs
- freestanding C++17 — no Arduino or ESP-IDF dependency, so the same code
  runs on firmware and in host-side unit tests

## Core Flow

### App Runtime

For most firmware, start with `FreeInkApp.h`. It is the "easy mode" wrapper over
the immediate-mode primitives in `FreeInkUI.h`.

`FreeInkApp` does:

- owns one fixed-capacity `InteractionBuffer`
- calls your active screen function each render pass
- routes the latest `InputSnapshot` through the interactions that screen
  registered
- dispatches semantic `ActionId` events to callbacks registered with `on(...)`
- tracks whether another render is needed after a callback changes app state
- carries a `RefreshHint` so firmware can choose an e-paper refresh mode

`FreeInkApp` deliberately does not:

- allocate or copy screen state
- own the display driver
- read hardware buttons/touch directly
- push pixels to the panel
- parse JSON or keep a retained widget tree

The app still owns persistent state, input sampling, and the final panel refresh.
That keeps it predictable on small MCUs and lets each board choose the right
refresh policy.

#### Minimal Screen

```cpp
#include <FreeInkApp.h>

using App = freeink::ui::FreeInkApp<32, 16>;

enum : freeink::ui::ActionId {
  ActionOpen = 1,
  ActionBack = 2,
};

struct AppState {
  freeink::ui::ListItem books[2] = {
      {.label = "Book one", .actionValue = 0},
      {.label = "Book two", .actionValue = 1},
  };
  int16_t selected = 0;
};

void homeScreen(App::ScreenType& screen, void* user) {
  auto& state = *static_cast<AppState*>(user);
  screen.header("Library");

  const freeink::ui::FooterAction footer[] = {
      {.label = "Open", .action = ActionOpen},
      {.label = "Back", .action = ActionBack},
  };
  screen.footer(footer, 2);

  screen.list(state.books, 2, state.selected, ActionOpen);
}

void handleOpen(const freeink::ui::ActionEvent& event, void* user) {
  auto& state = *static_cast<AppState*>(user);
  state.selected = event.value;
}
```

The screen function is re-run whenever you call `app.render(...)`. It should be
cheap and deterministic: read app state, draw components, and register actions.
Do not store pointers to `screen`, `Frame`, or component-local props after the
function returns.

#### Firmware Loop

Wire the app to your display target and input layer:

```cpp
AppState state;
freeink::ui::DisplayTarget* target = nullptr;
App* app = nullptr;

void setup() {
  display.begin();

  static freeink::ui::DisplayTarget targetInstance(display.getFrameBuffer(), display.getDisplayWidth(),
                                                   display.getDisplayHeight(), display.getDisplayWidthBytes());
  static App appInstance(targetInstance, targetInstance.deviceContext());
  target = &targetInstance;
  app = &appInstance;
  app->setClearColor(freeink::ui::Color::White);  // start each paint from a blank canvas
  app->setScreen(homeScreen, &state);
  app->on(ActionOpen, handleOpen, &state);
}

void loop() {
  freeink::ui::InputSnapshot input = readInputSnapshot();
  freeink::ui::ActionEvent event = app->render(input);
  // present() maps the frame's RefreshHint to the panel's refresh modes
  // (Full/Clean -> FULL_REFRESH, Fast -> FAST_REFRESH, None -> no push).
  freeink::ui::present(display, app->lastRenderRefreshHint());

  if (app->invalidated()) {
    // An action handler changed state after this render pass. Render again on
    // the next loop iteration, or immediately if your firmware wants synchronous
    // UI updates.
  }
}
```

`FreeInkDisplay` owns the framebuffer, and on PSRAM-backed boards it allocates
that memory in `display.begin()`. Construct `DisplayTarget` and `FreeInkApp`
after `display.begin()`; `display.getFrameBuffer()` is not valid at static-init
time.

The loop task gets room automatically: the render pipeline (screen builders +
text layout) runs deeper than Arduino's default 8 KB `loopTask` stack (the
overflow is a `Stack canary watchpoint triggered (loopTask)` panic and a
reboot mid-interaction), so on ESP32 FreeInkUI ships a weak 16 KB default for
`getArduinoLoopTaskStackSize()`. An app that needs a different size overrides
it the standard way — `SET_LOOP_TASK_STACK_SIZE(24 * 1024);` at global scope
in the sketch beats the SDK's weak default.

`FreeInkApp` does not own your display refresh policy. `lastRenderRefreshHint()`
describes the frame just drawn, while `invalidated()` / `refreshHint()` describe
whether an action handler changed state and a follow-up render is needed.
`present()` (from `FreeInkUIDisplayTarget.h`, compiled only when
`EInkDisplay.h` is on the include path) is the standard hint-to-panel mapping;
firmware with its own refresh policy can keep switching on the hint instead.

For interactive apps prefer `presentAsync()` + `display.refreshBusy()`:
`present()` blocks on the panel's BUSY pin for the whole waveform (~0.3–2 s),
during which the loop can't poll input — typing feels sluggish and taps get
lost. `presentAsync()` starts the refresh and returns (~25 ms); the panel
refreshes from its own RAM copy, so the loop keeps polling input and rendering
into the framebuffer, and pushes the newest frame once `refreshBusy()` goes
false. Accumulate the strongest `RefreshHint` across renders between presents.

Pair it with buffered input: `InputManager::beginAsync()` samples touch on its
own task and queues completed taps (`popTouchTap`), so taps that land during a
~200 ms render are never lost, and `FreeInkApp::route()` dispatches each
queued tap against the last rendered frame without drawing — a fast typing
burst costs one repaint, not one render per key.

Frames do not clear the target on their own — without `setClearColor()` (or an
app-side clear) the previous screen shows through wherever the new one doesn't
draw. For screen changes, `invalidateTransition()` requests a fast partial
refresh with a periodic full refresh (every 6th by default, see
`setTransitionFullEvery()`) to clear accumulated ghosting.

#### Input Snapshot

Your board-support code converts hardware into `InputSnapshot`:

```cpp
freeink::ui::InputSnapshot readInputSnapshot() {
  freeink::ui::InputSnapshot input;

  input.confirm = buttons.wasPressed(InputManager::BTN_CONFIRM);
  input.back = buttons.wasPressed(InputManager::BTN_BACK);
  input.focusNext = buttons.wasPressed(InputManager::BTN_DOWN);
  input.focusPrev = buttons.wasPressed(InputManager::BTN_UP);

  float nx, ny;
  if (buttons.wasTouchTap(nx, ny)) {
    freeink::ui::Point p = freeink::ui::touchToLogical(app.device(), nx, ny);
    input.touchReleased = true;
    input.touchX = p.x;
    input.touchY = p.y;
  }

  return input;
}
```

Use semantic fields (`confirm`, `back`, `focusNext`, `swipeLeft`) rather than
physical pin names inside UI code. The same screen can then run on touch-first
and button-first boards.

#### Screen Builder API

`Screen<MaxInteractions>` is a thin builder over existing FreeInkUI components.
It manages a remaining content rectangle from `safeRect()`:

- `header(title, subtitle, rightLabel)` consumes the top header band
- `header(props)` accepts `HeaderProps` for custom header chrome
- `status(props)` consumes the top status band
- `footer(actions, count)` consumes the bottom footer band
- `footer(props)` accepts `FooterProps` for nav spacing and button border edges
- `setContentMargin(insets)` reserves space relative to the device safe area;
  `setContentMarginFromScreen(insets)` reserves physical screen-edge chrome
  without applying the safe-area insets twice
- `button(label, action, value, state)` adds one full-width row button
- `list(items, count, selected, action, topIndex)` fills the remaining body
- `list(props)` uses a full `ListProps` when you need detailed styling
- `popup(message)` draws an auto-sized centered popup over the current screen
- `popup(props)` accepts `PopupProps` for max width, padding, progress, and text alignment
- `dialog(props, width)` sizes and draws an `optionDialog`
- `spacer(height)`, `takeTop(...)`, `takeBottom(...)`, and `body()` let custom
  code reserve or draw into exact regions

Header bars default to a bottom divider, and footer/nav buttons default to a
top divider. Set `HeaderProps::borderEdges` or
`FooterProps::buttonBorderEdges` to `EdgesNone`, `EdgeTop`, `EdgeBottom`,
`EdgesHorizontal`, `EdgesVertical`, or `EdgesAll` when you want different nav
chrome.

Most row-like builder calls also accept an optional `LayoutAnchor` argument.
Use `LayoutAnchor::Bottom` for controls that should reserve space from the
bottom edge while the rest of the screen continues to fill from the top:

```cpp
screen.header("Search");

freeink::ui::QwertyKeyboardProps keys;
keys.keyAction = ActionKeyboardKey;
screen.qwertyKeyboard(keys, 0, freeink::ui::LayoutAnchor::Bottom);

screen.list(results, resultCount, selected, ActionOpen);
```

Use the builder for common screens, then drop down to `screen.frame()` or
`screen.target()` for custom surfaces:

```cpp
void readerScreen(App::ScreenType& screen, void* user) {
  auto& state = *static_cast<ReaderState*>(user);
  screen.header(state.title);

  freeink::ui::Rect page = screen.body();
  renderPageText(screen.target(), page, state.currentPage);
}
```

#### Actions And Callbacks

Register handlers once during setup:

```cpp
app.on(ActionOpen, [](const freeink::ui::ActionEvent& event, void* user) {
  auto& state = *static_cast<AppState*>(user);
  state.selected = event.value;
}, &state);
```

Callbacks run after the current render pass finishes. If they change state,
`FreeInkApp` marks itself invalidated with `RefreshHint::Fast`. A callback can
request a stronger refresh:

```cpp
void handleThemeChange(const freeink::ui::ActionEvent&, void*) {
  settings.darkMode = !settings.darkMode;
  app.invalidate(freeink::ui::RefreshHint::Full);
}
```

If you register more handlers than `MaxHandlers`, `handlerOverflowed()` becomes
true. If a screen registers more interactions than `MaxInteractions`,
`interactionOverflowed()` becomes true and dropped controls will not receive
input. Size both template parameters deliberately.

#### Refresh Hints

`RefreshHint` is a policy hint, not a panel command:

- `None`: this render did not request a panel update
- `Fast`: use the board's fast/partial/user-interface refresh
- `Full`: use a full clean refresh
- `Clean`: use the firmware's cleanest practical refresh policy

For simple projects, map `Fast` to `FreeInkDisplay::FAST_REFRESH` and
`Full`/`Clean` to `FreeInkDisplay::FULL_REFRESH`. More advanced firmware can
map `Clean` to a board-specific sequence, such as a ghost-clearing pass or an
hourly full refresh on panels that need DC balancing.

### Generated Screens

The `Screen` API is also the target for design-time tooling. The bundled
`tools/gen_screen.py` turns a small JSON schema into ordinary C++ that uses
`FreeInkApp.h`:

```json
{
  "screen": "settings",
  "children": [
    {"type": "header", "title": "Settings"},
    {
      "type": "footer",
      "buttons": [
        {"label": "Back", "action": "back"},
        {"label": "Apply", "action": "apply"}
      ]
    },
    {"type": "toggleRow", "label": "Dark mode", "checked": false, "action": "darkMode"},
    {
      "type": "stepperRow",
      "label": "Font size",
      "value": "12",
      "decrement": "fontSmaller",
      "increment": "fontLarger",
      "controlSize": 14
    },
    {
      "type": "qwertyKeyboard",
      "anchor": "bottom",
      "layout": "spanish_es",
      "action": "keyboardKey",
      "shiftAction": "keyboardShift",
      "deleteAction": "keyboardDelete",
      "okAction": "keyboardOk"
    },
    {
      "type": "list",
      "action": "selectSetting",
      "selectedIndex": 0,
      "items": [
        {"label": "Wi-Fi", "value": "On", "actionValue": 1},
        {"label": "Frontlight", "value": "42%", "actionValue": 2}
      ]
    }
  ]
}
```

Generate a header:

```sh
python3 libs/ui/FreeInkUI/tools/gen_screen.py settings.freeinkui.json \
    --out SettingsScreen.h
```

Or launch the local builder:

```sh
python3 libs/ui/FreeInkUI/tools/builder/server.py
```

Open `http://127.0.0.1:8088/` to choose a device profile, switch the preview
between portrait and landscape, drag components into a screen, edit props,
export the same JSON schema, or generate C++ through the local server. The
builder palette is loaded from `docs/images/freeinkui-gallery.json`, and the
screen preview is rendered by compiling the generated screen against the real
FreeInkUI/`DisplayTarget` renderer and returning SVG. That keeps the visual
preview aligned with on-device component output instead of browser-only CSS.

The output contains an inline `settingsScreen(...)`-style function plus
generated `ActionId` constants. Supported schema component types currently
include `header`, `footer`, `list`, `button`, `settingRow`, `toggleRow`,
`stepperRow`, `checkbox`, `slider`, `dropdown`, `table`, `radioGroup`,
`qwertyKeyboard`, `spacer`, `popup`, `optionDialog`, `statusBar`, `bookCard`,
and `textArea`. Components that consume
layout space can set `"anchor": "top"` or `"anchor": "bottom"`; omitted anchors use the
component default (`footer` defaults bottom, most others default top). Modal
overlays such as `popup` and `optionDialog` render centered over the screen. The
`qwertyKeyboard` schema also accepts `"layout": "qwerty_en"`, `"azerty_fr"`,
`"qwertz_de"`, or `"spanish_es"`. The builder edits this same schema and
previews real device profiles, then emits the same static C++ so firmware
carries no JSON parser or GUI-builder runtime.

### Manual Frame API

The lower-level API is still available when a screen needs exact control:

```cpp
freeink::ui::InteractionBuffer<32> interactions;
freeink::ui::InputSnapshot input = readInput();

// Native render path — no external graphics library. Draws into the
// framebuffer FreeInkDisplay already owns (FreeInkUIDisplayTarget.h). Construct
// this after display.begin(), when getFrameBuffer() is valid.
freeink::ui::DisplayTarget draw(display.getFrameBuffer(), display.getDisplayWidth(),
                                display.getDisplayHeight(), display.getDisplayWidthBytes());
freeink::ui::DeviceContext device = draw.deviceContext();

freeink::ui::Frame<32> ui(draw, device, input, interactions);

freeink::ui::Stack<3> screen(ui.safeRect(), freeink::ui::Axis::Column, 0);
screen.fixed(statusBarHeight);
screen.flex(1);
screen.fixed(controlBarHeight);
screen.layout();

statusBar(ui, screen.rect(0), props);
pageRenderer.renderInto(screen.rect(1));  // app-drawn content slot
controlBar(ui, screen.rect(2), props);

if (auto event = ui.finish()) {
  handleAction(event.action, event.value);
}
```

`Frame::finish()` routes the input snapshot through the interactions registered
during the render pass. The app owns persistent state such as selected row,
focused index, scroll offset, clock buffers, reading statistics, and visibility
flags.

`Stack<N>` fixes the slot count at compile time. When the slots come from
parsed data instead, `FreeInkUILayout.h` is the runtime counterpart: it splits a
rect into row/column slots with no heap allocation or retained widget state.
Each slot is a `LayoutLength` — `fixed(px)`, `flexible(grow, minPx)`, or
`tokenized(id)` (resolved through an optional callback for theme spacing) — and
the split emits a `Rect` per slot:

```cpp
freeink::ui::LayoutLength cols[] = {
    freeink::ui::LayoutLength::fixed(coverW),     // cover
    freeink::ui::LayoutLength::flexible(1),       // text column takes the rest
};
freeink::ui::layoutLinear(rect, freeink::ui::Axis::Row, gap, 2,
                          [&](uint8_t i) { return cols[i]; },
                          [&](uint8_t i, freeink::ui::Rect slot) { /* draw slot i */ });
```

`layoutTree(LayoutNode, rect, emit)` nests the same split into a row/column
tree and emits one rect per leaf `id`, for whole-screen scaffolds driven by
data rather than fixed code.

`FreeInkUICore.h` also provides small clamping/builder helpers used across
components and apps: `clampI16`/`clampU8`/`clampRadius`, `makeRect`/`makeSize`/
`makeInsets`, and ready-made `StyleSet` builders (`outlinedButtonStyles`,
`selectedOutlineListRowStyles`, `selectedPlainListRowStyles`).

## Rendering

FreeInkUI draws through a `DrawTarget` — an interface of primitives (`fill`,
`stroke`, `line`, `triangle`, `text`, `bitmap`). The SDK ships two:

- **`DisplayTarget` (`FreeInkUIDisplayTarget.h`) — the default.** A
  self-contained renderer that needs nothing but a raw 1-bit framebuffer, so it
  has no external dependency and the same drawing code runs in host tests. It
  bundles a compact Noto Sans bitmap font and reproduces gray paints on a 1-bit
  panel with an ordered dither. Construct it over `FreeInkDisplay`'s buffer:

  ```cpp
  // Construct after display.begin(), when getFrameBuffer() is valid.
  freeink::ui::DisplayTarget draw(display.getFrameBuffer(), display.getDisplayWidth(),
                                  display.getDisplayHeight(), display.getDisplayWidthBytes());
  ```

  The framebuffer convention matches `FreeInkDisplay`: 1bpp, MSB-first, a set
  bit is white and a clear bit is black ink. After a frame, push the buffer with
  the display's normal refresh call.

- **`GfxRendererTarget` (`FreeInkUIGfxRenderer.h`) — optional.** An adapter for
  firmwares that already provide CrossPoint's `GfxRenderer` (its fonts, bidi,
  truncation, dithering). It compiles **only** where `<GfxRenderer.h>` is on the
  include path (guarded by `__has_include`), so it never interferes with apps
  that use the native target.

## Fonts

`DisplayTarget` bundles one font (Noto Sans, rasterized to a compact 1-bit
bitmap). Every logical font slot — `ThemeTokens::fontSmall`, `fontBody`,
`fontTitle`, which are just `FontId` values your app assigns — points at it by
default, so text works with zero setup.

### Swapping the font

Fonts are plain data (`freeink::ui::BitmapFont`): a concatenated glyph bitmap
plus a per-glyph metrics table. Glyphs come in two depths (`BitmapFont::bpp`):
a 1-bit on/off mask, or anti-aliased 4-bit coverage. To use your own typeface:

1. **Generate a font header from any TTF/OTF** with the bundled tool:

   ```sh
   pip install pillow
   python3 libs/ui/FreeInkUI/tools/gen_font.py \
       --ttf MyFont.ttf --size 16 --name MyFont \
       --out libs/ui/FreeInkUI/include/MyFontFont.h
   ```

   This emits `freeink::ui::kMyFontFont` (a `BitmapFont`). Pick `--size` for the
   pixel height you want; the tool prints the resulting line height and flash
   cost. Covers printable ASCII (`U+0020..U+007E`) by default — widen with
   `--first`/`--last`. Confirm the source font's license permits embedding.

   Add `--alpha` to rasterize anti-aliased: glyphs are stored as 4-bit
   coverage (~4x the flash of the 1-bit form) and `DisplayTarget` reproduces
   the edge coverage on 1-bit panels through its ordered Bayer dither, so
   diagonals and curves render noticeably smoother. Grayscale-capable targets
   can blend the same coverage directly. The dither also lands on
   partially-covered stem pixels, so anti-aliased fonts look best at title and
   display sizes (roughly 16px and up); keep small body text on 1-bit fonts.

2. **Point the target at it** — globally, or per slot so titles differ from body
   text:

   ```cpp
   #include <MyFontFont.h>

   draw.setFont(freeink::ui::kMyFontFont);                 // every slot
   draw.setFont(theme.tokens.fontTitle, freeink::ui::kMyFontFont);  // one slot
   ```

That's the whole process — no rebuild of the SDK, no driver changes.
`FreeInkUIFont.h` is the canonical home of the `FontGlyph`/`BitmapFont` struct
definitions and carries the bundled Noto Sans data; generated font headers
`#include` it for those structs.

## Actions, Not Hardware

Interactive components register semantic actions:

```cpp
button(ui, rect, {
    .label = tr(STR_SELECT),
    .action = ActionSelect,
    .inputMask = freeink::ui::InputTouch |
                 freeink::ui::InputFocus |
                 freeink::ui::InputConfirm,
});
```

The same component can be selectable by touch, GPIO/focus, side buttons, or
gestures depending on `inputMask` and device capabilities. Components never
reference physical button names or board pins.

## Localization

FreeInkUI does not translate strings. Apps pass already-localized, borrowed
strings:

```cpp
button(ui, rect, {
    .label = tr(STR_MENU),
    .action = ActionOpenMenu,
});
```

The app's localization system stays intact and unmodified. Text measurement and
drawing happen through the app's `DrawTarget` adapter, so bidi, font handling,
truncation, and glyph loading stay centralized wherever the app already does
them.

## Props

Reusable components take small props structs. The props borrow strings and
assets instead of copying them:

```cpp
struct ClockProps {
  const char* time;
  bool enabled;
};

void clock(freeink::ui::Frame<32>& ui, freeink::ui::Rect rect,
           const ClockProps& props) {
  freeink::ui::TextStyle style;
  style.font = FontSmall;
  style.align = freeink::ui::TextAlign::Right;
  style.maxLines = 1;
  style.color = props.enabled ? freeink::ui::Color::Black
                              : freeink::ui::Color::LightGray;
  ui.target().text(rect, props.time, style);
}
```

Multiple firmwares can share a component while passing their own clock or
statistics data.

## Built-In Components

The SDK provides a small set of immediate-mode components, deliberately not
tied to any application's screen structure:

- `button`
- `checkbox`
- `slider`
- `capsuleSlider` (finger-height filled-capsule slider with an edge-riding
  handle; touch-drag routed)
- `sliderRow` (caption + value readout over `[-]` capsule `[+]`, with an
  optional trailing icon toggle)
- `tileGrid` (quick-setting tile cards in fixed columns; checked tiles fill
  solid)
- `sheet` (partial-height sheet chrome: card body, edge rule, grabber, and an
  optional tap-outside dismiss action)
- `gestureBar`
- `header`
- `list` (virtualized; see below — supports hug-content pill rows and
  selection markers)
- settings rows: `settingRow`, `toggleRow`, `stepperRow`, `radioGroup`
- `dropdown`
- `table`
- reader surfaces: `readerChrome`, `tapZones`
- book surfaces: `bookCard`, `coverGrid`, `coverCarousel`
- `progressBar`
- `statusBar`
- `tabBar` (pill or underline-style tabs with optional divider; per-tab
  icons via `BitmapRef`/`AssetRef` or an `iconPainter` callback, optional
  labels, and disabled tabs)
- `popup`
- `toast`
- `contextMenu`
- `messagePanel` (empty/error/loading panels)
- `optionDialog`
- `textField`
- `keyGrid`
- `keyboard` (data-driven rows for localized or custom layouts)
- `qwertyKeyboard`
- `metricCard`
- `batteryIndicator`
- `coverCarousel` (prev/center/next cover slots with selection chrome and
  tap/swipe/button routing; the app renders cover art into the returned
  slot rects, so image decoding and frame caching stay app-owned)

These cover the shared surfaces of a typical e-reader or appliance UI: home
menus, settings lists, button hints, status/progress bars, popups,
confirmation dialogs, text entry fields, keyboard grids, full QWERTY entry
surfaces, statistics cards, and battery glyphs. Applications draw app-specific content directly into a
slot, such as a book page renderer or a cover image.

### E-Reader Components

The e-reader-specific components are still immediate-mode: every frame gets a
props struct, draws into a rect, and registers semantic actions.

Controls default to square corners. Pass a positive radius on the relevant
props (`ButtonProps::radius`, `SettingRowProps::radius`,
`ToggleRowProps::radius`/`knobRadius`, `StepperRowProps::buttonRadius`,
`CheckboxProps::radius`, `SliderProps::radius`, `DropdownProps::radius`,
`RadioGroupProps::radius`, `TableProps::cellRadius`, or
`QwertyKeyboardProps::keyRadius`) when a product theme wants rounded
controls. The control-center pieces are card language and default rounded
instead (`SliderRowProps::buttonRadius`, `TileGridProps::radius`; 0 gives
square cards), and `SheetProps::radius` rounds the sheet's free-edge corners.
The builder exposes the same fields in the inspector and JSON schema.
Dropdowns use a stroked chevron indicator by default; tune
`DropdownProps::indicatorWidth`, `indicatorSize`, and `indicatorStroke` for a
heavier, lighter, or wider affordance.

Settings rows cover common device preferences:

```cpp
freeink::ui::ToggleRowProps dark;
dark.row.label = "Dark mode";
dark.row.action = ActionDarkMode;
dark.checked = settings.darkMode;
// Optional: round the track/knob for a softer toggle.
dark.radius = 3;
dark.knobRadius = 1;
toggleRow(ui, rowRect, dark);

freeink::ui::StepperRowProps font;
font.row.label = "Font size";
font.value = fontSizeLabel;
font.decrement = ActionFontSmaller;
font.increment = ActionFontLarger;
font.controlSize = 14; // explicit plus/minus strokes, independent of font glyphs
stepperRow(ui, rowRect, font);
```

Control-panel surfaces (a pull-down control center, a bottom sheet of quick
settings) compose from `sheet`, `sliderRow`/`capsuleSlider`, and `tileGrid`:

```cpp
freeink::ui::SheetProps panel;             // top-anchored card with a grabber
panel.dismissAction = ActionClosePanel;    // tap outside the sheet closes it
sheet(ui, panelRect, panel);

freeink::ui::SliderRowProps brightness;    // caption + [-] [capsule] [+] [lamp]
brightness.label = "Brightness";
brightness.value = "62%";                  // caller-formatted readout
brightness.sliderValue = 62;
brightness.sliderAction = ActionBrightness;   // drag/tap; dragPermille carries the position
brightness.decrement = ActionBrightnessStep;  // value -1 / +1 per press
brightness.increment = ActionBrightnessStep;
brightness.toggleAction = ActionLightToggle;  // trailing icon button
brightness.toggleIcon = lampIcon;
sliderRow(ui, rowRect, brightness);

freeink::ui::TileGridItem tiles[2];
tiles[0].label = "Night mode";
tiles[0].value = TileNightMode;            // stable id, not grid position
tiles[0].state = nightMode ? freeink::ui::StateChecked : freeink::ui::StateNormal;
tiles[1].label = "Refresh";
tiles[1].value = TileRefresh;
freeink::ui::TileGridProps grid;
grid.items = tiles;
grid.count = 2;
grid.action = ActionTile;                  // event value = the tile's id
tileGrid(ui, gridRect, grid);
```

`sheetContentRect()` returns the part of the sheet its content may use (the
rect minus the grabber band), `sliderRowHeight()` and `tileGridHeight()` size
the bands, and the `Screen` wrappers (`screen.sheet(...)`, `screen.sliderRow(...)`,
`screen.tileGrid(...)`) apply theme fonts and spacing and reserve the bands
automatically. The capsule slider is the drag surface; the step buttons exist
because a drag on matte glass is unreliable and single steps land exact
values. On a rect too narrow for the capsule's handle the track is skipped
entirely and the step buttons alone drive the value.

Text-entry screens can use the generic `keyGrid` for compact custom pads,
`keyboard` for data-driven rows, or `qwertyKeyboard` for a ready-made four-row
keyboard with Shift, mode, space, delete, and OK keys:

```cpp
freeink::ui::QwertyKeyboardProps keyboard;
keyboard.layout = freeink::ui::KeyboardLayoutId::SpanishEs;
keyboard.keyAction = ActionKeyboardKey;
keyboard.shiftAction = ActionKeyboardShift;
keyboard.modeAction = ActionKeyboardMode;
keyboard.deleteAction = ActionKeyboardDelete;
keyboard.okAction = ActionKeyboardOk;
keyboard.selectedIndex = focusedKey;
keyboard.shifted = state.shifted;
keyboard.symbols = state.symbols;
qwertyKeyboard(ui, keyboardRect, keyboard);
```

`Screen::keyboard` and `Screen::qwertyKeyboard` use the full safe-area width,
including the space outside text-content side margins. Their automatic height
allocates at least 64px per row, scaling to 80px on a 480px-wide screen: 348px
for four rows, or 434px with a dedicated number row. On short screens the total
is capped at 50% of the safe height plus the extra row spacing, and the remaining
content space. An explicit
height still overrides automatic sizing. Low-level calls with a `Rect` use that
rectangle exactly; reserve `keyboardPreferredHeight(width, layout.rowCount)`
pixels to get the same taller rows there.

Keys are borderless by default, with a filled highlight when selected or pressed. Primary labels
use the body font slot, with smaller alternate hints. Set `labelText.font` to a
larger registered font and `controlText.font` to a smaller font for word labels
such as Shift or localized OK text. The gallery uses 36px letters, 24px control
labels, and 13px alternate hints on a 480×800 display, with the five-row keyboard
occupying the lower 416px. Rows are separated by 6px (`rowGap`), while the
horizontal key spacing remains 2px (`gap`). Selected and pressed highlights are
about 20% shorter and centered on the labels, without reducing hit targets; alternate
hints keep the same position and 10px right padding in every state, inside the
highlight area. Selecting or pressing a key changes only the hint color. Keys with alternate
hints reserve 4px of extra headroom above the primary glyph, with matching
clearance below so the highlight stays centered. Firmware can tune this geometry with
`altHintRightPadding`, `altLabelGap`, and the signed `digitLabelOffsetX` on
`KeyboardProps` or `QwertyKeyboardProps`; their defaults are 10px, 4px, and -4px.
Negative padding or gap values are treated as zero.

The keyboard is stateless like every component: Shift and mode ("?123"/"ABC")
keys only report their actions. With `symbols` set, `shifted` selects the
second symbols page — the shift slot reads "#+=" on page one and "123" on page
two. The two pages plus the letter layers cover every printable ASCII
character.

`KeyboardEntry` owns the editing state so apps don't hand-roll it: the
shift/symbol layer flags, layout-correct UTF-8 append, and multi-byte-aware
backspace over a caller-owned buffer. Note that keys report stable ids in
`ActionEvent::value` — ASCII keys their code point, localized keys (é, ñ, ß)
ids above 1000 — so casting the value to `char` corrupts non-ASCII layouts;
`KeyboardEntry::key()` (or the `keyboardOutputFor()` lookup) inserts the
layout's real output.

```cpp
freeink::ui::KeyboardEntry kb;               // in app state
kb.attach(state.name, sizeof state.name,     // bind the field being edited
          /*startShifted=*/true);

// Route the four keyboard actions:
kb.key(event.value);  // insert; Shift auto-releases, symbol pages stay sticky
kb.backspace();       // removes one code point, not one byte
kb.shift();           // letter case, or symbol page one/two
kb.mode();            // enter/leave the symbol layers

// Each frame, mirror the layer flags into the props:
freeink::ui::applyEntry(keys, kb);
```

Apps with cursor editing or their own text container can reuse the controller
pieces without adopting `KeyboardEntry`'s fixed buffer:

- `keyboardActivationFor(layout, value, longPress)` resolves a rendered key to
  text, Shift, mode, language, delete, or submit semantics.
- `utf8PreviousBoundary()` / `utf8NextBoundary()` move a byte cursor without
  splitting a UTF-8 code point.
- `KeyboardNavigator` owns row/column selection for irregular keyboard grids,
  including wrapping and proportional movement between differently sized rows.

When input and rendering run on different tasks, capture completed one-shot
taps before testing whether the interaction table is available. `TouchTapQueue`
is a fixed-capacity, allocation-free queue for that boundary:

```cpp
freeink::ui::TouchTapQueue<16> pendingTaps;
int16_t tapX = 0;
int16_t tapY = 0;

// Every input update, even while a renderer rebuilds hit targets:
if (tapCompleted) pendingTaps.push(tapX, tapY);

// Once the last complete interaction table is safe to route against:
while (interactionsReady && pendingTaps.pop(tapX, tapY)) {
  freeink::ui::InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = tapX;
  tap.touchY = tapY;
  interactions.route(tap);
}
```

For custom or app-provided layouts, pass `KeyboardProps` directly:

```cpp
freeink::ui::KeyboardProps keyboard;
keyboard.layout = &freeink::ui::builtinKeyboardLayout(freeink::ui::KeyboardLayoutId::AzertyFr);
keyboard.keyAction = ActionKeyboardKey;
keyboard.shiftAction = ActionKeyboardShift;
keyboard.deleteAction = ActionKeyboardDelete;
keyboard.okAction = ActionKeyboardOk;
freeink::ui::keyboard(ui, keyboardRect, keyboard);
```

Built-in layout IDs are `QwertyEn`, `AzertyFr`, `QwertzDe`, `SpanishEs`,
`CyrillicRu`, `CyrillicUk`, `CyrillicBe`, `CyrillicKk`, and `HebrewIl`.
Normal ASCII keys report their code point in `ActionEvent::value`; localized
keys use stable non-ASCII values so firmware can map the selected key back to
the active layout's UTF-8 output string. Visible glyph coverage depends on the
active `DrawTarget` font asset, so devices shipping wider language support
should include matching Noto Sans glyph ranges in their generated bitmap font or
use a renderer with native text shaping.

An app that reaches more than one script sets `builtinKeyboardLayout`'s
`langKey` flag and a `KeyboardProps::langAction`, which puts a script-switch key
in the bottom row. It draws a globe and takes no label of its own: which layout
is active is already visible in the letter keys. The flag is off by default and
the Latin layouts respect it, so a single-script keyboard renders without the
key; the non-Latin layouts carry it either way, since a keyboard with no Latin
letters cannot type a Wi-Fi password or a URL.

Reader screens can register invisible tap zones over the page while drawing
chrome separately:

```cpp
const freeink::ui::TapZone zones[] = {
    {leftThird, ActionPrevPage},
    {centerThird, ActionShowMenu},
    {rightThird, ActionNextPage},
};
freeink::ui::TapZonesProps taps;
taps.zones = zones;
taps.count = 3;
taps.swipeLeft = ActionNextPage;
taps.swipeRight = ActionPrevPage;
tapZones(ui, pageRect, taps);

freeink::ui::ReaderChromeProps chrome;
chrome.top.title = bookTitle;
chrome.bottom.trailing = progressLabel;
chrome.bottom.showProgress = true;
chrome.bottom.progress.value = percent;
chrome.bottom.progress.max = 100;
readerChrome(ui, ui.safeRect(), chrome);
```

Library screens can use `bookCard` for list views or `coverGrid` for visual
selection:

```cpp
freeink::ui::BookCardProps card;
card.title = book.title;
card.author = book.author;
card.meta = book.progressLabel;
card.progress = book.percent;
card.coverSize = {62, 84};
card.gap = 14;              // cover-to-title/author/bar spacing
card.textProgressGap = 8;   // keeps title/author clear of the bottom bar
card.action = ActionOpenBook;
card.value = bookIndex;
bookCard(ui, rowRect, card);
```

`bookCard` accepts `progressLabel` and `progressText` to place a percentage or
other short label before the bar, separated by `progressLabelGap`. Set
`centerTextOnCover = true` to center the title/author block against the cover;
the block shifts upward if needed to leave room for the progress row.

Both `bookCard` and `coverGrid` default to highlighting the whole
card/cell when selected. Set `selectionIndicator` to the `CoverFrame` mode
(`BookCardSelectionIndicator::CoverFrame` / `CoverGridSelectionIndicator::CoverFrame`)
to draw a frame around the cover art instead, tuned with
`selectedCoverFrameGap`/`Width`/`Radius`. Both also accept a `coverPainter`
callback, so the app can render decoded cover art into the slot rect while the
component still owns layout, the dithered placeholder, and selection chrome.
Grid titles default to centered across the cell. Set `labelAlign = TextAlign::Left`
and `labelFollowsCover = true` to left-align titles within the cover slot's width;
`labelInset` is applied inside those bounds.
`coverGrid` draws a scroll indicator when its contents overflow the visible
rows (`scrollIndicator`, `scrollIndicatorWidth`/`Gap`); pair it with the
`coverGridVisibleCells()` and `coverGridTopIndexFor()` helpers to keep the
selected cell on a stable page without app-side paging math.

`batteryIndicator` defaults to a battery-glyph (`BatteryIndicatorStyle::Icon`).
Switch `style` to `Bar` for a plain fill that the status/cluster layout can
size freely: `barFill` selects a solid, dithered, or segmented fill,
`barDirection`/`barOrientation` set the fill axis and growth direction,
`barTrack` adds a hairline, outline, or dithered track, and `barCaps`/`barRadius`
soften the ends.

Transient and edge-case surfaces are included too:

- `contextMenu`: long-press/menu-button command list for a selected book
- `toast`: static e-paper-safe notice such as "Saved" or "Sync failed"
- `messagePanel`: empty/error/loading panel with optional progress and retry
  action

### Styling

Every interactive component resolves a `StyleSet` — one `BoxStyle`
(background, foreground, border, radius, corner mask) per interaction state.
Rounded looks are first-class: `BoxStyle.radius` applies to both background
fills and borders, `tabBar` renders filled pill tabs, and
`ListProps.hugContents` shrinks selection pills to the label width — no
custom drawing code needed.

### Drawing primitives

The `DrawTarget` interface is small but covers everything hand-rolled
e-reader chrome typically needs:

- `fill` / `stroke` with per-corner radius masks (`Corners`) — cards that
  round only their top or bottom band
- `line()` — underlines, dividers, key-glyph art
- `triangle()` — selection markers, arrows, bookmark notches, battery bolts
- `text()` honoring alignment, `maxLines` wrapping, and ellipsis truncation —
  and implementors don't write that algorithm: `layoutText()` provides
  SDK-owned greedy word wrap, hard `\\n` breaks, character breaking for
  over-wide words, ellipsis shrinking, alignment, and vertical centering,
  built only on `measureText`. A target's `text()` reduces to drawing the
  emitted single-line runs; targets with a native bidi/kerning-aware wrapping
  pipeline (like the `GfxRenderer` adapter) keep using their own
- `measureWrappedText()` — the bounds of text wrapped into a width (widest
  line × line count), built on `layoutText` so it agrees exactly with what
  renders. Use it to reserve space for wrapping titles, auto-size dialogs
  (`optionDialogHeight()` is sugar over it), or compute row heights — never
  estimate line counts from single-line `measureText`
- `bitmap()` for icon masks

Components build on these: `keyGrid`, `keyboard`, and `qwertyKeyboard` draw space-bar and
delete-arrow glyph art themselves for special keys without labels,
`stepperRow` draws plus/minus controls as centered strokes instead of relying
on font glyph sizing, the `list` component offers `Underline` and `Triangle`
selection markers alongside fill/outline/pill styles, and `batteryIndicator`
draws a triangle-built lightning bolt while charging (or an app-supplied icon).
`testThemePrimitiveParity` in the host suite locks this in.

### Rotation and Scaling

Whole-screen rotation is inherited from the renderer: layout happens in
logical coordinates, `DeviceContext.orientation` reports the active
orientation, and the adapter's drawing pipeline maps to panel space. Remap
touch coordinates app-side if the panel's touch space differs.

Per-element rotation — an element rotated *relative to* its screen, such as
side-bezel button hints — rides on `TextStyle.rotation` for labels and the
`rotation` parameter of `DrawTarget::bitmap()` for icons (`CW90`, `R180`,
`CCW90`). Rotated text is single-line, aligned along the rotated axis; the
`GfxRenderer` adapter implements `CW90` natively and draws other text
rotations unrotated as a visible fallback. Icon rotation is exact for
standard row-major masks at any angle, composing freely with every
`BitmapMode`.

Smaller displays scale through layout, not transforms: rects and flex splits
adapt to any `DeviceContext` size, themes override sizes per device, and font
slots bind smaller font ids — deliberate, since fractional glyph scaling
produces mush on a 1-bit panel. Bitmaps do scale: every `BitmapMode`
(`Center`, `Stretch`, `Contain`, `Cover`, `Tile`, `TileX`, `TileY`) is
implemented via `forEachBitmapPixel()`, a shared nearest-neighbor sampling
helper that adapters drive with a per-pixel callback — host-tested, clipped
to the target rect, suitable for icons and pattern fills.

### Virtualized Lists

`list` never creates a node per item. The app owns the full item array plus the
scroll state; the component lays out, draws, and registers interactions only
for the rows that fully fit in the rect, and draws a right-edge scroll
indicator when the list overflows:

```cpp
const uint16_t visible = freeink::ui::listVisibleRows(rect, theme.rowHeight);
topIndex = freeink::ui::listTopIndexFor(selectedIndex, topIndex, visible, count);

freeink::ui::ListProps props;
props.items = items;
props.count = count;            // total items, not just visible ones
props.topIndex = topIndex;      // first row drawn at the top of the rect
props.selectedIndex = selectedIndex;
props.action = ActionOpenBook;
freeink::ui::list(ui, rect, props);
```

`listTopIndexFor` scrolls the window the minimal amount to keep the selection
visible and clamps to the valid range, so GPIO up/down navigation gets correct
scrolling for free.

Rows are not all the same height: a wrapped label or subtitle grows one, so a
layout routinely fits fewer indexes than `listVisibleRows()` estimates. Screens
that scroll (swipe or button navigation) should therefore own a `ListNav` and
call `screen.syncListViewport(nav, props, count)` right before
`list()`. `list()` reports the viewport it actually laid out back through
`props.nav`. Rendering, hit targets, preview rows, and the scroll indicator
share that measurement. The indicator uses the current layout immediately.
A selection that falls below variable-height rows may require another layout;
clear and rebuild before displaying:

```cpp
for (int pass = 0; pass < 8; ++pass) {
  // Clear the framebuffer and redraw chrome here.
  app.render();
  if (!nav.consumeRebuildNeeded()) break;
}
```

For input and rendering on separate tasks, submit navigation without acquiring
an e-ink refresh lock:

```cpp
// Input task: logical selection changes immediately, including for Confirm.
nav.requestSelection(nextIndex);
// Or scroll without changing selection; multiple pending deltas accumulate.
nav.requestScroll(nav.inputPageRows());
// Notify/schedule a render after either request.
```

`requestSelection()` supersedes pending scroll deltas. A subsequent
`requestScroll()` applies after following that selection. `syncToProps()`
consumes requests on the render task and captures the selection used by that
frame, so input arriving during drawing cannot change its layout feedback.
When data shrinks, an atomic compare-and-exchange clamps a stale selection
without overwriting a newer input request.
`inputPageRows()` is an atomic snapshot of the most recently measured page
(initially 1). Pending scroll deltas saturate at ±65,535 rows.

Use one input producer and one render consumer. `selected` and `followOnBuild`
are atomic; use `nav.selected.load()` when passing the value to a template such
as `std::min`, or when capturing it with `auto`. Other fields, `follow()`,
`scrollBy()`, and `pageRows()`/`pageRowsFor()` remain render-owned. Resetting or
copying a nav requires quiescent access. List items and their strings must
remain valid and stable during the build; atomics do not synchronize app data.
For a tab ring with index 0 reserved for the tab bar, pass `selectionOffset = 1`
to `syncToProps()`; layout and follow feedback then use row indexes consistently.

`Screen::list()` sizes default rows from their actual label, subtitle, value,
icon and wrapping. Button devices use `theme.listRowPaddingY` (4 pixels per
side). Touch devices use `listTouchRowPaddingY` (8 pixels per side), a
`listTouchMinRowHeight` of 56 pixels, and at least `listTouchRowGap` (6 pixels)
between rows unless the caller explicitly sets the gap. Device and theme hit
target minimums are still enforced. These touch defaults provide comfortable
spacing rather than using the smallest valid hit target as the row design.
`theme.rowHeight` sizes generic controls; it no longer reserves two text lines
for every list item. Use `theme.listMinRowHeight` for a deliberate list minimum,
or a positive `props.rowHeight` for a particular list.

Use `screen.syncListViewport(nav, props, count, selectionOffset)` before loading
a virtualized window or calling `screen.list(props)`. It resolves the same fonts,
padding and minimum as drawing, then computes the allocation bound and applies
navigation. `nav.visibleRows` is an upper bound; load one extra item for a
trailing preview. The rendered full-row count remains the swipe page size.

For raw `list(frame, rect, props)`, an unset height still falls back to 36 pixels.
An explicit height with `rowPaddingY = -1` retains legacy padding. Set a
nonnegative `rowPaddingY` to request content plus padding with that minimum.

With `partialTrailingRow = true`, the next row uses exactly the same text,
value, icon, and toggle layout as a full row. It is clipped at the viewport
edge and contributes neither an interaction nor a navigation row. A row whose
bottom exactly meets the viewport edge is a full, selectable row; no trailing
gap is required. A preview clips the entire next section block, including its
inline heading. The heading itself can signal that more content follows even
when the book beneath it is still outside the viewport. Decorative section
padding does not count toward `partialTrailingMinHeight`. Section and row
spacing remain unchanged, and previews remain non-interactive.

Pixel clipping is optional for custom `DrawTarget` implementations: implement
`clipRect()` and `setClipRect()` to enable previews. `DisplayTarget` supports
it, `InvertedDrawTarget` forwards it, and `GfxRendererTarget` supports it when
the application's renderer provides `setClipRect(x, y, width, height)`.
Targets without clipping omit partial rows. A preview restores the previous
clip after drawing, so it cannot spill into a footer or alter later painting.

### Dialogs

`optionDialog` composes a panel, up to three text slots, and a row (or
column) of option buttons that route through the normal action table, with an
optional dithered scrim behind the panel. The slots cover the common
confirmation shapes without hand-rolled cards: a small caption (`title`), a
prominent multi-line headline that reserves exactly the lines it wraps to
(`headline`), and a body line (`message`) — each honoring its own style's
font, alignment, and maxLines:

```cpp
const freeink::ui::DialogOption options[2] = {
    {tr(STR_SKIP), ActionSkipConfirmed},
    {tr(STR_CANCEL), ActionDismiss},
};
freeink::ui::OptionDialogProps props;
props.title = tr(STR_SKIP_THIS_EVENT);   // small caption
props.headline = event.title.c_str();    // large, wraps up to maxLines
props.headlineText = theme.titleText;
props.headlineText.maxLines = 2;
props.message = timeRange.c_str();       // body detail
props.options = options;
props.optionCount = 2;
props.dimBackground = true;
freeink::ui::optionDialog(ui, freeink::ui::centeredRect(ui.safeRect(), {356, 172}), props);
```

### Status Bars and Overlays

`statusBar` lays out measured clusters instead of fixed splits: an optional
leading icon plus up to two leading strings on the left, up to two trailing
strings on the right, and a centered title that falls back to the space
between the clusters when either side is wide. `leadingReserve` keeps the
layout clear of app-drawn chrome (such as a custom battery glyph), and a
bottom-anchored progress bar is built in.

The same component doubles as a page overlay: draw it into a top- or
bottom-anchored rect with `fillBackground = true` over pre-rendered content.

## Theme Runtime

Ownership is split deliberately: **the SDK owns the in-memory types
(`ThemeTokens`, `ThemeDocument`, `StyleSet`, `AssetResolver`); apps own JSON
and storage parsing.** FreeInkUI never reads files, parses JSON, or allocates —
a firmware parses its theme files (with whatever JSON library and caching it
already has) into `ThemeTokens` plus its own extension structs, then renders
from those. This keeps the UI package dependency-free and lets each app evolve
its theme format independently.

`ThemeTokens` is the compact in-memory shape a firmware renders from:

- font ids: small/body/title
- spacing and standard heights
- text styles
- component style sets for buttons, list rows, keys, popups, and text fields

`ThemeDocument` adds identity and an optional `AssetResolver`. The resolver is
host-owned so file IO, image parsing, caching, and decompression stay in the
firmware rather than in the generic UI package.

A theme file shaped for these tokens looks like:

```json
{
  "schema": 1,
  "id": "minimal",
  "name": "Minimal",
  "tokens": {
    "font": {
      "small": "small",
      "body": "ui12",
      "title": "ui12"
    },
    "space": {
      "xs": 2,
      "sm": 4,
      "md": 8,
      "lg": 16
    },
    "size": {
      "row": 44,
      "header": 48,
      "footer": 40,
      "progress": 4,
      "minTouch": 44
    }
  },
  "components": {
    "button.primary": {
      "normal": {"bg": "white", "fg": "black", "border": "black"},
      "selected": {"bg": "black", "fg": "white"}
    },
    "list.row": {
      "selected": {"bg": "black", "fg": "white"}
    },
    "statusBar.reader": {
      "progressHeight": 4
    },
    "keyboard.key": {
      "normal": {"bg": "white", "fg": "black"},
      "focused": {"border": "black"}
    }
  },
  "assets": {
    "icons.book": "icons/book.bmp",
    "patterns.panel": "patterns/panel.bmp"
  },
  "devices": {
    "x3": {
      "size": {"row": 42}
    }
  },
  "extensions": {
    "my-reader": {
      "readingStatsFooter": true
    }
  }
}
```

The token and state-style sections are part of the one theme schema, parsed by
the app's theme loader alongside any app-specific fields. App-only features
live under `extensions.<namespace>`: each app reads its own namespace and
ignores the others, so multiple firmwares can share one theme file. New
styling should be expressed as tokens plus app-owned extension structs rather
than ever-growing hardcoded theme classes.

## State

All interactive elements use a compact state bitmask:

- `StateSelected` for selected/current values
- `StateFocused` for GPIO/focus navigation
- `StateActive` for a pressed/touched element
- `StateDisabled` for inert controls
- `StateChecked` for toggles/check rows

The app supplies stable state; the UI runtime adds transient focus/active state
when it resolves styles.

## Bitmaps

Bitmaps are general paint sources, not only icons:

- icon: small fixed-size symbol
- image: content image/cover/logo
- pattern: tiled/stretched fill for a rectangle

`BitmapRef` describes borrowed bitmap bytes. `BitmapFill` selects `Center`,
`Stretch`, `Contain`, `Cover`, `Tile`, `TileX`, or `TileY`.

Large image decoding remains app/renderer-owned. Pattern fills draw directly
into the target rather than decoding into a full temporary buffer.

`AssetRef` lets components refer to an on-storage asset by id/path while
keeping the actual lookup host-owned:

```cpp
class AppAssets : public freeink::ui::AssetResolver {
 public:
  freeink::ui::BitmapRef bitmapFor(const freeink::ui::AssetRef& asset) override {
    return cache.lookupOrLoad(asset.path);
  }
};
```

## Inverted UI (Dark Mode)

Whole-UI inversion is one call, at the draw-target level rather than per
component. `InvertedDrawTarget` wraps any `DrawTarget` and flips every color
drawn through it — black↔white, light↔dark gray, dithers included — so
component defaults, theme styles, and app-drawn chrome all invert together:

```cpp
freeink::ui::GfxRendererTarget real(renderer);
freeink::ui::InvertedDrawTarget target(real, settings.darkMode);
freeink::ui::Frame<32> ui(target, device, input, interactions);
// ... render exactly as in light mode ...
```

Flip `target.setEnabled(...)` from a setting and the next frame renders
inverted; when disabled the wrapper is a pure passthrough. The screen clear
stays app-owned (clear to black when inverted). No per-draw-call dark-mode
flags needed anywhere.

## Adapters

FreeInkUI itself has no dependencies. Optional header-only adapters bridge it
to common stacks, and only compile in firmwares that include them:

- **`FreeInkUIGfxRenderer.h`** — `GfxRendererTarget`, a `DrawTarget` over the
  `GfxRenderer` drawing library: dither-mapped colors, per-corner rounded
  rects, and text measurement/truncation/wrapping through the renderer's own
  pipeline. `deviceContext()` derives screen size and orientation.
  `GfxRendererFrame<MaxInteractions>` bundles the target, input snapshot,
  interaction buffer, and `Frame` into one stack object (binding the small/
  body/title font ids), so a render pass over the `GfxRenderer` stack is a
  single declaration instead of four.
- **`FreeInkUIInputManager.h`** — builds the per-frame `InputSnapshot` from
  the SDK's `InputManager`:

```cpp
#include <FreeInkUIInputManager.h>

inputManager.update();
freeink::ui::InputSnapshot input = freeink::ui::snapshotFrom(inputManager);
```

`ButtonBindings` overrides the default UP/DOWN→focus, LEFT/RIGHT→prev/next,
CONFIRM/BACK mapping per board. Long-press and swipe synthesis stay
app-owned. Apps with their own input layer write the same few lines against
it.

Touch hit areas are declarative: `minTouchSize` center-expands small targets,
`ButtonProps.hitPadding` extends a button's tap band per edge (give adjacent
controls contiguous, non-overlapping bands instead of overlapping centered
expansion), and hit rects within 12px of a screen edge snap to the bezel —
all composed in `ensureMinTouchRect`, one interaction per control.

Touch orientation mapping is SDK-owned: `touchToLogical()` converts
normalized panel-native portrait coordinates to the logical frame for any
`DeviceContext.orientation`, with `flipX`/`flipY` for mirrored panel mounting
(a board property — set it once per device, not per app). The
orientation-aware `snapshotFrom(input, device, flipX, flipY)` overload
returns taps already mapped, so hit-testing works without hand-derived
transforms:

```cpp
freeink::ui::InputSnapshot input =
    freeink::ui::snapshotFrom(inputManager, device, TOUCH_FLIP_X, TOUCH_FLIP_Y);
```

## Testing

FreeInkUI is freestanding C++17 with no Arduino or ESP-IDF dependency, so the
layout, routing, focus, and virtualization logic runs in plain host unit
tests:

```sh
libs/ui/FreeInkUI/test/host/run.sh
```

The suite uses a recording `FakeDrawTarget` and covers stack layout math,
touch/focus/gesture routing (including stale-focus and disabled-element
guards), list virtualization windows, component interaction registration, and
drawing-primitive parity. New UI logic should land with a host test next to
it.

## Adopting FreeInkUI in an Existing Firmware

1. Pick or write a `DrawTarget`. There are two paths, and text wrapping is
   the fork between them:
   - Firmwares built on the `GfxRenderer` drawing stack use the SDK's
     `GfxRendererTarget` directly — it wraps and truncates through that
     renderer's own bidi/kerning-aware text pipeline.
   - Everything else (lightweight glyph engines, bare framebuffers)
     implements the `DrawTarget` methods over its own renderer and delegates
     `text()` to the SDK's `layoutText()` — wrap, ellipsis, alignment, and
     vertical centering come from the SDK; the target only draws the emitted
     single-line runs.
2. Build the per-frame `InputSnapshot` from the firmware's input layer (the
   `FreeInkUIInputManager.h` adapter covers the SDK's own `InputManager`).
3. Parse the theme format's token/state-style sections into `ThemeTokens`
   plus app extension structs in the existing theme loader.
4. Port one screen of chrome first — a status bar + content slot + control
   bar split is a good proof of pipeline.
5. Move shared surfaces screen-by-screen: headers, lists, button hints,
   popups, keyboards. Each port should delete the hand-rolled layout code it
   replaces.

App-specific page rendering (book text layout, image decoding) stays
app-owned: FreeInkUI hands the app a computed slot rect and the app renders
into it.

## Surface Coverage

The host suite exercises the full range of e-reader chrome against the
component set:

- reader status bars: clock, app-formatted page/percent text, centered
  chapter/book title with cluster-aware fallback, themed progress thickness,
  composable battery glyph
- top/bottom status overlays drawn over pre-rendered pages with an opaque
  background
- keyboard entry: character grids composed with special-key rows
  (shift/mode/space/delete/confirm via `KeyKind`), glyph art, secondary
  labels, and a chunk-measured `textField` cursor that stays accurate for
  long URLs and passphrases (password masking stays app-side — pass the
  masked string)
- menus and settings: rows with label, subtitle, right-aligned value, icon,
  and the full selected/focused/active/disabled state set; disabled rows
  register no interactions; section header rows (`ListItem::isHeader`) render
  shorter, underlined, and non-interactive with configurable section padding
- sleep/standby screens: app-drawn cover slot plus title block and a stats
  overlay composed from `metricCard` cells, icons, and `progressBar` — no
  bespoke surface needed
- statistics dashboards: value/label cells (`metricCard`), outlined section
  cards with dividers, and horizontal bar charts built from `progressBar`
  with `minFill` so tiny nonzero values stay visible
- pill tab bars, hug-content menus, per-corner rounded cards, and the
  `Underline`/`Triangle` selection markers

## Recent additions

- **`flatButtonStyles(radius)`** — the borderless sibling of
  `outlinedButtonStyles`: nothing drawn at rest, a light fill for
  selected/pressed feedback. Assign once (`theme.button =
  ui::flatButtonStyles(8)`) for the "just text on paper" look.
- **Dropdown upgrades** — `DropdownProps` gained a leading `icon`
  (settingRow-style), a `subtitle` two-line layout (label over current
  selection), and a legible default chevron (10 px, 2 px stroke — the old
  8 px/1 px indicator was invisible on e-paper).
- **`ButtonProps.iconSize`** — scales a button icon to any square size via
  nearest-neighbor `Contain`, so one asset serves several button sizes.
- **`DisplayTarget::setOrientation()`** — runtime portrait/landscape (and
  flipped) switching; refresh your `DeviceContext` via `deviceContext()`
  and repaint. Touch mapping follows automatically.
- **Runtime glyph fallback** — `DisplayTarget::setGlyphFallback(...)` takes
  a `RuntimeGlyphSource` consulted whenever the bitmap font lacks a glyph,
  sized per slot and dithered like the alpha fonts. This is how UI chrome
  renders scripts too large to pre-bake (Hangul, CJK) from a TTF on the
  card.
- **Opt-in bridges** (compilable only when the paired library is present,
  like `FreeInkUIGfxRenderer.h`):
  - `FreeInkUIBookFont.h` — `BitmapBookFont` lets FreeInkBook read books
    with the bundled bitmap font (typographic punctuation normalized to
    ASCII equivalents), and `TtfGlyphSource` adapts a FreeInkBook `TtfFont`
    as the chrome glyph fallback above.
  - `FreeInkUIIcon.h` — `bitmapFromIcon()` adapts `freeink::Icon` assets
    (generated at any size by `libs/assets/Icons/tools/gen_icons.py`) to
    the `BitmapRef` every component takes.

## Publication detail page

`publicationPage` composes a store/library-style book detail screen for OPDS
browsers. It provides a portrait cover (or a typeset fallback), title, author,
series and format, followed by availability, copies, holds, loan terms, metadata,
and a plain-text description. The primary action stays at the bottom; optional
sample and full-description actions sit above it. All actions use the existing
touch and button/focus routing.

![Publication available to borrow](images/freeinkui-publication.svg)
![Publication on hold](images/freeinkui-publication-hold.svg)

```cpp
PublicationPageProps page;
page.book.title = publication.title.c_str();
page.book.author = publication.author.c_str();
page.book.format = "EPUB";
page.book.cover = loadedCover; // app-owned BitmapRef; an empty ref is supported
page.book.titleText.font = FONT_SLOT_TITLE; // configure this slot in your renderer
page.description = plainDescription.c_str();
page.metadata = localizedPublicationDetails.c_str();
page.availability.status = localizedAvailability.c_str();
page.availability.copies = localizedCopies.c_str();
page.availability.holds = localizedHolds.c_str();
page.primary.label = publication.purchase ? localizedBuyLabel.c_str() : "Borrow book";
page.primary.action = ActionAcquire;
page.primary.enabled = canAcquire && !requestPending;
page.more.label = "Full description";
page.more.action = ActionDescription;
// After screen.header(...) and any footer:
screen.publicationPage(page);
```

The SDK supplies presentation and action IDs, not acquisition policy. The app
must derive labels and enabled states from the actual link relations, account
state, and server capabilities. Use "Download" for an open-access acquisition,
"Place hold" only if the service supports placing a hold, and "Manage hold"
only when there is a corresponding action. A purchase label can include the
formatted price. Supply a sample button only when a sample link exists. While a
request runs, disable its button and supply a localized pending label; render
errors using the app's existing message/toast components.

Do not infer availability from missing fields: OPDS counts of `-1` are unknown,
while `0` is a valid count. Omit unknown counts with null/empty strings and use
an explicit "Availability unknown" label if needed. Convert HTML descriptions
to plain text before rendering. All strings and cover assets remain app-owned
and must live through the render call; no network/parser dependency is added to
FreeInkUI.

`publicationHeader` and `publicationAvailability` can also be used independently
with explicit rectangles. `PublicationPageProps` exposes padding, hero height,
action height, text styles, and full `ButtonProps` for each action. The page uses
the body rectangle remaining after app chrome; text is limited to complete lines
and lower-priority content is omitted on short displays. It does not scroll:
wire `more` to a full-description screen when needed. Prefer at least a 240×480
body and fonts sized for the device; firmware remains responsible for choosing
fonts and localizing every label (including `descriptionHeading`).

### Publication styling and physical buttons

Publication sections expose the same `StyleSet`/`State` conventions as other
FreeInkUI surfaces: normal, selected, focused, active, disabled, background,
foreground, borders, corner masks, and per-state radii. A positive `radius`
overrides all state radii; zero leaves the style values intact. `borderEdges`
controls which border edges paint. `padding` belongs to each surface separately.
The page's `enabled = false` (or `StateDisabled`) disables all child actions.

```cpp
page.radius = 12;
page.styles = outlinedButtonStyles();
page.padding = {20, 24, 20, 24};
page.sectionGap = 16;
page.actionGap = 10;
page.book.padding = {8, 8, 8, 8};
page.book.coverSize = {112, 168};
page.book.gap = 20;
page.book.titleGap = 12;
page.availability.styles = outlinedButtonStyles();
page.availability.radius = 8;
page.availability.padding = {12, 16, 12, 16};
page.availability.divider = Paint::none();
page.primary.radius = 8;
page.primary.padding = {12, 20, 12, 20};
page.primary.styles = outlinedButtonStyles();
```

Additional controls include header text gaps/line limits, cover mode, placeholder
`coverStyle` and `coverPadding`, a `coverPainter` hook, availability divider and
gap, metadata/description line limits, and heading weight flags. Real bitmap
corner masking is app-owned through the cover painter. Button padding is now
part of `ButtonProps` for all buttons, with the existing `{2,4,2,4}` default.
Publication actions preserve their `inputMask`, `value`, `styles`, `radius`,
`hitPadding`, and minimum touch size. Their rows grow to fit the requested
minimum size and padded text height.

For hardware buttons, map Next/Previous to `InputSnapshot.focusNext` /
`focusPrev` and Select to `confirm`. The default `InputDefault` mask includes
focus and confirm. Focus order starts with the primary action, then secondary,
full description, and any optional header/availability actions. Disabled actions
are skipped; focus wraps and survives redraws using the shared interaction
buffer. Section `action`, `value`, and `inputMask` can make the cover/title or
availability panel focusable too. Keep the app's Back action in its header or
footer. The full-description action is equally reachable by physical buttons;
the app owns that destination screen.

## Catalog shelves

`catalogCover`, `coverShelf`, and `catalogPage` present OPDS groups as horizontal
cover shelves, following the group-heading and fixed-width-card structure in
Common Stacks. The catalog does not expand a group into a vertical book list.
Each shelf has its own horizontal window, Previous/Next buttons, optional
"See all", and titles on the covers. Set `card.titleOnCover = false` for the
Common Stacks variant with titles below the art. Authors appear underneath.

![Catalog with horizontal shelves](images/freeinkui-catalog.svg)

```cpp
// Persistent app state, not render-local:
CatalogWindow shelfWindows[3];
CatalogWindow catalogWindow;
CoverShelfProps shelves[3];

// Configure each shelf using data owned by your app:
for (int i = 0; i < 3; ++i) {
  shelves[i].title = groupNames[i];
  shelves[i].items = groupItems[i]; // arrays of CatalogItem
  shelves[i].count = groupCounts[i];
  shelves[i].window = &shelfWindows[i];
  shelves[i].card.action = ActionOpenPublication;
  shelves[i].card.radius = 6;
  shelves[i].card.padding = {4, 4, 4, 4};
  shelves[i].next.label = ">";
  shelves[i].next.action = ActionShelfNext;
  shelves[i].next.value = i;
  shelves[i].previous.label = "<";
  shelves[i].previous.action = ActionShelfPrevious;
  shelves[i].previous.value = i;
  shelves[i].seeAll.label = "See all";
  shelves[i].seeAll.action = ActionOpenGroup;
  shelves[i].seeAll.value = i;
}

CatalogPageProps catalog;
catalog.shelves = shelves;
catalog.count = 3;
catalog.window = &catalogWindow;
catalog.next.label = "More groups";
catalog.next.action = ActionCatalogNext;
catalog.previous.label = "Previous groups";
catalog.previous.action = ActionCatalogPrevious;
screen.catalogPage(catalog);
```

In action handlers, call `shelfWindows[event.value].next()` or `.previous()` for
horizontal navigation, and `catalogWindow.next()` / `.previous()` for vertical
group navigation, then redraw. Validate group IDs as you do for other app
callbacks. `CatalogItem.value` is a stable app identifier sent with the open
publication action; assign unique values across groups or use distinct group
open actions. Opening a book is separate from moving the shelf.

Next/Previous/Confirm focus navigation reaches all visible covers, shelf paging,
See all, and catalog paging. No touchscreen is required. Navigation buttons
disable at boundaries. Horizontal movement is discrete paging for e-paper;
only visible covers are rendered. `CatalogWindow` preserves independent group
positions and clamps them when counts or viewport sizes change. Supply persistent
windows and navigation action handlers to browse beyond the initial window.
For swipes, set `CatalogPageProps.activeShelf` to the intended group index (default
`-1` disables swipes); only that visible shelf claims global swipe events.
Standalone shelves can opt in with `swipeNavigation`. The shared router's swipe
events do not carry a shelf target, so the app chooses the active group.

For large feeds, use `itemProvider(index, userData)` instead of `items`. It is
called only for visible cards. Providers, cover assets, decoding, feed loading,
and string lifetime remain app-owned. `coverPainter` supports custom covers;
missing images use a neutral placeholder with the title retained. Shelf and
catalog surfaces expose styles, radius, borders, padding, gaps, enabled state,
and size controls; each card also has title-band colors/padding, text styles,
author height, cover mode, and interaction masks. The `Screen::coverShelf`
wrapper applies theme typography; `catalogPage` accepts independently styled
shelf props, so assign each shelf's fonts explicitly when using multiple styles.

Use app-level loading/error/empty-catalog screens as appropriate. A shelf's
`emptyLabel` is customizable. No OPDS requests or CrossPoint firmware screen
changes are performed by these SDK components.

### Evenly distributed cover columns and tabs

Set `CoverGridProps::columnLayout` to `CoverGridColumnLayout::SpaceBetween` to
keep cover cells at their natural width (cover width plus cell insets) and
spread the remaining width between columns. Both outside cell edges stay fixed;
rounding is absorbed between columns. A single column is centered. `gap` is the
minimum spacing; layouts that do not fit fall back to equal-width cells.

Set `TabBarProps::layout` to `TabBarLayout::SpaceBetween` for the same distribution
of tab slots. `distributedSlotWidth` specifies the slot width; zero uses the
widest natural tab. Slot bounds include the tab insets, and touch targets and
selection indicators follow the slots. A single tab is centered. If the slots
and minimum `gap` do not fit, the bar falls back to equal-width slots.
Existing default layouts are unchanged. Neither option allocates memory.
