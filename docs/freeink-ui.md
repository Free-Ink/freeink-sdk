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
freeink::ui::DisplayTarget target(display.getFrameBuffer(), display.getDisplayWidth(),
                                  display.getDisplayHeight(), display.getDisplayWidthBytes());
AppState state;
App app(target, target.deviceContext());

void setup() {
  app.setScreen(homeScreen, &state);
  app.on(ActionOpen, handleOpen, &state);
}

void loop() {
  freeink::ui::InputSnapshot input = readInputSnapshot();
  freeink::ui::ActionEvent event = app.render(input);
  switch (app.lastRenderRefreshHint()) {
    case freeink::ui::RefreshHint::Full:
    case freeink::ui::RefreshHint::Clean:
      display.displayBuffer(FreeInkDisplay::FULL_REFRESH);
      break;
    case freeink::ui::RefreshHint::Fast:
      display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
      break;
    case freeink::ui::RefreshHint::None:
      break;
  }

  if (app.invalidated()) {
    // An action handler changed state after this render pass. Render again on
    // the next loop iteration, or immediately if your firmware wants synchronous
    // UI updates.
  }
}
```

`FreeInkApp` does not own your display refresh policy. `lastRenderRefreshHint()`
describes the frame just drawn, while `invalidated()` / `refreshHint()` describe
whether an action handler changed state and a follow-up render is needed.

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
screen.qwertyKeyboard(keys, 144, freeink::ui::LayoutAnchor::Bottom);

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
      "okAction": "keyboardOk",
      "height": 144
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
`qwertyKeyboard`, `spacer`, `popup`, and `optionDialog`. Components that consume
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
// framebuffer FreeInkDisplay already owns (FreeInkUIDisplayTarget.h).
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

Fonts are plain data (`freeink::ui::BitmapFont`): a concatenated 1-bit glyph
bitmap plus a per-glyph metrics table. To use your own typeface:

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
`QwertyKeyboardProps::keyRadius`) when a product theme wants rounded controls.
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
qwertyKeyboard(ui, keyboardRect, keyboard);
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

Built-in layout IDs are `QwertyEn`, `AzertyFr`, `QwertzDe`, and `SpanishEs`.
Normal ASCII keys report their code point in `ActionEvent::value`; localized
keys use stable non-ASCII values so firmware can map the selected key back to
the active layout's UTF-8 output string. Visible glyph coverage depends on the
active `DrawTarget` font asset, so devices shipping wider language support
should include matching Noto Sans glyph ranges in their generated bitmap font or
use a renderer with native text shaping.

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

Both `bookCard` and `coverGrid` default to highlighting the whole
card/cell when selected. Set `selectionIndicator` to the `CoverFrame` mode
(`BookCardSelectionIndicator::CoverFrame` / `CoverGridSelectionIndicator::CoverFrame`)
to draw a frame around the cover art instead, tuned with
`selectedCoverFrameGap`/`Width`/`Radius`. Both also accept a `coverPainter`
callback, so the app can render decoded cover art into the slot rect while the
component still owns layout, the dithered placeholder, and selection chrome.
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
