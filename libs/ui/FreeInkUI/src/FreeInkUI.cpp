#include <FreeInkUI.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
// FreeInkUI's render pipeline (screen builders + text layout) runs deeper
// than Arduino's default 8 KB loopTask stack; the overflow shows up as a
// "Stack canary watchpoint triggered (loopTask)" panic and a reboot
// mid-interaction. Ship a roomier weak default so every FreeInkUI app gets
// it for free. Apps still override with the standard
// SET_LOOP_TASK_STACK_SIZE(...) macro — that strong definition beats this
// weak one, and this weak one beats the core's 8 KB weak default because
// app-side libraries link ahead of the Arduino framework archive.
__attribute__((weak)) size_t getArduinoLoopTaskStackSize(void) { return 16 * 1024; }
#endif

namespace freeink {
namespace ui {

StyleSet defaultButtonStyles() {
  StyleSet styles;
  styles.explicitlySet = true;
  styles.normal.background = Paint::solid(Color::White);
  styles.normal.foreground = Paint::solid(Color::Black);

  styles.selected.background = Paint::solid(Color::Black);
  styles.selected.foreground = Paint::solid(Color::White);

  styles.focused.background = Paint::dither(Color::LightGray);
  styles.focused.foreground = Paint::solid(Color::Black);

  styles.active.background = Paint::solid(Color::Black);
  styles.active.foreground = Paint::solid(Color::White);

  styles.disabled.background = Paint::solid(Color::White);
  styles.disabled.foreground = Paint::dither(Color::LightGray);
  return styles;
}

StyleSet defaultListRowStyles() {
  StyleSet styles;
  styles.explicitlySet = true;
  styles.normal.background = Paint::solid(Color::White);
  styles.normal.foreground = Paint::solid(Color::Black);

  styles.selected.background = Paint::solid(Color::Black);
  styles.selected.foreground = Paint::solid(Color::White);

  styles.focused.background = Paint::dither(Color::LightGray);
  styles.focused.foreground = Paint::solid(Color::Black);

  styles.active = styles.selected;

  styles.disabled.background = Paint::solid(Color::White);
  styles.disabled.foreground = Paint::dither(Color::LightGray);
  return styles;
}

StyleSet defaultKeyStyles() {
  StyleSet styles = defaultButtonStyles();
  return styles;
}

StyleSet defaultPopupStyles() {
  StyleSet styles;
  styles.explicitlySet = true;
  styles.normal.background = Paint::solid(Color::White);
  styles.normal.foreground = Paint::solid(Color::Black);
  styles.selected = styles.normal;
  styles.focused = styles.normal;
  styles.active = styles.normal;
  styles.disabled = styles.normal;
  return styles;
}

StyleSet plainStyles(Paint foreground) {
  StyleSet styles;
  styles.explicitlySet = true;
  styles.normal.foreground = foreground;
  styles.selected = styles.normal;
  styles.focused = styles.normal;
  styles.active = styles.normal;
  styles.disabled = styles.normal;
  return styles;
}

ThemeTokens defaultThemeTokens(FontId smallFont, FontId bodyFont, FontId titleFont) {
  ThemeTokens tokens;
  tokens.fontSmall = smallFont;
  tokens.fontBody = bodyFont;
  tokens.fontTitle = titleFont;
  tokens.smallText.font = smallFont;
  tokens.smallText.align = TextAlign::Left;
  tokens.bodyText.font = bodyFont;
  tokens.bodyText.align = TextAlign::Left;
  tokens.titleText.font = titleFont;
  tokens.titleText.align = TextAlign::Left;
  tokens.titleText.bold = true;
  tokens.button = defaultButtonStyles();
  tokens.listRow = defaultListRowStyles();
  tokens.key = defaultKeyStyles();
  tokens.popup = defaultPopupStyles();
  tokens.textField = defaultListRowStyles();
  tokens.textField.normal.border = Paint::solid(Color::Black);
  tokens.textField.normal.borderWidth = 1;
  tokens.textField.selected.border = Paint::solid(Color::Black);
  tokens.textField.selected.borderWidth = 2;
  return tokens;
}

ThemeTokens themeTokensForLineHeight(const int16_t lineHeight, const FontId smallFont, const FontId bodyFont,
                                     const FontId titleFont) {
  ThemeTokens tokens = defaultThemeTokens(smallFont, bodyFont, titleFont);
  if (lineHeight <= 0) return tokens;
  tokens.rowHeight = static_cast<int16_t>(lineHeight * 2 + 8);  // label + subtitle + breathing room
  tokens.headerHeight = static_cast<int16_t>(lineHeight + 26);
  tokens.footerHeight = static_cast<int16_t>(lineHeight + 22);
  if (lineHeight + 14 > tokens.minTouchSize) tokens.minTouchSize = static_cast<int16_t>(lineHeight + 14);
  if (lineHeight / 6 > tokens.spaceSm) tokens.spaceSm = static_cast<int16_t>(lineHeight / 6);
  return tokens;
}

namespace {

#define K(label, output, value) KeyboardKey{label, output, KeyKind::Normal, StateNormal, value, 1, true}
#define K2(label, output, value) KeyboardKey{label, output, KeyKind::Normal, StateNormal, value, 2, true}
#define KS(label, kind, value, units) KeyboardKey{label, nullptr, kind, StateNormal, value, units, true}
#define KA(label, output, value, alt) KeyboardKey{label, output, KeyKind::Normal, StateNormal, value, 1, true, alt}

// Optional digit row for the letter layers (builtinKeyboardLayout's numberRow
// flag). Each digit long-presses to its shift symbol; the shifted variant
// swaps the pair so shift-then-tap matches long-press output.
static const KeyboardKey NUM_ROW[] = {KA("1", "1", '1', "!"), KA("2", "2", '2', "@"), KA("3", "3", '3', "#"),
                                      KA("4", "4", '4', "$"), KA("5", "5", '5', "%"), KA("6", "6", '6', "^"),
                                      KA("7", "7", '7', "&"), KA("8", "8", '8', "*"), KA("9", "9", '9', "("),
                                      KA("0", "0", '0', ")")};
static const KeyboardKey NUM_SHIFT_ROW[] = {KA("!", "!", '!', "1"), KA("@", "@", '@', "2"), KA("#", "#", '#', "3"),
                                            KA("$", "$", '$', "4"), KA("%", "%", '%', "5"), KA("^", "^", '^', "6"),
                                            KA("&", "&", '&', "7"), KA("*", "*", '*', "8"), KA("(", "(", '(', "9"),
                                            KA(")", ")", ')', "0")};

static const KeyboardKey EN_ROW1[] = {K("q", "q", 'q'), K("w", "w", 'w'), K("e", "e", 'e'), K("r", "r", 'r'),
                                      K("t", "t", 't'), K("y", "y", 'y'), K("u", "u", 'u'), K("i", "i", 'i'),
                                      K("o", "o", 'o'), K("p", "p", 'p')};
static const KeyboardKey EN_ROW2[] = {K("a", "a", 'a'), K("s", "s", 's'), K("d", "d", 'd'), K("f", "f", 'f'),
                                      K("g", "g", 'g'), K("h", "h", 'h'), K("j", "j", 'j'), K("k", "k", 'k'),
                                      K("l", "l", 'l')};
static const KeyboardKey EN_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K("z", "z", 'z'),
                                      K("x", "x", 'x'), K("c", "c", 'c'), K("v", "v", 'v'), K("b", "b", 'b'),
                                      K("n", "n", 'n'), K("m", "m", 'm'),
                                      KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};
static const KeyboardKey EN_ROW4[] = {KS("?123", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                      KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 6),
                                      KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

static const KeyboardKey EN_SHIFT_ROW1[] = {K("Q", "Q", 'Q'), K("W", "W", 'W'), K("E", "E", 'E'), K("R", "R", 'R'),
                                            K("T", "T", 'T'), K("Y", "Y", 'Y'), K("U", "U", 'U'), K("I", "I", 'I'),
                                            K("O", "O", 'O'), K("P", "P", 'P')};
static const KeyboardKey EN_SHIFT_ROW2[] = {K("A", "A", 'A'), K("S", "S", 'S'), K("D", "D", 'D'), K("F", "F", 'F'),
                                            K("G", "G", 'G'), K("H", "H", 'H'), K("J", "J", 'J'), K("K", "K", 'K'),
                                            K("L", "L", 'L')};
static const KeyboardKey EN_SHIFT_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K("Z", "Z", 'Z'),
                                            K("X", "X", 'X'), K("C", "C", 'C'), K("V", "V", 'V'), K("B", "B", 'B'),
                                            K("N", "N", 'N'), K("M", "M", 'M'),
                                            KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey SYMBOL_ROW1[] = {K("1", "1", '1'), K("2", "2", '2'), K("3", "3", '3'), K("4", "4", '4'),
                                          K("5", "5", '5'), K("6", "6", '6'), K("7", "7", '7'), K("8", "8", '8'),
                                          K("9", "9", '9'), K("0", "0", '0')};
static const KeyboardKey SYMBOL_ROW2[] = {K("-", "-", '-'), K("/", "/", '/'), K(":", ":", ':'), K(";", ";", ';'),
                                          K("(", "(", '('), K(")", ")", ')'), K("$", "$", '$'), K("&", "&", '&'),
                                          K("@", "@", '@')};
static const KeyboardKey SYMBOL_ROW3[] = {KS("#+=", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K(".", ".", '.'),
                                          K(",", ",", ','), K("?", "?", '?'), K("!", "!", '!'), K("'", "'", '\''),
                                          K("\"", "\"", '"'), K("#", "#", '#'),
                                          KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};
static const KeyboardKey SYMBOL_ROW4[] = {KS("ABC", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                          KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 6),
                                          KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

// Second symbols page (the "#+=" layer): together with the first page it
// covers every printable ASCII character the letter layers don't.
static const KeyboardKey SYMBOL2_ROW1[] = {K("[", "[", '['), K("]", "]", ']'), K("{", "{", '{'), K("}", "}", '}'),
                                           K("<", "<", '<'), K(">", ">", '>'), K("^", "^", '^'), K("*", "*", '*'),
                                           K("+", "+", '+'), K("=", "=", '=')};
static const KeyboardKey SYMBOL2_ROW2[] = {K("_", "_", '_'), K("\\", "\\", '\\'), K("|", "|", '|'),
                                           K("~", "~", '~'), K("`", "`", '`'), K("%", "%", '%')};
static const KeyboardKey SYMBOL2_ROW3[] = {KS("123", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K(".", ".", '.'),
                                           K(",", ",", ','), K("?", "?", '?'), K("!", "!", '!'), K("'", "'", '\''),
                                           K("\"", "\"", '"'), K("#", "#", '#'),
                                           KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey FR_ROW1[] = {K("a", "a", 'a'), K("z", "z", 'z'), K("e", "e", 'e'), K("r", "r", 'r'),
                                      K("t", "t", 't'), K("y", "y", 'y'), K("u", "u", 'u'), K("i", "i", 'i'),
                                      K("o", "o", 'o'), K("p", "p", 'p')};
static const KeyboardKey FR_ROW2[] = {K("q", "q", 'q'), K("s", "s", 's'), K("d", "d", 'd'), K("f", "f", 'f'),
                                      K("g", "g", 'g'), K("h", "h", 'h'), K("j", "j", 'j'), K("k", "k", 'k'),
                                      K("l", "l", 'l'), K("m", "m", 'm')};
static const KeyboardKey FR_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K("w", "w", 'w'),
                                      K("x", "x", 'x'), K("c", "c", 'c'), K("v", "v", 'v'), K("b", "b", 'b'),
                                      K("n", "n", 'n'), K("é", "é", 1001),
                                      KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey DE_ROW1[] = {K("q", "q", 'q'), K("w", "w", 'w'), K("e", "e", 'e'), K("r", "r", 'r'),
                                      K("t", "t", 't'), K("z", "z", 'z'), K("u", "u", 'u'), K("i", "i", 'i'),
                                      K("o", "o", 'o'), K("p", "p", 'p')};
static const KeyboardKey DE_ROW2[] = {K("a", "a", 'a'), K("s", "s", 's'), K("d", "d", 'd'), K("f", "f", 'f'),
                                      K("g", "g", 'g'), K("h", "h", 'h'), K("j", "j", 'j'), K("k", "k", 'k'),
                                      K("l", "l", 'l'), K("ü", "ü", 1101)};
static const KeyboardKey DE_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K("y", "y", 'y'),
                                      K("x", "x", 'x'), K("c", "c", 'c'), K("v", "v", 'v'), K("b", "b", 'b'),
                                      K("n", "n", 'n'), K("m", "m", 'm'), K("ß", "ß", 1102),
                                      KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey ES_ROW1[] = {K("q", "q", 'q'), K("w", "w", 'w'), K("e", "e", 'e'), K("r", "r", 'r'),
                                      K("t", "t", 't'), K("y", "y", 'y'), K("u", "u", 'u'), K("i", "i", 'i'),
                                      K("o", "o", 'o'), K("p", "p", 'p')};
static const KeyboardKey ES_ROW2[] = {K("a", "a", 'a'), K("s", "s", 's'), K("d", "d", 'd'), K("f", "f", 'f'),
                                      K("g", "g", 'g'), K("h", "h", 'h'), K("j", "j", 'j'), K("k", "k", 'k'),
                                      K("l", "l", 'l'), K("ñ", "ñ", 1201)};
static const KeyboardKey ES_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K("z", "z", 'z'),
                                      K("x", "x", 'x'), K("c", "c", 'c'), K("v", "v", 'v'), K("b", "b", 'b'),
                                      K("n", "n", 'n'), K("m", "m", 'm'),
                                      KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardRow EN_ROWS[] = {{EN_ROW1, 10, 0}, {EN_ROW2, 9, 1}, {EN_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow EN_SHIFT_ROWS[] = {{EN_SHIFT_ROW1, 10, 0}, {EN_SHIFT_ROW2, 9, 1}, {EN_SHIFT_ROW3, 9, 0},
                                           {EN_ROW4, 3, 0}};
static const KeyboardRow SYMBOL_ROWS[] = {{SYMBOL_ROW1, 10, 0}, {SYMBOL_ROW2, 9, 1}, {SYMBOL_ROW3, 9, 0},
                                         {SYMBOL_ROW4, 3, 0}};
static const KeyboardRow SYMBOL2_ROWS[] = {{SYMBOL2_ROW1, 10, 0}, {SYMBOL2_ROW2, 6, 2}, {SYMBOL2_ROW3, 9, 0},
                                          {SYMBOL_ROW4, 3, 0}};
static const KeyboardRow FR_ROWS[] = {{FR_ROW1, 10, 0}, {FR_ROW2, 10, 0}, {FR_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow DE_ROWS[] = {{DE_ROW1, 10, 0}, {DE_ROW2, 10, 0}, {DE_ROW3, 10, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow ES_ROWS[] = {{ES_ROW1, 10, 0}, {ES_ROW2, 10, 0}, {ES_ROW3, 9, 0}, {EN_ROW4, 3, 0}};

static const KeyboardLayout EN_LAYOUT{EN_ROWS, 4};
static const KeyboardLayout EN_SHIFT_LAYOUT{EN_SHIFT_ROWS, 4};
static const KeyboardLayout SYMBOL_LAYOUT{SYMBOL_ROWS, 4};
static const KeyboardLayout SYMBOL2_LAYOUT{SYMBOL2_ROWS, 4};
static const KeyboardLayout FR_LAYOUT{FR_ROWS, 4};
static const KeyboardLayout DE_LAYOUT{DE_ROWS, 4};
static const KeyboardLayout ES_LAYOUT{ES_ROWS, 4};

// numberRow variants: the digit row prepended to each letter layer.
static const KeyboardRow EN_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {EN_ROW1, 10, 0}, {EN_ROW2, 9, 1}, {EN_ROW3, 9, 0},
                                          {EN_ROW4, 3, 0}};
static const KeyboardRow EN_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {EN_SHIFT_ROW1, 10, 0}, {EN_SHIFT_ROW2, 9, 1},
                                                {EN_SHIFT_ROW3, 9, 0},  {EN_ROW4, 3, 0}};
static const KeyboardRow FR_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {FR_ROW1, 10, 0}, {FR_ROW2, 10, 0}, {FR_ROW3, 9, 0},
                                          {EN_ROW4, 3, 0}};
static const KeyboardRow DE_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {DE_ROW1, 10, 0}, {DE_ROW2, 10, 0}, {DE_ROW3, 10, 0},
                                          {EN_ROW4, 3, 0}};
static const KeyboardRow ES_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {ES_ROW1, 10, 0}, {ES_ROW2, 10, 0}, {ES_ROW3, 9, 0},
                                          {EN_ROW4, 3, 0}};

static const KeyboardLayout EN_NUM_LAYOUT{EN_NUM_ROWS, 5};
static const KeyboardLayout EN_SHIFT_NUM_LAYOUT{EN_SHIFT_NUM_ROWS, 5};
static const KeyboardLayout FR_NUM_LAYOUT{FR_NUM_ROWS, 5};
static const KeyboardLayout DE_NUM_LAYOUT{DE_NUM_ROWS, 5};
static const KeyboardLayout ES_NUM_LAYOUT{ES_NUM_ROWS, 5};

#undef K
#undef K2
#undef KS
#undef KA

}  // namespace

const KeyboardLayout& builtinKeyboardLayout(KeyboardLayoutId id, bool shifted, bool symbols, bool numberRow) {
  // In the symbols layers `shifted` selects the second page: the shift slot
  // reads "#+=" on page one and "123" on page two, mirroring phone keyboards.
  // The symbols pages already carry digits, so numberRow only affects the
  // letter layers.
  if (symbols) return shifted ? SYMBOL2_LAYOUT : SYMBOL_LAYOUT;
  if (shifted && id == KeyboardLayoutId::QwertyEn) return numberRow ? EN_SHIFT_NUM_LAYOUT : EN_SHIFT_LAYOUT;
  switch (id) {
    case KeyboardLayoutId::AzertyFr:
      return numberRow ? FR_NUM_LAYOUT : FR_LAYOUT;
    case KeyboardLayoutId::QwertzDe:
      return numberRow ? DE_NUM_LAYOUT : DE_LAYOUT;
    case KeyboardLayoutId::SpanishEs:
      return numberRow ? ES_NUM_LAYOUT : ES_LAYOUT;
    case KeyboardLayoutId::QwertyEn:
    default:
      return numberRow ? EN_NUM_LAYOUT : EN_LAYOUT;
  }
}

const char* keyboardOutputFor(const KeyboardLayout& layout, int16_t value) {
  for (uint8_t row = 0; row < layout.rowCount; ++row) {
    for (uint8_t col = 0; col < layout.rows[row].count; ++col) {
      const KeyboardKey& key = layout.rows[row].keys[col];
      if (key.value != value) continue;
      if (key.kind == KeyKind::Normal) return key.output;
      // Space keys draw a glyph instead of a label, so the layout tables leave
      // their output null — but they still insert text.
      if (key.kind == KeyKind::Space) return key.output ? key.output : " ";
    }
  }
  return nullptr;
}

const char* keyboardAltOutputFor(const KeyboardLayout& layout, int16_t value) {
  for (uint8_t row = 0; row < layout.rowCount; ++row) {
    for (uint8_t col = 0; col < layout.rows[row].count; ++col) {
      const KeyboardKey& key = layout.rows[row].keys[col];
      if (key.value != value) continue;
      if (key.kind != KeyKind::Normal) return nullptr;
      if (key.alt) return key.alt;
      // ASCII letters without an explicit alt flip case (hold-for-capital).
      // Static buffer: single UI-loop caller assumption (see header doc).
      static char flipped[2] = {0, 0};
      if (value >= 'a' && value <= 'z') {
        flipped[0] = static_cast<char>(value - ('a' - 'A'));
        return flipped;
      }
      if (value >= 'A' && value <= 'Z') {
        flipped[0] = static_cast<char>(value + ('a' - 'A'));
        return flipped;
      }
      return nullptr;
    }
  }
  return nullptr;
}

Rect centeredRect(Rect outer, Size inner) {
  return Rect{static_cast<int16_t>(outer.x + (outer.width - inner.width) / 2),
              static_cast<int16_t>(outer.y + (outer.height - inner.height) / 2), inner.width, inner.height};
}

Rect ensureMinTouchRect(Rect visual, int16_t minSize, Rect bounds) {
  // Edge snap: hit rects whose edge lies within EDGE_SNAP_PX of a bounds edge
  // extend to that boundary. The touch transforms clamp bezel-adjacent taps to
  // the exact border pixels (touchToLogical / raw-range clamping), so an inset
  // control near an edge otherwise has a dead gutter its own users tap into —
  // an edge target should reach the physical edge (the Fitts's-law rule).
  constexpr int16_t EDGE_SNAP_PX = 12;

  Rect rect = visual;
  if (rect.width < minSize) {
    const int16_t delta = static_cast<int16_t>(minSize - rect.width);
    rect.x = static_cast<int16_t>(rect.x - delta / 2);
    rect.width = minSize;
  }
  if (rect.height < minSize) {
    const int16_t delta = static_cast<int16_t>(minSize - rect.height);
    rect.y = static_cast<int16_t>(rect.y - delta / 2);
    rect.height = minSize;
  }
  if (rect.x < bounds.x) rect.x = bounds.x;
  if (rect.y < bounds.y) rect.y = bounds.y;
  if (rect.right() > bounds.right()) rect.x = static_cast<int16_t>(bounds.right() - rect.width);
  if (rect.bottom() > bounds.bottom()) rect.y = static_cast<int16_t>(bounds.bottom() - rect.height);

  if (rect.x - bounds.x < EDGE_SNAP_PX) {
    rect.width = static_cast<int16_t>(rect.width + (rect.x - bounds.x));
    rect.x = bounds.x;
  }
  if (rect.y - bounds.y < EDGE_SNAP_PX) {
    rect.height = static_cast<int16_t>(rect.height + (rect.y - bounds.y));
    rect.y = bounds.y;
  }
  if (bounds.right() - rect.right() < EDGE_SNAP_PX) {
    rect.width = static_cast<int16_t>(bounds.right() - rect.x);
  }
  if (bounds.bottom() - rect.bottom() < EDGE_SNAP_PX) {
    rect.height = static_cast<int16_t>(bounds.bottom() - rect.y);
  }
  return rect;
}

uint16_t listVisibleRows(Rect rect, int16_t rowHeight, int16_t rowGap) {
  if (rect.height <= 0 || rowHeight <= 0) return 0;
  if (rowGap < 0) rowGap = 0;
  // n rows occupy n*rowHeight + (n-1)*rowGap, so add one trailing gap to both
  // sides of the division.
  return static_cast<uint16_t>((rect.height + rowGap) / (rowHeight + rowGap));
}

uint16_t listTopIndexFor(int16_t selectedIndex, uint16_t topIndex, uint16_t visibleRows, uint16_t count) {
  if (count == 0 || visibleRows == 0) return 0;
  const uint16_t maxTop = count > visibleRows ? static_cast<uint16_t>(count - visibleRows) : 0;
  uint16_t top = topIndex > maxTop ? maxTop : topIndex;
  if (selectedIndex >= 0 && selectedIndex < static_cast<int16_t>(count)) {
    const uint16_t selected = static_cast<uint16_t>(selectedIndex);
    if (selected < top) {
      top = selected;
    } else if (selected >= top + visibleRows) {
      top = static_cast<uint16_t>(selected - visibleRows + 1);
    }
  }
  return top > maxTop ? maxTop : top;
}

BitmapRef resolveBitmap(AssetResolver* resolver, const AssetRef& asset) {
  if (asset.bitmap) return asset.bitmap;
  if (!resolver || !asset) return BitmapRef{};
  return resolver->bitmapFor(asset);
}

int16_t clampInt16(int32_t value, int16_t minValue, int16_t maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return static_cast<int16_t>(value);
}

TextStyle textStyleWithForeground(TextStyle text, Paint foreground) {
  if (foreground.kind == PaintKind::Solid) {
    text.color = foreground.color;
    text.inverted = foreground.color == Color::White;
  }
  return text;
}

void drawText(DrawTarget& target, Rect rect, const char* text, TextStyle style) {
  if (!text || rect.empty()) return;
  target.text(rect, text, style);
}

void drawBitmap(DrawTarget& target, Rect rect, BitmapRef bitmap, BitmapMode mode, Paint foreground) {
  if (!bitmap || rect.empty()) return;
  target.bitmap(rect, bitmap, mode, foreground);
}

}  // namespace ui
}  // namespace freeink
