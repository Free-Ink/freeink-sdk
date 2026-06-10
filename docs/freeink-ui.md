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

```cpp
freeink::ui::InteractionBuffer<32> interactions;
freeink::ui::InputSnapshot input = readInput();
freeink::ui::GfxRendererTarget draw(renderer);  // FreeInkUIGfxRenderer.h
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
- `gestureBar`
- `header`
- `list` (virtualized; see below — supports hug-content pill rows and
  selection markers)
- `progressBar`
- `statusBar`
- `tabBar` (pill or underline-style tabs with optional divider)
- `popup`
- `optionDialog`
- `textField`
- `keyGrid`
- `metricCard`
- `batteryIndicator`
- `coverCarousel` (prev/center/next cover slots with selection chrome and
  tap/swipe/button routing; the app renders cover art into the returned
  slot rects, so image decoding and frame caching stay app-owned)

These cover the shared surfaces of a typical e-reader or appliance UI: home
menus, settings lists, button hints, status/progress bars, popups,
confirmation dialogs, text entry fields, keyboard grids, statistics cards,
and battery glyphs. Applications draw app-specific content directly into a
slot, such as a book page renderer or a cover image.

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
- `bitmap()` for icon masks

Components build on these: `keyGrid` draws space-bar and delete-arrow glyph
art itself for `KeyKind::Space`/`KeyKind::Delete` keys without labels, the
`list` component offers `Underline` and `Triangle` selection markers
alongside fill/outline/pill styles, and `batteryIndicator` draws a
triangle-built lightning bolt while charging (or an app-supplied icon).
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
