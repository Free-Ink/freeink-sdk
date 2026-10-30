#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <Wire.h>
#include <driver/gpio.h>

#include <cassert>
#include <cstdio>

#define private public
#include <InputManager.h>
#undef private
#include "../../src/gsl/EegoA4GslFirmware.h"

int main() {
  InputManager input;
  assert(input.gslUploadFirmware());
  assert(Wire.writes.size() == 417);
  size_t entryIndex = 0;
  for (const auto& tx : Wire.writes) {
    assert(tx.size() >= 2 && tx.size() <= 65);
    assert(entryIndex < freeink::EEGO_A4_GSL_FIRMWARE_LEN);
    if (tx[0] == 0xF0) {
      const auto& entry = freeink::EEGO_A4_GSL_FIRMWARE[entryIndex++];
      assert(tx.size() == 2 && entry.reg == 0xF0 && tx[1] == entry.value);
      continue;
    }
    assert((tx.size() - 1) % 4 == 0);
    for (size_t offset = 1; offset < tx.size(); offset += 4) {
      assert(entryIndex < freeink::EEGO_A4_GSL_FIRMWARE_LEN);
      const auto& entry = freeink::EEGO_A4_GSL_FIRMWARE[entryIndex++];
      const uint32_t value = uint32_t(tx[offset]) | (uint32_t(tx[offset + 1]) << 8) | (uint32_t(tx[offset + 2]) << 16) |
                             (uint32_t(tx[offset + 3]) << 24);
      assert(entry.reg != 0xF0 && entry.reg == tx[0] + offset - 1 && value == entry.value);
    }
  }
  assert(entryIndex == freeink::EEGO_A4_GSL_FIRMWARE_LEN);

  for (const int failAt : {0, 1, 16, 416}) {
    Wire = {};
    Wire.failAt = failAt;
    assert(!input.gslUploadFirmware());
    assert(Wire.writes.size() == static_cast<size_t>(failAt + 1));
  }

  Wire = {};
  input.beginGslx680();
  assert(input.hasTouch());
  // A successful ACK cannot hide a later failed upload or invalid firmware magic.
  Wire = {};
  Wire.failAt = 25;
  loggedErrors = 0;
  input.beginGslx680();
  assert(!input.hasTouch() && loggedErrors == 1 && Wire.writes.size() == 26);
  Wire = {};
  Wire.magicValid = false;
  loggedErrors = 0;
  input.beginGslx680();
  assert(!input.hasTouch() && loggedErrors == 1);

  for (const bool nack : {false, true}) {
    Wire = {};
    Wire.failAt = nack ? 0 : -1;
    input.touchDataEnabled = true;
    gpioLevels[3] = HIGH;
    gpioHeld[3] = true;
    assert(input.prepareForDeepSleep() == !nack);
    assert(Wire.writes.size() == 1 && Wire.writes[0] == std::vector<uint8_t>({0xE0, 0x88}));
    assert(Wire.ended && gpioLevels[3] == LOW && gpioHeld[3] && !input.hasTouch());
    Wire = {};
    input.beginGslx680();
    assert(!gpioHeld[3] && gpioLevels[3] == HIGH && input.hasTouch());
  }
  // Home key / touch classification mirrors the stock firmware.
  struct Frame {
    uint8_t count;
    uint16_t xWord, yWord;
  };
  const auto feed = [&](const Frame& f, unsigned long now) {
    Wire = {};
    input.touchReadAt = 0;
    Wire.touchFrame = {f.count, 0, 0, 0, static_cast<uint8_t>(f.yWord), static_cast<uint8_t>(f.yWord >> 8),
                       static_cast<uint8_t>(f.xWord), static_cast<uint8_t>(f.xWord >> 8)};
    input.pollGslx680(now);
  };
  const auto reset = [&]() {
    input.touchPressed = false;
    input.touchHomeKeyDown = false;
    input.touchHomeKeyEvent = false;
    input.touchHomeKeyTapEvent = false;
  };
  // The exact key sample is the key, whatever its track id or contact count.
  for (const auto& f : {Frame{1, 0x03a0, 0x1020}, Frame{1, 0x13a0, 0x2020}, Frame{1, 0x03a0, 0xf020},
                        Frame{2, 0x03a0, 0x1020}}) {
    reset();
    feed(f, 1000);
    assert(input.touchHomeKeyEvent && !input.touchPressed && "key sample not read as the home key");
  }
  // Other samples past the glass (X >= 920) are dropped: neither key nor touch.
  for (const auto& f : {Frame{1, 0x03a7, 0x1020}, Frame{1, 0x03a0, 0x1031}, Frame{1, 0x0398, 0x1020}}) {
    reset();
    feed(f, 1000);
    assert(!input.touchHomeKeyEvent && !input.touchPressed && "off-glass sample was not dropped");
  }
  // X 919 is on the glass.
  reset();
  feed(Frame{1, 0x0397, 0x1020}, 1000);
  assert(!input.touchHomeKeyEvent && input.touchPressed);
  // A key sample during a screen touch is ignored.
  reset();
  feed(Frame{1, 0x0200, 0x1100}, 1000);
  feed(Frame{1, 0x03a0, 0x1020}, 1020);
  assert(input.touchPressed && !input.touchHomeKeyDown && "key fired during a screen touch");
  // A touch that follows a key press cancels it: no tap on release.
  reset();
  feed(Frame{1, 0x03a0, 0x1020}, 1000);
  assert(input.touchHomeKeyDown);
  feed(Frame{1, 0x0200, 0x1100}, 1020);
  feed(Frame{0, 0, 0}, 1040);
  assert(!input.touchHomeKeyTapEvent && "key press followed by a touch still tapped");
  // A clean press and release is a tap.
  reset();
  feed(Frame{1, 0x03a0, 0x1020}, 1000);
  feed(Frame{0, 0, 0}, 1060);
  assert(input.touchHomeKeyTapEvent);
  // Stock factory calibration: every table point maps to its own screen pixel,
  // and samples between them interpolate inside their triangle.
  struct Cal {
    uint16_t rx, ry;
    int16_t sx, sy;
  };
  for (const auto& c : {Cal{549, 337, 276, 306}, Cal{720, 94, 80, 168}, Cal{694, 560, 472, 168},
                        Cal{392, 558, 472, 444}, Cal{374, 97, 80, 444}, Cal{455, 40, 34, 379}}) {
    int16_t x = -1, y = -1;
    InputManager::gslCalibrate(c.rx, c.ry, x, y);
    assert(x == c.sx && y == c.sy && "stock calibration point mismatch");
  }
  int16_t edgeX = -1, edgeY = -1;
  InputManager::gslCalibrate(0, 680, edgeX, edgeY);  // extrapolates to (595, 775)
  assert(edgeX == 551 && edgeY == 767 && "calibration must clamp to the screen");
  // A frame lands in the panel-native frame: x = portrait y, y = 551 - portrait x.
  reset();
  feed(Frame{1, 549, 0x1000 | 337}, 1000);
  assert(input.touchPressed && input.touchPoint.x == 306 && input.touchPoint.y == 551 - 276);

  // --- Multi-touch snapshot: the GSLX680 backend mirrors the GT911 contract ---
  assert(input.supportsMultiTouch() && "GSLX680 must report multi-touch support");

  struct Contact {
    uint16_t xWord, yWord;
  };
  const auto feedMulti = [&](std::initializer_list<Contact> contacts, unsigned long now) {
    Wire = {};
    input.touchReadAt = 0;
    Wire.touchFrame = {static_cast<uint8_t>(contacts.size()), 0, 0, 0};
    for (const auto& c : contacts) {
      Wire.touchFrame.push_back(static_cast<uint8_t>(c.yWord));
      Wire.touchFrame.push_back(static_cast<uint8_t>(c.yWord >> 8));
      Wire.touchFrame.push_back(static_cast<uint8_t>(c.xWord));
      Wire.touchFrame.push_back(static_cast<uint8_t>(c.xWord >> 8));
    }
    input.pollGslx680(now);
  };
  const auto clearTouchState = [&]() {
    reset();
    input.touchSnapshot = {};
    // Independent cases must not inherit gesture or event state: a stale
    // multi-contact sequence or release flag made later assertions vacuous.
    input.resetMultiTouchGesture();
    input.touchPressedEvent = false;
    input.touchReleasedEvent = false;
    input.touchMovedBeyondTapSlop = false;
    input.touchMovedBeyondTapReleaseSlop = false;
    input.touchLongPressEvent = false;
    input.touchLongPressFired = false;
    input.touchSuppressed = false;
  };

  // Two contacts: reportedCount for truncation detection, both stored with
  // stock-calibrated panel-native coordinates; the single-touch API keeps the
  // first contact and stops being a tap candidate. Hardware-validated on the
  // A4 LITE (2026-10-29): the real glass reports record top nibbles of 0 —
  // the A4's records are coordinate-only (no track ids), so ids are
  // frame-local record slots and idsStable is false, mirroring GT911's
  // coordinate-only contract. Callers must not retain ids across frames.
  clearTouchState();
  feedMulti({{549, 337}, {720, 94}}, 2000);
  assert(input.touchPressed);
  assert(input.touchSnapshot.reportedCount == 2 && input.touchSnapshot.count == 2);
  assert(!input.touchSnapshot.idsStable && "A4 records are coordinate-only (no track ids)");
  assert(input.touchSnapshot.points[0].id == 0 && input.touchSnapshot.points[1].id == 1 &&
         "coordinate-only ids are frame-local record slots");
  assert(input.touchSnapshot.points[0].point.x == 306 && input.touchSnapshot.points[0].point.y == 275);
  assert(input.touchSnapshot.points[1].point.x == 168 && input.touchSnapshot.points[1].point.y == 471);
  assert(input.touchSnapshot.points[0].point.timestamp == 2000);
  assert(input.touchPoint.x == 306 && input.touchPoint.y == 275 && "primary contact must stay the first");
  assert(input.touchMovedBeyondTapSlop && "a multi-contact frame must not remain a tap candidate");

  // Records swap slots across frames; slot ids follow records, not fingers.
  feedMulti({{720, 94}, {549, 337}}, 2020);
  assert(input.touchSnapshot.points[0].id == 0 && input.touchSnapshot.points[0].point.x == 168);
  assert(input.touchSnapshot.points[1].id == 1 && input.touchSnapshot.points[1].point.x == 306);

  // Three contacts.
  feedMulti({{549, 337}, {720, 94}, {694, 560}}, 2040);
  assert(input.touchSnapshot.reportedCount == 3 && input.touchSnapshot.count == 3);
  assert(input.touchSnapshot.points[2].id == 2 && input.touchSnapshot.points[2].point.x == 168 &&
         input.touchSnapshot.points[2].point.y == 79);

  // Five contacts: stored count caps at MAX_TOUCH_CONTACTS, reportedCount keeps 5.
  clearTouchState();
  feedMulti({{549, 337}, {720, 94}, {694, 560}, {374, 97}, {455, 40}}, 2060);
  assert(input.touchSnapshot.reportedCount == 5);
  assert(input.touchSnapshot.count == InputManager::MAX_TOUCH_CONTACTS && "stored contacts must cap");
  assert(input.touchSnapshot.points[3].id == 3);

  // A past-glass contact (X >= 920) drops per-contact, not per-frame, and is
  // excluded from reportedCount: the frame is a single-contact touch.
  clearTouchState();
  feedMulti({{549, 337}, {0x2000 | 950, 0x2000 | 306}}, 2080);
  assert(!input.touchHomeKeyEvent && input.touchPressed);
  assert(input.touchSnapshot.reportedCount == 1 && input.touchSnapshot.count == 1 &&
         "past-glass contact must drop alone");

  // A key contact alongside an on-glass contact: the touch wins, no key
  // event, and the key contact is excluded from the snapshot.
  clearTouchState();
  feedMulti({{549, 337}, {0x03a0, 0x1020}}, 2100);
  assert(!input.touchHomeKeyEvent && !input.touchHomeKeyDown && input.touchPressed);
  assert(input.touchSnapshot.count == 1 && input.touchSnapshot.reportedCount == 1 &&
         "key contact must not enter the snapshot");

  // An all-zero record is an absent contact (padded frame), never on-glass and
  // never counted in reportedCount.
  clearTouchState();
  feedMulti({{549, 337}, {0, 0}}, 2120);
  assert(input.touchSnapshot.reportedCount == 1 && input.touchSnapshot.count == 1);

  // Release: the authoritative zero-contact frame clears the snapshot.
  feedMulti({}, 2140);
  assert(input.touchSnapshot.count == 0 && input.touchSnapshot.reportedCount == 0);
  assert(!input.touchPressed && input.touchReleasedEvent);

  // A multi-contact press/release never becomes a primary tap.
  clearTouchState();
  feedMulti({{549, 337}, {720, 94}}, 2200);
  feedMulti({}, 2260);
  float tapNx = -1, tapNy = -1;
  assert(!input.wasTouchTap(tapNx, tapNy) && "multi-contact release must not tap");

  // A finger sharing its frame with a dropped record (key sentinel, past-glass
  // sample, padded slot) is still a single-contact touch and must still tap:
  // reportedCount counts only the contacts that survive the filters.
  clearTouchState();
  feedMulti({{0x1000 | 549, 0x1000 | 337}, {0x03a0, 0x1020}}, 2300);
  assert(input.touchPressed && !input.touchMovedBeyondTapSlop);
  feedMulti({}, 2320);
  float tapKeyX = -1, tapKeyY = -1;
  assert(input.wasTouchTap(tapKeyX, tapKeyY) && "a finger with a key record must still tap");

  clearTouchState();
  feedMulti({{0x1000 | 549, 0x1000 | 337}, {0x2000 | 950, 0x2000 | 306}}, 2340);
  assert(input.touchPressed && !input.touchMovedBeyondTapSlop);
  feedMulti({}, 2360);
  float tapOffX = -1, tapOffY = -1;
  assert(input.wasTouchTap(tapOffX, tapOffY) && "a finger with a past-glass record must still tap");

  std::puts("A4 firmware bytes, batching, failure handling, touch sleep and wake, home key classification, stock "
            "calibration, multi-touch snapshot passed");
}
