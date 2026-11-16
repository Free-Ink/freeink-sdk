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
  const auto addOutline = [](BoxStyle& style) {
    style.border = Paint::solid(Color::Black);
    style.borderWidth = 1;
  };
  addOutline(styles.normal);
  addOutline(styles.selected);
  addOutline(styles.focused);
  addOutline(styles.active);
  addOutline(styles.disabled);
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

// Width weights use half-key increments: character keys weigh 2 and the
// compact Shift/Delete controls weigh 3 (1.5 character keys).
constexpr uint8_t KEY_WIDTH = 2;
constexpr uint8_t WIDE_CONTROL_WIDTH = 3;

#define K(label, output, value) KeyboardKey{label, output, KeyKind::Normal, StateNormal, value, KEY_WIDTH, true}
#define K2(label, output, value) KeyboardKey{label, output, KeyKind::Normal, StateNormal, value, 2 * KEY_WIDTH, true}
#define KS(label, kind, value, units) \
  KeyboardKey { label, nullptr, kind, StateNormal, value, units *KEY_WIDTH, true }
#define K15(label, kind, value) \
  KeyboardKey { label, nullptr, kind, StateNormal, value, WIDE_CONTROL_WIDTH, true }
#define KA(label, output, value, alt) \
  KeyboardKey { label, output, KeyKind::Normal, StateNormal, value, KEY_WIDTH, true, alt }

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
static const KeyboardKey EN_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("z", "z", 'z'),
                                      K("x", "x", 'x'), K("c", "c", 'c'), K("v", "v", 'v'), K("b", "b", 'b'),
                                      K("n", "n", 'n'), K("m", "m", 'm'),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};
static const KeyboardKey EN_ROW4[] = {KS("?123", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                      KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 6),
                                      KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

static const KeyboardKey EN_SHIFT_ROW1[] = {K("Q", "Q", 'Q'), K("W", "W", 'W'), K("E", "E", 'E'), K("R", "R", 'R'),
                                            K("T", "T", 'T'), K("Y", "Y", 'Y'), K("U", "U", 'U'), K("I", "I", 'I'),
                                            K("O", "O", 'O'), K("P", "P", 'P')};
static const KeyboardKey EN_SHIFT_ROW2[] = {K("A", "A", 'A'), K("S", "S", 'S'), K("D", "D", 'D'), K("F", "F", 'F'),
                                            K("G", "G", 'G'), K("H", "H", 'H'), K("J", "J", 'J'), K("K", "K", 'K'),
                                            K("L", "L", 'L')};
static const KeyboardKey EN_SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("Z", "Z", 'Z'),
                                            K("X", "X", 'X'), K("C", "C", 'C'), K("V", "V", 'V'), K("B", "B", 'B'),
                                            K("N", "N", 'N'), K("M", "M", 'M'),
                                            K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

static const KeyboardKey SYMBOL_ROW1[] = {K("1", "1", '1'), K("2", "2", '2'), K("3", "3", '3'), K("4", "4", '4'),
                                          K("5", "5", '5'), K("6", "6", '6'), K("7", "7", '7'), K("8", "8", '8'),
                                          K("9", "9", '9'), K("0", "0", '0')};
static const KeyboardKey SYMBOL_ROW2[] = {K("-", "-", '-'), K("/", "/", '/'), K(":", ":", ':'), K(";", ";", ';'),
                                          K("(", "(", '('), K(")", ")", ')'), K("$", "$", '$'), K("&", "&", '&'),
                                          K("@", "@", '@')};
static const KeyboardKey SYMBOL_ROW3[] = {K(".", ".", '.'),
                                          K(",", ",", ','), K("?", "?", '?'), K("!", "!", '!'), K("'", "'", '\''),
                                          K("\"", "\"", '"'), K("#", "#", '#'),
                                          K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};
static const KeyboardKey SYMBOL_ROW4[] = {KS("ABC", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                          KS("#+=", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                          KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 4),
                                          KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

static const KeyboardKey SYMBOL_LANG_ROW4[] = {KS("ABC", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                               KS(nullptr, KeyKind::Lang, QWERTY_KEY_LANG, 1),
                                               KS("#+=", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                               KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 3),
                                               KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

static const KeyboardKey SYMBOL2_ROW4[] = {KS("ABC", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                           KS("123", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                           KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 4),
                                           KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

static const KeyboardKey SYMBOL2_LANG_ROW4[] = {KS("ABC", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                                KS(nullptr, KeyKind::Lang, QWERTY_KEY_LANG, 1),
                                                KS("123", KeyKind::Shift, QWERTY_KEY_SHIFT, 2),
                                                KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 3),
                                                KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

// Second symbols page (the "#+=" layer): together with the first page it
// covers every printable ASCII character the letter layers don't.
static const KeyboardKey SYMBOL2_ROW1[] = {K("[", "[", '['), K("]", "]", ']'), K("{", "{", '{'), K("}", "}", '}'),
                                           K("<", "<", '<'), K(">", ">", '>'), K("^", "^", '^'), K("*", "*", '*'),
                                           K("+", "+", '+'), K("=", "=", '=')};
static const KeyboardKey SYMBOL2_ROW2[] = {K("_", "_", '_'), K("\\", "\\", '\\'), K("|", "|", '|'),
                                           K("~", "~", '~'), K("`", "`", '`'), K("%", "%", '%')};
static const KeyboardKey SYMBOL2_ROW3[] = {K(".", ".", '.'),
                                           K(",", ",", ','), K("?", "?", '?'), K("!", "!", '!'), K("'", "'", '\''),
                                           K("\"", "\"", '"'), K("#", "#", '#'),
                                           K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

// é, by far the most frequent accented letter in French, keeps a key of its
// own; its grave twin è long-presses off it. The rest ride their base letter,
// one alternate each, picked by frequency: ê on e (être, même), à on a (the
// preposition), ç on c, ù on u (où), î on i, ô on o. A key carries a single
// alternate, so â, û, ë, ï, œ and æ stay off the layer. As in the Spanish
// layout, an explicit alternate replaces the long-press case flip on those keys.
static const KeyboardKey FR_ROW1[] = {KA("a", "a", 'a', "à"), K("z", "z", 'z'), KA("e", "e", 'e', "ê"), K("r", "r", 'r'),
                                      K("t", "t", 't'), K("y", "y", 'y'), KA("u", "u", 'u', "ù"), KA("i", "i", 'i', "î"),
                                      KA("o", "o", 'o', "ô"), K("p", "p", 'p')};
static const KeyboardKey FR_ROW2[] = {K("q", "q", 'q'), K("s", "s", 's'), K("d", "d", 'd'), K("f", "f", 'f'),
                                      K("g", "g", 'g'), K("h", "h", 'h'), K("j", "j", 'j'), K("k", "k", 'k'),
                                      K("l", "l", 'l'), K("m", "m", 'm')};
static const KeyboardKey FR_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("w", "w", 'w'),
                                      K("x", "x", 'x'), KA("c", "c", 'c', "ç"), K("v", "v", 'v'), K("b", "b", 'b'),
                                      K("n", "n", 'n'), KA("é", "é", 1001, "è"),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

// QWERTZ as printed on a German keyboard: ü closes the top row, ö and ä the home
// row, ß sits beside m. All four are keys rather than long-press alternates --
// they are ordinary letters in German, and two of them used to be unreachable.
static const KeyboardKey DE_ROW1[] = {K("q", "q", 'q'), K("w", "w", 'w'), K("e", "e", 'e'), K("r", "r", 'r'),
                                      K("t", "t", 't'), K("z", "z", 'z'), K("u", "u", 'u'), K("i", "i", 'i'),
                                      K("o", "o", 'o'), K("p", "p", 'p'), K("ü", "ü", 1101)};
static const KeyboardKey DE_ROW2[] = {K("a", "a", 'a'), K("s", "s", 's'), K("d", "d", 'd'), K("f", "f", 'f'),
                                      K("g", "g", 'g'), K("h", "h", 'h'), K("j", "j", 'j'), K("k", "k", 'k'),
                                      K("l", "l", 'l'), K("ö", "ö", 1103), K("ä", "ä", 1104)};
static const KeyboardKey DE_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("y", "y", 'y'),
                                      K("x", "x", 'x'), K("c", "c", 'c'), K("v", "v", 'v'), K("b", "b", 'b'),
                                      K("n", "n", 'n'), K("m", "m", 'm'), K("ß", "ß", 1102),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

// Vowel alts are the acute accent (á é í ó ú), the common case. ü has no key
// of its own -- it only ever follows g (güe/güi/güito), so it rides g's alt
// the same way ё rides е in the Cyrillic layouts (see RU_ROW1): no natural
// key, but worth showing rather than leaving unreachable.
static const KeyboardKey ES_ROW1[] = {K("q", "q", 'q'), K("w", "w", 'w'), KA("e", "e", 'e', "é"), K("r", "r", 'r'),
                                      K("t", "t", 't'), K("y", "y", 'y'), KA("u", "u", 'u', "ú"), KA("i", "i", 'i', "í"),
                                      KA("o", "o", 'o', "ó"), K("p", "p", 'p')};
static const KeyboardKey ES_ROW2[] = {KA("a", "a", 'a', "á"), K("s", "s", 's'), K("d", "d", 'd'), K("f", "f", 'f'),
                                      KA("g", "g", 'g', "ü"), K("h", "h", 'h'), K("j", "j", 'j'), K("k", "k", 'k'),
                                      K("l", "l", 'l'), K("ñ", "ñ", 1201)};
static const KeyboardKey ES_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("z", "z", 'z'),
                                      K("x", "x", 'x'), K("c", "c", 'c'), K("v", "v", 'v'), K("b", "b", 'b'),
                                      K("n", "n", 'n'), K("m", "m", 'm'),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

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
static const KeyboardKey RU_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("я", "я", 0x44F),
                                      K("ч", "ч", 0x447),
                                      K("с", "с", 0x441),
                                      K("м", "м", 0x43C),
                                      K("и", "и", 0x438),
                                      K("т", "т", 0x442),
                                      K("ь", "ь", 0x44C),
                                      K("б", "б", 0x431),
                                      K("ю", "ю", 0x44E),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

static const KeyboardKey RU_SHIFT_ROW1[] = {
    K("Й", "Й", 0x419), K("Ц", "Ц", 0x426), K("У", "У", 0x423), K("К", "К", 0x41A),
    KA("Е", "Е", 0x415, "Ё"), K("Н", "Н", 0x41D), K("Г", "Г", 0x413), K("Ш", "Ш", 0x428),
    K("Щ", "Щ", 0x429), K("З", "З", 0x417), K("Х", "Х", 0x425), K("Ъ", "Ъ", 0x42A)};
static const KeyboardKey RU_SHIFT_ROW2[] = {
    K("Ф", "Ф", 0x424), K("Ы", "Ы", 0x42B), K("В", "В", 0x412), K("А", "А", 0x410),
    K("П", "П", 0x41F), K("Р", "Р", 0x420), K("О", "О", 0x41E), K("Л", "Л", 0x41B),
    K("Д", "Д", 0x414), K("Ж", "Ж", 0x416), K("Э", "Э", 0x42D)};
static const KeyboardKey RU_SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("Я", "Я", 0x42F),
                                            K("Ч", "Ч", 0x427),
                                            K("С", "С", 0x421),
                                            K("М", "М", 0x41C),
                                            K("И", "И", 0x418),
                                            K("Т", "Т", 0x422),
                                            K("Ь", "Ь", 0x42C),
                                            K("Б", "Б", 0x411),
                                            K("Ю", "Ю", 0x42E),
                                            K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};


// Uppercase layers for the Latin locale layouts. Their letter keys report ASCII
// ids, so long-press already reached the opposite case through the implicit
// flip; what was missing is the shift key doing anything, even though the
// tables have always drawn one.
//
// The locale letters keep dedicated ids in the shifted layer (É/Ü/Ö/Ä/Ñ) because
// keyboardOutputFor resolves ids within a layout and the lowercase ids are
// taken. ß has no uppercase in this font (U+1E9E is absent) and German
// capitalises it as SS anyway, so it stays as it is.
static const KeyboardKey FR_SHIFT_ROW1[] = {KA("A", "A", 'A', "À"), K("Z", "Z", 'Z'), KA("E", "E", 'E', "Ê"), K("R", "R", 'R'),
                                            K("T", "T", 'T'), K("Y", "Y", 'Y'), KA("U", "U", 'U', "Ù"), KA("I", "I", 'I', "Î"),
                                            KA("O", "O", 'O', "Ô"), K("P", "P", 'P')};
static const KeyboardKey FR_SHIFT_ROW2[] = {K("Q", "Q", 'Q'), K("S", "S", 'S'), K("D", "D", 'D'), K("F", "F", 'F'),
                                            K("G", "G", 'G'), K("H", "H", 'H'), K("J", "J", 'J'), K("K", "K", 'K'),
                                            K("L", "L", 'L'), K("M", "M", 'M')};
static const KeyboardKey FR_SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("W", "W", 'W'),
                                            K("X", "X", 'X'), KA("C", "C", 'C', "Ç"), K("V", "V", 'V'), K("B", "B", 'B'),
                                            K("N", "N", 'N'), KA("É", "É", 1051, "È"),
                                            K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

static const KeyboardKey DE_SHIFT_ROW1[] = {K("Q", "Q", 'Q'), K("W", "W", 'W'), K("E", "E", 'E'), K("R", "R", 'R'),
                                            K("T", "T", 'T'), K("Z", "Z", 'Z'), K("U", "U", 'U'), K("I", "I", 'I'),
                                            K("O", "O", 'O'), K("P", "P", 'P'), K("Ü", "Ü", 1151)};
static const KeyboardKey DE_SHIFT_ROW2[] = {K("A", "A", 'A'), K("S", "S", 'S'), K("D", "D", 'D'), K("F", "F", 'F'),
                                            K("G", "G", 'G'), K("H", "H", 'H'), K("J", "J", 'J'), K("K", "K", 'K'),
                                            K("L", "L", 'L'), K("Ö", "Ö", 1153), K("Ä", "Ä", 1154)};
static const KeyboardKey DE_SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("Y", "Y", 'Y'),
                                            K("X", "X", 'X'), K("C", "C", 'C'), K("V", "V", 'V'), K("B", "B", 'B'),
                                            K("N", "N", 'N'), K("M", "M", 'M'), K("ß", "ß", 1102),
                                            K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

static const KeyboardKey ES_SHIFT_ROW1[] = {K("Q", "Q", 'Q'), K("W", "W", 'W'), KA("E", "E", 'E', "É"), K("R", "R", 'R'),
                                            K("T", "T", 'T'), K("Y", "Y", 'Y'), KA("U", "U", 'U', "Ú"), KA("I", "I", 'I', "Í"),
                                            KA("O", "O", 'O', "Ó"), K("P", "P", 'P')};
static const KeyboardKey ES_SHIFT_ROW2[] = {KA("A", "A", 'A', "Á"), K("S", "S", 'S'), K("D", "D", 'D'), K("F", "F", 'F'),
                                            KA("G", "G", 'G', "Ü"), K("H", "H", 'H'), K("J", "J", 'J'), K("K", "K", 'K'),
                                            K("L", "L", 'L'), K("Ñ", "Ñ", 1251)};
static const KeyboardKey ES_SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("Z", "Z", 'Z'),
                                            K("X", "X", 'X'), K("C", "C", 'C'), K("V", "V", 'V'), K("B", "B", 'B'),
                                            K("N", "N", 'N'), K("M", "M", 'M'),
                                            K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

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
static const KeyboardKey UK_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("я", "я", 0x44F),
                                      K("ч", "ч", 0x447),
                                      K("с", "с", 0x441),
                                      K("м", "м", 0x43C),
                                      K("и", "и", 0x438),
                                      K("т", "т", 0x442),
                                      K("ь", "ь", 0x44C),
                                      K("б", "б", 0x431),
                                      K("ю", "ю", 0x44E),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

static const KeyboardKey UK_SHIFT_ROW1[] = {K("Й", "Й", 0x419),        K("Ц", "Ц", 0x426), K("У", "У", 0x423),
                                            KA("Г", "Г", 0x413, "Ґ"),  K("К", "К", 0x41A), K("Е", "Е", 0x415),
                                            K("Н", "Н", 0x41D),        K("Ш", "Ш", 0x428), K("Щ", "Щ", 0x429),
                                            K("З", "З", 0x417),        K("Х", "Х", 0x425), K("Ї", "Ї", 0x407)};
static const KeyboardKey UK_SHIFT_ROW2[] = {K("Ф", "Ф", 0x424), K("І", "І", 0x406), K("В", "В", 0x412),
                                            K("А", "А", 0x410), K("П", "П", 0x41F), K("Р", "Р", 0x420),
                                            K("О", "О", 0x41E), K("Л", "Л", 0x41B), K("Д", "Д", 0x414),
                                            K("Ж", "Ж", 0x416), K("Є", "Є", 0x404)};
static const KeyboardKey UK_SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("Я", "Я", 0x42F),
                                            K("Ч", "Ч", 0x427),
                                            K("С", "С", 0x421),
                                            K("М", "М", 0x41C),
                                            K("И", "И", 0x418),
                                            K("Т", "Т", 0x422),
                                            K("Ь", "Ь", 0x42C),
                                            K("Б", "Б", 0x411),
                                            K("Ю", "Ю", 0x42E),
                                            K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

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
static const KeyboardKey BE_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("я", "я", 0x44F),
                                      K("ч", "ч", 0x447),
                                      K("с", "с", 0x441),
                                      K("м", "м", 0x43C),
                                      K("і", "і", 0x456),
                                      K("т", "т", 0x442),
                                      K("ь", "ь", 0x44C),
                                      K("б", "б", 0x431),
                                      K("ю", "ю", 0x44E),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

static const KeyboardKey BE_SHIFT_ROW1[] = {K("Й", "Й", 0x419), K("Ц", "Ц", 0x426), K("У", "У", 0x423),
                                            K("К", "К", 0x41A), K("Е", "Е", 0x415), K("Н", "Н", 0x41D),
                                            K("Г", "Г", 0x413), K("Ш", "Ш", 0x428), K("Ў", "Ў", 0x40E),
                                            K("З", "З", 0x417), K("Х", "Х", 0x425), K("'", "'", '\'')};
static const KeyboardKey BE_SHIFT_ROW2[] = {K("Ф", "Ф", 0x424), K("Ы", "Ы", 0x42B), K("В", "В", 0x412),
                                            K("А", "А", 0x410), K("П", "П", 0x41F), K("Р", "Р", 0x420),
                                            K("О", "О", 0x41E), K("Л", "Л", 0x41B), K("Д", "Д", 0x414),
                                            K("Ж", "Ж", 0x416), K("Э", "Э", 0x42D)};
static const KeyboardKey BE_SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("Я", "Я", 0x42F),
                                            K("Ч", "Ч", 0x427),
                                            K("С", "С", 0x421),
                                            K("М", "М", 0x41C),
                                            K("І", "І", 0x406),
                                            K("Т", "Т", 0x422),
                                            K("Ь", "Ь", 0x42C),
                                            K("Б", "Б", 0x411),
                                            K("Ю", "Ю", 0x42E),
                                            K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

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
static const KeyboardKey KK_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("я", "я", 0x44F),
                                      K("ч", "ч", 0x447),
                                      K("с", "с", 0x441),
                                      K("м", "м", 0x43C),
                                      K("и", "и", 0x438),
                                      K("т", "т", 0x442),
                                      K("ь", "ь", 0x44C),
                                      K("б", "б", 0x431),
                                      K("ю", "ю", 0x44E),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

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
static const KeyboardKey KK_SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("Я", "Я", 0x42F),
                                            K("Ч", "Ч", 0x427),
                                            K("С", "С", 0x421),
                                            K("М", "М", 0x41C),
                                            K("И", "И", 0x418),
                                            K("Т", "Т", 0x422),
                                            K("Ь", "Ь", 0x42C),
                                            K("Б", "Б", 0x411),
                                            K("Ю", "Ю", 0x42E),
                                            K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

// Hebrew, standard Israeli arrangement mapped onto the QWERTY grid. Two things
// make it simpler than the Cyrillic layouts: Hebrew has no letter case, so
// there is one layer and no shift key, and its letters have no contextual
// forms, so no shaping is needed at the table level.
//
// All five final forms (ן ם ך ץ ף) get their own key. The top row drops the
// geresh (') that a physical Israeli keyboard puts at its left end and shifts
// one step left to free the slot; ' stays one tap away on the symbols layer.
//
// Right-to-left is handled downstream: the renderer bidi-reorders the text it
// draws, so the keyboard only has to insert code points in logical order.
static const KeyboardKey HE_ROW1[] = {K("/", "/", '/'),   K("ק", "ק", 0x5E7), K("ר", "ר", 0x5E8),
                                      K("א", "א", 0x5D0), K("ט", "ט", 0x5D8), K("ו", "ו", 0x5D5),
                                      K("ן", "ן", 0x5DF), K("ם", "ם", 0x5DD), K("פ", "פ", 0x5E4),
                                      K("ף", "ף", 0x5E3)};
static const KeyboardKey HE_ROW2[] = {K("ש", "ש", 0x5E9), K("ד", "ד", 0x5D3), K("ג", "ג", 0x5D2),
                                      K("כ", "כ", 0x5DB), K("ע", "ע", 0x5E2), K("י", "י", 0x5D9),
                                      K("ח", "ח", 0x5D7), K("ל", "ל", 0x5DC), K("ך", "ך", 0x5DA)};
static const KeyboardKey HE_ROW3[] = {K("ז", "ז", 0x5D6), K("ס", "ס", 0x5E1),
                                      K("ב", "ב", 0x5D1), K("ה", "ה", 0x5D4),
                                      K("נ", "נ", 0x5E0), K("מ", "מ", 0x5DE),
                                      K("צ", "צ", 0x5E6), K("ת", "ת", 0x5EA),
                                      K("ץ", "ץ", 0x5E5),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};

// Letter layers place a compact Shift key at the start of the third row and
// Delete at its end. Case-less layouts omit Shift; Arabic uses a footer slot
// for Delete because all twelve positions in its third row are letters.
static const KeyboardKey LANG_ROW4[] = {KS("?123", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                        KS(nullptr, KeyKind::Lang, QWERTY_KEY_LANG, 1),
                                        KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 5),
                                        KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

static const KeyboardKey HE_ROW4[] = {KS("?123", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                      KS(nullptr, KeyKind::Lang, QWERTY_KEY_LANG, 1),
                                      KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 5),
                                      KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

static const KeyboardKey AR_ROW4[] = {KS("?123", KeyKind::Mode, QWERTY_KEY_MODE, 2),
                                      KS(nullptr, KeyKind::Lang, QWERTY_KEY_LANG, 1),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE),
                                      KS("Space", KeyKind::Space, QWERTY_KEY_SPACE, 4),
                                      KS("OK", KeyKind::Ok, QWERTY_KEY_ENTER, 2)};

static const KeyboardRow EN_ROWS[] = {{EN_ROW1, 10, 0}, {EN_ROW2, 9, 0}, {EN_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow EN_SHIFT_ROWS[] = {{EN_SHIFT_ROW1, 10, 0}, {EN_SHIFT_ROW2, 9, 0}, {EN_SHIFT_ROW3, 9, 0},
                                           {EN_ROW4, 3, 0}};
static const KeyboardRow SYMBOL_ROWS[] = {{SYMBOL_ROW1, 10, 0}, {SYMBOL_ROW2, 9, 0}, {SYMBOL_ROW3, 8, 0},
                                         {SYMBOL_ROW4, 4, 0}};
static const KeyboardRow SYMBOL_LANG_ROWS[] = {{SYMBOL_ROW1, 10, 0}, {SYMBOL_ROW2, 9, 0}, {SYMBOL_ROW3, 8, 0},
                                         {SYMBOL_LANG_ROW4, 5, 0}};
static const KeyboardRow SYMBOL2_ROWS[] = {{SYMBOL2_ROW1, 10, 0}, {SYMBOL2_ROW2, 6, 0}, {SYMBOL2_ROW3, 8, 0},
                                          {SYMBOL2_ROW4, 4, 0}};
static const KeyboardRow SYMBOL2_LANG_ROWS[] = {{SYMBOL2_ROW1, 10, 0}, {SYMBOL2_ROW2, 6, 0}, {SYMBOL2_ROW3, 8, 0},
                                          {SYMBOL2_LANG_ROW4, 5, 0}};
static const KeyboardRow FR_ROWS[] = {{FR_ROW1, 10, 0}, {FR_ROW2, 10, 0}, {FR_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow DE_ROWS[] = {{DE_ROW1, 11, 0}, {DE_ROW2, 11, 0}, {DE_ROW3, 10, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow ES_ROWS[] = {{ES_ROW1, 10, 0}, {ES_ROW2, 10, 0}, {ES_ROW3, 9, 0}, {EN_ROW4, 3, 0}};

static const KeyboardLayout EN_LAYOUT{EN_ROWS, 4};
static const KeyboardLayout EN_SHIFT_LAYOUT{EN_SHIFT_ROWS, 4};
static const KeyboardLayout SYMBOL_LANG_LAYOUT{SYMBOL_LANG_ROWS, 4};
static const KeyboardLayout SYMBOL_LAYOUT{SYMBOL_ROWS, 4};
static const KeyboardLayout SYMBOL2_LANG_LAYOUT{SYMBOL2_LANG_ROWS, 4};
static const KeyboardLayout SYMBOL2_LAYOUT{SYMBOL2_ROWS, 4};
static const KeyboardLayout FR_LAYOUT{FR_ROWS, 4};
static const KeyboardLayout DE_LAYOUT{DE_ROWS, 4};
static const KeyboardLayout ES_LAYOUT{ES_ROWS, 4};

// numberRow variants: the digit row prepended to each letter layer.
static const KeyboardRow EN_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {EN_ROW1, 10, 0}, {EN_ROW2, 9, 0}, {EN_ROW3, 9, 0},
                                          {EN_ROW4, 3, 0}};
static const KeyboardRow EN_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {EN_SHIFT_ROW1, 10, 0}, {EN_SHIFT_ROW2, 9, 0},
                                                {EN_SHIFT_ROW3, 9, 0},  {EN_ROW4, 3, 0}};
static const KeyboardRow FR_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {FR_ROW1, 10, 0}, {FR_ROW2, 10, 0}, {FR_ROW3, 9, 0},
                                          {EN_ROW4, 3, 0}};
static const KeyboardRow DE_NUM_ROWS[] = {{NUM_ROW, 10, 0, true}, {DE_ROW1, 11, 0}, {DE_ROW2, 11, 0}, {DE_ROW3, 10, 0},
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
static const KeyboardRow RU_NUM_ROWS[] = {{NUM_ROW, 10, 0, true},
                                          {RU_ROW1, 12, 0},
                                          {RU_ROW2, 11, 0},
                                          {RU_ROW3, 11, 0},
                                          {LANG_ROW4, 4, 0}};
static const KeyboardRow RU_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0, true},
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
static const KeyboardRow UK_NUM_ROWS[] = {{NUM_ROW, 10, 0, true},
                                          {UK_ROW1, 12, 0},
                                          {UK_ROW2, 11, 0},
                                          {UK_ROW3, 11, 0},
                                          {LANG_ROW4, 4, 0}};
static const KeyboardRow UK_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0, true},
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
static const KeyboardRow BE_NUM_ROWS[] = {{NUM_ROW, 10, 0, true},
                                          {BE_ROW1, 12, 0},
                                          {BE_ROW2, 11, 0},
                                          {BE_ROW3, 11, 0},
                                          {LANG_ROW4, 4, 0}};
static const KeyboardRow BE_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0, true},
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
static const KeyboardRow KK_NUM_ROWS[] = {{NUM_ROW, 10, 0, true},
                                          {KK_ROW1, 12, 0},
                                          {KK_ROW2, 11, 0},
                                          {KK_ROW3, 11, 0},
                                          {LANG_ROW4, 4, 0}};
static const KeyboardRow KK_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0, true},
                                                {KK_SHIFT_ROW1, 12, 0},
                                                {KK_SHIFT_ROW2, 11, 0},
                                                {KK_SHIFT_ROW3, 11, 0},
                                                {LANG_ROW4, 4, 0}};

// Arabic (standard 101 arrangement). Like Hebrew: no letter case, so one layer
// and no shift key. Right-to-left is the renderer's job -- the layout inserts
// code points in logical order.
//
// All 28 letters have their own key, plus the hamza carriers (ء ئ ؤ أ إ), ta
// marbuta and alef maqsura, which a reader types often enough that hiding them
// behind long-press would be wrong. Only alef madda is a long-press, on أ: it is
// the rarest of the alef forms and the row is already 12 wide, the maximum this
// panel fits (same constraint that puts Kazakh's extra letters on long-press).
static const KeyboardKey AR_ROW1[] = {K("ض", "ض", 0x636), K("ص", "ص", 0x635), K("ث", "ث", 0x62B),
                                      K("ق", "ق", 0x642), K("ف", "ف", 0x641), K("غ", "غ", 0x63A),
                                      K("ع", "ع", 0x639), K("ه", "ه", 0x647), K("خ", "خ", 0x62E),
                                      K("ح", "ح", 0x62D), K("ج", "ج", 0x62C), K("د", "د", 0x62F)};
static const KeyboardKey AR_ROW2[] = {K("ش", "ش", 0x634), K("س", "س", 0x633), K("ي", "ي", 0x64A),
                                      K("ب", "ب", 0x628), K("ل", "ل", 0x644), K("ا", "ا", 0x627),
                                      K("ت", "ت", 0x62A), K("ن", "ن", 0x646), K("م", "م", 0x645),
                                      K("ك", "ك", 0x643), K("ط", "ط", 0x637)};
static const KeyboardKey AR_ROW3[] = {K("ذ", "ذ", 0x630),  K("ئ", "ئ", 0x626), K("ء", "ء", 0x621),
                                      K("ؤ", "ؤ", 0x624),  K("ر", "ر", 0x631), K("ى", "ى", 0x649),
                                      K("ة", "ة", 0x629),  K("و", "و", 0x648), K("ز", "ز", 0x632),
                                      K("ظ", "ظ", 0x638),  KA("أ", "أ", 0x623, "آ"), K("إ", "إ", 0x625)};

static const KeyboardRow HE_ROWS[] = {
    {HE_ROW1, 10, 0}, {HE_ROW2, 9, 0}, {HE_ROW3, 10, 0}, {HE_ROW4, 4, 0}};
static const KeyboardRow HE_NUM_ROWS[] = {{NUM_ROW, 10, 0},
                                          {HE_ROW1, 10, 0},
                                          {HE_ROW2, 9, 0},
                                          {HE_ROW3, 10, 0},
                                          {HE_ROW4, 4, 0}};


static const KeyboardRow AR_ROWS[] = {
    {AR_ROW1, 12, 0}, {AR_ROW2, 11, 0}, {AR_ROW3, 12, 0}, {AR_ROW4, 5, 0}};
static const KeyboardRow AR_NUM_ROWS[] = {{NUM_ROW, 10, 0, true},
                                          {AR_ROW1, 12, 0},
                                          {AR_ROW2, 11, 0},
                                          {AR_ROW3, 12, 0},
                                          {AR_ROW4, 5, 0}};

static const KeyboardRow FR_LANG_ROWS[] = {{FR_ROW1, 10, 0}, {FR_ROW2, 10, 0}, {FR_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow FR_LANG_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {FR_ROW1, 10, 0}, {FR_ROW2, 10, 0}, {FR_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow DE_LANG_ROWS[] = {{DE_ROW1, 11, 0}, {DE_ROW2, 11, 0}, {DE_ROW3, 10, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow DE_LANG_NUM_ROWS[] = {{NUM_ROW, 10, 0, true}, {DE_ROW1, 11, 0}, {DE_ROW2, 11, 0}, {DE_ROW3, 10, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow ES_LANG_ROWS[] = {{ES_ROW1, 10, 0}, {ES_ROW2, 10, 0}, {ES_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow ES_LANG_NUM_ROWS[] = {{NUM_ROW, 10, 0}, {ES_ROW1, 10, 0}, {ES_ROW2, 10, 0}, {ES_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};

static const KeyboardRow FR_SHIFT_ROWS[] = {{FR_SHIFT_ROW1, 10, 0}, {FR_SHIFT_ROW2, 10, 0}, {FR_SHIFT_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow FR_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {FR_SHIFT_ROW1, 10, 0}, {FR_SHIFT_ROW2, 10, 0}, {FR_SHIFT_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow FR_SHIFT_LANG_ROWS[] = {{FR_SHIFT_ROW1, 10, 0}, {FR_SHIFT_ROW2, 10, 0}, {FR_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow FR_SHIFT_LANG_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {FR_SHIFT_ROW1, 10, 0}, {FR_SHIFT_ROW2, 10, 0}, {FR_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow DE_SHIFT_ROWS[] = {{DE_SHIFT_ROW1, 11, 0}, {DE_SHIFT_ROW2, 11, 0}, {DE_SHIFT_ROW3, 10, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow DE_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0, true}, {DE_SHIFT_ROW1, 11, 0}, {DE_SHIFT_ROW2, 11, 0}, {DE_SHIFT_ROW3, 10, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow DE_SHIFT_LANG_ROWS[] = {{DE_SHIFT_ROW1, 11, 0}, {DE_SHIFT_ROW2, 11, 0}, {DE_SHIFT_ROW3, 10, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow DE_SHIFT_LANG_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0, true}, {DE_SHIFT_ROW1, 11, 0}, {DE_SHIFT_ROW2, 11, 0}, {DE_SHIFT_ROW3, 10, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow ES_SHIFT_ROWS[] = {{ES_SHIFT_ROW1, 10, 0}, {ES_SHIFT_ROW2, 10, 0}, {ES_SHIFT_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow ES_SHIFT_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {ES_SHIFT_ROW1, 10, 0}, {ES_SHIFT_ROW2, 10, 0}, {ES_SHIFT_ROW3, 9, 0}, {EN_ROW4, 3, 0}};
static const KeyboardRow ES_SHIFT_LANG_ROWS[] = {{ES_SHIFT_ROW1, 10, 0}, {ES_SHIFT_ROW2, 10, 0}, {ES_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow ES_SHIFT_LANG_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0}, {ES_SHIFT_ROW1, 10, 0}, {ES_SHIFT_ROW2, 10, 0}, {ES_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};

// Latin layers wearing the lang-key bottom row, so a multi-script keyboard can
// switch back. Letter rows are the plain EN tables — same QWERTY, no copies.
static const KeyboardRow EN_LANG_ROWS[] = {{EN_ROW1, 10, 0}, {EN_ROW2, 9, 0}, {EN_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow EN_SHIFT_LANG_ROWS[] = {{EN_SHIFT_ROW1, 10, 0},
                                                 {EN_SHIFT_ROW2, 9, 0},
                                                 {EN_SHIFT_ROW3, 9, 0},
                                                 {LANG_ROW4, 4, 0}};
static const KeyboardRow EN_LANG_NUM_ROWS[] = {{NUM_ROW, 10, 0},
                                               {EN_ROW1, 10, 0},
                                               {EN_ROW2, 9, 0},
                                               {EN_ROW3, 9, 0},
                                               {LANG_ROW4, 4, 0}};
static const KeyboardRow EN_SHIFT_LANG_NUM_ROWS[] = {{NUM_SHIFT_ROW, 10, 0},
                                                     {EN_SHIFT_ROW1, 10, 0},
                                                     {EN_SHIFT_ROW2, 9, 0},
                                                     {EN_SHIFT_ROW3, 9, 0},
                                                     {LANG_ROW4, 4, 0}};

// Greek. Key ids are the letters' code points (U+0386..U+03CE). The first
// key is ς in lowercase and ";" (the Greek question mark) in uppercase, since
// final sigma has no capital of its own.
static const KeyboardKey EL_ROW1[] = {K("ς", "ς", 0x3C2), KA("ε", "ε", 0x3B5, "έ"), K("ρ", "ρ", 0x3C1),
                                      K("τ", "τ", 0x3C4), KA("υ", "υ", 0x3C5, "ύ"), K("θ", "θ", 0x3B8),
                                      KA("ι", "ι", 0x3B9, "ί"), KA("ο", "ο", 0x3BF, "ό"), K("π", "π", 0x3C0),
                                      KA("ϊ", "ϊ", 0x3CA, "ΐ")};
static const KeyboardKey EL_ROW2[] = {KA("α", "α", 0x3B1, "ά"), K("σ", "σ", 0x3C3), K("δ", "δ", 0x3B4),
                                      K("φ", "φ", 0x3C6), K("γ", "γ", 0x3B3), KA("η", "η", 0x3B7, "ή"),
                                      K("ξ", "ξ", 0x3BE), K("κ", "κ", 0x3BA), K("λ", "λ", 0x3BB),
                                      KA("ϋ", "ϋ", 0x3CB, "ΰ")};
static const KeyboardKey EL_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("ζ", "ζ", 0x3B6),
                                      K("χ", "χ", 0x3C7), K("ψ", "ψ", 0x3C8), KA("ω", "ω", 0x3C9, "ώ"),
                                      K("β", "β", 0x3B2), K("ν", "ν", 0x3BD), K("μ", "μ", 0x3BC),
                                      K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};
static const KeyboardKey EL_SHIFT_ROW1[] = {K(";", ";", ';'), KA("Ε", "Ε", 0x395, "Έ"), K("Ρ", "Ρ", 0x3A1),
                                            K("Τ", "Τ", 0x3A4), KA("Υ", "Υ", 0x3A5, "Ύ"), K("Θ", "Θ", 0x398),
                                            KA("Ι", "Ι", 0x399, "Ί"), KA("Ο", "Ο", 0x39F, "Ό"), K("Π", "Π", 0x3A0),
                                            K("Ϊ", "Ϊ", 0x3AA)};
static const KeyboardKey EL_SHIFT_ROW2[] = {KA("Α", "Α", 0x391, "Ά"), K("Σ", "Σ", 0x3A3), K("Δ", "Δ", 0x394),
                                            K("Φ", "Φ", 0x3A6), K("Γ", "Γ", 0x393), KA("Η", "Η", 0x397, "Ή"),
                                            K("Ξ", "Ξ", 0x39E), K("Κ", "Κ", 0x39A), K("Λ", "Λ", 0x39B),
                                            K("Ϋ", "Ϋ", 0x3AB)};
static const KeyboardKey EL_SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, QWERTY_KEY_SHIFT), K("Ζ", "Ζ", 0x396),
                                            K("Χ", "Χ", 0x3A7), K("Ψ", "Ψ", 0x3A8), KA("Ω", "Ω", 0x3A9, "Ώ"),
                                            K("Β", "Β", 0x392), K("Ν", "Ν", 0x39D), K("Μ", "Μ", 0x39C),
                                            K15("Del", KeyKind::Delete, QWERTY_KEY_BACKSPACE)};
// Like Cyrillic, Greek ships only with the lang key so Latin stays reachable.
static const KeyboardRow EL_ROWS[] = {{EL_ROW1, 10, 0}, {EL_ROW2, 10, 0}, {EL_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow EL_SHIFT_ROWS[] = {
    {EL_SHIFT_ROW1, 10, 0}, {EL_SHIFT_ROW2, 10, 0}, {EL_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow EL_NUM_ROWS[] = {
    {NUM_ROW, 10, 0}, {EL_ROW1, 10, 0}, {EL_ROW2, 10, 0}, {EL_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardRow EL_SHIFT_NUM_ROWS[] = {
    {NUM_SHIFT_ROW, 10, 0}, {EL_SHIFT_ROW1, 10, 0}, {EL_SHIFT_ROW2, 10, 0}, {EL_SHIFT_ROW3, 9, 0}, {LANG_ROW4, 4, 0}};
static const KeyboardLayout EL_LAYOUT{EL_ROWS, 4};
static const KeyboardLayout EL_SHIFT_LAYOUT{EL_SHIFT_ROWS, 4};
static const KeyboardLayout EL_NUM_LAYOUT{EL_NUM_ROWS, 5};
static const KeyboardLayout EL_SHIFT_NUM_LAYOUT{EL_SHIFT_NUM_ROWS, 5};

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
static const KeyboardLayout AR_LAYOUT{AR_ROWS, 4};
static const KeyboardLayout AR_NUM_LAYOUT{AR_NUM_ROWS, 5};
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


// ---- Compact layouts, for buildKeyboardLayout() -----------------------------
//
// The keyboards above once more, one string per row. A row is space-separated
// keys, and a key is one character; a second character in the same token is
// that key's long-press alternate ("её" is е, which long-presses to ё). Ids are
// derived rather than stored: a key's id is its code point, except for the
// Latin locale letters, which keep the ids the tables above gave them (see
// LEGACY_LETTER_IDS). Shift and Delete in the third row, the digit row and the
// bottom control row are added by CompactBuilder from the layout's flags.
//
// The tables above cost 20 bytes per key per layer; these strings cost about
// two per key, and a layer is expanded only when an app asks for it.
//
// Both describe the same keyboards, so a layout change has to be made in both
// places. testCompactLayoutsMatchBuiltin compares every layer of every layout
// key by key, which is what stops the two from drifting apart.

enum CompactLayoutFlags : uint8_t {
  COMPACT_ALWAYS_LANG = 1 << 0,    // a non-Latin script: the globe key is always in the bottom row
  COMPACT_CASELESS = 1 << 1,       // one layer: shift is ignored and the third row has no Shift key
  COMPACT_WIDE_DIGITS = 1 << 2,    // the digit row sizes itself (KeyboardRow::independentKeyWidth)
  COMPACT_FOOTER_DELETE = 1 << 3,  // Delete sits in the bottom row, not at the end of the third
};

struct CompactLayout {
  const char* rows[3];
  const char* shiftRows[3];  // unused for a caseless script
  uint8_t flags;
};

// Indexed by KeyboardLayoutId.
constexpr CompactLayout COMPACT_LAYOUTS[] = {
    // QwertyEn
    {{"q w e r t y u i o p", "a s d f g h j k l", "z x c v b n m"},
     {"Q W E R T Y U I O P", "A S D F G H J K L", "Z X C V B N M"},
     0},
    // AzertyFr
    {{"aà z eê r t y uù iî oô p", "q s d f g h j k l m", "w x cç v b n éè"},
     {"AÀ Z EÊ R T Y UÙ IÎ OÔ P", "Q S D F G H J K L M", "W X CÇ V B N ÉÈ"},
     0},
    // QwertzDe
    {{"q w e r t z u i o p ü", "a s d f g h j k l ö ä", "y x c v b n m ß"},
     {"Q W E R T Z U I O P Ü", "A S D F G H J K L Ö Ä", "Y X C V B N M ß"},
     COMPACT_WIDE_DIGITS},
    // SpanishEs
    {{"q w eé r t y uú ií oó p", "aá s d f gü h j k l ñ", "z x c v b n m"},
     {"Q W EÉ R T Y UÚ IÍ OÓ P", "AÁ S D F GÜ H J K L Ñ", "Z X C V B N M"},
     0},
    // CyrillicRu
    {{"й ц у к её н г ш щ з х ъ", "ф ы в а п р о л д ж э", "я ч с м и т ь б ю"},
     {"Й Ц У К ЕЁ Н Г Ш Щ З Х Ъ", "Ф Ы В А П Р О Л Д Ж Э", "Я Ч С М И Т Ь Б Ю"},
     COMPACT_ALWAYS_LANG | COMPACT_WIDE_DIGITS},
    // CyrillicUk
    {{"й ц у гґ к е н ш щ з х ї", "ф і в а п р о л д ж є", "я ч с м и т ь б ю"},
     {"Й Ц У ГҐ К Е Н Ш Щ З Х Ї", "Ф І В А П Р О Л Д Ж Є", "Я Ч С М И Т Ь Б Ю"},
     COMPACT_ALWAYS_LANG | COMPACT_WIDE_DIGITS},
    // CyrillicBe
    {{"й ц у к е н г ш ў з х '", "ф ы в а п р о л д ж э", "я ч с м і т ь б ю"},
     {"Й Ц У К Е Н Г Ш Ў З Х '", "Ф Ы В А П Р О Л Д Ж Э", "Я Ч С М І Т Ь Б Ю"},
     COMPACT_ALWAYS_LANG | COMPACT_WIDE_DIGITS},
    // CyrillicKk
    {{"й ц уұ кқ е нң гғ ш щ з хһ ъ", "фү ыі в аә п р оө л д ж э", "я ч с м и т ь б ю"},
     {"Й Ц УҰ КҚ Е НҢ ГҒ Ш Щ З ХҺ Ъ", "ФҮ ЫІ В АӘ П Р ОӨ Л Д Ж Э", "Я Ч С М И Т Ь Б Ю"},
     COMPACT_ALWAYS_LANG | COMPACT_WIDE_DIGITS},
    // HebrewIl
    {{"/ ק ר א ט ו ן ם פ ף", "ש ד ג כ ע י ח ל ך", "ז ס ב ה נ מ צ ת ץ"},
     {nullptr, nullptr, nullptr},
     COMPACT_ALWAYS_LANG | COMPACT_CASELESS},
    // ArabicAr
    {{"ض ص ث ق ف غ ع ه خ ح ج د", "ش س ي ب ل ا ت ن م ك ط", "ذ ئ ء ؤ ر ى ة و ز ظ أآ إ"},
     {nullptr, nullptr, nullptr},
     COMPACT_ALWAYS_LANG | COMPACT_CASELESS | COMPACT_WIDE_DIGITS | COMPACT_FOOTER_DELETE},
    // GreekEl
    {{"ς εέ ρ τ υύ θ ιί οό π ϊΐ", "αά σ δ φ γ ηή ξ κ λ ϋΰ", "ζ χ ψ ωώ β ν μ"},
     {"; ΕΈ Ρ Τ ΥΎ Θ ΙΊ ΟΌ Π Ϊ", "ΑΆ Σ Δ Φ Γ ΗΉ Ξ Κ Λ Ϋ", "Ζ Χ Ψ ΩΏ Β Ν Μ"},
     COMPACT_ALWAYS_LANG},
};
static_assert(sizeof(COMPACT_LAYOUTS) / sizeof(COMPACT_LAYOUTS[0]) ==
                  static_cast<size_t>(KeyboardLayoutId::GreekEl) + 1,
              "one compact layout per KeyboardLayoutId, in enum order");

constexpr const char* COMPACT_DIGITS = "1! 2@ 3# 4$ 5% 6^ 7& 8* 9( 0)";
constexpr const char* COMPACT_DIGITS_SHIFTED = "!1 @2 #3 $4 %5 ^6 &7 *8 (9 )0";

// Symbol pages one and two. Their third row closes with Delete.
constexpr const char* COMPACT_SYMBOLS[2][3] = {
    {"1 2 3 4 5 6 7 8 9 0", "- / : ; ( ) $ & @", ". , ? ! ' \" #"},
    {"[ ] { } < > ^ * + =", "_ \\ | ~ ` %", ". , ? ! ' \" #"},
};

// The Latin locale letters were given ids above 1000 rather than their code
// points; they keep them, so an id an app already stores means the same key.
struct LegacyLetterId {
  uint16_t codePoint;
  int16_t id;
};
constexpr LegacyLetterId LEGACY_LETTER_IDS[] = {
    {0xE9, 1001}, {0xC9, 1051},                                            // é É
    {0xFC, 1101}, {0xDC, 1151}, {0xDF, 1102},                              // ü Ü ß
    {0xF6, 1103}, {0xD6, 1153}, {0xE4, 1104}, {0xC4, 1154},                // ö Ö ä Ä
    {0xF1, 1201}, {0xD1, 1251},                                            // ñ Ñ
};

int16_t compactKeyId(uint32_t codePoint) {
  for (const LegacyLetterId& legacy : LEGACY_LETTER_IDS) {
    if (legacy.codePoint == codePoint) return legacy.id;
  }
  return static_cast<int16_t>(codePoint);
}

// Byte length of the UTF-8 sequence at `s`, stopping short at a NUL so a
// malformed string can never be read past its end.
uint8_t utf8SequenceLength(const char* s) {
  const auto lead = static_cast<unsigned char>(*s);
  const uint8_t want = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
  uint8_t len = 1;
  while (len < want && s[len]) ++len;
  return len;
}

uint32_t utf8Decode(const char* s, uint8_t len) {
  const auto lead = static_cast<unsigned char>(s[0]);
  if (len == 1) return lead;
  uint32_t cp = lead & (0x7F >> len);
  for (uint8_t i = 1; i < len; ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
  return cp;
}

// Expands compact rows into a KeyboardLayoutBuffer. Anything that would
// overflow the buffer is dropped rather than written past it;
// testCompactLayoutsMatchBuiltin proves no built-in layer comes close.
class CompactBuilder {
 public:
  explicit CompactBuilder(KeyboardLayoutBuffer& buffer) : buffer_(buffer) {}

  void row(const char* text, bool leadingShift, bool trailingDelete, bool independentKeyWidth) {
    if (rows_ >= KeyboardLayoutBuffer::MAX_ROWS) return;
    const uint8_t first = keys_;
    if (leadingShift) push(KeyboardKey{nullptr, nullptr, KeyKind::Shift, StateNormal, QWERTY_KEY_SHIFT, WIDE_CONTROL_WIDTH, true});
    for (const char* p = text; *p;) {
      if (*p == ' ') {
        ++p;
        continue;
      }
      const uint8_t keyLen = utf8SequenceLength(p);
      const char* altStart = p + keyLen;
      const uint8_t altLen = *altStart && *altStart != ' ' ? utf8SequenceLength(altStart) : 0;
      const char* label = copy(p, keyLen);
      const char* alt = altLen ? copy(altStart, altLen) : nullptr;
      if (label && (altLen == 0 || alt)) {
        push(KeyboardKey{label, label, KeyKind::Normal, StateNormal, compactKeyId(utf8Decode(p, keyLen)), KEY_WIDTH,
                         true, alt});
      }
      p = altStart + altLen;
    }
    if (trailingDelete) push(KeyboardKey{"Del", nullptr, KeyKind::Delete, StateNormal, QWERTY_KEY_BACKSPACE, WIDE_CONTROL_WIDTH, true});
    buffer_.rows[rows_++] = KeyboardRow{&buffer_.keys[first], static_cast<uint8_t>(keys_ - first), 0, independentKeyWidth};
  }

  // A row of control keys, pointed at rather than copied.
  void sharedRow(const KeyboardKey* keys, uint8_t count) {
    if (rows_ >= KeyboardLayoutBuffer::MAX_ROWS) return;
    buffer_.rows[rows_++] = KeyboardRow{keys, count, 0};
  }

  const KeyboardLayout& finish() {
    buffer_.layout = KeyboardLayout{buffer_.rows, rows_};
    return buffer_.layout;
  }

 private:
  void push(const KeyboardKey& key) {
    if (keys_ < KeyboardLayoutBuffer::MAX_KEYS) buffer_.keys[keys_++] = key;
  }

  // A NUL-terminated copy in the buffer's text area, or nullptr when it is full.
  const char* copy(const char* s, uint8_t len) {
    if (text_ + len + 1 > KeyboardLayoutBuffer::TEXT_BYTES) return nullptr;
    char* out = &buffer_.text[text_];
    for (uint8_t i = 0; i < len; ++i) out[i] = s[i];
    out[len] = 0;
    text_ = static_cast<uint16_t>(text_ + len + 1);
    return out;
  }

  KeyboardLayoutBuffer& buffer_;
  uint8_t keys_ = 0;
  uint8_t rows_ = 0;
  uint16_t text_ = 0;
};

#undef K
#undef K2
#undef KS
#undef K15
#undef KA

}  // namespace

const KeyboardLayout& builtinKeyboardLayout(KeyboardLayoutId id, bool shifted, bool symbols, bool numberRow,
                                            bool langKey) {
  // In the symbols layers `shifted` selects the second page: the shift slot
  // reads "#+=" on page one and "123" on page two, mirroring phone keyboards.
  // The symbols pages already carry digits, so numberRow only affects the
  // letter layers.
  if (symbols) {
    const bool showLang = langKey || id >= KeyboardLayoutId::CyrillicRu;
    if (showLang) return shifted ? SYMBOL2_LANG_LAYOUT : SYMBOL_LANG_LAYOUT;
    return shifted ? SYMBOL2_LAYOUT : SYMBOL_LAYOUT;
  }
  // Cyrillic and Latin layouts have explicit lower- and uppercase layers.
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
  // Arabic has no case either, so shift is ignored here too.
  if (id == KeyboardLayoutId::ArabicAr) return numberRow ? AR_NUM_LAYOUT : AR_LAYOUT;
  if (id == KeyboardLayoutId::GreekEl) {
    if (shifted) return numberRow ? EL_SHIFT_NUM_LAYOUT : EL_SHIFT_LAYOUT;
    return numberRow ? EL_NUM_LAYOUT : EL_LAYOUT;
  }
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

const KeyboardLayout& buildKeyboardLayout(KeyboardLayoutBuffer& buffer, KeyboardLayoutId id, bool shifted,
                                          bool symbols, bool numberRow, bool langKey) {
  CompactBuilder layout(buffer);
  if (symbols) {
    // As builtinKeyboardLayout(): `shifted` picks page two, the pages carry
    // their own digits so numberRow does not apply, and every id from
    // CyrillicRu up -- an unknown one included -- keeps the globe key.
    const bool showLang = langKey || id >= KeyboardLayoutId::CyrillicRu;
    const char* const* page = COMPACT_SYMBOLS[shifted ? 1 : 0];
    layout.row(page[0], false, false, false);
    layout.row(page[1], false, false, false);
    layout.row(page[2], false, true, false);
    if (shifted && showLang) {
      layout.sharedRow(SYMBOL2_LANG_ROW4, 5);
    } else if (shifted) {
      layout.sharedRow(SYMBOL2_ROW4, 4);
    } else if (showLang) {
      layout.sharedRow(SYMBOL_LANG_ROW4, 5);
    } else {
      layout.sharedRow(SYMBOL_ROW4, 4);
    }
    return layout.finish();
  }

  // An unknown id gets what builtinKeyboardLayout() gives it: English, with no
  // uppercase layer and no globe key.
  const auto index = static_cast<size_t>(id);
  const bool known = index < sizeof(COMPACT_LAYOUTS) / sizeof(COMPACT_LAYOUTS[0]);
  const CompactLayout& compact = COMPACT_LAYOUTS[known ? index : 0];
  const bool caseless = compact.flags & COMPACT_CASELESS;
  const bool upper = known && shifted && !caseless;
  const bool globe = known && (langKey || (compact.flags & COMPACT_ALWAYS_LANG));
  const bool footerDelete = compact.flags & COMPACT_FOOTER_DELETE;
  const char* const* rows = upper ? compact.shiftRows : compact.rows;

  if (numberRow) {
    layout.row(upper ? COMPACT_DIGITS_SHIFTED : COMPACT_DIGITS, false, false, compact.flags & COMPACT_WIDE_DIGITS);
  }
  layout.row(rows[0], false, false, false);
  layout.row(rows[1], false, false, false);
  layout.row(rows[2], !caseless, !footerDelete, false);
  if (footerDelete) {
    layout.sharedRow(AR_ROW4, 5);
  } else if (globe) {
    layout.sharedRow(LANG_ROW4, 4);
  } else {
    layout.sharedRow(EN_ROW4, 3);
  }
  return layout.finish();
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
      // Greek: Α-Ω is U+0391..U+03A9 and α-ω U+03B1..U+03C9, again 0x20
      // apart (U+03A2 is unassigned; final ς has no capital).
      if (value >= 0x0391 && value <= 0x03A9 && value != 0x03A2) cp = value + 0x20;
      if (value >= 0x03B1 && value <= 0x03C9 && value != 0x03C2) cp = value - 0x20;
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
    // `color` already names the ink; DisplayTarget::text() treats `inverted`
    // as a further flip of it. Setting both made a white foreground resolve
    // back to black, so every inverted state (the default InvertFill list
    // selection, an active button, a selected tab) drew black-on-black and
    // lost its label. Carry the colour, and leave the flip to the caller.
    text.inverted = false;
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
