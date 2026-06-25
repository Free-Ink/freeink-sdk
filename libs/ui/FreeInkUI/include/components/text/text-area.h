#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

// --- TextArea: multi-line scrollable writing canvas --------------------------
//
// A pure-render multi-line text surface (the editor body). The app owns the text
// buffer and caret offset and feeds it keystrokes; textArea word-wraps `text`
// into the rect, draws the window of visual lines starting at `topLine`, and (if
// showCaret) a thin caret at `cursor`. Unlike layoutText() it is top-aligned and
// has no line cap, so it scrolls arbitrarily long documents. Each source byte
// belongs to exactly one visual line (no spaces dropped) so a caret offset maps
// 1:1 to a (line, column). Keep the caret on screen with textAreaMeasure() +
// textAreaTopLineFor(), mirroring listTopIndexFor() for lists.
struct TextAreaProps {
  const char* text = nullptr;  // NUL-terminated, contiguous
  uint32_t cursor = 0;         // caret byte offset into text
  uint32_t topLine = 0;        // first visual line drawn at the top of the rect
  TextStyle style{};
  bool showCaret = true;
  // Selection highlight: byte range [selStart, selEnd). Empty (selStart>=selEnd)
  // means no selection. Drawn as a dithered band behind the text so it reads on
  // a 1-bit panel without inverting glyphs.
  uint32_t selStart = 0;
  uint32_t selEnd = 0;
};

// One wrapped visual line: byte range [start, start+len) of the source text plus
// whether it was terminated by an explicit '\n' (consumed, not part of len).
struct TextAreaLine {
  uint32_t start;
  uint16_t len;
  bool hardBreak;
};

// Walks `text` wrapped to `width`, calling emit(index, TextAreaLine) for each
// visual line in order; returns the total visual-line count. Empty text yields
// one empty line; a trailing '\n' yields a final empty line (so the caret has a
// line to sit on). Greedy word wrap, breaking after the last space that fits, or
// mid-word (codepoint-aligned) for words wider than the rect.
template <typename Emit>
inline uint32_t textAreaWalk(const DrawTarget& target, int16_t width, const char* text, const TextStyle& style,
                             Emit&& emit) {
  if (!text) text = "";
  if (width < 1) width = 1;
  char buf[224];
  const auto fits = [&](uint32_t start, uint32_t len) -> bool {
    uint32_t n = len < 220 ? len : 220;
    for (uint32_t i = 0; i < n; ++i) buf[i] = text[start + i];
    buf[n] = '\0';
    return target.measureText(style.font, buf, style).width <= width;
  };
  uint32_t total = 0;
  uint32_t lineStart = 0;
  for (;;) {
    uint32_t i = lineStart;
    uint32_t lastBreak = lineStart;  // byte after the last fitting space
    bool haveBreak = false;
    while (text[i] != '\0' && text[i] != '\n') {
      const uint32_t candidate = i + 1 - lineStart;
      if (candidate > 1 && !fits(lineStart, candidate)) break;  // would overflow
      if (text[i] == ' ') {
        lastBreak = i + 1;
        haveBreak = true;
      }
      ++i;
    }
    uint32_t lineEnd;
    uint32_t nextStart;
    bool hard = false;
    if (text[i] == '\n') {
      lineEnd = i;
      nextStart = i + 1;
      hard = true;
    } else if (text[i] == '\0') {
      lineEnd = i;
      nextStart = i;
    } else if (haveBreak && lastBreak > lineStart) {
      lineEnd = lastBreak;  // soft wrap after a space
      nextStart = lastBreak;
    } else {
      uint32_t brk = i > lineStart ? i : lineStart + 1;  // word wider than rect: char break
      while (brk > lineStart + 1 && (text[brk] & 0xC0) == 0x80) --brk;  // keep codepoints whole
      lineEnd = brk;
      nextStart = brk;
    }
    emit(total, TextAreaLine{lineStart, static_cast<uint16_t>(lineEnd - lineStart), hard});
    ++total;
    if (!hard && text[lineEnd] == '\0') break;  // end of buffer, no trailing newline
    if (hard && text[nextStart] == '\0') {
      emit(total, TextAreaLine{nextStart, 0, false});  // trailing newline -> empty final line
      ++total;
      break;
    }
    lineStart = nextStart;
  }
  return total;
}

struct TextAreaMetrics {
  uint32_t lineCount = 0;  // total visual lines
  uint32_t caretLine = 0;  // visual line containing `cursor`
};

// Total visual lines and the line the caret sits on, for scroll math.
inline TextAreaMetrics textAreaMeasure(const DrawTarget& target, int16_t width, const char* text,
                                       const TextStyle& style, uint32_t cursor) {
  TextAreaMetrics m;
  bool found = false;
  m.lineCount = textAreaWalk(target, width, text, style, [&](uint32_t idx, const TextAreaLine& ln) {
    if (!found && cursor >= ln.start && cursor <= static_cast<uint32_t>(ln.start + ln.len)) {
      m.caretLine = idx;
      found = true;
    }
  });
  if (!found) m.caretLine = m.lineCount > 0 ? m.lineCount - 1 : 0;
  return m;
}

// Visual lines that fully fit in the rect at the given line height.
inline uint16_t textAreaVisibleLines(Rect rect, int16_t lineHeight) {
  if (rect.height <= 0 || lineHeight <= 0) return 0;
  return static_cast<uint16_t>(rect.height / lineHeight);
}

// Adjusts topLine the minimal amount so caretLine is visible, then clamps to the
// valid scroll range — the textArea analogue of listTopIndexFor().
inline uint32_t textAreaTopLineFor(uint32_t caretLine, uint32_t topLine, uint16_t visibleLines, uint32_t lineCount) {
  if (visibleLines == 0) return 0;
  const uint32_t maxTop = lineCount > visibleLines ? lineCount - visibleLines : 0;
  uint32_t top = topLine > maxTop ? maxTop : topLine;
  if (caretLine < top) {
    top = caretLine;
  } else if (caretLine >= top + visibleLines) {
    top = caretLine - visibleLines + 1;
  }
  return top > maxTop ? maxTop : top;
}

template <size_t MaxInteractions>
void textArea(Frame<MaxInteractions>& frame, Rect rect, const TextAreaProps& props) {
  DrawTarget& t = frame.target();
  if (rect.empty()) return;
  const int16_t lh = t.lineHeight(props.style.font);
  if (lh <= 0) return;
  const uint16_t visible = static_cast<uint16_t>(rect.height / lh);
  if (visible == 0) return;
  const char* text = props.text ? props.text : "";
  const uint32_t top = props.topLine;
  TextStyle lineStyle = props.style;
  lineStyle.maxLines = 1;
  lineStyle.align = TextAlign::Left;
  char buf[224];
  bool caretDrawn = false;
  const bool hasSel = props.selEnd > props.selStart;
  textAreaWalk(t, rect.width, text, props.style, [&](uint32_t idx, const TextAreaLine& ln) {
    if (idx < top || idx >= top + visible) return;
    const int16_t y = static_cast<int16_t>(rect.y + (idx - top) * lh);
    uint16_t n = ln.len < 220 ? ln.len : 220;
    const uint32_t lineEnd = ln.start + ln.len;

    // Selection band behind the text. Dithered so 1-bit glyphs stay readable.
    if (hasSel && props.selStart <= lineEnd && props.selEnd > ln.start) {
      uint32_t ovS = props.selStart > ln.start ? props.selStart : ln.start;
      uint16_t ps = static_cast<uint16_t>(ovS - ln.start);
      if (ps > n) ps = n;
      for (uint16_t i = 0; i < ps; ++i) buf[i] = text[ln.start + i];
      buf[ps] = '\0';
      const int16_t xs = static_cast<int16_t>(rect.x + t.measureText(props.style.font, buf, props.style).width);
      int16_t xe;
      if (props.selEnd > lineEnd) {
        xe = rect.right();  // selection continues onto the next line: show the line break selected
      } else {
        uint16_t pe = static_cast<uint16_t>(props.selEnd - ln.start);
        if (pe > n) pe = n;
        for (uint16_t i = 0; i < pe; ++i) buf[i] = text[ln.start + i];
        buf[pe] = '\0';
        xe = static_cast<int16_t>(rect.x + t.measureText(props.style.font, buf, props.style).width);
      }
      if (xe > xs) t.fill(Rect{xs, y, static_cast<int16_t>(xe - xs), lh}, Paint::dither(Color::LightGray));
    }

    for (uint16_t i = 0; i < n; ++i) buf[i] = text[ln.start + i];
    buf[n] = '\0';
    if (n > 0) t.text(Rect{rect.x, y, rect.width, lh}, buf, lineStyle);
    if (props.showCaret && !caretDrawn && props.cursor >= ln.start &&
        props.cursor <= static_cast<uint32_t>(ln.start + ln.len)) {
      caretDrawn = true;
      uint16_t pre = static_cast<uint16_t>(props.cursor - ln.start);
      if (pre > n) pre = n;
      for (uint16_t i = 0; i < pre; ++i) buf[i] = text[ln.start + i];
      buf[pre] = '\0';
      const int16_t cx = static_cast<int16_t>(rect.x + t.measureText(props.style.font, buf, props.style).width);
      t.fill(Rect{cx, y, 2, lh}, Paint::solid(Color::Black));
    }
  });
}

}  // namespace ui
}  // namespace freeink
