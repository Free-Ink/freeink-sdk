// Host-side unit tests for FreeInkUI. The library is freestanding C++, so the
// layout, routing, and virtualization logic is verified here without any
// device in the loop. Run with test/host/run.sh.

#include <FreeInkUI.h>
#include <FreeInkApp.h>
#include <FreeInkUIDisplayTarget.h>

#include <cstdio>
#include <cstring>

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++checksRun;                                                           \
    if (!(cond)) {                                                         \
      ++checksFailed;                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                                                  \
  do {                                                                                                  \
    ++checksRun;                                                                                        \
    const auto va = (a);                                                                                \
    const auto vb = (b);                                                                                \
    if (!(va == vb)) {                                                                                  \
      ++checksFailed;                                                                                   \
      std::printf("FAIL %s:%d  %s == %s  (%ld != %ld)\n", __FILE__, __LINE__, #a, #b,                   \
                  static_cast<long>(va), static_cast<long>(vb));                                        \
    }                                                                                                   \
  } while (0)

using namespace freeink::ui;

// Records draw calls so component tests can assert on geometry and paint
// without a real panel.
class FakeDrawTarget : public DrawTarget {
 public:
  struct Op {
    enum Kind { Fill, Stroke, Text, Bitmap, Line, Triangle } kind;
    Rect rect;
    PaintKind paint;
    Color color;
    uint8_t radius;
    uint8_t corners;
    Rotation rotation;
  };

  Op ops[256]{};
  size_t opCount = 0;
  int16_t charWidth = 6;
  int16_t lineH = 12;

  Size measureText(FontId, const char* text, TextStyle) const override {
    return Size{static_cast<int16_t>(charWidth * static_cast<int16_t>(std::strlen(text))), lineH};
  }
  int16_t lineHeight(FontId) const override { return lineH; }
  void fill(Rect rect, Paint paint, uint8_t radius, uint8_t corners) override {
    record(Op::Fill, rect, paint, radius, corners);
  }
  void stroke(Rect rect, Paint paint, uint8_t, uint8_t radius, uint8_t corners) override {
    record(Op::Stroke, rect, paint, radius, corners);
  }
  void line(Point from, Point to, uint8_t width, Paint paint) override {
    record(Op::Line,
           Rect{from.x, from.y, static_cast<int16_t>(to.x - from.x), static_cast<int16_t>(to.y - from.y)}, paint,
           width);
  }
  void triangle(Point a, Point, Point c, Paint paint) override {
    record(Op::Triangle, Rect{a.x, a.y, static_cast<int16_t>(c.x - a.x), static_cast<int16_t>(c.y - a.y)}, paint);
  }
  void text(Rect rect, const char*, TextStyle style) override {
    record(Op::Text, rect, Paint::solid(style.color), 0, CornersAll, style.rotation);
  }
  void bitmap(Rect rect, BitmapRef, BitmapMode, Paint foreground, Rotation rotation) override {
    record(Op::Bitmap, rect, foreground, 0, CornersAll, rotation);
  }

  size_t countKind(Op::Kind kind) const {
    size_t n = 0;
    for (size_t i = 0; i < opCount; ++i)
      if (ops[i].kind == kind) ++n;
    return n;
  }

 private:
  void record(Op::Kind kind, Rect rect, Paint paint, uint8_t radius = 0, uint8_t corners = CornersAll,
              Rotation rotation = Rotation::None) {
    if (opCount < sizeof(ops) / sizeof(ops[0]))
      ops[opCount++] = Op{kind, rect, paint.kind, paint.color, radius, corners, rotation};
  }
};

DeviceContext makeDevice(int16_t w = 480, int16_t h = 800) {
  DeviceContext device;
  device.width = w;
  device.height = h;
  device.hasTouch = true;
  return device;
}

void testRect() {
  Rect r{10, 20, 100, 50};
  CHECK_EQ(r.right(), 110);
  CHECK_EQ(r.bottom(), 70);
  CHECK(r.contains(10, 20));
  CHECK(r.contains(109, 69));
  CHECK(!r.contains(110, 20));
  CHECK(!r.contains(10, 70));
  Rect inset = r.inset(Insets{5, 10, 5, 10});
  CHECK_EQ(inset.x, 20);
  CHECK_EQ(inset.y, 25);
  CHECK_EQ(inset.width, 80);
  CHECK_EQ(inset.height, 40);
  CHECK((Rect{0, 0, 0, 10}.empty()));
  CHECK(!r.empty());
}

// Framebuffer-backed native target: draws into a real 1-bit buffer with no
// GfxRenderer. Convention: set bit = white, clear bit = black ink.
void testDisplayTarget() {
  constexpr int16_t W = 64, H = 32, WB = W / 8;
  DisplayTarget unbound(nullptr, W, H, WB, Orientation::LandscapeCounterClockwise);
  CHECK(!unbound.ready());
  unbound.fill(Rect{0, 0, 4, 4}, Paint::solid(Color::Black));

  uint8_t fb[WB * H];
  std::memset(fb, 0xFF, sizeof(fb));  // white page
  // Native orientation so the raw-framebuffer reads below use logical == panel
  // coordinates (the default would rotate this landscape buffer to portrait).
  DisplayTarget target(fb, W, H, WB, Orientation::LandscapeCounterClockwise);
  CHECK(target.ready());

  const auto pixelInk = [&](int16_t x, int16_t y) {
    return ((fb[y * WB + (x >> 3)] >> (7 - (x & 7))) & 0x01) == 0;  // clear bit = ink
  };

  // Solid black fill flips the covered region to ink and leaves the rest white.
  target.fill(Rect{2, 2, 8, 8}, Paint::solid(Color::Black));
  CHECK(pixelInk(2, 2));
  CHECK(pixelInk(9, 9));
  CHECK(!pixelInk(10, 10));  // outside the rect stays white
  CHECK(!pixelInk(1, 1));

  // White fill clears ink back to white (idempotent on a white page).
  target.fill(Rect{2, 2, 8, 8}, Paint::solid(Color::White));
  CHECK(!pixelInk(5, 5));

  // measureText is proportional and positive; lineHeight matches the font.
  const TextStyle style{};
  CHECK_EQ(target.lineHeight(0), kNotoSansFont.yAdvance);
  const Size w1 = target.measureText(0, "i", style);
  const Size w2 = target.measureText(0, "W", style);
  CHECK(w1.width > 0);
  CHECK(w2.width > w1.width);  // 'W' is wider than 'i' (proportional)

  // Text lays down ink somewhere in its rect.
  std::memset(fb, 0xFF, sizeof(fb));
  target.text(Rect{0, 0, W, H}, "Ag", style);
  size_t inkCount = 0;
  for (int16_t y = 0; y < H; ++y)
    for (int16_t x = 0; x < W; ++x)
      if (pixelInk(x, y)) ++inkCount;
  CHECK(inkCount > 0);

  // Inverted text (color White) draws nothing onto an already-white page.
  std::memset(fb, 0xFF, sizeof(fb));
  TextStyle white = style;
  white.color = Color::White;
  target.text(Rect{0, 0, W, H}, "Ag", white);
  for (int16_t y = 0; y < H; ++y)
    for (int16_t x = 0; x < W; ++x) CHECK(!pixelInk(x, y));

  // The ellipsis codepoint measures as three dots, not one unknown box.
  const Size dots = target.measureText(0, "...", style);
  const Size ell = target.measureText(0, "\xE2\x80\xA6", style);
  CHECK_EQ(dots.width, ell.width);

  // Swapping a slot's font changes its metrics independently of slot 0.
  // (Re-pointing at the same font is a no-op; just exercise the API.)
  target.setFont(2, kNotoSansFont);
  CHECK_EQ(target.lineHeight(2), kNotoSansFont.yAdvance);
}

// Anti-aliased fonts (bpp == 4) store 4-bit coverage per pixel; DisplayTarget
// reproduces partial coverage through its ordered Bayer dither.
void testDisplayTargetAlphaFont() {
  constexpr int16_t W = 32, H = 16, WB = W / 8;
  uint8_t fb[WB * H];
  DisplayTarget target(fb, W, H, WB, Orientation::LandscapeCounterClockwise);

  const auto pixelInk = [&](int16_t x, int16_t y) {
    return ((fb[y * WB + (x >> 3)] >> (7 - (x & 7))) & 0x01) == 0;  // clear bit = ink
  };
  const auto inkCount = [&] {
    size_t count = 0;
    for (int16_t y = 0; y < H; ++y)
      for (int16_t x = 0; x < W; ++x)
        if (pixelInk(x, y)) ++count;
    return count;
  };

  // One 8x8 glyph mapped to 'A'; 64 pixels = 32 bytes of packed nibbles.
  static constexpr FontGlyph glyphs8x8[] = {{0, 8, 8, 9, 0, -8}};
  const TextStyle style{};

  // Full coverage (15) plots every glyph pixel, exactly like a 1-bit glyph.
  static constexpr uint8_t solid[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static constexpr BitmapFont solidFont = {solid, glyphs8x8, 'A', 'A', 10, 8, 8, 8, 4};
  target.setFont(0, solidFont);
  std::memset(fb, 0xFF, sizeof(fb));
  target.text(Rect{0, 0, W, H}, "A", style);
  CHECK_EQ(inkCount(), 64u);

  // Half coverage (8) dithers to exactly 8 of every 16 pixels: an 8x8 glyph
  // spans each Bayer cell four times regardless of where it lands.
  static constexpr uint8_t half[32] = {0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
                                       0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
                                       0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88};
  static constexpr BitmapFont halfFont = {half, glyphs8x8, 'A', 'A', 10, 8, 8, 8, 4};
  target.setFont(0, halfFont);
  std::memset(fb, 0xFF, sizeof(fb));
  target.text(Rect{0, 0, W, H}, "A", style);
  CHECK_EQ(inkCount(), 32u);

  // Zero coverage leaves the background untouched.
  static constexpr uint8_t clear[32] = {};
  static constexpr BitmapFont clearFont = {clear, glyphs8x8, 'A', 'A', 10, 8, 8, 8, 4};
  target.setFont(0, clearFont);
  std::memset(fb, 0xFF, sizeof(fb));
  target.text(Rect{0, 0, W, H}, "A", style);
  CHECK_EQ(inkCount(), 0u);

  // Nibble order is high-first: a 2x1 glyph with coverage [15, 0] inks only
  // its left pixel.
  static constexpr uint8_t pair[] = {0xF0};
  static constexpr FontGlyph glyphs2x1[] = {{0, 2, 1, 3, 0, -1}};
  static constexpr BitmapFont pairFont = {pair, glyphs2x1, 'A', 'A', 3, 2, 2, 1, 4};
  target.setFont(0, pairFont);
  std::memset(fb, 0xFF, sizeof(fb));
  target.text(Rect{0, 0, W, H}, "A", style);
  CHECK_EQ(inkCount(), 1u);

  // Inverted (white) alpha text plots white onto a black page.
  target.setFont(0, solidFont);
  std::memset(fb, 0x00, sizeof(fb));
  TextStyle white = style;
  white.color = Color::White;
  target.text(Rect{0, 0, W, H}, "A", white);
  size_t whiteCount = 0;
  for (int16_t y = 0; y < H; ++y)
    for (int16_t x = 0; x < W; ++x)
      if (!pixelInk(x, y)) ++whiteCount;
  CHECK_EQ(whiteCount, 64u);

  target.setFont(kNotoSansFont);  // restore for any later use
}

void testStackFillsExactly() {
  // header + flex content + footer: classic screen split.
  Stack<3> stack(Rect{0, 0, 480, 800}, Axis::Column, 0);
  stack.fixed(48);
  stack.flex(1);
  stack.fixed(40);
  stack.layout();
  CHECK_EQ(stack.rect(0).height, 48);
  CHECK_EQ(stack.rect(1).height, 712);
  CHECK_EQ(stack.rect(2).height, 40);
  CHECK_EQ(stack.rect(2).bottom(), 800);
}

void testStackFlexRemainderWithTrailingFixed() {
  // 100px across three equal flex slots leaves a remainder of 1; the last
  // *flex* slot must absorb it even when a fixed slot comes after.
  Stack<4> stack(Rect{0, 0, 130, 40}, Axis::Row, 0);
  stack.flex(1);
  stack.flex(1);
  stack.flex(1);
  stack.fixed(30);
  stack.layout();
  CHECK_EQ(stack.rect(0).width + stack.rect(1).width + stack.rect(2).width, 100);
  CHECK_EQ(stack.rect(3).width, 30);
  CHECK_EQ(stack.rect(3).right(), 130);
}

void testStackGaps() {
  Stack<3> stack(Rect{0, 0, 100, 320}, Axis::Column, 10);
  stack.fixed(100);
  stack.fixed(100);
  stack.fixed(100);
  stack.layout();
  CHECK_EQ(stack.rect(0).y, 0);
  CHECK_EQ(stack.rect(1).y, 110);
  CHECK_EQ(stack.rect(2).y, 220);
}

void testEnsureMinTouchRect() {
  const Rect bounds{0, 0, 480, 800};
  Rect grown = ensureMinTouchRect(Rect{100, 100, 20, 20}, 44, bounds);
  CHECK_EQ(grown.width, 44);
  CHECK_EQ(grown.height, 44);
  CHECK_EQ(grown.x, 88);
  CHECK_EQ(grown.y, 88);
  // Clamped at the screen edge instead of spilling off-panel.
  Rect corner = ensureMinTouchRect(Rect{470, 790, 10, 10}, 44, bounds);
  CHECK_EQ(corner.right(), 480);
  CHECK_EQ(corner.bottom(), 800);
  CHECK_EQ(corner.width, 44);
}

void testTouchRouting() {
  InteractionBuffer<8> buffer;
  buffer.addInteraction(Interaction{Rect{0, 0, 100, 100}, 1, 0, InputTouch, StateNormal, 0});
  buffer.addInteraction(Interaction{Rect{50, 50, 100, 100}, 2, 7, InputTouch, StateNormal, 0});

  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 60;
  tap.touchY = 60;
  ActionEvent event = buffer.route(tap);
  // Overlap: the last registered interaction (drawn on top) wins.
  CHECK_EQ(event.action, 2);
  CHECK_EQ(event.value, 7);

  tap.touchX = 10;
  tap.touchY = 10;
  event = buffer.route(tap);
  CHECK_EQ(event.action, 1);

  tap.touchX = 300;
  tap.touchY = 300;
  event = buffer.route(tap);
  CHECK(!event);
}

void testDisabledSkipsTouch() {
  InteractionBuffer<8> buffer;
  buffer.addInteraction(Interaction{Rect{0, 0, 100, 100}, 1, 0, InputTouch, StateDisabled, 0});
  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 10;
  tap.touchY = 10;
  CHECK(!buffer.route(tap));
}

void testLongPressRouting() {
  InteractionBuffer<8> buffer;
  buffer.addInteraction(Interaction{Rect{0, 0, 100, 100}, 1, 5,
                                    static_cast<uint16_t>(InputTouch | InputLongPress | InputConfirm), StateNormal, 0});
  buffer.addInteraction(Interaction{Rect{100, 0, 100, 100}, 2, 0, InputTouch, StateNormal, 0});

  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 10;
  tap.touchY = 10;
  ActionEvent event = buffer.route(tap);
  CHECK_EQ(event.action, 1);
  CHECK(!event.longPress);

  InputSnapshot hold = tap;
  hold.longPress = true;
  event = buffer.route(hold);
  CHECK_EQ(event.action, 1);
  CHECK_EQ(event.value, 5);
  CHECK(event.longPress);

  // A touch-only interaction never receives long-press releases.
  hold.touchX = 110;
  CHECK(!buffer.route(hold));

  // Non-touch dispatch paths report longPress false.
  buffer.setFocusedIndex(0);
  InputSnapshot confirm;
  confirm.confirm = true;
  event = buffer.route(confirm);
  CHECK_EQ(event.action, 1);
  CHECK(!event.longPress);
}

void testFocusNavigationWrapsAndSkips() {
  InteractionBuffer<8> buffer;
  buffer.addInteraction(Interaction{Rect{0, 0, 10, 10}, 1, 0, InputDefault, StateNormal, 0});
  buffer.addInteraction(Interaction{Rect{0, 10, 10, 10}, 2, 0, InputDefault, StateDisabled, 0});
  buffer.addInteraction(Interaction{Rect{0, 20, 10, 10}, 3, 0, InputDefault, StateNormal, 0});
  buffer.addInteraction(Interaction{Rect{0, 30, 10, 10}, 4, 0, InputTouch, StateNormal, 0});  // not focusable

  InputSnapshot next;
  next.focusNext = true;
  buffer.route(next);
  CHECK_EQ(buffer.focusedIndex(), 0);
  buffer.route(next);  // skips disabled index 1
  CHECK_EQ(buffer.focusedIndex(), 2);
  buffer.route(next);  // skips touch-only index 3, wraps to 0
  CHECK_EQ(buffer.focusedIndex(), 0);

  InputSnapshot prev;
  prev.focusPrev = true;
  buffer.route(prev);  // wraps backward past 3 and 1
  CHECK_EQ(buffer.focusedIndex(), 2);

  InputSnapshot confirm;
  confirm.confirm = true;
  ActionEvent event = buffer.route(confirm);
  CHECK_EQ(event.action, 3);
}

void testConfirmIgnoresStaleFocus() {
  InteractionBuffer<8> buffer;
  for (int i = 0; i < 5; ++i) {
    buffer.addInteraction(Interaction{Rect{0, static_cast<int16_t>(i * 10), 10, 10}, static_cast<ActionId>(i + 1), 0,
                                      InputDefault, StateNormal, 0});
  }
  buffer.setFocusedIndex(4);

  // New screen renders fewer interactions; the old focus index is now stale.
  buffer.clear();
  buffer.addInteraction(Interaction{Rect{0, 0, 10, 10}, 9, 0, InputDefault, StateNormal, 0});
  InputSnapshot confirm;
  confirm.confirm = true;
  CHECK(!buffer.route(confirm));
}

void testConfirmRespectsInputMask() {
  InteractionBuffer<8> buffer;
  buffer.addInteraction(Interaction{Rect{0, 0, 10, 10}, 1, 0, InputTouch | InputFocus, StateNormal, 0});
  buffer.setFocusedIndex(0);
  InputSnapshot confirm;
  confirm.confirm = true;
  CHECK(!buffer.route(confirm));
}

void testEdgeButtonsAndSwipes() {
  InteractionBuffer<8> buffer;
  buffer.addInteraction(Interaction{Rect{0, 0, 10, 10}, 1, 0, InputBack, StateNormal, 0});
  buffer.addInteraction(Interaction{Rect{0, 0, 480, 800}, 2, 0, InputSwipeLeft, StateNormal, 0});
  buffer.addInteraction(Interaction{Rect{0, 0, 480, 800}, 3, 0, InputSwipeRight, StateNormal, 0});
  buffer.addInteraction(Interaction{Rect{0, 0, 10, 10}, 4, 0, InputPrev, StateNormal, 0});
  buffer.addInteraction(Interaction{Rect{0, 0, 10, 10}, 5, 0, InputNext, StateNormal, 0});

  InputSnapshot input;
  input.back = true;
  CHECK_EQ(buffer.route(input).action, 1);
  input = InputSnapshot{};
  input.swipeLeft = true;
  CHECK_EQ(buffer.route(input).action, 2);
  input = InputSnapshot{};
  input.swipeRight = true;
  CHECK_EQ(buffer.route(input).action, 3);
  input = InputSnapshot{};
  input.prev = true;
  CHECK_EQ(buffer.route(input).action, 4);
  input = InputSnapshot{};
  input.next = true;
  CHECK_EQ(buffer.route(input).action, 5);
}

void testListHelpers() {
  CHECK_EQ(listVisibleRows(Rect{0, 0, 100, 360}, 36, 0), 10);
  CHECK_EQ(listVisibleRows(Rect{0, 0, 100, 359}, 36, 0), 9);
  CHECK_EQ(listVisibleRows(Rect{0, 0, 100, 100}, 30, 5), 3);  // 3*30 + 2*5 = 100
  CHECK_EQ(listVisibleRows(Rect{0, 0, 100, 0}, 36, 0), 0);

  // Selection below the window scrolls down just enough.
  CHECK_EQ(listTopIndexFor(12, 0, 5, 20), 8);
  // Selection above the window scrolls up to it.
  CHECK_EQ(listTopIndexFor(2, 8, 5, 20), 2);
  // Selection already visible keeps the window.
  CHECK_EQ(listTopIndexFor(9, 8, 5, 20), 8);
  // Top index clamps to the end of the list.
  CHECK_EQ(listTopIndexFor(-1, 99, 5, 20), 15);
  // Short lists never scroll.
  CHECK_EQ(listTopIndexFor(3, 0, 5, 4), 0);
}

void testListVirtualization() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<32> interactions;
  Frame<32> frame(draw, device, input, interactions);

  ListItem items[20]{};
  char labels[20][8];
  for (int i = 0; i < 20; ++i) {
    std::snprintf(labels[i], sizeof(labels[i]), "row%d", i);
    items[i].label = labels[i];
    items[i].actionValue = static_cast<int16_t>(i);
    items[i].enabled = true;
  }

  ListProps props;
  props.items = items;
  props.count = 20;
  props.topIndex = 10;
  props.selectedIndex = 12;
  props.action = 42;
  props.rowHeight = 40;
  list(frame, Rect{0, 0, 480, 200}, props);  // fits 5 full rows

  // Only the window [10, 15) registers interactions.
  CHECK_EQ(interactions.count(), 5u);
  CHECK_EQ(interactions.data()[0].value, 10);
  CHECK_EQ(interactions.data()[4].value, 14);

  // Tapping the third visible row resolves to absolute item 12.
  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 100;
  tap.touchY = 90;
  ActionEvent event = interactions.route(tap);
  CHECK_EQ(event.action, 42);
  CHECK_EQ(event.value, 12);

  // Overflowing list draws the scroll indicator on the right edge.
  bool sawThumb = false;
  for (size_t i = 0; i < draw.opCount; ++i) {
    const FakeDrawTarget::Op& op = draw.ops[i];
    if (op.kind == FakeDrawTarget::Op::Fill && op.rect.x >= 477 && op.paint == PaintKind::Solid &&
        op.color == Color::Black && op.rect.height < 200) {
      sawThumb = true;
    }
  }
  CHECK(sawThumb);
}

void testListClampsBadTopIndex() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<32> interactions;
  Frame<32> frame(draw, device, input, interactions);

  ListItem items[6]{};
  for (int i = 0; i < 6; ++i) {
    items[i].label = "x";
    items[i].actionValue = static_cast<int16_t>(i);
    items[i].enabled = true;
  }
  ListProps props;
  props.items = items;
  props.count = 6;
  props.topIndex = 50;  // way past the end
  props.action = 7;
  props.rowHeight = 40;
  list(frame, Rect{0, 0, 480, 160}, props);  // fits 4 rows

  CHECK_EQ(interactions.count(), 4u);
  CHECK_EQ(interactions.data()[0].value, 2);  // clamped to count - visible
  CHECK_EQ(interactions.data()[3].value, 5);
}

void testButtonRegistersExpandedHit() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, input, interactions);

  ButtonProps props;
  props.label = "OK";
  props.action = 5;
  button(frame, Rect{200, 200, 30, 20}, props);
  CHECK_EQ(interactions.count(), 1u);
  CHECK(interactions.data()[0].rect.width >= 44);
  CHECK(interactions.data()[0].rect.height >= 44);

  ButtonProps disabled = props;
  disabled.enabled = false;
  button(frame, Rect{0, 0, 30, 20}, disabled);
  CHECK_EQ(interactions.count(), 1u);  // disabled button registers nothing
}

void testProgressBarClamps() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<4> interactions;
  Frame<4> frame(draw, device, input, interactions);

  ProgressBarProps props;
  props.value = 250;
  props.max = 100;
  props.track = Paint::solid(Color::White);
  progressBar(frame, Rect{0, 0, 200, 4}, props);
  // Fill is clamped to the full track width, never beyond.
  CHECK_EQ(draw.opCount, 2u);
  CHECK_EQ(draw.ops[1].rect.width, 200);

  FakeDrawTarget draw2;
  Frame<4> frame2(draw2, device, input, interactions);
  props.value = 0;
  progressBar(frame2, Rect{0, 0, 200, 4}, props);
  CHECK_EQ(draw2.opCount, 1u);  // track only, no fill
}

void testBatteryIndicator() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<4> interactions;
  Frame<4> frame(draw, device, input, interactions);

  BatteryIndicatorProps props;
  props.percent = 50;
  props.glyphWidth = 22;
  props.glyphHeight = 11;
  batteryIndicator(frame, Rect{400, 0, 80, 20}, props);
  // Outline stroke, terminal nub fill, and a half-width charge fill.
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Stroke), 1u);
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Fill), 2u);
  const FakeDrawTarget::Op& charge = draw.ops[2];
  CHECK_EQ(charge.rect.width, 9);  // cavity is 18 wide at 50%
  CHECK(charge.paint == PaintKind::Solid);

  // Charging without an icon keeps the solid fill and overlays a bolt.
  FakeDrawTarget draw2;
  Frame<4> frame2(draw2, device, input, interactions);
  props.charging = true;
  batteryIndicator(frame2, Rect{400, 0, 80, 20}, props);
  CHECK(draw2.ops[2].paint == PaintKind::Solid);
  CHECK_EQ(draw2.countKind(FakeDrawTarget::Op::Triangle), 2u);

  // Percent above 100 clamps to a full cavity.
  FakeDrawTarget draw3;
  Frame<4> frame3(draw3, device, input, interactions);
  props.charging = false;
  props.percent = 250;
  batteryIndicator(frame3, Rect{400, 0, 80, 20}, props);
  CHECK_EQ(draw3.ops[2].rect.width, 18);
}

void testMetricCard() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<4> interactions;
  Frame<4> frame(draw, device, input, interactions);

  MetricCardProps props;
  props.label = "PAGES";
  props.value = "128";
  props.unit = "min";
  props.caption = "today";
  props.action = 11;
  metricCard(frame, Rect{0, 0, 160, 100}, props);
  CHECK_EQ(interactions.count(), 1u);
  CHECK_EQ(interactions.data()[0].action, 11);
  // label + caption + value + unit all drawn
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Text), 4u);
}

void testOptionDialog() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, input, interactions);

  const DialogOption options[2] = {
      {"Cancel", 20, 0, StateNormal, true},
      {"Delete", 21, 0, StateNormal, true},
  };
  OptionDialogProps props;
  props.title = "Delete book?";
  props.message = "This cannot be undone.";
  props.options = options;
  props.optionCount = 2;
  props.dimBackground = true;
  const Rect dialog = centeredRect(Rect{0, 0, 480, 800}, Size{320, 200});
  optionDialog(frame, dialog, props);

  CHECK_EQ(interactions.count(), 2u);
  // First fill is the full-screen dither scrim.
  CHECK_EQ(draw.ops[0].rect.width, 480);
  CHECK(draw.ops[0].paint == PaintKind::Dither);

  // Touch on the right half of the button row resolves to the second option.
  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = static_cast<int16_t>(dialog.right() - 40);
  tap.touchY = static_cast<int16_t>(dialog.bottom() - 30);
  ActionEvent event = interactions.route(tap);
  CHECK_EQ(event.action, 21);

  // Focus navigation reaches both options, confirm fires the focused one.
  InputSnapshot next;
  next.focusNext = true;
  interactions.route(next);
  InputSnapshot confirm;
  confirm.confirm = true;
  CHECK_EQ(interactions.route(confirm).action, 20);

  // WakeInk-shaped dialog: caption + wrapping headline + body + two buttons,
  // left-aligned — no hand-rolled card needed.
  FakeDrawTarget draw5;
  InteractionBuffer<8> interactions5;
  Frame<8> frame5(draw5, device, input, interactions5);
  OptionDialogProps skipDialog;
  skipDialog.title = "Skip this event?";
  skipDialog.headline = "Quarterly planning sync with the hardware and firmware teams";  // wraps
  skipDialog.message = "3:30 PM - 4:00 PM";
  skipDialog.headlineText.maxLines = 2;
  skipDialog.options = options;
  skipDialog.optionCount = 2;
  const Rect card{30, 34, 356, 172};
  optionDialog(frame5, card, skipDialog);
  CHECK_EQ(interactions5.count(), 2u);
  // caption + 2 headline lines + message + 2 button labels = 6 text ops
  CHECK_EQ(draw5.countKind(FakeDrawTarget::Op::Text), 6u);
  // Caption honors its style alignment (left) instead of being force-centered.
  CHECK_EQ(draw5.ops[2].rect.x, 46);  // card.x + default padding.left 16
}

// CrossInk compatibility surfaces: these tests mirror the hardest screens in
// the CrossInk fork (keyboard entry, reader status bar, XTC overlay, reader
// menu) to prove the SDK primitives cover them without fork-specific code.

void testCrossInkKeyboardComposition() {
  // CrossInk's keyboard is a 4x10 character grid plus a 5-key bottom row
  // (Shift/Mode/Space/Del/Ok). FreeInkUI composes that as two keyGrids
  // sharing one action.
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<64> interactions;
  Frame<64> frame(draw, device, input, interactions);

  KeyGridKey chars[40]{};
  for (int i = 0; i < 40; ++i) {
    chars[i].label = "q";
    chars[i].secondaryLabel = i < 10 ? "1" : nullptr;
    chars[i].value = static_cast<int16_t>(i);
    chars[i].enabled = true;
  }
  KeyGridProps charGrid;
  charGrid.keys = chars;
  charGrid.rows = 4;
  charGrid.cols = 10;
  charGrid.action = 30;
  charGrid.selectedIndex = 11;
  keyGrid(frame, Rect{0, 500, 480, 160}, charGrid);

  const KeyGridKey bottom[5] = {
      {"Shift", nullptr, {}, {}, KeyKind::Shift, StateNormal, 100, true},
      {"?123", nullptr, {}, {}, KeyKind::Mode, StateNormal, 101, true},
      {" ", nullptr, {}, {}, KeyKind::Space, StateNormal, 102, true},
      {"Del", nullptr, {}, {}, KeyKind::Delete, StateNormal, 103, true},
      {"OK", nullptr, {}, {}, KeyKind::Ok, StateNormal, 104, true},
  };
  KeyGridProps bottomRow;
  bottomRow.keys = bottom;
  bottomRow.rows = 1;
  bottomRow.cols = 5;
  bottomRow.action = 30;
  keyGrid(frame, Rect{0, 660, 480, 40}, bottomRow);

  CHECK_EQ(interactions.count(), 45u);

  // Touch on the bottom row resolves to the special keys, not the char grid.
  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 470;
  tap.touchY = 680;
  CHECK_EQ(interactions.route(tap).value, 104);

  // Text field with a long URL: cursor measurement past the old 96-byte
  // prefix limit still advances (chunked measurement).
  char url[160];
  for (size_t i = 0; i < sizeof(url) - 1; ++i) url[i] = 'a';
  url[sizeof(url) - 1] = '\0';
  TextFieldProps field;
  field.text = url;
  field.cursor = 150;
  field.cursorVisible = true;
  FakeDrawTarget draw2;
  Frame<64> frame2(draw2, device, input, interactions);
  textField(frame2, Rect{0, 0, 480, 40}, field);
  bool sawCursorPastPrefix = false;
  for (size_t i = 0; i < draw2.opCount; ++i) {
    const FakeDrawTarget::Op& op = draw2.ops[i];
    // Cursor fill: a narrow black rect placed at 150 chars * 6px.
    if (op.kind == FakeDrawTarget::Op::Fill && op.rect.width <= 8 && op.rect.x >= 6 * 150) sawCursorPastPrefix = true;
  }
  CHECK(sawCursorPastPrefix);
}

void testCrossInkStatusBarAndXtcOverlay() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<4> interactions;
  Frame<4> frame(draw, device, input, interactions);

  // Reader status bar: clock leading, "page/count percent" trailing, chapter
  // title centered, themed progress thickness.
  StatusBarProps reader;
  reader.leading = "12:34";
  reader.trailing = "128/342 37%";
  reader.title = "Chapter Four";
  reader.showProgress = true;
  reader.progressHeight = 6;  // "thick" themed thickness
  reader.progress.value = 37;
  reader.progress.max = 100;
  statusBar(frame, Rect{0, 760, 480, 40}, reader);
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Text), 3u);

  // XTC overlay: same component, anchored at the top over a pre-rendered
  // page, with an opaque background so the page underneath is masked.
  FakeDrawTarget draw2;
  Frame<4> frame2(draw2, device, input, interactions);
  StatusBarProps overlay = reader;
  overlay.fillBackground = true;
  overlay.showProgress = false;
  statusBar(frame2, Rect{0, 0, 480, 32}, overlay);
  CHECK(draw2.opCount >= 4u);
  CHECK_EQ(draw2.ops[0].kind, FakeDrawTarget::Op::Fill);
  CHECK_EQ(draw2.ops[0].rect.y, 0);
  CHECK_EQ(draw2.ops[0].rect.width, 480);

  // Development-branch status bar: a left cluster of clock + estimated time
  // left, page progress trailing, and the title centered without colliding
  // with either cluster even when the cluster is wide.
  FakeDrawTarget draw3;
  Frame<4> frame3(draw3, device, input, interactions);
  StatusBarProps dev;
  dev.leading = "12:34";
  dev.leadingSecondary = "1h 20m left";  // 11 chars * 6px = 66px
  dev.trailing = "128/342 37%";
  dev.title = "A Fairly Long Chapter Title Here";
  statusBar(frame3, Rect{0, 760, 480, 40}, dev);
  CHECK_EQ(draw3.countKind(FakeDrawTarget::Op::Text), 4u);
  // Ops: leading, leadingSecondary, trailing, title (in draw order).
  const FakeDrawTarget::Op& secondary = draw3.ops[1];
  const FakeDrawTarget::Op& trailingOp = draw3.ops[2];
  const FakeDrawTarget::Op& titleOp = draw3.ops[3];
  CHECK(secondary.rect.x > draw3.ops[0].rect.right());
  CHECK(titleOp.rect.x >= secondary.rect.right());           // clear of the left cluster
  CHECK(titleOp.rect.right() <= trailingOp.rect.x);          // clear of the trailing text
  CHECK(titleOp.rect.width > 0);

  // Dark mode: black background fill plus white text, no SDK support needed
  // beyond paints — mirrors BaseTheme's darkMode flag.
  FakeDrawTarget draw4;
  Frame<4> frame4(draw4, device, input, interactions);
  StatusBarProps dark = dev;
  dark.fillBackground = true;
  dark.background = Paint::solid(Color::Black);
  dark.text.color = Color::White;
  statusBar(frame4, Rect{0, 760, 480, 40}, dark);
  CHECK_EQ(draw4.ops[0].color, Color::Black);
  bool allTextWhite = true;
  for (size_t i = 0; i < draw4.opCount; ++i) {
    if (draw4.ops[i].kind == FakeDrawTarget::Op::Text && draw4.ops[i].color != Color::White) allTextWhite = false;
  }
  CHECK(allTextWhite);
}

void testCrossInkReaderMenuList() {
  // Reader menu rows: label plus a right-aligned current value (rotate
  // orientation, auto page turn) and dimmed/disabled entries.
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<16> interactions;
  Frame<16> frame(draw, device, input, interactions);

  ListItem items[3]{};
  items[0].label = "Rotate screen";
  items[0].value = "Portrait";
  items[0].enabled = true;
  items[1].label = "Auto page turn";
  items[1].value = "Off";
  items[1].enabled = true;
  items[2].label = "Delete cache";
  items[2].enabled = false;

  ListProps menu;
  menu.items = items;
  menu.count = 3;
  menu.selectedIndex = 1;
  menu.action = 50;
  menu.rowHeight = 45;
  for (int i = 0; i < 3; ++i) items[i].actionValue = static_cast<int16_t>(i);
  list(frame, Rect{0, 100, 480, 300}, menu);

  // Disabled row registers no interaction; the two enabled rows do.
  CHECK_EQ(interactions.count(), 2u);
  // Values drawn right-aligned: label+value text ops for rows 0/1, label only
  // for row 2.
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Text), 5u);
}

void testCrossInkReadingStatsSurfaces() {
  // Mirrors CrossInk's feat/x3-reading-stats BookStatsView: a stat-cell grid
  // (value + label), a section card with title divider, and horizontal bar
  // charts where any nonzero value must stay visible.
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, input, interactions);

  // 3-across stat cells inside a card: borderless via transparent background.
  Stack<3> cells(Rect{0, 100, 480, 70}, Axis::Row, 0);
  cells.flex(1);
  cells.flex(1);
  cells.flex(1);
  cells.layout();
  const char* values[3] = {"12", "4h 32m", "37%"};
  const char* labelTexts[3] = {"Sessions", "Time", "Progress"};
  StyleSet plain;
  plain.normal.background = Paint::solid(Color::Transparent);
  for (int i = 0; i < 3; ++i) {
    MetricCardProps cell;
    cell.value = values[i];
    cell.caption = labelTexts[i];
    cell.styles = plain;
    metricCard(frame, cells.rect(i), cell);
  }
  // Borderless cells: no strokes, two text ops per cell.
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Stroke), 0u);
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Text), 6u);

  // Section card: outline + title + 1px divider, then bar rows.
  FakeDrawTarget draw2;
  Frame<8> frame2(draw2, device, input, interactions);
  const Rect card{0, 200, 480, 160};
  frame2.target().stroke(card, Paint::solid(Color::Black), 1);
  frame2.target().fill(Rect{card.x, static_cast<int16_t>(card.y + 30), card.width, 1}, Paint::solid(Color::Black));

  // Day-of-week chart: one row had 10 seconds out of a 36000 max — the bar
  // must still draw at minFill width instead of rounding to nothing.
  const uint32_t seconds[7] = {36000, 0, 10, 1200, 0, 9000, 400};
  uint32_t maxSeconds = 0;
  for (uint32_t s : seconds) maxSeconds = s > maxSeconds ? s : maxSeconds;
  int16_t rowY = static_cast<int16_t>(card.y + 40);
  for (int i = 0; i < 7; ++i) {
    ProgressBarProps bar;
    bar.value = static_cast<int32_t>(seconds[i]);
    bar.max = static_cast<int32_t>(maxSeconds);
    bar.minFill = 2;
    progressBar(frame2, Rect{90, rowY, 360, 14}, bar);
    rowY = static_cast<int16_t>(rowY + 16);
  }
  // Count only solid fills: progressBar also issues a no-op fill for the
  // (transparent) track that real targets ignore.
  int16_t tinyBarWidth = 0;
  int16_t maxBarWidth = 0;
  size_t barFills = 0;
  for (size_t i = 0; i < draw2.opCount; ++i) {
    const FakeDrawTarget::Op& op = draw2.ops[i];
    if (op.kind != FakeDrawTarget::Op::Fill || op.rect.height != 14 || op.paint != PaintKind::Solid) continue;
    ++barFills;
    if (op.rect.width > maxBarWidth) maxBarWidth = op.rect.width;
    if (tinyBarWidth == 0 || op.rect.width < tinyBarWidth) tinyBarWidth = op.rect.width;
  }
  CHECK_EQ(maxBarWidth, 360);  // the max row fills the chart
  CHECK_EQ(tinyBarWidth, 2);   // 10s of 36000s renders at minFill, not 0
  CHECK_EQ(barFills, 5u);      // zero rows draw nothing

}

void testInteractionOverflowFlag() {
  InteractionBuffer<2> buffer;
  CHECK(!buffer.overflowed());
  buffer.addInteraction(Interaction{Rect{0, 0, 10, 10}, 1, 0, InputDefault, StateNormal, 0});
  buffer.addInteraction(Interaction{Rect{0, 0, 10, 10}, 2, 0, InputDefault, StateNormal, 0});
  CHECK(!buffer.overflowed());
  buffer.addInteraction(Interaction{Rect{0, 0, 10, 10}, 3, 0, InputDefault, StateNormal, 0});
  CHECK(buffer.overflowed());
  buffer.clear();
  CHECK(!buffer.overflowed());
}


void testRoundedRaffSurfaces() {
  // Mirrors the retired RoundedRaffTheme: pill settings tabs with a bottom
  // divider, hug-content menu rows, and rounded keyboard keys — all from
  // styles, no custom drawing code.
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<16> interactions;
  Frame<16> frame(draw, device, input, interactions);

  TabItem tabs[3];
  tabs[0].label = "Display";
  tabs[0].value = 0;
  tabs[1].label = "Reader";
  tabs[1].value = 1;
  tabs[1].selected = true;
  tabs[2].label = "System";
  tabs[2].value = 2;
  TabBarProps bar;
  bar.tabs = tabs;
  bar.count = 3;
  bar.action = 60;
  bar.divider = true;
  bar.tabStyles.selected.background = Paint::solid(Color::Black);
  bar.tabStyles.selected.foreground = Paint::solid(Color::White);
  bar.tabStyles.selected.radius = 18;
  tabBar(frame, Rect{0, 0, 480, 50}, bar);

  CHECK_EQ(interactions.count(), 3u);
  // Selected tab fills a rounded pill; unselected tabs draw no background.
  size_t pillFills = 0;
  for (size_t i = 0; i < draw.opCount; ++i) {
    if (draw.ops[i].kind == FakeDrawTarget::Op::Fill && draw.ops[i].radius == 18) ++pillFills;
  }
  CHECK_EQ(pillFills, 1u);
  // Divider hugs the bottom edge at 1px.
  const FakeDrawTarget::Op& divider = draw.ops[draw.opCount - 1];
  CHECK_EQ(divider.rect.height, 1);
  CHECK_EQ(divider.rect.bottom(), 50);
  // Tapping the third slot routes its value.
  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 400;
  tap.touchY = 25;
  CHECK_EQ(interactions.route(tap).value, 2);

  // Hug-content rows: the selection pill wraps the label, not the full width.
  FakeDrawTarget draw2;
  Frame<16> frame2(draw2, device, input, interactions);
  ListItem items[2]{};
  items[0].label = "Browse Files";  // 12 chars * 6px = 72px
  items[0].enabled = true;
  items[1].label = "Settings";
  items[1].enabled = true;
  ListProps menu;
  menu.items = items;
  menu.count = 2;
  menu.selectedIndex = 0;
  menu.action = 61;
  menu.rowHeight = 40;
  menu.sidePadding = 20;
  menu.hugContents = true;
  menu.rowStyles.normal.background = Paint::solid(Color::White);
  menu.rowStyles.selected.background = Paint::solid(Color::Black);
  menu.rowStyles.selected.foreground = Paint::solid(Color::White);
  menu.rowStyles.selected.radius = 14;
  menu.rowStyles.normal.radius = 14;
  list(frame2, Rect{0, 100, 480, 200}, menu);
  bool sawHuggedPill = false;
  for (size_t i = 0; i < draw2.opCount; ++i) {
    const FakeDrawTarget::Op& op = draw2.ops[i];
    if (op.kind == FakeDrawTarget::Op::Fill && op.rect.height == 40 && op.color == Color::Black) {
      CHECK_EQ(op.rect.width, 72 + 40);  // label + 2 * sidePadding
      CHECK_EQ(op.radius, 14);
      sawHuggedPill = true;
    }
  }
  CHECK(sawHuggedPill);
}


void testThemePrimitiveParity() {
  // Everything the retired firmware themes drew by hand must be expressible:
  // selection markers, per-corner cards, key glyph art, and a charging bolt.
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<16> interactions;
  Frame<16> frame(draw, device, input, interactions);

  // Underline selection marker (super-minimal theme style).
  ListItem items[2]{};
  items[0].label = "Browse";
  items[0].enabled = true;
  items[1].label = "Settings";
  items[1].enabled = true;
  ListProps menu;
  menu.items = items;
  menu.count = 2;
  menu.selectedIndex = 1;
  menu.rowHeight = 40;
  menu.sidePadding = 10;
  menu.selectionMarker = SelectionMarker::Underline;
  menu.markerThickness = 3;
  list(frame, Rect{0, 0, 480, 100}, menu);
  bool sawUnderline = false;
  for (size_t i = 0; i < draw.opCount; ++i) {
    const FakeDrawTarget::Op& op = draw.ops[i];
    if (op.kind == FakeDrawTarget::Op::Fill && op.rect.height == 3 && op.rect.bottom() == 80) sawUnderline = true;
  }
  CHECK(sawUnderline);

  // Triangle selection marker (v1 Triangle style: 12x18 at the row edge).
  FakeDrawTarget draw2;
  Frame<16> frame2(draw2, device, input, interactions);
  menu.selectionMarker = SelectionMarker::Triangle;
  menu.markerInset = 4;
  list(frame2, Rect{0, 0, 480, 100}, menu);
  CHECK_EQ(draw2.countKind(FakeDrawTarget::Op::Triangle), 1u);

  // RoundedRaff cover card bands: top band rounds only its top corners.
  FakeDrawTarget draw3;
  Frame<16> frame3(draw3, device, input, interactions);
  frame3.target().fill(Rect{20, 100, 440, 30}, Paint::dither(Color::LightGray), 14, CornersTop);
  frame3.target().fill(Rect{20, 400, 440, 30}, Paint::dither(Color::LightGray), 14, CornersBottom);
  CHECK_EQ(draw3.ops[0].corners, static_cast<uint8_t>(CornersTop));
  CHECK_EQ(draw3.ops[1].corners, static_cast<uint8_t>(CornersBottom));

  // Keyboard space/delete affordances come from the component now.
  FakeDrawTarget draw4;
  Frame<16> frame4(draw4, device, input, interactions);
  const KeyGridKey bottom[2] = {
      {nullptr, nullptr, {}, {}, KeyKind::Space, StateNormal, 1, true},
      {nullptr, nullptr, {}, {}, KeyKind::Delete, StateNormal, 2, true},
  };
  KeyGridProps row;
  row.keys = bottom;
  row.rows = 1;
  row.cols = 2;
  row.action = 70;
  keyGrid(frame4, Rect{0, 600, 200, 40}, row);
  CHECK_EQ(draw4.countKind(FakeDrawTarget::Op::Line), 1u);
  CHECK_EQ(draw4.countKind(FakeDrawTarget::Op::Bitmap), 1u);

  // Charging battery draws a bolt (two triangles) instead of a dithered fill.
  FakeDrawTarget draw5;
  Frame<16> frame5(draw5, device, input, interactions);
  BatteryIndicatorProps battery;
  battery.percent = 80;
  battery.charging = true;
  batteryIndicator(frame5, Rect{400, 0, 80, 20}, battery);
  CHECK_EQ(draw5.countKind(FakeDrawTarget::Op::Triangle), 2u);
}


void testRotationAndBitmapSampling() {
  // Rotated labels (side-bezel button hints) carry rotation through TextStyle.
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<4> interactions;
  Frame<4> frame(draw, device, input, interactions);
  TextStyle vertical;
  vertical.rotation = Rotation::CW90;
  vertical.align = TextAlign::Center;
  frame.target().text(Rect{450, 155, 30, 78}, "Up", vertical);
  CHECK(draw.ops[0].rotation == Rotation::CW90);

  // Shared bitmap sampling math: a 2x2 checker mask (bits 10 / 01).
  const uint8_t checker[2] = {0x80, 0x40};  // row 0: ink at x0; row 1: ink at x1
  BitmapRef mask;
  mask.data = checker;
  mask.width = 2;
  mask.height = 2;
  mask.format = BitmapFormat::BW1;

  int pixels = 0;
  int16_t maxX = 0, maxY = 0;
  auto count = [&](int16_t x, int16_t y) {
    ++pixels;
    if (x > maxX) maxX = x;
    if (y > maxY) maxY = y;
  };

  // Stretch 2x2 -> 8x8: each source pixel becomes 4x4, half the area is ink.
  forEachBitmapPixel(Rect{0, 0, 8, 8}, mask, BitmapMode::Stretch, count);
  CHECK_EQ(pixels, 32);
  CHECK_EQ(maxX, 7);
  CHECK_EQ(maxY, 7);

  // Contain in a 8x4 rect: limited by height -> 4x4 output, centered at x=2.
  pixels = 0;
  int16_t minX = 100;
  forEachBitmapPixel(Rect{0, 0, 8, 4}, mask, BitmapMode::Contain,
                     [&](int16_t x, int16_t y) { ++pixels; if (x < minX) minX = x; (void)y; });
  CHECK_EQ(pixels, 8);
  CHECK_EQ(minX, 2);

  // Cover the same rect: scales to 8x8 and clips to the 8x4 rect.
  pixels = 0;
  maxY = 0;
  forEachBitmapPixel(Rect{0, 0, 8, 4}, mask, BitmapMode::Cover, [&](int16_t x, int16_t y) {
    ++pixels;
    if (y > maxY) maxY = y;
    (void)x;
  });
  CHECK_EQ(pixels, 16);
  CHECK(maxY <= 3);

  // Tile a 6x6 rect: 9 repeats of the 2-ink-pixel cell.
  pixels = 0;
  forEachBitmapPixel(Rect{0, 0, 6, 6}, mask, BitmapMode::Tile, count);
  CHECK_EQ(pixels, 18);

  // TileX repeats horizontally only: 3 repeats wide, native height.
  pixels = 0;
  forEachBitmapPixel(Rect{0, 0, 6, 6}, mask, BitmapMode::TileX, count);
  CHECK_EQ(pixels, 6);

  // Per-element icon rotation: a single ink pixel at (0,0) lands in the
  // rotation-appropriate corner of a 2x2 draw.
  const uint8_t dot[2] = {0x80, 0x00};
  BitmapRef dotMask;
  dotMask.data = dot;
  dotMask.width = 2;
  dotMask.height = 2;
  dotMask.format = BitmapFormat::BW1;
  int16_t gotX = -1, gotY = -1;
  auto capture = [&](int16_t x, int16_t y) { gotX = x; gotY = y; };
  forEachBitmapPixel(Rect{0, 0, 2, 2}, dotMask, BitmapMode::Center, capture, Rotation::CW90);
  CHECK_EQ(gotX, 1);
  CHECK_EQ(gotY, 0);
  forEachBitmapPixel(Rect{0, 0, 2, 2}, dotMask, BitmapMode::Center, capture, Rotation::R180);
  CHECK_EQ(gotX, 1);
  CHECK_EQ(gotY, 1);
  forEachBitmapPixel(Rect{0, 0, 2, 2}, dotMask, BitmapMode::Center, capture, Rotation::CCW90);
  CHECK_EQ(gotX, 0);
  CHECK_EQ(gotY, 1);

  // Mask1 polarity (the freeink::Icon convention): bit 0 = draw, bit 1 =
  // transparent — the inverse of BW1. The same `dot` bits (one set bit at
  // 0,0) plot the OTHER three pixels under Mask1.
  BitmapRef dotInv = dotMask;
  dotInv.format = BitmapFormat::Mask1;
  int drawn = 0;
  bool hitOrigin = false;
  forEachBitmapPixel(Rect{0, 0, 2, 2}, dotInv, BitmapMode::Center, [&](int16_t x, int16_t y) {
    ++drawn;
    if (x == 0 && y == 0) hitOrigin = true;
  });
  CHECK_EQ(drawn, 3);       // three clear bits draw
  CHECK(!hitOrigin);        // the one set bit is transparent
}


void testListSectionHeaders() {
  // Settings-style list: section header rows are shorter, non-interactive,
  // underlined, and add padding above each section after the first.
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<16> interactions;
  Frame<16> frame(draw, device, input, interactions);

  ListItem items[5]{};
  items[0].label = "Display";
  items[0].isHeader = true;
  items[1].label = "Theme";
  items[1].actionValue = 1;
  items[1].enabled = true;
  items[2].label = "Sleep Screen";
  items[2].actionValue = 2;
  items[2].enabled = true;
  items[3].label = "Reader";
  items[3].isHeader = true;
  items[4].label = "Font Size";
  items[4].actionValue = 4;
  items[4].enabled = true;

  ListProps menu;
  menu.items = items;
  menu.count = 5;
  menu.selectedIndex = 1;
  menu.action = 80;
  menu.rowHeight = 40;
  menu.sidePadding = 10;
  menu.sectionGap = 20;
  list(frame, Rect{0, 0, 480, 400}, menu);

  // Only the three item rows are interactive.
  CHECK_EQ(interactions.count(), 3u);
  CHECK_EQ(interactions.data()[0].value, 1);
  CHECK_EQ(interactions.data()[2].value, 4);

  // Header underlines: two 1px fills spanning the padded width.
  size_t underlines = 0;
  int16_t secondHeaderY = 0;
  for (size_t i = 0; i < draw.opCount; ++i) {
    const FakeDrawTarget::Op& op = draw.ops[i];
    if (op.kind == FakeDrawTarget::Op::Fill && op.rect.height == 1 && op.rect.width == 460) {
      ++underlines;
      secondHeaderY = op.rect.y;
    }
  }
  CHECK_EQ(underlines, 2u);
  // Second section: header height 16 (12 line + 4 gap) + two 40px rows +
  // 20px section gap puts its underline at 16 + 80 + 20 + 12 + 2 = 130.
  CHECK_EQ(secondHeaderY, 130);
}


void testCrossInkSleepScreenComposition() {
  // The minimal-stats sleep screen composes from existing pieces: an
  // app-drawn cover slot, a title block, and a stats overlay row of
  // value/label cells with an icon — no bespoke SDK surface needed.
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, input, interactions);

  Stack<3> screen(Rect{0, 0, 480, 800}, Axis::Column, 0);
  screen.fixed(520);  // cover slot, app-drawn
  screen.fixed(80);   // title/author
  screen.flex(1);     // stats overlay
  screen.layout();

  TextStyle title;
  title.align = TextAlign::Center;
  frame.target().text(screen.rect(1), "The Name of the Wind", title);

  Stack<3> statsRow(screen.rect(2), Axis::Row, 8);
  statsRow.flex(1);
  statsRow.flex(1);
  statsRow.flex(1);
  statsRow.layout();
  StyleSet plain;
  plain.normal.background = Paint::solid(Color::Transparent);
  const char* values[3] = {"12", "4h 32m", "37%"};
  const char* captions[3] = {"day streak", "this book", "complete"};
  for (int i = 0; i < 3; ++i) {
    MetricCardProps cell;
    cell.value = values[i];
    cell.caption = captions[i];
    cell.styles = plain;
    metricCard(frame, statsRow.rect(i), cell);
  }
  ProgressBarProps progress;
  progress.value = 37;
  progress.max = 100;
  progressBar(frame, Rect{40, 780, 400, 6}, progress);

  // Title + 3 cells x (value + caption) = 7 text ops; progress fill present;
  // nothing interactive on a sleep screen.
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Text), 7u);
  CHECK_EQ(interactions.count(), 0u);
}


void testCoverCarousel() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<16> interactions;
  Frame<16> frame(draw, device, input, interactions);

  CarouselProps props;
  props.count = 5;
  props.selectedIndex = 2;
  props.action = 90;
  props.centerSize = Size{200, 300};
  props.sideSize = Size{100, 150};
  props.gap = 10;
  CarouselSlot slots[3];
  coverCarousel(frame, Rect{0, 100, 480, 400}, props, slots);

  // Geometry: center is centered and larger; sides flank it, vertically
  // centered, app gets content rects inside the frames.
  CHECK(slots[1].valid);
  CHECK(slots[1].isCenter);
  CHECK_EQ(slots[1].frame.x, 140);
  CHECK_EQ(slots[1].frame.width, 200);
  CHECK_EQ(slots[0].frame.right(), 130);
  CHECK_EQ(slots[2].frame.x, 350);
  CHECK_EQ(slots[0].itemIndex, 1);
  CHECK_EQ(slots[2].itemIndex, 3);
  CHECK_EQ(slots[1].content.width, 192);  // frame inset by contentInset 4

  // Default carousel chrome is borderless; the center slot is selected via fill.
  bool sawSelectedCenterFill = false;
  for (size_t i = 0; i < draw.opCount; ++i) {
    if (draw.ops[i].kind == FakeDrawTarget::Op::Fill && draw.ops[i].rect.x == 140 &&
        draw.ops[i].color == Color::Black) {
      sawSelectedCenterFill = true;
    }
  }
  CHECK(sawSelectedCenterFill);
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Stroke), 0u);

  // Tap a side cover -> that item; swipe left -> next item.
  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 80;
  tap.touchY = 300;
  CHECK_EQ(interactions.route(tap).value, 1);
  InputSnapshot swipe;
  swipe.swipeLeft = true;
  CHECK_EQ(interactions.route(swipe).value, 3);
  InputSnapshot prev;
  prev.prev = true;
  CHECK_EQ(interactions.route(prev).value, 1);

  // Edges without wrap: first item has no previous slot.
  FakeDrawTarget draw2;
  InteractionBuffer<16> interactions2;
  Frame<16> frame2(draw2, device, input, interactions2);
  props.selectedIndex = 0;
  coverCarousel(frame2, Rect{0, 100, 480, 400}, props, slots);
  CHECK(!slots[0].valid);
  CHECK(slots[2].valid);

  // With wrap, the previous slot comes from the far end.
  FakeDrawTarget draw3;
  InteractionBuffer<16> interactions3;
  Frame<16> frame3(draw3, device, input, interactions3);
  props.wrap = true;
  coverCarousel(frame3, Rect{0, 100, 480, 400}, props, slots);
  CHECK(slots[0].valid);
  CHECK_EQ(slots[0].itemIndex, 4);
}


void testLayoutTextWrapping() {
  // The SDK owns wrap/ellipsis so DrawTarget implementors only draw runs.
  FakeDrawTarget draw;  // 6px per char, 12px line height
  char lines[4][64];
  Rect rects[4];
  int n = 0;
  auto collect = [&](const char* line, Rect r) {
    std::snprintf(lines[n], sizeof(lines[n]), "%s", line);
    rects[n] = r;
    ++n;
  };

  // Greedy word wrap: 72px fits 12 chars.
  TextStyle style;
  style.maxLines = 3;
  layoutText(draw, Rect{0, 0, 72, 100}, "hello world again", style, collect);
  CHECK_EQ(n, 2);
  CHECK(std::strcmp(lines[0], "hello world") == 0);
  CHECK(std::strcmp(lines[1], "again") == 0);
  CHECK_EQ(rects[0].width, 66);
  // Two 12px lines centered in 100px: block starts at 38.
  CHECK_EQ(rects[0].y, 38);
  CHECK_EQ(rects[1].y, 50);

  // maxLines 1 with leftover text: last line shrinks until line+ellipsis fits.
  n = 0;
  style.maxLines = 1;
  layoutText(draw, Rect{0, 0, 72, 20}, "hello world again", style, collect);
  CHECK_EQ(n, 1);
  CHECK(std::strcmp(lines[0], "hello wor\xE2\x80\xA6") == 0);
  CHECK_EQ(rects[0].width, 72);

  // Hard line breaks.
  n = 0;
  style.maxLines = 3;
  layoutText(draw, Rect{0, 0, 200, 60}, "one\ntwo", style, collect);
  CHECK_EQ(n, 2);
  CHECK(std::strcmp(lines[0], "one") == 0);
  CHECK(std::strcmp(lines[1], "two") == 0);

  // A word wider than the rect breaks at characters.
  n = 0;
  style.maxLines = 2;
  layoutText(draw, Rect{0, 0, 30, 40}, "abcdefghij", style, collect);
  CHECK_EQ(n, 2);
  CHECK(std::strcmp(lines[0], "abcde") == 0);
  CHECK(std::strcmp(lines[1], "fghij") == 0);

  // Center alignment positions the measured run.
  n = 0;
  style.maxLines = 1;
  style.align = TextAlign::Center;
  layoutText(draw, Rect{0, 0, 100, 20}, "hi", style, collect);
  CHECK_EQ(n, 1);
  CHECK_EQ(rects[0].x, 44);  // (100 - 12) / 2
}


void testTouchToLogical() {
  // Panel-native normalized coords -> logical frame, per the device's
  // touchOrientation transform selector (default Portrait = identity).
  DeviceContext portrait = makeDevice(480, 800);
  Point p = touchToLogical(portrait, 0.5f, 0.25f);
  CHECK_EQ(p.x, 240);
  CHECK_EQ(p.y, 200);

  portrait.touchOrientation = Orientation::PortraitInverted;
  p = touchToLogical(portrait, 0.0f, 0.0f);
  CHECK_EQ(p.x, 479);
  CHECK_EQ(p.y, 799);

  // The WakeInk case: 416x240, CCW transform — fbX = ny*W, fbY = (1-nx)*H.
  DeviceContext landscape = makeDevice(416, 240);
  landscape.touchOrientation = Orientation::LandscapeCounterClockwise;
  p = touchToLogical(landscape, 0.0f, 0.0f);
  CHECK_EQ(p.x, 0);
  CHECK_EQ(p.y, 239);
  p = touchToLogical(landscape, 1.0f, 1.0f);
  CHECK_EQ(p.x, 415);
  CHECK_EQ(p.y, 0);
  p = touchToLogical(landscape, 0.25f, 0.5f);
  CHECK_EQ(p.x, 208);
  CHECK_EQ(p.y, 180);

  landscape.touchOrientation = Orientation::LandscapeClockwise;
  p = touchToLogical(landscape, 0.0f, 0.0f);
  CHECK_EQ(p.x, 415);
  CHECK_EQ(p.y, 0);

  // Mounting mirrors apply in panel space, before the rotation.
  landscape.touchOrientation = Orientation::LandscapeCounterClockwise;
  p = touchToLogical(landscape, 0.0f, 0.0f, /*flipX=*/true, /*flipY=*/false);
  CHECK_EQ(p.x, 0);
  CHECK_EQ(p.y, 0);

  // Out-of-range input clamps inside the screen.
  p = touchToLogical(landscape, 1.0f, 1.0f);
  CHECK(p.x <= 415);
  CHECK(p.y <= 239);

  // touchOrientationFor() picks the transform that inverts a target's render
  // rotation (DisplayTarget's Portrait render rotates 90 CW, so touch maps
  // back through LandscapeClockwise, and so on).
  CHECK(touchOrientationFor(Orientation::Portrait) == Orientation::LandscapeClockwise);
  CHECK(touchOrientationFor(Orientation::PortraitInverted) == Orientation::LandscapeCounterClockwise);
  CHECK(touchOrientationFor(Orientation::LandscapeClockwise) == Orientation::PortraitInverted);
  CHECK(touchOrientationFor(Orientation::LandscapeCounterClockwise) == Orientation::Portrait);
}


void testMeasureWrappedText() {
  FakeDrawTarget draw;  // 6px per char, 12px line height

  // Two wrapped lines: height = 2 * 12, width = widest line.
  TextStyle style;
  style.maxLines = 3;
  Size size = measureWrappedText(draw, "hello world again", style, 72);
  CHECK_EQ(size.height, 24);
  CHECK_EQ(size.width, 66);  // "hello world"

  // maxLines 1 ellipsizes; the measured width includes the ellipsis tail.
  style.maxLines = 1;
  size = measureWrappedText(draw, "hello world again", style, 72);
  CHECK_EQ(size.height, 12);
  CHECK_EQ(size.width, 72);

  // optionDialogHeight: padding + caption + wrapped headline + body + buttons,
  // and a dialog rendered at exactly that height fits all six text ops.
  OptionDialogProps d;
  d.title = "Skip this event?";
  d.headline = "Quarterly planning sync with the hardware and firmware teams";
  d.headlineText.maxLines = 2;
  d.message = "3:30 PM - 4:00 PM";
  const DialogOption options[2] = {
      {"Skip", 20, 0, StateNormal, true},
      {"Cancel", 21, 0, StateNormal, true},
  };
  d.options = options;
  d.optionCount = 2;
  const int16_t h = optionDialogHeight(draw, d, 356);
  // 24 padding + (12 + 8) caption + (24 + 8) headline + 12 message + (8 + 44) buttons
  CHECK_EQ(h, 140);

  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  FakeDrawTarget draw2;
  Frame<8> frame(draw2, device, input, interactions);
  optionDialog(frame, Rect{30, 34, 356, h}, d);
  CHECK_EQ(draw2.countKind(FakeDrawTarget::Op::Text), 6u);
  CHECK_EQ(interactions.count(), 2u);
}


void testButtonHitPadding() {
  // hitPadding gives adjacent controls contiguous, non-overlapping tap bands
  // with a single interaction each — no separate band registration.
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  device.minTouchSize = 0;
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, input, interactions);

  // A stepper pair: [-] at x=302 w=52, [+] at x=358 w=52, 4px gap split 2/2.
  ButtonProps minus;
  minus.label = "-";
  minus.action = 100;
  minus.minTouchSize = 0;
  minus.hitPadding = Insets{2, 2, 4, 4};  // top, right, bottom, left
  button(frame, Rect{302, 100, 52, 30}, minus);
  ButtonProps plus = minus;
  plus.action = 101;
  plus.hitPadding = Insets{2, 4, 4, 2};
  button(frame, Rect{358, 100, 52, 30}, plus);

  CHECK_EQ(interactions.count(), 2u);
  // One interaction each, bands contiguous at x=356: [-] owns 298..356,
  // [+] owns 356..414.
  CHECK_EQ(interactions.data()[0].rect.x, 298);
  CHECK_EQ(interactions.data()[0].rect.right(), 356);
  CHECK_EQ(interactions.data()[1].rect.x, 356);
  // A tap in the gap right of the [-] visual resolves to [-], not [+].
  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 355;
  tap.touchY = 115;
  CHECK_EQ(interactions.route(tap).action, 100);

  // hitPadding composes with edge snapping: a button 4px from the bottom
  // bezel reaches it.
  FakeDrawTarget draw2;
  InteractionBuffer<8> interactions2;
  Frame<8> frame2(draw2, device, input, interactions2);
  ButtonProps pager;
  pager.label = "Next";
  pager.action = 102;
  pager.minTouchSize = 0;
  button(frame2, Rect{352, 768, 120, 28}, pager);  // bottom gap 4 < snap 12
  CHECK_EQ(interactions2.data()[0].rect.bottom(), 800);
}

void testInvertedDrawTarget() {
  CHECK(invertedColor(Color::Black) == Color::White);
  CHECK(invertedColor(Color::White) == Color::Black);
  CHECK(invertedColor(Color::LightGray) == Color::DarkGray);
  CHECK(invertedColor(Color::DarkGray) == Color::LightGray);
  CHECK(invertedColor(Color::Transparent) == Color::Transparent);
  CHECK(invertedPaint(Paint::none()).kind == PaintKind::None);
  CHECK(invertedPaint(Paint::dither(Color::LightGray)).color == Color::DarkGray);

  // Render a default-styled button through the inverted target: its white
  // background must come out black and its black label white — with no
  // component-level dark-mode props involved.
  FakeDrawTarget draw;
  InvertedDrawTarget dark(draw);
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<4> interactions;
  Frame<4> frame(dark, device, input, interactions);

  ButtonProps props;
  props.label = "OK";
  props.action = 1;
  button(frame, Rect{0, 0, 100, 44}, props);
  CHECK(draw.ops[0].kind == FakeDrawTarget::Op::Fill);
  CHECK(draw.ops[0].color == Color::Black);  // default white background, inverted
  bool labelWhite = false;
  for (size_t i = 0; i < draw.opCount; ++i) {
    if (draw.ops[i].kind == FakeDrawTarget::Op::Text && draw.ops[i].color == Color::White) labelWhite = true;
  }
  CHECK(labelWhite);

  // Disabled wrapper is a pure passthrough.
  FakeDrawTarget draw2;
  InvertedDrawTarget off(draw2, false);
  Frame<4> frame2(off, device, input, interactions);
  button(frame2, Rect{0, 0, 100, 44}, props);
  CHECK(draw2.ops[0].color == Color::White);

  // Flipping at runtime — the one call that inverts the whole UI next frame.
  off.setEnabled(true);
  FakeDrawTarget draw3;
  InvertedDrawTarget on(draw3, off.enabled());
  Frame<4> frame3(on, device, input, interactions);
  batteryIndicator(frame3, Rect{0, 0, 80, 20}, BatteryIndicatorProps{50});
  bool sawWhiteInk = false;
  for (size_t i = 0; i < draw3.opCount; ++i) {
    if (draw3.ops[i].color == Color::White) sawWhiteInk = true;
  }
  CHECK(sawWhiteInk);  // the battery's default black ink inverted too
}

void testStyleSetUnset() {
  StyleSet styles;
  CHECK(styles.unset());
  styles.normal.border = Paint::solid(Color::Black);  // outline-only style counts as set
  CHECK(!styles.unset());
  StyleSet selectedOnly;
  selectedOnly.selected.background = Paint::solid(Color::Black);
  CHECK(!selectedOnly.unset());
  StyleSet plain = plainStyles();
  CHECK(!plain.unset());
  CHECK(plain.normal.border.kind == PaintKind::None);
  CHECK(plain.normal.borderWidth == 0);
}

void testDefaultStylesAreBorderless() {
  StyleSet button = defaultButtonStyles();
  CHECK(button.normal.border.kind == PaintKind::None);
  CHECK(button.selected.border.kind == PaintKind::None);
  CHECK(button.focused.border.kind == PaintKind::None);
  CHECK(button.active.border.kind == PaintKind::None);
  CHECK(button.disabled.border.kind == PaintKind::None);

  StyleSet row = defaultListRowStyles();
  CHECK(row.normal.border.kind == PaintKind::None);
  CHECK(row.selected.border.kind == PaintKind::None);
  CHECK(row.focused.border.kind == PaintKind::None);

  StyleSet popup = defaultPopupStyles();
  CHECK(popup.normal.border.kind == PaintKind::None);
  CHECK(popup.selected.border.kind == PaintKind::None);
}

void testEReaderSettingsComponents() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<24> interactions;
  Frame<24> frame(draw, device, input, interactions);

  SettingRowProps row;
  row.label = "Wi-Fi";
  row.value = "On";
  row.action = 300;
  row.valueId = 1;
  row.labelText.maxLines = 1;
  row.valueText.maxLines = 1;
  settingRow(frame, Rect{0, 0, 240, 44}, row);

  ToggleRowProps toggle;
  toggle.row.label = "Dark mode";
  toggle.row.labelText.maxLines = 1;
  toggle.row.action = 301;
  toggle.checked = true;
  toggle.radius = 3;
  toggle.knobRadius = 1;
  toggleRow(frame, Rect{0, 48, 240, 44}, toggle);

  StepperRowProps stepper;
  stepper.row.label = "Font";
  stepper.row.labelText.maxLines = 1;
  stepper.row.valueText.maxLines = 1;
  stepper.value = "12";
  stepper.decrement = 302;
  stepper.increment = 303;
  stepperRow(frame, Rect{0, 96, 240, 44}, stepper);

  const RadioOption options[3] = {{"Small", 1, true}, {"Medium", 2, true}, {"Large", 3, true}};
  RadioGroupProps radio;
  radio.options = options;
  radio.count = 3;
  radio.selectedValue = 2;
  radio.action = 304;
  radioGroup(frame, Rect{0, 144, 240, 40}, radio);

  CHECK_EQ(interactions.count(), 7u);
  CHECK_EQ(interactions.data()[0].action, 300);
  CHECK_EQ(interactions.data()[1].action, 301);
  CHECK_EQ(interactions.data()[2].action, 302);
  CHECK_EQ(interactions.data()[3].action, 303);
  CHECK_EQ(interactions.data()[5].value, 2);
  CHECK(draw.countKind(FakeDrawTarget::Op::Line) >= 3u);
  CHECK(draw.countKind(FakeDrawTarget::Op::Text) >= 7);
  CHECK(draw.countKind(FakeDrawTarget::Op::Stroke) >= 1);
  bool sawToggleRadius = false;
  bool sawInsetStepperPlus = false;
  for (size_t i = 0; i < draw.opCount; ++i) {
    if (draw.ops[i].kind == FakeDrawTarget::Op::Stroke && draw.ops[i].radius == 3) sawToggleRadius = true;
    // Stepper sizes derive from the value font (lineH 12): buttons 22h/30w,
    // value slot measure("12") + 12 = 24. Plus button: 240 - 8 - 30 - 6 - 24
    // - 6 - 30... i.e. controlsX 136 + 30 + 6 + 24 + 6 = 202.
    if (draw.ops[i].kind == FakeDrawTarget::Op::Fill && draw.ops[i].rect.x == 202 && draw.ops[i].rect.width == 30) {
      sawInsetStepperPlus = true;
    }
  }
  CHECK(sawToggleRadius);
  CHECK(sawInsetStepperPlus);
}

void testLvglParityControls() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice(320, 240);
  InputSnapshot input;
  InteractionBuffer<16> interactions;
  Frame<16> frame(draw, device, input, interactions);

  CheckboxProps check;
  check.label = "Sync";
  check.checked = true;
  check.action = 610;
  checkbox(frame, Rect{0, 0, 160, 40}, check);

  SliderProps slide;
  slide.value = 50;
  slide.max = 100;
  slide.action = 611;
  slider(frame, Rect{0, 48, 160, 34}, slide);

  DropdownProps drop;
  drop.label = "Font";
  drop.value = "Noto Sans";
  drop.action = 612;
  dropdown(frame, Rect{0, 90, 180, 40}, drop);

  const char* cells[6] = {"Name", "Value", "Battery", "82%", "Wi-Fi", "On"};
  TableProps tableProps;
  tableProps.cells = cells;
  tableProps.rows = 3;
  tableProps.cols = 2;
  tableProps.headerRow = true;
  table(frame, Rect{0, 138, 220, 72}, tableProps);

  CHECK_EQ(interactions.count(), 3u);
  CHECK_EQ(interactions.data()[0].action, 610);
  CHECK_EQ(interactions.data()[1].action, 611);
  CHECK_EQ(interactions.data()[2].action, 612);
  CHECK(draw.countKind(FakeDrawTarget::Op::Line) >= 4u);
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Triangle), 0u);
  CHECK(draw.countKind(FakeDrawTarget::Op::Text) >= 8u);
}

void testQwertyKeyboardComponent() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<40> interactions;
  Frame<40> frame(draw, device, input, interactions);

  QwertyKeyboardProps keyboard;
  keyboard.keyAction = 400;
  keyboard.shiftAction = 401;
  keyboard.modeAction = 402;
  keyboard.deleteAction = 403;
  keyboard.okAction = 404;
  keyboard.selectedIndex = 5;
  qwertyKeyboard(frame, Rect{0, 0, 480, 160}, keyboard);

  CHECK_EQ(interactions.count(), 31u);
  CHECK_EQ(interactions.data()[0].value, static_cast<int16_t>('q'));
  CHECK_EQ(interactions.data()[19].action, 401);
  CHECK_EQ(interactions.data()[27].action, 403);
  CHECK_EQ(interactions.data()[29].value, QWERTY_KEY_SPACE);
  CHECK_EQ(interactions.data()[30].action, 404);
  CHECK(draw.countKind(FakeDrawTarget::Op::Bitmap) >= 1u);
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Stroke), 0u);

  InputSnapshot tap;
  tap.touchReleased = true;
  tap.touchX = 250;
  tap.touchY = 145;
  CHECK_EQ(interactions.route(tap).value, QWERTY_KEY_SPACE);
}

void testLocalizedKeyboardLayout() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<40> interactions;
  Frame<40> frame(draw, device, input, interactions);

  KeyboardProps props;
  props.layout = &builtinKeyboardLayout(KeyboardLayoutId::SpanishEs);
  props.keyAction = 410;
  props.shiftAction = 411;
  props.deleteAction = 412;
  props.okAction = 413;
  props.labelText.align = TextAlign::Center;
  keyboard(frame, Rect{0, 0, 480, 160}, props);

  CHECK_EQ(interactions.count(), 32u);
  CHECK_EQ(interactions.data()[19].value, 1201);  // Spanish ñ key has a stable non-ASCII key id.
  CHECK_EQ(interactions.data()[28].action, 412);
  CHECK_EQ(interactions.data()[31].action, 413);
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Stroke), 0u);
}

void testSymbolKeyboardPages() {
  // `shifted` pages the symbols layers: page one ("?123") and page two ("#+=").
  const KeyboardLayout& page1 = builtinKeyboardLayout(KeyboardLayoutId::QwertyEn, false, true);
  const KeyboardLayout& page2 = builtinKeyboardLayout(KeyboardLayoutId::QwertyEn, true, true);
  CHECK(&page1 != &page2);
  CHECK(std::strcmp(page1.rows[2].keys[0].label, "#+=") == 0);   // shift slot pages forward
  CHECK(std::strcmp(page2.rows[2].keys[0].label, "123") == 0);   // ...and back
  CHECK(std::strcmp(page1.rows[3].keys[0].label, "ABC") == 0);   // mode slot exits to letters
  CHECK(std::strcmp(page2.rows[3].keys[0].label, "ABC") == 0);

  // The four English layers together cover every printable ASCII character.
  bool covered[128] = {};
  covered[' '] = true;  // the space key emits value 32 (QWERTY_KEY_SPACE)
  const KeyboardLayout* layers[] = {
      &builtinKeyboardLayout(KeyboardLayoutId::QwertyEn, false, false),
      &builtinKeyboardLayout(KeyboardLayoutId::QwertyEn, true, false),
      &page1,
      &page2,
  };
  for (const KeyboardLayout* layout : layers) {
    for (uint8_t row = 0; row < layout->rowCount; ++row) {
      for (uint8_t col = 0; col < layout->rows[row].count; ++col) {
        const char* out = layout->rows[row].keys[col].output;
        if (out && out[0] > 0 && out[1] == 0) covered[static_cast<int>(out[0])] = true;
      }
    }
  }
  for (int c = 0x20; c <= 0x7E; ++c) CHECK(covered[c]);
}

void testKeyboardEntry() {
  char buf[12] = "";
  KeyboardEntry kb;
  kb.attach(buf, sizeof buf, /*startShifted=*/true);

  // Shift auto-releases after one letter.
  CHECK(kb.key('H'));
  CHECK(!kb.shifted);
  CHECK(kb.key('i'));
  CHECK(std::strcmp(buf, "Hi") == 0);
  CHECK_EQ(kb.length(), 2u);

  // Space keys carry KeyKind::Space with a null layout output (they draw a
  // glyph, not a label) but must still insert a space.
  CHECK(kb.key(QWERTY_KEY_SPACE));
  CHECK(std::strcmp(buf, "Hi ") == 0);
  CHECK(kb.backspace());
  CHECK(std::strcmp(buf, "Hi") == 0);

  // Symbols: mode() enters, shift() pages, layers stay sticky across keys.
  kb.mode();
  CHECK(kb.symbols);
  CHECK(kb.key('1'));
  CHECK(kb.symbols);
  kb.shift();  // page two ("#+=")
  CHECK(kb.key('['));
  CHECK(std::strcmp(buf, "Hi1[") == 0);
  kb.mode();  // back to letters
  CHECK(!kb.symbols);
  CHECK(!kb.shifted);

  // Localized keys insert the layout's UTF-8 output, and backspace removes
  // the whole code point — the (char)value cast this replaces corrupted both.
  kb.layout = KeyboardLayoutId::SpanishEs;
  CHECK(kb.key(1201));  // ñ
  CHECK(std::strcmp(buf, "Hi1[\xc3\xb1") == 0);
  CHECK(kb.backspace());
  CHECK(std::strcmp(buf, "Hi1[") == 0);

  // Full buffer: append fails, contents stay intact and terminated.
  kb.attach(buf, 3);  // resumes from "Hi1[" truncated view: len clamps to cap-1
  CHECK_EQ(kb.length(), 2u);
  CHECK(!kb.key('x'));
  CHECK_EQ(kb.length(), 2u);

  // Unknown ids (shift/mode/delete key values) insert nothing.
  kb.attach(buf, sizeof buf);
  CHECK(!kb.key(QWERTY_KEY_MODE));
}

void testNumberRowLayouts() {
  // numberRow prepends a digit row to every letter layer; symbols ignore it.
  const KeyboardLayout& en = builtinKeyboardLayout(KeyboardLayoutId::QwertyEn, false, false, true);
  CHECK_EQ(en.rowCount, 5);
  CHECK(std::strcmp(en.rows[0].keys[0].output, "1") == 0);
  CHECK(std::strcmp(en.rows[0].keys[0].alt, "!") == 0);
  CHECK(std::strcmp(en.rows[1].keys[0].output, "q") == 0);

  // Shifted English swaps the digit/symbol pairs (symbol primary, digit alt).
  const KeyboardLayout& enShift = builtinKeyboardLayout(KeyboardLayoutId::QwertyEn, true, false, true);
  CHECK_EQ(enShift.rowCount, 5);
  CHECK(std::strcmp(enShift.rows[0].keys[0].output, "!") == 0);
  CHECK(std::strcmp(enShift.rows[0].keys[0].alt, "1") == 0);
  CHECK(std::strcmp(enShift.rows[1].keys[0].output, "Q") == 0);

  // Localized letter layers gain the same digit row.
  const KeyboardLayout& fr = builtinKeyboardLayout(KeyboardLayoutId::AzertyFr, false, false, true);
  CHECK_EQ(fr.rowCount, 5);
  CHECK(std::strcmp(fr.rows[1].keys[0].output, "a") == 0);

  // Symbols pages already carry digits: numberRow is a no-op there.
  CHECK_EQ(builtinKeyboardLayout(KeyboardLayoutId::QwertyEn, false, true, true).rowCount, 4);

  // Alt lookup: digit ids resolve to their long-press symbol; letters flip
  // case (see testKeyboardAltCaseFlip); non-normal keys return nullptr.
  CHECK(std::strcmp(keyboardAltOutputFor(en, '1'), "!") == 0);
  CHECK(std::strcmp(keyboardAltOutputFor(en, 'q'), "Q") == 0);
  CHECK(keyboardAltOutputFor(en, QWERTY_KEY_BACKSPACE) == nullptr);
}

void testKeyboardEntryLongPressAlt() {
  char buf[8] = "";
  KeyboardEntry kb;
  kb.numberRow = true;
  kb.attach(buf, sizeof buf);

  CHECK(kb.key('1'));
  CHECK(kb.key('1', /*longPress=*/true));  // alt output
  CHECK(std::strcmp(buf, "1!") == 0);

  // Letters without an explicit alt flip case on long-press.
  CHECK(kb.key('q', /*longPress=*/true));
  CHECK(std::strcmp(buf, "1!Q") == 0);
}

void testKeyboardAltCaseFlip() {
  const KeyboardLayout& en = builtinKeyboardLayout(KeyboardLayoutId::QwertyEn);
  CHECK(std::strcmp(keyboardAltOutputFor(en, 'q'), "Q") == 0);
  const KeyboardLayout& enShift = builtinKeyboardLayout(KeyboardLayoutId::QwertyEn, true);
  CHECK(std::strcmp(keyboardAltOutputFor(enShift, 'Q'), "q") == 0);
  // Non-letters without an explicit alt still have none; special keys never do.
  const KeyboardLayout& sym = builtinKeyboardLayout(KeyboardLayoutId::QwertyEn, false, true);
  CHECK(keyboardAltOutputFor(sym, '/') == nullptr);
  CHECK(keyboardAltOutputFor(en, QWERTY_KEY_BACKSPACE) == nullptr);
}

void testTouchHoldRouter() {
  InteractionBuffer<8> interactions;
  const auto rebuild = [&] {
    interactions.clear();
    interactions.addInteraction(Interaction{Rect{10, 10, 40, 40}, 1, 'q',
                                            static_cast<uint16_t>(InputTouch | InputLongPress), StateNormal, 0});
    interactions.addInteraction(Interaction{Rect{60, 10, 40, 40}, 1, QWERTY_KEY_BACKSPACE,
                                            static_cast<uint16_t>(InputTouch | InputLongPress), StateNormal, 0});
  };
  rebuild();

  TouchHoldRouter router;

  // Quick tap: no hold event while down, tap release dispatches once.
  auto r = router.update(interactions, true, 20, 20, false, 0, 0, true, 1000);
  CHECK(!r.event);
  CHECK(r.activeChanged);
  r = router.update(interactions, false, 0, 0, true, 20, 20, false, 1100);
  CHECK_EQ(r.event.value, 'q');
  CHECK(!r.event.longPress);

  // Hold past the threshold: long-press fires exactly once at threshold and
  // the timer must NOT re-arm on later frames (the repeat bug), and the real
  // release is swallowed.
  r = router.update(interactions, true, 20, 20, false, 0, 0, true, 2000);
  CHECK(!r.event);
  r = router.update(interactions, true, 20, 20, false, 0, 0, true, 2360);
  CHECK_EQ(r.event.value, 'q');
  CHECK(r.event.longPress);
  r = router.update(interactions, true, 20, 20, false, 0, 0, true, 2800);
  CHECK(!r.event);
  r = router.update(interactions, true, 20, 20, false, 0, 0, true, 3300);
  CHECK(!r.event);
  r = router.update(interactions, false, 0, 0, true, 20, 20, false, 3400);
  CHECK(!r.event);  // swallowed

  // Delete key uses the longer threshold.
  r = router.update(interactions, true, 70, 20, false, 0, 0, true, 4000);
  r = router.update(interactions, true, 70, 20, false, 0, 0, true, 4500);
  CHECK(!r.event);  // 500ms < 900ms override
  r = router.update(interactions, true, 70, 20, false, 0, 0, true, 4950);
  CHECK_EQ(r.event.value, QWERTY_KEY_BACKSPACE);
  CHECK(r.event.longPress);
  router.reset();

  // Sliding onto another key restarts the hold timer.
  r = router.update(interactions, true, 20, 20, false, 0, 0, true, 6000);
  r = router.update(interactions, true, 70, 20, false, 0, 0, true, 6300);
  CHECK(r.activeChanged);
  r = router.update(interactions, true, 70, 20, false, 0, 0, true, 6500);
  CHECK(!r.event);  // only 200ms on the new key
  router.reset();

  // Contact drifting into a swipe clears the active highlight.
  r = router.update(interactions, true, 20, 20, false, 0, 0, true, 7000);
  CHECK(interactions.activeIndex() >= 0);
  r = router.update(interactions, false, 0, 0, false, 0, 0, false, 7100);
  CHECK(r.activeChanged);
  CHECK(interactions.activeIndex() < 0);

  // Release drifting off the pressed key (within tap slop) still dispatches
  // the key the press landed on — finger occlusion drops releases low.
  r = router.update(interactions, true, 20, 20, false, 0, 0, true, 8000);
  r = router.update(interactions, false, 0, 0, true, 20, 55, false, 8100);  // below the key
  CHECK_EQ(r.event.value, 'q');
  CHECK(!r.event.longPress);
}

void testKeyboardBottomHitOverflow() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;

  const auto hitBottomFor = [&](const int16_t overflow, const int16_t value) {
    InteractionBuffer<48> interactions;
    Frame<48> frame(draw, device, input, interactions);
    KeyboardProps props;
    props.layout = &builtinKeyboardLayout(KeyboardLayoutId::QwertyEn);
    props.keyAction = 77;
    props.bottomHitOverflow = overflow;
    keyboard(frame, Rect{0, 0, 480, 200}, props);
    for (size_t i = 0; i < interactions.count(); ++i) {
      const Interaction& it = interactions.data()[i];
      if (it.value == value) return it.rect.bottom();
    }
    return static_cast<int16_t>(-1);
  };

  // The overflow extends only the last row's hit band.
  CHECK_EQ(hitBottomFor(20, QWERTY_KEY_ENTER), hitBottomFor(0, QWERTY_KEY_ENTER) + 20);
  CHECK_EQ(hitBottomFor(20, 'q'), hitBottomFor(0, 'q'));
}

void testHeaderLeadingButton() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice();
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, input, interactions);

  HeaderProps props;
  props.title = "Settings";
  props.centered = true;
  props.borderEdges = EdgeBottom;
  props.leadingIcon = lucideDeleteIcon16();  // any bitmap works as the icon
  props.leadingAction = 500;
  header(frame, Rect{0, 0, 240, 44}, props);

  // The leading button registers its action and draws the icon.
  CHECK_EQ(interactions.count(), 1u);
  CHECK_EQ(interactions.data()[0].action, 500);
  CHECK(interactions.data()[0].rect.width >= 36);
  CHECK(draw.countKind(FakeDrawTarget::Op::Bitmap) >= 1u);

  // Without a leading action the header registers nothing.
  InteractionBuffer<8> plain;
  Frame<8> plainFrame(draw, device, input, plain);
  HeaderProps bare;
  bare.title = "Settings";
  header(plainFrame, Rect{0, 0, 240, 44}, bare);
  CHECK_EQ(plain.count(), 0u);
}

void testScreenKeyboardUsesResponsiveHeight() {
  FakeDrawTarget draw;
  DeviceContext device{800, 480};
  InputSnapshot input;
  InteractionBuffer<40> interactions;
  Frame<40> frame(draw, device, input, interactions);
  ThemeTokens theme;
  Screen<40> screen(frame, theme);

  QwertyKeyboardProps keyboard;
  keyboard.keyAction = 400;
  keyboard.shiftAction = 401;
  keyboard.modeAction = 402;
  keyboard.deleteAction = 403;
  keyboard.okAction = 404;
  screen.qwertyKeyboard(keyboard, 0, LayoutAnchor::Bottom);

  CHECK_EQ(interactions.count(), 31u);
  CHECK(interactions.data()[0].rect.y >= 275);
  CHECK(interactions.data()[0].rect.y < 320);
  CHECK(interactions.data()[30].rect.bottom() <= device.height);
}

void testEReaderChromeMenusAndPanels() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice(300, 400);
  InputSnapshot input;
  InteractionBuffer<24> interactions;
  Frame<24> frame(draw, device, input, interactions);

  const TapZone zones[3] = {
      {Rect{0, 0, 100, 400}, 400, -1, InputTouch, StateNormal, true},
      {Rect{100, 0, 100, 400}, 401, 0, InputTouch, StateNormal, true},
      {Rect{200, 0, 100, 400}, 402, 1, InputTouch, StateNormal, true},
  };
  TapZonesProps tapProps;
  tapProps.zones = zones;
  tapProps.count = 3;
  tapProps.swipeLeft = 403;
  tapProps.swipeRight = 404;
  tapZones(frame, Rect{0, 0, 300, 400}, tapProps);

  ReaderChromeProps chrome;
  chrome.top.title = "Chapter";
  chrome.top.text.maxLines = 1;
  chrome.bottom.trailing = "42%";
  chrome.bottom.text.maxLines = 1;
  readerChrome(frame, Rect{0, 0, 300, 400}, chrome);

  const DialogOption options[2] = {{"Open", 405, 0, StateNormal, true}, {"Delete", 406, 0, StateNormal, true}};
  ContextMenuProps menu;
  menu.title = "Book";
  menu.options = options;
  menu.optionCount = 2;
  contextMenu(frame, Rect{40, 80, 220, 140}, menu);

  ToastProps toastProps;
  toastProps.message = "Saved";
  toast(frame, Rect{0, 0, 300, 400}, toastProps);

  MessagePanelProps panel;
  panel.title = "No books";
  panel.message = "Add files to the SD card.";
  panel.actionLabel = "Retry";
  panel.action = 407;
  messagePanel(frame, Rect{30, 120, 240, 160}, panel);

  CHECK_EQ(interactions.count(), 8u);
  CHECK_EQ(interactions.data()[0].action, 400);
  CHECK_EQ(interactions.data()[3].action, 403);
  CHECK_EQ(interactions.data()[4].action, 404);
  CHECK_EQ(interactions.data()[7].action, 407);
  CHECK(draw.countKind(FakeDrawTarget::Op::Fill) > 0);
  CHECK(draw.countKind(FakeDrawTarget::Op::Text) >= 6);
}

void testEReaderBookSurfaces() {
  FakeDrawTarget draw;
  DeviceContext device = makeDevice(320, 240);
  InputSnapshot input;
  InteractionBuffer<24> interactions;
  Frame<24> frame(draw, device, input, interactions);

  BookCardProps card;
  card.title = "A Long Book Title";
  card.author = "Author";
  card.meta = "42% read";
  card.action = 500;
  card.value = 9;
  card.titleText.maxLines = 2;
  card.authorText.maxLines = 1;
  card.metaText.maxLines = 1;
  card.progress = 42;
  bookCard(frame, Rect{0, 0, 320, 96}, card);

  const CoverGridItem items[4] = {
      {"One", {}, {}, StateNormal, 1, true},
      {"Two", {}, {}, StateNormal, 2, true},
      {"Three", {}, {}, StateNormal, 3, true},
      {"Four", {}, {}, StateNormal, 4, true},
  };
  CoverGridProps grid;
  grid.items = items;
  grid.count = 4;
  grid.action = 501;
  grid.columns = 2;
  grid.rowHeight = 120;
  grid.coverSize = Size{48, 72};
  grid.labelHeight = 18;
  coverGrid(frame, Rect{0, 104, 220, 132}, grid);

  CHECK_EQ(interactions.count(), 3u);  // one card + two visible grid cells
  CHECK_EQ(interactions.data()[0].action, 500);
  CHECK_EQ(interactions.data()[1].action, 501);
  CHECK_EQ(interactions.data()[2].value, 2);
  bool sawLargeCover = false;
  bool sawCoverAlignedProgress = false;
  for (size_t i = 0; i < draw.opCount; ++i) {
    const auto& op = draw.ops[i];
    if (op.kind != FakeDrawTarget::Op::Fill) continue;
    if (op.rect.x == 8 && op.rect.y == 6 && op.rect.width == 62 && op.rect.height == 84) {
      sawLargeCover = true;
    }
    if (op.rect.x == 84 && op.rect.y == 86 && op.rect.height == 4) {
      sawCoverAlignedProgress = true;
    }
  }
  CHECK(sawLargeCover);
  CHECK(sawCoverAlignedProgress);
  CHECK(draw.countKind(FakeDrawTarget::Op::Text) >= 5);
  CHECK(draw.countKind(FakeDrawTarget::Op::Fill) >= 5);
}

static constexpr ActionId ActionOpen = 101;
static constexpr ActionId ActionBack = 102;

struct AppTestState {
  ListItem items[2] = {{"First", nullptr, nullptr, {}, {}, StateNormal, 7, true, false},
                       {"Second", nullptr, nullptr, {}, {}, StateNormal, 8, true, false}};
  int handled = 0;
  ActionEvent last{};
};

void appTestScreen(Screen<8>& screen, void* user) {
  AppTestState* state = static_cast<AppTestState*>(user);
  screen.header("Home");
  const FooterAction actions[2] = {{"Open", ActionOpen, 1}, {"Back", ActionBack, 2}};
  screen.footer(actions, 2);
  screen.list(state->items, 2, 0, ActionOpen);
}

void appTestHandler(const ActionEvent& event, void* user) {
  AppTestState* state = static_cast<AppTestState*>(user);
  ++state->handled;
  state->last = event;
}

void testHeaderBorderEdges() {
  FakeDrawTarget draw;
  DeviceContext device{200, 120};
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, input, interactions);
  ThemeTokens theme;
  theme.headerHeight = 20;
  Screen<8> screen(frame, theme);

  // The themed header supplies a 1px divider when the theme's popup style has
  // no border of its own, so default headers match the documented divider.
  screen.header("Top");
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Line), 1u);
  CHECK_EQ(draw.countKind(FakeDrawTarget::Op::Stroke), 0u);

  FakeDrawTarget boxedDraw;
  InteractionBuffer<8> boxedInteractions;
  Frame<8> boxedFrame(boxedDraw, device, input, boxedInteractions);
  Screen<8> boxedScreen(boxedFrame, theme);
  HeaderProps headerProps;
  headerProps.title = "Top";
  headerProps.styles = defaultPopupStyles();
  headerProps.styles.normal.border = Paint::solid(Color::Black);
  headerProps.styles.normal.borderWidth = 1;
  headerProps.borderEdges = EdgesAll;
  boxedScreen.header(headerProps);
  CHECK_EQ(boxedDraw.countKind(FakeDrawTarget::Op::Stroke), 1u);
}

void testPopupAutoSizeAndAlignment() {
  FakeDrawTarget draw;
  DeviceContext device{240, 320};
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, input, interactions);
  ThemeTokens theme;
  Screen<8> screen(frame, theme);

  PopupProps popupProps;
  popupProps.message = "Saved";
  popupProps.maxWidth = 180;
  popupProps.text.align = TextAlign::Center;
  screen.popup(popupProps);

  CHECK(draw.opCount >= 2);
  CHECK(draw.ops[0].kind == FakeDrawTarget::Op::Fill);
  CHECK(draw.ops[0].rect.width < 180);
  CHECK(draw.ops[0].rect.x > 0);
  CHECK(draw.ops[0].rect.right() < device.width);

  bool sawCenteredText = false;
  for (size_t i = 0; i < draw.opCount; ++i) {
    if (draw.ops[i].kind == FakeDrawTarget::Op::Text && draw.ops[i].rect.x > draw.ops[0].rect.x &&
        draw.ops[i].rect.right() < draw.ops[0].rect.right()) {
      sawCenteredText = true;
    }
  }
  CHECK(sawCenteredText);
}

void testScreenAnchoredLayout() {
  FakeDrawTarget draw;
  DeviceContext device{200, 120};
  InputSnapshot input;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, input, interactions);
  ThemeTokens theme;
  theme.headerHeight = 20;
  theme.rowHeight = 30;
  theme.footerHeight = 20;
  Screen<8> screen(frame, theme);

  screen.header("Top");
  screen.button("Bottom", ActionBack, 0, StateNormal, LayoutAnchor::Bottom);
  SettingRowProps row;
  row.label = "Middle";
  screen.settingRow(row);

  CHECK_EQ(screen.body().y, 54);
  CHECK_EQ(screen.body().height, 32);

  bool sawBottomButton = false;
  for (size_t i = 0; i < draw.opCount; ++i) {
    if (draw.ops[i].kind == FakeDrawTarget::Op::Fill && draw.ops[i].rect.y == 90 &&
        draw.ops[i].rect.height == 30) {
      sawBottomButton = true;
    }
  }
  CHECK(sawBottomButton);

  FakeDrawTarget footerDraw;
  InteractionBuffer<8> footerInteractions;
  Frame<8> footerFrame(footerDraw, device, input, footerInteractions);
  Screen<8> footerScreen(footerFrame, theme);
  const FooterAction actions[2] = {{"Open", ActionOpen, 1}, {"Back", ActionBack, 2}};
  footerScreen.footer(actions, 2);
  bool sawInsetFooterStart = false;
  bool sawInsetFooterEnd = false;
  for (size_t i = 0; i < footerDraw.opCount; ++i) {
    if (footerDraw.ops[i].kind == FakeDrawTarget::Op::Fill && footerDraw.ops[i].rect.x == 8) {
      sawInsetFooterStart = true;
    }
    if (footerDraw.ops[i].kind == FakeDrawTarget::Op::Fill && footerDraw.ops[i].rect.right() == 192) {
      sawInsetFooterEnd = true;
    }
  }
  CHECK(sawInsetFooterStart);
  CHECK(sawInsetFooterEnd);
  CHECK_EQ(footerDraw.countKind(FakeDrawTarget::Op::Line), 0u);
  CHECK_EQ(footerDraw.countKind(FakeDrawTarget::Op::Stroke), 0u);

  FakeDrawTarget boxedFooterDraw;
  InteractionBuffer<8> boxedFooterInteractions;
  Frame<8> boxedFooterFrame(boxedFooterDraw, device, input, boxedFooterInteractions);
  Screen<8> boxedFooterScreen(boxedFooterFrame, theme);
  FooterProps footerProps;
  footerProps.actions = actions;
  footerProps.count = 2;
  footerProps.buttonBorderEdges = EdgesAll;
  boxedFooterScreen.footer(footerProps);
  CHECK_EQ(boxedFooterDraw.countKind(FakeDrawTarget::Op::Stroke), 2u);
}

void testFreeInkAppDispatchesScreenActions() {
  FakeDrawTarget draw;
  DeviceContext device{200, 120};
  AppTestState state;
  FreeInkApp<8, 4> app(draw, device);
  app.setScreen(appTestScreen, &state);
  app.on(ActionBack, appTestHandler, &state);

  CHECK(app.invalidated());
  CHECK(app.refreshHint() == RefreshHint::Full);
  ActionEvent event = app.render();
  CHECK(!event);
  CHECK(app.lastRenderRefreshHint() == RefreshHint::Full);
  CHECK(!app.invalidated());
  CHECK(app.refreshHint() == RefreshHint::None);
  CHECK(draw.opCount > 0);

  InputSnapshot input;
  input.touchReleased = true;
  input.touchX = 150;
  input.touchY = 100;
  event = app.render(input);
  CHECK(event);
  CHECK_EQ(event.action, ActionBack);
  CHECK_EQ(event.value, 2);
  CHECK_EQ(state.handled, 1);
  CHECK_EQ(state.last.action, ActionBack);
  CHECK(app.invalidated());
  CHECK(app.refreshHint() == RefreshHint::Fast);
}

void noopHandler(const ActionEvent&, void*) {}

void testFreeInkAppHandlerOverflowFlag() {
  FakeDrawTarget draw;
  DeviceContext device{100, 100};
  FreeInkApp<4, 1> app(draw, device);
  app.on(1, noopHandler);
  app.on(2, noopHandler);
  CHECK(app.handlerOverflowed());
}

// Editor canvas: wrapping, caret-line tracking, scroll helpers, and the render
// window. FakeDrawTarget is monospace (charWidth 6, lineH 12), so widths are
// exactly 6*strlen — easy to reason about.
void testTextArea() {
  FakeDrawTarget draw;
  const TextStyle style{};

  // Empty buffer is a single line with the caret on it.
  TextAreaMetrics m = textAreaMeasure(draw, 100, "", style, 0);
  CHECK_EQ(m.lineCount, 1u);
  CHECK_EQ(m.caretLine, 0u);

  // Hard newlines split paragraphs; the caret tracks its byte offset.
  const char* doc = "a\nb\nc";  // a=0 \n=1 b=2 \n=3 c=4
  CHECK_EQ(textAreaMeasure(draw, 100, doc, style, 0).lineCount, 3u);
  CHECK_EQ(textAreaMeasure(draw, 100, doc, style, 2).caretLine, 1u);
  CHECK_EQ(textAreaMeasure(draw, 100, doc, style, 5).caretLine, 2u);  // end of buffer

  // A trailing newline yields a final empty line for the caret.
  m = textAreaMeasure(draw, 100, "a\n", style, 2);
  CHECK_EQ(m.lineCount, 2u);
  CHECK_EQ(m.caretLine, 1u);

  // Word wrap at the rect width: 60px / 6px = 10 chars, so it breaks after the
  // second space (offset 10) and "cccc" flows to line 1.
  const char* wrap = "aaaa bbbb cccc";
  CHECK_EQ(textAreaMeasure(draw, 60, wrap, style, 0).lineCount, 2u);
  CHECK_EQ(textAreaMeasure(draw, 60, wrap, style, 12).caretLine, 1u);

  // Scroll helpers mirror the list helpers.
  CHECK_EQ(textAreaVisibleLines(Rect{0, 0, 100, 120}, 12), 10);
  CHECK_EQ(textAreaTopLineFor(12, 0, 5, 20), 8u);
  CHECK_EQ(textAreaTopLineFor(2, 8, 5, 20), 2u);
  CHECK_EQ(textAreaTopLineFor(3, 0, 5, 4), 0u);

  // Render only draws the visible window of lines.
  DeviceContext device = makeDevice();
  InputSnapshot in;
  InteractionBuffer<8> interactions;
  Frame<8> frame(draw, device, in, interactions);
  TextAreaProps props;
  props.text = "line0\nline1\nline2\nline3";
  props.cursor = 8;     // inside line1
  props.topLine = 1;    // scrolled so line1 is at the top
  props.style = style;
  draw.opCount = 0;
  textArea(frame, Rect{0, 0, 120, 24}, props);  // 2 lines tall at lineH 12
  int textOps = 0;
  bool sawCaret = false;
  for (size_t i = 0; i < draw.opCount; ++i) {
    if (draw.ops[i].kind == FakeDrawTarget::Op::Text) ++textOps;
    if (draw.ops[i].kind == FakeDrawTarget::Op::Fill && draw.ops[i].rect.width == 2) sawCaret = true;
  }
  CHECK_EQ(textOps, 2);  // lines 1 and 2 only
  CHECK(sawCaret);       // caret on the now-visible line 1
}

}  // namespace

int main() {
  testRect();
  testDisplayTarget();
  testDisplayTargetAlphaFont();
  testStackFillsExactly();
  testStackFlexRemainderWithTrailingFixed();
  testStackGaps();
  testEnsureMinTouchRect();
  testTouchRouting();
  testDisabledSkipsTouch();
  testLongPressRouting();
  testFocusNavigationWrapsAndSkips();
  testConfirmIgnoresStaleFocus();
  testConfirmRespectsInputMask();
  testEdgeButtonsAndSwipes();
  testListHelpers();
  testListVirtualization();
  testListClampsBadTopIndex();
  testButtonRegistersExpandedHit();
  testProgressBarClamps();
  testBatteryIndicator();
  testMetricCard();
  testOptionDialog();
  testCrossInkKeyboardComposition();
  testCrossInkStatusBarAndXtcOverlay();
  testCrossInkReaderMenuList();
  testCrossInkReadingStatsSurfaces();
  testInteractionOverflowFlag();
  testRoundedRaffSurfaces();
  testThemePrimitiveParity();
  testRotationAndBitmapSampling();
  testListSectionHeaders();
  testCrossInkSleepScreenComposition();
  testCoverCarousel();
  testLayoutTextWrapping();
  testTouchToLogical();
  testMeasureWrappedText();
  testButtonHitPadding();
  testInvertedDrawTarget();
  testStyleSetUnset();
  testDefaultStylesAreBorderless();
  testEReaderSettingsComponents();
  testLvglParityControls();
  testQwertyKeyboardComponent();
  testLocalizedKeyboardLayout();
  testSymbolKeyboardPages();
  testKeyboardEntry();
  testNumberRowLayouts();
  testKeyboardEntryLongPressAlt();
  testKeyboardAltCaseFlip();
  testTouchHoldRouter();
  testKeyboardBottomHitOverflow();
  testHeaderLeadingButton();
  testScreenKeyboardUsesResponsiveHeight();
  testEReaderChromeMenusAndPanels();
  testEReaderBookSurfaces();
  testHeaderBorderEdges();
  testPopupAutoSizeAndAlignment();
  testScreenAnchoredLayout();
  testFreeInkAppDispatchesScreenActions();
  testFreeInkAppHandlerOverflowFlag();
  testTextArea();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
