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

// ЙЦУКЕН. Key ids are the letters' code points (U+0410..U+044F): they fit
// int16_t and stay clear of the ASCII ids and the negative control ids.
//
// They do overlap the localized-letter ids of the Latin layouts — э is 0x44D,
// which is 1101, the same number as QwertzDe's ü. That is safe because ids are
// only ever resolved within one layout (keyboardOutputFor walks the layout it
// is handed), and two scripts are never on screen at once. Anything that maps
// an id to a character without knowing the layout would break, which is exactly
// why keyboardOutputFor takes the layout as its first argument.
//
// Keys carry no explicit `alt`, so long-press falls through to the implicit
// case flip in keyboardAltOutputFor, exactly as the Latin layouts do. Spelling
// the opposite case out would also make the renderer draw it as a corner hint,
// putting "йЙ" on every key.
//
// The one deliberate `alt` is е/Е → ё/Ё: that letter has no key of its own in
// this arrangement, so the hint is worth showing and the flip is worth losing.
static const KeyboardKey RU_ROW1[] = {K("й", "й", 0x439), K("ц", "ц", 0x446), K("у", "у", 0x443),
                                      K("к", "к", 0x43A), KA("е", "е", 0x435, "ё"), K("н", "н", 0x43D),
                                      K("г", "г", 0x433), K("ш", "ш", 0x448), K("щ", "щ", 0x449),
                                      K("з", "з", 0x437), K("х", "х", 0x445), K("ъ", "ъ", 0x44A)};
static const KeyboardKey RU_ROW2[] = {K("ф", "ф", 0x444), K("ы", "ы", 0x44B), K("в", "в", 0x432),
                                      K("а", "а", 0x430), K("п", "п", 0x43F), K("р", "р", 0x440),
                                      K("о", "о", 0x43E), K("л", "л", 0x43B), K("д", "д", 0x434),
                                      K("ж", "ж", 0x436), K("э", "э", 0x44D)};
static const KeyboardKey RU_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                      K("я", "я", 0x44F),
                                      K("ч", "ч", 0x447),
                                      K("с", "с", 0x441),
                                      K("м", "м", 0x43C),
                                      K("и", "и", 0x438),
                                      K("т", "т", 0x442),
                                      K("ь", "ь", 0x44C),
                                      K("б", "б", 0x431),
                                      K("ю", "ю", 0x44E),
                                      KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey RU_SHIFT_ROW1[] = {
    K("Й", "Й", 0x419), K("Ц", "Ц", 0x426), K("У", "У", 0x423), K("К", "К", 0x41A),
    KA("Е", "Е", 0x415, "Ё"), K("Н", "Н", 0x41D), K("Г", "Г", 0x413), K("Ш", "Ш", 0x428),
    K("Щ", "Щ", 0x429), K("З", "З", 0x417), K("Х", "Х", 0x425), K("Ъ", "Ъ", 0x42A)};
static const KeyboardKey RU_SHIFT_ROW2[] = {
    K("Ф", "Ф", 0x424), K("Ы", "Ы", 0x42B), K("В", "В", 0x412), K("А", "А", 0x410),
    K("П", "П", 0x41F), K("Р", "Р", 0x420), K("О", "О", 0x41E), K("Л", "Л", 0x41B),
    K("Д", "Д", 0x414), K("Ж", "Ж", 0x416), K("Э", "Э", 0x42D)};
static const KeyboardKey RU_SHIFT_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                            K("Я", "Я", 0x42F),
                                            K("Ч", "Ч", 0x427),
                                            K("С", "С", 0x421),
                                            K("М", "М", 0x41C),
                                            K("И", "И", 0x418),
                                            K("Т", "Т", 0x422),
                                            K("Ь", "Ь", 0x42C),
                                            K("Б", "Б", 0x411),
                                            K("Ю", "Ю", 0x42E),
                                            KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};


// Uppercase layers for the Latin locale layouts. Their letter keys report ASCII
// ids, so long-press already reached the opposite case through the implicit
// flip; what was missing is the shift key doing anything, even though the
// tables have always drawn one.
//
// The locale letters keep dedicated ids in the shifted layer (É/Ü/Ñ) because
// keyboardOutputFor resolves ids within a layout and the lowercase ids are
// taken. ß has no uppercase in this font (U+1E9E is absent) and German
// capitalises it as SS anyway, so it stays as it is.
static const KeyboardKey FR_SHIFT_ROW1[] = {K("A", "A", 'A'), K("Z", "Z", 'Z'), K("E", "E", 'E'), K("R", "R", 'R'),
                                            K("T", "T", 'T'), K("Y", "Y", 'Y'), K("U", "U", 'U'), K("I", "I", 'I'),
                                            K("O", "O", 'O'), K("P", "P", 'P')};
static const KeyboardKey FR_SHIFT_ROW2[] = {K("Q", "Q", 'Q'), K("S", "S", 'S'), K("D", "D", 'D'), K("F", "F", 'F'),
                                            K("G", "G", 'G'), K("H", "H", 'H'), K("J", "J", 'J'), K("K", "K", 'K'),
                                            K("L", "L", 'L'), K("M", "M", 'M')};
static const KeyboardKey FR_SHIFT_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K("W", "W", 'W'),
                                            K("X", "X", 'X'), K("C", "C", 'C'), K("V", "V", 'V'), K("B", "B", 'B'),
                                            K("N", "N", 'N'), K("É", "É", 1051),
                                            KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey DE_SHIFT_ROW1[] = {K("Q", "Q", 'Q'), K("W", "W", 'W'), K("E", "E", 'E'), K("R", "R", 'R'),
                                            K("T", "T", 'T'), K("Z", "Z", 'Z'), K("U", "U", 'U'), K("I", "I", 'I'),
                                            K("O", "O", 'O'), K("P", "P", 'P')};
static const KeyboardKey DE_SHIFT_ROW2[] = {K("A", "A", 'A'), K("S", "S", 'S'), K("D", "D", 'D'), K("F", "F", 'F'),
                                            K("G", "G", 'G'), K("H", "H", 'H'), K("J", "J", 'J'), K("K", "K", 'K'),
                                            K("L", "L", 'L'), K("Ü", "Ü", 1151)};
static const KeyboardKey DE_SHIFT_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K("Y", "Y", 'Y'),
                                            K("X", "X", 'X'), K("C", "C", 'C'), K("V", "V", 'V'), K("B", "B", 'B'),
                                            K("N", "N", 'N'), K("M", "M", 'M'), K("ß", "ß", 1102),
                                            KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey ES_SHIFT_ROW1[] = {K("Q", "Q", 'Q'), K("W", "W", 'W'), K("E", "E", 'E'), K("R", "R", 'R'),
                                            K("T", "T", 'T'), K("Y", "Y", 'Y'), K("U", "U", 'U'), K("I", "I", 'I'),
                                            K("O", "O", 'O'), K("P", "P", 'P')};
static const KeyboardKey ES_SHIFT_ROW2[] = {K("A", "A", 'A'), K("S", "S", 'S'), K("D", "D", 'D'), K("F", "F", 'F'),
                                            K("G", "G", 'G'), K("H", "H", 'H'), K("J", "J", 'J'), K("K", "K", 'K'),
                                            K("L", "L", 'L'), K("Ñ", "Ñ", 1251)};
static const KeyboardKey ES_SHIFT_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2), K("Z", "Z", 'Z'),
                                            K("X", "X", 'X'), K("C", "C", 'C'), K("V", "V", 'V'), K("B", "B", 'B'),
                                            K("N", "N", 'N'), K("M", "M", 'M'),
                                            KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

// Ukrainian ЙЦУКЕН. Differs from Russian in four slots: ї replaces ъ, і
// replaces ы, є replaces э, and и is the Ukrainian и (U+0438) as in Russian
// while й stays put. ґ has no key of its own — it long-presses off г, the
// letter it derives from, mirroring how ё hangs off е in the Russian layer.
// The apostrophe is a real letter-level separator in Ukrainian, so it takes
// the slot Russian gives to ъ's neighbour.
static const KeyboardKey UK_ROW1[] = {K("й", "й", 0x439), K("ц", "ц", 0x446),  K("у", "у", 0x443),
                                      KA("г", "г", 0x433, "ґ"), K("к", "к", 0x43A), K("е", "е", 0x435),
                                      K("н", "н", 0x43D), K("ш", "ш", 0x448),  K("щ", "щ", 0x449),
                                      K("з", "з", 0x437), K("х", "х", 0x445),  K("ї", "ї", 0x457)};
static const KeyboardKey UK_ROW2[] = {K("ф", "ф", 0x444), K("і", "і", 0x456), K("в", "в", 0x432),
                                      K("а", "а", 0x430), K("п", "п", 0x43F), K("р", "р", 0x440),
                                      K("о", "о", 0x43E), K("л", "л", 0x43B), K("д", "д", 0x434),
                                      K("ж", "ж", 0x436), K("є", "є", 0x454)};
static const KeyboardKey UK_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                      K("я", "я", 0x44F),
                                      K("ч", "ч", 0x447),
                                      K("с", "с", 0x441),
                                      K("м", "м", 0x43C),
                                      K("и", "и", 0x438),
                                      K("т", "т", 0x442),
                                      K("ь", "ь", 0x44C),
                                      K("б", "б", 0x431),
                                      K("ю", "ю", 0x44E),
                                      KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey UK_SHIFT_ROW1[] = {K("Й", "Й", 0x419),        K("Ц", "Ц", 0x426), K("У", "У", 0x423),
                                            KA("Г", "Г", 0x413, "Ґ"),  K("К", "К", 0x41A), K("Е", "Е", 0x415),
                                            K("Н", "Н", 0x41D),        K("Ш", "Ш", 0x428), K("Щ", "Щ", 0x429),
                                            K("З", "З", 0x417),        K("Х", "Х", 0x425), K("Ї", "Ї", 0x407)};
static const KeyboardKey UK_SHIFT_ROW2[] = {K("Ф", "Ф", 0x424), K("І", "І", 0x406), K("В", "В", 0x412),
                                            K("А", "А", 0x410), K("П", "П", 0x41F), K("Р", "Р", 0x420),
                                            K("О", "О", 0x41E), K("Л", "Л", 0x41B), K("Д", "Д", 0x414),
                                            K("Ж", "Ж", 0x416), K("Є", "Є", 0x404)};
static const KeyboardKey UK_SHIFT_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                            K("Я", "Я", 0x42F),
                                            K("Ч", "Ч", 0x427),
                                            K("С", "С", 0x421),
                                            K("М", "М", 0x41C),
                                            K("И", "И", 0x418),
                                            K("Т", "Т", 0x422),
                                            K("Ь", "Ь", 0x42C),
                                            K("Б", "Б", 0x411),
                                            K("Ю", "Ю", 0x42E),
                                            KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

// Belarusian ЙЦУКЕН. Against Russian: ў replaces щ, і replaces и, and there is
// no ъ (its job is done by the apostrophe, which sits where ъ would be).
static const KeyboardKey BE_ROW1[] = {K("й", "й", 0x439), K("ц", "ц", 0x446), K("у", "у", 0x443),
                                      K("к", "к", 0x43A), K("е", "е", 0x435), K("н", "н", 0x43D),
                                      K("г", "г", 0x433), K("ш", "ш", 0x448), K("ў", "ў", 0x45E),
                                      K("з", "з", 0x437), K("х", "х", 0x445), K("'", "'", '\'')};
static const KeyboardKey BE_ROW2[] = {K("ф", "ф", 0x444), K("ы", "ы", 0x44B), K("в", "в", 0x432),
                                      K("а", "а", 0x430), K("п", "п", 0x43F), K("р", "р", 0x440),
                                      K("о", "о", 0x43E), K("л", "л", 0x43B), K("д", "д", 0x434),
                                      K("ж", "ж", 0x436), K("э", "э", 0x44D)};
static const KeyboardKey BE_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                      K("я", "я", 0x44F),
                                      K("ч", "ч", 0x447),
                                      K("с", "с", 0x441),
                                      K("м", "м", 0x43C),
                                      K("і", "і", 0x456),
                                      K("т", "т", 0x442),
                                      K("ь", "ь", 0x44C),
                                      K("б", "б", 0x431),
                                      K("ю", "ю", 0x44E),
                                      KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey BE_SHIFT_ROW1[] = {K("Й", "Й", 0x419), K("Ц", "Ц", 0x426), K("У", "У", 0x423),
                                            K("К", "К", 0x41A), K("Е", "Е", 0x415), K("Н", "Н", 0x41D),
                                            K("Г", "Г", 0x413), K("Ш", "Ш", 0x428), K("Ў", "Ў", 0x40E),
                                            K("З", "З", 0x417), K("Х", "Х", 0x425), K("'", "'", '\'')};
static const KeyboardKey BE_SHIFT_ROW2[] = {K("Ф", "Ф", 0x424), K("Ы", "Ы", 0x42B), K("В", "В", 0x412),
                                            K("А", "А", 0x410), K("П", "П", 0x41F), K("Р", "Р", 0x420),
                                            K("О", "О", 0x41E), K("Л", "Л", 0x41B), K("Д", "Д", 0x414),
                                            K("Ж", "Ж", 0x416), K("Э", "Э", 0x42D)};
static const KeyboardKey BE_SHIFT_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                            K("Я", "Я", 0x42F),
                                            K("Ч", "Ч", 0x427),
                                            K("С", "С", 0x421),
                                            K("М", "М", 0x41C),
                                            K("І", "І", 0x406),
                                            K("Т", "Т", 0x422),
                                            K("Ь", "Ь", 0x42C),
                                            K("Б", "Б", 0x411),
                                            K("Ю", "Ю", 0x42E),
                                            KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

// Kazakh ЙЦУКЕН. Kazakh adds nine letters to the Russian set, which is more
// than a row can take on a 480px panel. The physical layout puts them over the
// digits; here they hang off the Russian letters they are derived from, so
// long-press reaches ә from а, ң from н and so on. That keeps the grid the same
// width as the other Cyrillic layouts at the cost of one hold per letter.
static const KeyboardKey KK_ROW1[] = {K("й", "й", 0x439),        K("ц", "ц", 0x446), KA("у", "у", 0x443, "ұ"),
                                      KA("к", "к", 0x43A, "қ"),  K("е", "е", 0x435), KA("н", "н", 0x43D, "ң"),
                                      KA("г", "г", 0x433, "ғ"),  K("ш", "ш", 0x448), K("щ", "щ", 0x449),
                                      K("з", "з", 0x437),        KA("х", "х", 0x445, "һ"), K("ъ", "ъ", 0x44A)};
static const KeyboardKey KK_ROW2[] = {KA("ф", "ф", 0x444, "ү"), KA("ы", "ы", 0x44B, "і"), K("в", "в", 0x432),
                                      KA("а", "а", 0x430, "ә"), K("п", "п", 0x43F),       K("р", "р", 0x440),
                                      KA("о", "о", 0x43E, "ө"), K("л", "л", 0x43B),       K("д", "д", 0x434),
                                      K("ж", "ж", 0x436),       K("э", "э", 0x44D)};
static const KeyboardKey KK_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                      K("я", "я", 0x44F),
                                      K("ч", "ч", 0x447),
                                      K("с", "с", 0x441),
                                      K("м", "м", 0x43C),
                                      K("и", "и", 0x438),
                                      K("т", "т", 0x442),
                                      K("ь", "ь", 0x44C),
                                      K("б", "б", 0x431),
                                      K("ю", "ю", 0x44E),
                                      KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

static const KeyboardKey KK_SHIFT_ROW1[] = {
    K("Й", "Й", 0x419),       K("Ц", "Ц", 0x426),        KA("У", "У", 0x423, "Ұ"), KA("К", "К", 0x41A, "Қ"),
    K("Е", "Е", 0x415),       KA("Н", "Н", 0x41D, "Ң"),  KA("Г", "Г", 0x413, "Ғ"), K("Ш", "Ш", 0x428),
    K("Щ", "Щ", 0x429),       K("З", "З", 0x417),        KA("Х", "Х", 0x425, "Һ"), K("Ъ", "Ъ", 0x42A)};
static const KeyboardKey KK_SHIFT_ROW2[] = {KA("Ф", "Ф", 0x424, "Ү"), KA("Ы", "Ы", 0x42B, "І"),
                                            K("В", "В", 0x412),      KA("А", "А", 0x410, "Ә"),
                                            K("П", "П", 0x41F),      K("Р", "Р", 0x420),
                                            KA("О", "О", 0x41E, "Ө"), K("Л", "Л", 0x41B),
                                            K("Д", "Д", 0x414),      K("Ж", "Ж", 0x416),
                                            K("Э", "Э", 0x42D)};
static const KeyboardKey KK_SHIFT_ROW3[] = {KS("Shift", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                            K("Я", "Я", 0x42F),
                                            K("Ч", "Ч", 0x427),
                                            K("С", "С", 0x421),
                                            K("М", "М", 0x41C),
                                            K("И", "И", 0x418),
                                            K("Т", "Т", 0x422),
                                            K("Ь", "Ь", 0x42C),
                                            K("Б", "Б", 0x411),
                                            K("Ю", "Ю", 0x42E),
                                            KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

// Hebrew, standard Israeli arrangement mapped onto the QWERTY grid. Two things
// make it simpler than the Cyrillic layouts: Hebrew has no letter case, so
// there is one layer and no shift key, and its letters have no contextual
// forms, so no shaping is needed at the table level.
//
// Five letters have a final form used at the end of a word. Four of them get
// their own key, as on a physical Israeli keyboard (ן ם ך ץ). The fifth, ף,
// does not fit the grid and long-presses off פ instead — the same
// base-letter-holds-its-variant idea used for ё in Russian and ґ in Ukrainian.
//
// Right-to-left is handled downstream: the renderer bidi-reorders the text it
// draws, so the keyboard only has to insert code points in logical order.
static const KeyboardKey HE_ROW1[] = {K("/", "/", '/'),   K("'", "'", '\''), K("ק", "ק", 0x5E7),
                                      K("ר", "ר", 0x5E8), K("א", "א", 0x5D0),  K("ט", "ט", 0x5D8),
                                      K("ו", "ו", 0x5D5), K("ן", "ן", 0x5DF),  K("ם", "ם", 0x5DD),
                                      KA("פ", "פ", 0x5E4, "ף")};
static const KeyboardKey HE_ROW2[] = {K("ש", "ש", 0x5E9), K("ד", "ד", 0x5D3), K("ג", "ג", 0x5D2),
                                      K("כ", "כ", 0x5DB), K("ע", "ע", 0x5E2), K("י", "י", 0x5D9),
                                      K("ח", "ח", 0x5D7), K("ל", "ל", 0x5DC), K("ך", "ך", 0x5DA)};
static const KeyboardKey HE_ROW3[] = {K("ז", "ז", 0x5D6), K("ס", "ס", 0x5E1),
                                      K("ב", "ב", 0x5D1), K("ה", "ה", 0x5D4),
                                      K("נ", "נ", 0x5E0), K("מ", "מ", 0x5DE),
                                      K("צ", "צ", 0x5E6), K("ת", "ת", 0x5EA),
                                      K("ץ", "ץ", 0x5E5),
                                      KS("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE, 2)};

// Bottom row carrying the script-switch key. Kept separate from EN_ROW4 so a
// single-script build renders exactly as before — the key costs a slot in the
// row, and there is no point spending it when there is nowhere to switch to.
// Its "EN" label is a placeholder: the app overrides it per frame through
// KeyboardProps::langLabel with the name of whatever layout comes next.
static const KeyboardKey LANG_ROW4[] = {KS("?123", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                        KS("EN", KeyKind::Lang, QWERTY_KEY_LANG, 2),
                                        KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 4),
                                        KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

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

// Cyrillic ships only in the lang-key flavour: a Cyrillic-only keyboard cannot
// type a Wi-Fi password or a URL, so there always has to be a way back to
// Latin. Its rows run 12/11/11 keys against Latin's 10/9/9 — callers sizing a
// hit-test buffer from the widest layout must account for that.
static const KeyboardRow RU_ROWS[] = {{RU_ROW1, 12, 0}, {RU_ROW2, 11, 0}, {RU_ROW3, 11, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow RU_SHIFT_ROWS[] = {{RU_SHIFT_ROW1, 12, 0},
                                            {RU_SHIFT_ROW2, 11, 0},
                                            {RU_SHIFT_ROW3, 11, 0},
                                            {LANG_ROW4, 4, 0}};
static const KeyboardRow RU_NUM_ROWS[] = {{NUM_ROW, 10, 0},
                                          {RU_ROW1, 12, 0},
                                          {RU_ROW2, 11, 0},
                                          {RU_ROW3, 11, 0},
                                          {LANG_ROW4, 4, 0}};
static const KeyboardRow RU_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0},
                                                {RU_SHIFT_ROW1, 12, 0},
                                                {RU_SHIFT_ROW2, 11, 0},
                                                {RU_SHIFT_ROW3, 11, 0},
                                                {LANG_ROW4, 4, 0}};

static const KeyboardRow UK_ROWS[] = {
    {UK_ROW1, 12, 0}, {UK_ROW2, 11, 0}, {UK_ROW3, 11, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow UK_SHIFT_ROWS[] = {{UK_SHIFT_ROW1, 12, 0},
                                            {UK_SHIFT_ROW2, 11, 0},
                                            {UK_SHIFT_ROW3, 11, 0},
                                            {LANG_ROW4, 4, 0}};
static const KeyboardRow UK_NUM_ROWS[] = {{NUM_ROW, 10, 0},
                                          {UK_ROW1, 12, 0},
                                          {UK_ROW2, 11, 0},
                                          {UK_ROW3, 11, 0},
                                          {LANG_ROW4, 4, 0}};
static const KeyboardRow UK_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0},
                                                {UK_SHIFT_ROW1, 12, 0},
                                                {UK_SHIFT_ROW2, 11, 0},
                                                {UK_SHIFT_ROW3, 11, 0},
                                                {LANG_ROW4, 4, 0}};

static const KeyboardRow BE_ROWS[] = {
    {BE_ROW1, 12, 0}, {BE_ROW2, 11, 0}, {BE_ROW3, 11, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow BE_SHIFT_ROWS[] = {{BE_SHIFT_ROW1, 12, 0},
                                            {BE_SHIFT_ROW2, 11, 0},
                                            {BE_SHIFT_ROW3, 11, 0},
                                            {LANG_ROW4, 4, 0}};
static const KeyboardRow BE_NUM_ROWS[] = {{NUM_ROW, 10, 0},
                                          {BE_ROW1, 12, 0},
                                          {BE_ROW2, 11, 0},
                                          {BE_ROW3, 11, 0},
                                          {LANG_ROW4, 4, 0}};
static const KeyboardRow BE_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0},
                                                {BE_SHIFT_ROW1, 12, 0},
                                                {BE_SHIFT_ROW2, 11, 0},
                                                {BE_SHIFT_ROW3, 11, 0},
                                                {LANG_ROW4, 4, 0}};

static const KeyboardRow KK_ROWS[] = {
    {KK_ROW1, 12, 0}, {KK_ROW2, 11, 0}, {KK_ROW3, 11, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow KK_SHIFT_ROWS[] = {{KK_SHIFT_ROW1, 12, 0},
                                            {KK_SHIFT_ROW2, 11, 0},
                                            {KK_SHIFT_ROW3, 11, 0},
                                            {LANG_ROW4, 4, 0}};
static const KeyboardRow KK_NUM_ROWS[] = {{NUM_ROW, 10, 0},
                                          {KK_ROW1, 12, 0},
                                          {KK_ROW2, 11, 0},
                                          {KK_ROW3, 11, 0},
                                          {LANG_ROW4, 4, 0}};
static const KeyboardRow KK_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0},
                                                {KK_SHIFT_ROW1, 12, 0},
                                                {KK_SHIFT_ROW2, 11, 0},
                                                {KK_SHIFT_ROW3, 11, 0},
                                                {LANG_ROW4, 4, 0}};

static const KeyboardRow HE_ROWS[] = {
    {HE_ROW1, 10, 0}, {HE_ROW2, 9, 1}, {HE_ROW3, 10, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow HE_NUM_ROWS[] = {{NUM_ROW, 10, 0},
                                          {HE_ROW1, 10, 0},
                                          {HE_ROW2, 9, 1},
                                          {HE_ROW3, 10, 0},
                                          {LANG_ROW4, 4, 0}};


static const KeyboardRow FR_LANG_ROWS[] = {{FR_ROW1, 10, 0}, {FR_ROW2, 10, 0}, {FR_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow FR_LANG_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {FR_ROW1, 10, 0}, {FR_ROW2, 10, 0}, {FR_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow DE_LANG_ROWS[] = {{DE_ROW1, 10, 0}, {DE_ROW2, 10, 0}, {DE_ROW3, 10, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow DE_LANG_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {DE_ROW1, 10, 0}, {DE_ROW2, 10, 0}, {DE_ROW3, 10, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow ES_LANG_ROWS[] = {{ES_ROW1, 10, 0}, {ES_ROW2, 10, 0}, {ES_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow ES_LANG_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {ES_ROW1, 10, 0}, {ES_ROW2, 10, 0}, {ES_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};

static const KeyboardRow FR_SHIFT_ROWS[] = {{FR_SHIFT_ROW1, 10, 0}, {FR_SHIFT_ROW2, 10, 0}, {FR_SHIFT_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow FR_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {FR_SHIFT_ROW1, 10, 0}, {FR_SHIFT_ROW2, 10, 0}, {FR_SHIFT_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow FR_SHIFT_LANG_ROWS[] = {{FR_SHIFT_ROW1, 10, 0}, {FR_SHIFT_ROW2, 10, 0}, {FR_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow FR_SHIFT_LANG_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {FR_SHIFT_ROW1, 10, 0}, {FR_SHIFT_ROW2, 10, 0}, {FR_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow DE_SHIFT_ROWS[] = {{DE_SHIFT_ROW1, 10, 0}, {DE_SHIFT_ROW2, 10, 0}, {DE_SHIFT_ROW3, 10, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow DE_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {DE_SHIFT_ROW1, 10, 0}, {DE_SHIFT_ROW2, 10, 0}, {DE_SHIFT_ROW3, 10, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow DE_SHIFT_LANG_ROWS[] = {{DE_SHIFT_ROW1, 10, 0}, {DE_SHIFT_ROW2, 10, 0}, {DE_SHIFT_ROW3, 10, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow DE_SHIFT_LANG_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {DE_SHIFT_ROW1, 10, 0}, {DE_SHIFT_ROW2, 10, 0}, {DE_SHIFT_ROW3, 10, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow ES_SHIFT_ROWS[] = {{ES_SHIFT_ROW1, 10, 0}, {ES_SHIFT_ROW2, 10, 0}, {ES_SHIFT_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow ES_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {ES_SHIFT_ROW1, 10, 0}, {ES_SHIFT_ROW2, 10, 0}, {ES_SHIFT_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow ES_SHIFT_LANG_ROWS[] = {{ES_SHIFT_ROW1, 10, 0}, {ES_SHIFT_ROW2, 10, 0}, {ES_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow ES_SHIFT_LANG_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {ES_SHIFT_ROW1, 10, 0}, {ES_SHIFT_ROW2, 10, 0}, {ES_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};

// Latin layers wearing the lang-key bottom row, so a multi-script keyboard can
// switch back. Letter rows are the plain EN tables — same QWERTY, no copies.
static const KeyboardRow EN_LANG_ROWS[] = {{EN_ROW1, 10, 0}, {EN_ROW2, 9, 1}, {EN_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow EN_SHIFT_LANG_ROWS[] = {{EN_SHIFT_ROW1, 10, 0},
                                                 {EN_SHIFT_ROW2, 9, 1},
                                                 {EN_SHIFT_ROW3, 9, 0},
                                                 {LANG_ROW4, 4, 0}};
static const KeyboardRow EN_LANG_NUM_ROWS[] = {{NUM_ROW, 10, 0},
                                               {EN_ROW1, 10, 0},
                                               {EN_ROW2, 9, 1},
                                               {EN_ROW3, 9, 0},
                                               {LANG_ROW4, 4, 0}};
static const KeyboardRow EN_SHIFT_LANG_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0},
                                                     {EN_SHIFT_ROW1, 10, 0},
                                                     {EN_SHIFT_ROW2, 9, 1},
                                                     {EN_SHIFT_ROW3, 9, 0},
                                                     {LANG_ROW4, 4, 0}};

static const KeyboardLayout RU_LAYOUT{RU_ROWS, 4};
static const KeyboardLayout RU_SHIFT_LAYOUT{RU_SHIFT_ROWS, 4};
static const KeyboardLayout RU_NUM_LAYOUT{RU_NUM_ROWS, 5};
static const KeyboardLayout RU_SHIFT_NUM_LAYOUT{RU_SHIFT_NUM_ROWS, 5};
static const KeyboardLayout UK_LAYOUT{UK_ROWS, 4};
static const KeyboardLayout UK_SHIFT_LAYOUT{UK_SHIFT_ROWS, 4};
static const KeyboardLayout UK_NUM_LAYOUT{UK_NUM_ROWS, 5};
static const KeyboardLayout UK_SHIFT_NUM_LAYOUT{UK_SHIFT_NUM_ROWS, 5};
static const KeyboardLayout BE_LAYOUT{BE_ROWS, 4};
static const KeyboardLayout BE_SHIFT_LAYOUT{BE_SHIFT_ROWS, 4};
static const KeyboardLayout BE_NUM_LAYOUT{BE_NUM_ROWS, 5};
static const KeyboardLayout BE_SHIFT_NUM_LAYOUT{BE_SHIFT_NUM_ROWS, 5};
static const KeyboardLayout KK_LAYOUT{KK_ROWS, 4};
static const KeyboardLayout KK_SHIFT_LAYOUT{KK_SHIFT_ROWS, 4};
static const KeyboardLayout KK_NUM_LAYOUT{KK_NUM_ROWS, 5};
static const KeyboardLayout KK_SHIFT_NUM_LAYOUT{KK_SHIFT_NUM_ROWS, 5};
static const KeyboardLayout HE_LAYOUT{HE_ROWS, 4};
static const KeyboardLayout HE_NUM_LAYOUT{HE_NUM_ROWS, 5};
static const KeyboardLayout FR_LANG_LAYOUT{FR_LANG_ROWS, 4};
static const KeyboardLayout FR_LANG_NUM_LAYOUT{FR_LANG_NUM_ROWS, 5};
static const KeyboardLayout DE_LANG_LAYOUT{DE_LANG_ROWS, 4};
static const KeyboardLayout DE_LANG_NUM_LAYOUT{DE_LANG_NUM_ROWS, 5};
static const KeyboardLayout ES_LANG_LAYOUT{ES_LANG_ROWS, 4};
static const KeyboardLayout ES_LANG_NUM_LAYOUT{ES_LANG_NUM_ROWS, 5};
static const KeyboardLayout FR_SHIFT_LAYOUT{FR_SHIFT_ROWS, 4};
static const KeyboardLayout FR_SHIFT_NUM_LAYOUT{FR_SHIFT_NUM_ROWS, 5};
static const KeyboardLayout FR_SHIFT_LANG_LAYOUT{FR_SHIFT_LANG_ROWS, 4};
static const KeyboardLayout FR_SHIFT_LANG_NUM_LAYOUT{FR_SHIFT_LANG_NUM_ROWS, 5};
static const KeyboardLayout DE_SHIFT_LAYOUT{DE_SHIFT_ROWS, 4};
static const KeyboardLayout DE_SHIFT_NUM_LAYOUT{DE_SHIFT_NUM_ROWS, 5};
static const KeyboardLayout DE_SHIFT_LANG_LAYOUT{DE_SHIFT_LANG_ROWS, 4};
static const KeyboardLayout DE_SHIFT_LANG_NUM_LAYOUT{DE_SHIFT_LANG_NUM_ROWS, 5};
static const KeyboardLayout ES_SHIFT_LAYOUT{ES_SHIFT_ROWS, 4};
static const KeyboardLayout ES_SHIFT_NUM_LAYOUT{ES_SHIFT_NUM_ROWS, 5};
static const KeyboardLayout ES_SHIFT_LANG_LAYOUT{ES_SHIFT_LANG_ROWS, 4};
static const KeyboardLayout ES_SHIFT_LANG_NUM_LAYOUT{ES_SHIFT_LANG_NUM_ROWS, 5};
static const KeyboardLayout EN_LANG_LAYOUT{EN_LANG_ROWS, 4};
static const KeyboardLayout EN_SHIFT_LANG_LAYOUT{EN_SHIFT_LANG_ROWS, 4};
static const KeyboardLayout EN_LANG_NUM_LAYOUT{EN_LANG_NUM_ROWS, 5};
static const KeyboardLayout EN_SHIFT_LANG_NUM_LAYOUT{EN_SHIFT_LANG_NUM_ROWS, 5};

#undef K
#undef K2
#undef KS
#undef KA

}  // namespace

const KeyboardLayout& builtinKeyboardLayout(KeyboardLayoutId id, bool shifted, bool symbols, bool numberRow,
                                            bool langKey) {
  // In the symbols layers `shifted` selects the second page: the shift slot
  // reads "#+=" on page one and "123" on page two, mirroring phone keyboards.
  // The symbols pages already carry digits, so numberRow only affects the
  // letter layers.
  if (symbols) return shifted ? SYMBOL2_LAYOUT : SYMBOL_LAYOUT;
  // Cyrillic is case-explicit: both layers exist and shift picks between them,
  // unlike FR/DE/ES which keep a single letter layer.
  if (id == KeyboardLayoutId::CyrillicRu) {
    if (shifted) return numberRow ? RU_SHIFT_NUM_LAYOUT : RU_SHIFT_LAYOUT;
    return numberRow ? RU_NUM_LAYOUT : RU_LAYOUT;
  }
  if (id == KeyboardLayoutId::CyrillicUk) {
    if (shifted) return numberRow ? UK_SHIFT_NUM_LAYOUT : UK_SHIFT_LAYOUT;
    return numberRow ? UK_NUM_LAYOUT : UK_LAYOUT;
  }
  if (id == KeyboardLayoutId::CyrillicBe) {
    if (shifted) return numberRow ? BE_SHIFT_NUM_LAYOUT : BE_SHIFT_LAYOUT;
    return numberRow ? BE_NUM_LAYOUT : BE_LAYOUT;
  }
  if (id == KeyboardLayoutId::CyrillicKk) {
    if (shifted) return numberRow ? KK_SHIFT_NUM_LAYOUT : KK_SHIFT_LAYOUT;
    return numberRow ? KK_NUM_LAYOUT : KK_LAYOUT;
  }
  // Hebrew has no case, so shift is ignored -- there is only one letter layer.
  if (id == KeyboardLayoutId::HebrewIl) return numberRow ? HE_NUM_LAYOUT : HE_LAYOUT;
  if (id == KeyboardLayoutId::QwertyEn && langKey) {
    if (shifted) return numberRow ? EN_SHIFT_LANG_NUM_LAYOUT : EN_SHIFT_LANG_LAYOUT;
    return numberRow ? EN_LANG_NUM_LAYOUT : EN_LANG_LAYOUT;
  }
  if (shifted && id == KeyboardLayoutId::QwertyEn) return numberRow ? EN_SHIFT_NUM_LAYOUT : EN_SHIFT_LAYOUT;
  switch (id) {
    case KeyboardLayoutId::AzertyFr:
      if (shifted && langKey) return numberRow ? FR_SHIFT_LANG_NUM_LAYOUT : FR_SHIFT_LANG_LAYOUT;
      if (shifted) return numberRow ? FR_SHIFT_NUM_LAYOUT : FR_SHIFT_LAYOUT;
      if (langKey) return numberRow ? FR_LANG_NUM_LAYOUT : FR_LANG_LAYOUT;
      return numberRow ? FR_NUM_LAYOUT : FR_LAYOUT;
    case KeyboardLayoutId::QwertzDe:
      if (shifted && langKey) return numberRow ? DE_SHIFT_LANG_NUM_LAYOUT : DE_SHIFT_LANG_LAYOUT;
      if (shifted) return numberRow ? DE_SHIFT_NUM_LAYOUT : DE_SHIFT_LAYOUT;
      if (langKey) return numberRow ? DE_LANG_NUM_LAYOUT : DE_LANG_LAYOUT;
      return numberRow ? DE_NUM_LAYOUT : DE_LAYOUT;
    case KeyboardLayoutId::SpanishEs:
      if (shifted && langKey) return numberRow ? ES_SHIFT_LANG_NUM_LAYOUT : ES_SHIFT_LANG_LAYOUT;
      if (shifted) return numberRow ? ES_SHIFT_NUM_LAYOUT : ES_SHIFT_LAYOUT;
      if (langKey) return numberRow ? ES_LANG_NUM_LAYOUT : ES_LANG_LAYOUT;
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
      // Letters without an explicit alt flip case (hold-for-capital).
      // Static buffer: single UI-loop caller assumption (see header doc). Three
      // bytes because Cyrillic encodes to two in UTF-8, plus the terminator.
      static char flipped[3] = {0, 0, 0};
      if (value >= 'a' && value <= 'z') {
        flipped[0] = static_cast<char>(value - ('a' - 'A'));
        flipped[1] = 0;
        return flipped;
      }
      if (value >= 'A' && value <= 'Z') {
        flipped[0] = static_cast<char>(value + ('a' - 'A'));
        flipped[1] = 0;
        return flipped;
      }
      // Cyrillic: А-Я is U+0410..U+042F and а-я is U+0430..U+044F, the same
      // 0x20 offset as ASCII. Ё/ё sit outside that block at U+0401/U+0451.
      int32_t cp = -1;
      if (value >= 0x0410 && value <= 0x042F) cp = value + 0x20;
      if (value >= 0x0430 && value <= 0x044F) cp = value - 0x20;
      if (value == 0x0401) cp = 0x0451;
      if (value == 0x0451) cp = 0x0401;
      if (cp > 0) {
        // Two-byte UTF-8: 110xxxxx 10xxxxxx.
        flipped[0] = static_cast<char>(0xC0 | (cp >> 6));
        flipped[1] = static_cast<char>(0x80 | (cp & 0x3F));
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
