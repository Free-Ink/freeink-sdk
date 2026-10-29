// Host tests for the BLE HID host (src/BleKeyboardHost.cpp) against the fake
// NimBLE stack in this directory. No radio: these check the host's own state
// machine, report decode and teardown order, not on-air behaviour.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <thread>
#include <vector>

#include "BleRemoteProfiles.h"
#include "fake_ble.h"

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(condition)                                               \
  do {                                                                 \
    ++checksRun;                                                       \
    if (!(condition)) {                                                \
      ++checksFailed;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                  \
  } while (0)

using fakeble::host;

constexpr const char* kRemote = "AA:BB:CC:DD:EE:01";
constexpr const char* kSecondRemote = "AA:BB:CC:DD:EE:02";

// HID 1.11 Appendix B.1 boot keyboard: modifiers, reserved byte, six key bytes.
constexpr uint8_t kKeyboardMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,              // Generic Desktop, Keyboard, Collection
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,              //   Keyboard page, modifier usages
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,  //   8 x 1 bit
    0x81, 0x02,                                      //   Input: modifiers
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,              //   Input: reserved byte
    0x19, 0x00, 0x29, 0x65, 0x25, 0x65,              //   key usages 0..0x65
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00,              //   Input: six key bytes
    0xC0,                                            // End Collection
};

// The same keyboard (report 1) plus a Consumer Control report (report 2), as
// many page turners and keyboards with media keys describe themselves.
constexpr uint8_t kKeyboardConsumerMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,  // Keyboard collection, Report ID 1
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,              //   Keyboard page, modifier usages
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,  //   8 x 1 bit
    0x81, 0x02,                                      //   Input: modifiers
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,              //   Input: reserved byte
    0x19, 0x00, 0x29, 0x65, 0x25, 0x65,              //   key usages 0..0x65
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00,              //   Input: six key bytes
    0xC0,                                            // End Collection
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,  // Consumer Control collection, Report ID 2
    0x19, 0x00, 0x2A, 0xFF, 0x03, 0x26, 0xFF, 0x03,  //   usages and logical range 0..0x3FF
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,              //   Input: one 16-bit usage
    0xC0,                                            // End Collection
};

int g_inputReport = -1;

void serveRemote(const uint8_t* map, size_t len) {
  const int mapChar = fakeble::addCharacteristic(0x2A4B, true, false, false);
  fakeble::setValue(mapChar, map, len);
  g_inputReport = fakeble::addInputReport();
}

void sendKeys(uint8_t usage) {
  const uint8_t report[8] = {0, 0, usage, 0, 0, 0, 0, 0};
  fakeble::notify(g_inputReport, report, sizeof report);
}

int drainKeys(uint8_t* lastKeycode = nullptr) {
  int count = 0;
  freeink::KeyEvent ev;
  while (host().popKey(ev)) {
    ++count;
    if (lastKeycode != nullptr) *lastKeycode = ev.keycode;
  }
  return count;
}

bool waitConnected(uint32_t timeoutMs = 2000) {
  for (uint32_t i = 0; i < timeoutMs; ++i) {
    if (host().isConnected()) return fakeble::waitForWorkerIdle();
    vTaskDelay(0);
  }
  return false;
}

void testKeyboardPressIsOneKeyEvent() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));

  sendKeys(0x4B);  // Page Up down
  freeink::KeyEvent ev;
  CHECK(host().popKey(ev));
  CHECK(ev.keycode == 0x4B);
  CHECK(ev.special == freeink::SpecialKey::PageUp);
  sendKeys(0);  // released
  CHECK(!host().popKey(ev));
}

// A peer that returns nothing for a search for the HID service UUID (Flic) still
// connects: listing all its services finds it.
void testHidFoundByListingWhenUuidSearchFails() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  fakeble::failServiceSearchByUuid(true);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));
}

// A peer that is pairing or being discovered sits in a NimBLE wait that only a
// disconnect ends (cancelConnect() covers the GAP connect alone). end() must not
// delete the connection task inside that wait: NimBLE later completes the wait
// on a task handle that no longer exists.
void testEndDuringPairingDoesNotDeleteTaskInsideNimble(fakeble::Stage stage) {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  fakeble::holdAt(stage);
  CHECK(host().connect(kRemote));
  CHECK(fakeble::waitUntilHeld(stage));

  const unsigned long start = fakeble::clockMs();
  host().end();
  const unsigned long elapsed = fakeble::clockMs() - start;

  CHECK(!fakeble::taskDeletedWhileHeld());
  CHECK(elapsed < 2000);  // the 8.5 s connect wait is for a GAP cancel, not for this
  CHECK(!host().isRunning());
}

// Scanning from a settings screen cancels a pending reconnect. A reconnect that
// is already pairing must end too, instead of holding the caller for the whole
// connect wait and leaving the attempt running beside the scan.
void testScanCancelsReconnectThatIsPairing() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  fakeble::holdAt(fakeble::Stage::Security);
  CHECK(host().connect(kRemote));
  CHECK(fakeble::waitUntilHeld(fakeble::Stage::Security));

  const unsigned long start = fakeble::clockMs();
  host().startScan(5000);
  const unsigned long elapsed = fakeble::clockMs() - start;

  CHECK(elapsed < 2000);
  CHECK(host().isScanning());
  CHECK(fakeble::waitForWorkerIdle(200));  // the attempt is over, not still pairing
}

// disconnect() is what a settings "Disconnect" row calls. Auto-reconnect must
// not bring the remote straight back 4 s later; an explicit connect(), a new
// begin(), or a link the peer dropped turn it back on.
void testDisconnectIsNotUndoneByAutoReconnect() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));
  CHECK(host().pairedCount() == 1);

  host().disconnect();
  CHECK(!host().isConnected());
  fakeble::advanceMillis(5000);
  host().poll();
  CHECK(!host().isConnecting());
  CHECK(fakeble::connectCalls() == 1);

  // An explicit connect works and re-arms auto-reconnect.
  CHECK(fakeble::connectTo(kRemote));
  fakeble::peerDisconnect();
  fakeble::advanceMillis(5000);
  host().poll();
  CHECK(host().isConnecting());
  CHECK(waitConnected());

  // Bluetooth off and on again also re-arms it.
  host().disconnect();
  host().end();
  CHECK(fakeble::beginHost());
  fakeble::advanceMillis(5000);
  host().poll();
  CHECK(host().isConnecting());
  CHECK(waitConnected());
}

// Remotes that stream a held key send the same keyboard report again while the
// button is down. On a device whose report map also has a Consumer page, the
// generic fallback read that repeat as a new press: two page turns per press.
void testStreamedHeldKeyIsOnePress() {
  fakeble::resetWorld();
  serveRemote(kKeyboardConsumerMap, sizeof kKeyboardConsumerMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));

  sendKeys(0x51);  // Down arrow pressed
  sendKeys(0x51);  // still held
  sendKeys(0x51);  // still held
  sendKeys(0);     // released
  CHECK(drainKeys() == 1);

  // A keyboard-shaped frame with no key in its key slots still reaches the
  // generic path, as before (here a vendor code in the reserved byte).
  const uint8_t vendor[8] = {0, 0x05, 0, 0, 0, 0, 0, 0};
  const uint8_t idle[8] = {0};
  fakeble::notify(g_inputReport, vendor, sizeof vendor);
  fakeble::notify(g_inputReport, idle, sizeof idle);
  uint8_t code = 0;
  CHECK(drainKeys(&code) == 1);
  CHECK(code == 0x05);
}

// A link that drops while a key is down (remote asleep, out of range) never
// sends that key's release. The next session's first press of the same key
// must still be a press.
void testKeyHeldAcrossLinkDropIsPressedAgain() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));

  sendKeys(0x4E);  // Page Down, link drops before the release frame
  CHECK(drainKeys() == 1);
  fakeble::peerDisconnect();
  for (int i = 0; i < 10; ++i) {
    fakeble::advanceMillis(1000);
    host().poll();
  }
  CHECK(waitConnected());

  sendKeys(0x4E);
  uint8_t code = 0;
  CHECK(drainKeys(&code) == 1);
  CHECK(code == 0x4E);
  sendKeys(0);
}

// The host keeps its own 24-entry scan table. NimBLE's result list is not read
// anywhere, so every advertiser it keeps there is heap the reader cannot use.
void testScanKeepsNoAdvertiserInNimble() {
  fakeble::resetWorld();
  CHECK(fakeble::beginHost());
  host().startScan(5000);
  char addr[18];
  for (int i = 0; i < 10; ++i) {
    std::snprintf(addr, sizeof addr, "AA:BB:CC:00:00:%02X", i);
    fakeble::advertise(addr, "Remote");
  }
  CHECK(host().deviceCount() == 10);
  CHECK(fakeble::retainedScanResults() == 0);
}

void testConnectedAddressFollowsTheLink() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(std::strcmp(host().connectedAddr(), "") == 0);
  CHECK(fakeble::connectTo(kRemote));
  CHECK(std::strcmp(host().connectedAddr(), kRemote) == 0);
  fakeble::peerDisconnect();
  CHECK(std::strcmp(host().connectedAddr(), "") == 0);
}

void testEndCancelsAPairingWaitBeforeDeletingTheTask() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  fakeble::holdAt(fakeble::Stage::Security);
  CHECK(host().connect(kRemote));
  CHECK(fakeble::waitUntilHeld(fakeble::Stage::Security));

  CHECK(host().end(1000));
  CHECK(!fakeble::taskDeletedWhileHeld());
  CHECK(!host().isStopping());
  CHECK(!fakeble::clientExists());
  CHECK(!NimBLEDevice::isInitialized());
}

void testEndLeavesAStuckTaskAloneAndFinishesLater() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  fakeble::holdStubbornlyAt(fakeble::Stage::Connect);
  CHECK(host().connect(kRemote));
  CHECK(fakeble::waitUntilHeld(fakeble::Stage::Connect));

  CHECK(!host().end(0));
  CHECK(!host().isRunning());
  CHECK(host().isStopping());
  CHECK(!host().end(100));
  CHECK(host().isStopping());
  CHECK(fakeble::clientExists());
  CHECK(NimBLEDevice::isInitialized());
  CHECK(!fakeble::taskDeletedWhileHeld());
  CHECK(!fakeble::beginHost());  // nothing may re-initialize under the live task

  fakeble::releaseHold();
  CHECK(host().end(1000));
  CHECK(!host().isStopping());
  CHECK(!fakeble::clientExists());
  CHECK(!NimBLEDevice::isInitialized());
  CHECK(!fakeble::taskDeletedWhileHeld());
  CHECK(fakeble::beginHost());
  CHECK(host().end());
}

void testEndWaitsForTheClientToFinishDisconnecting() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));

  fakeble::lingerOnDisconnect();
  CHECK(!host().end(0));
  CHECK(!host().isConnected());
  CHECK(host().isStopping());
  CHECK(fakeble::clientExists());
  CHECK(NimBLEDevice::isInitialized());
  CHECK(!host().end(50));
  CHECK(fakeble::clientExists());

  fakeble::finishDisconnect();
  CHECK(host().end(0));
  CHECK(!fakeble::clientExists());
  CHECK(!NimBLEDevice::isInitialized());
  CHECK(!host().isStopping());
}

// end() runs on more than one task in an app (main loop and render task). A
// second end() that lands while one is inside the teardown must not tear down
// alongside it, and must not report "stopped" while the stack is still up.
void testSecondEndDuringTeardownReportsStillStopping() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));

  fakeble::holdNextDeinit();
  std::atomic<bool> firstDone{false};
  std::atomic<bool> firstResult{false};
  std::thread first([&] {
    firstResult = host().end(1000);
    firstDone = true;
  });
  CHECK(fakeble::waitUntilDeinitHeld());

  CHECK(!host().end(0));
  CHECK(!host().end(1000));
  CHECK(host().isStopping());
  CHECK(!host().isRunning());
  CHECK(NimBLEDevice::isInitialized());
  CHECK(!firstDone.load());

  fakeble::releaseDeinit();
  first.join();
  CHECK(firstResult.load());
  CHECK(fakeble::maxTeardownOverlap() == 1);
  CHECK(!host().isStopping());
  CHECK(!NimBLEDevice::isInitialized());
  CHECK(host().end(0));
}

// Two tasks retrying end(0) through a link that takes a while to close, as the
// main loop and a render task do around a chapter build with a remote connected.
// Whichever sees true must see the stack fully down; the teardown never overlaps.
void testTwoTasksRetryingEndNeverOverlap() {
  constexpr int kRounds = 25;
  constexpr int kMaxTries = 20000;
  int tornStops = 0;
  int stuck = 0;
  size_t overlap = 0;
  for (int round = 0; round < kRounds; ++round) {
    fakeble::resetWorld();
    serveRemote(kKeyboardMap, sizeof kKeyboardMap);
    CHECK(fakeble::beginHost());
    CHECK(fakeble::connectTo(kRemote));
    fakeble::lingerOnDisconnect();
    fakeble::stallTeardown(2);

    std::atomic<int> torn{0};
    std::atomic<int> gaveUp{0};
    auto retry = [&] {
      for (int tries = 0; tries < kMaxTries; ++tries) {
        if (host().end(0)) {
          if (NimBLEDevice::isInitialized() || fakeble::clientExists() || host().isStopping()) ++torn;
          return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      ++gaveUp;
    };
    std::thread loopTask(retry);
    std::thread renderTask(retry);
    std::this_thread::sleep_for(std::chrono::milliseconds(round % 5));
    fakeble::finishDisconnect();
    loopTask.join();
    renderTask.join();
    tornStops += torn.load();
    stuck += gaveUp.load();
    if (fakeble::maxTeardownOverlap() > overlap) overlap = fakeble::maxTeardownOverlap();
  }
  CHECK(overlap == 1);
  CHECK(tornStops == 0);
  CHECK(stuck == 0);
  fakeble::resetWorld();
}

// Both remotes bonded, nothing connected, every later connect failing, and the
// connect log cleared: the start of a reader visit with the remotes switched off.
void bondTwoRemotesThenSwitchThemOff() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));
  host().disconnect();
  CHECK(fakeble::connectTo(kSecondRemote));
  host().disconnect();
  CHECK(host().pairedCount() == 2);
  CHECK(host().end());
  fakeble::failConnects(true);
  CHECK(fakeble::beginHost());
  const size_t before = fakeble::connectAddresses().size();
  CHECK(before == 2);
}

// One poll, then wait for the connection task to finish what it started.
void pollOnce() {
  host().poll();
  CHECK(fakeble::waitForWorkerIdle());
}

size_t attemptsAt(const char* addr) {
  size_t n = 0;
  const std::vector<std::string> all = fakeble::connectAddresses();
  for (size_t i = 2; i < all.size(); ++i) n += all[i] == addr ? 1 : 0;
  return n;
}

// Pairing from the scan list after the scan ended, while the default turn over the bonds still
// dials a remote that is away: the app's connect ends that attempt and goes ahead.
void testConnectEndsAReconnectInFlight() {
  bondTwoRemotesThenSwitchThemOff();
  fakeble::failConnects(false);
  fakeble::holdAt(fakeble::Stage::Connect);
  for (int i = 0; i < 3 && !host().isConnecting(); ++i) {  // the default turn waits out its backoff
    fakeble::advanceMillis(4001);
    host().poll();
  }
  CHECK(fakeble::waitUntilHeld(fakeble::Stage::Connect));
  CHECK(host().connect(kSecondRemote));
  CHECK(waitConnected());
  CHECK(std::strcmp(host().connectedAddr(), kSecondRemote) == 0);
}

// The attempt in flight already has its link and is pairing. Ending it leaves the client
// disconnecting for a while, and NimBLE refuses to connect until that is over.
void testConnectWaitsForTheClientAfterEndingAPairing() {
  bondTwoRemotesThenSwitchThemOff();
  fakeble::failConnects(false);
  fakeble::holdAt(fakeble::Stage::Security);
  for (int i = 0; i < 3 && !host().isConnecting(); ++i) {
    fakeble::advanceMillis(4001);
    host().poll();
  }
  CHECK(fakeble::waitUntilHeld(fakeble::Stage::Security));
  fakeble::lingerOnDisconnect();
  std::thread peer([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fakeble::finishDisconnect();
  });
  const bool started = host().connect(kSecondRemote);
  peer.join();
  CHECK(started);
  CHECK(waitConnected());
  CHECK(std::strcmp(host().connectedAddr(), kSecondRemote) == 0);
}

void testSelectedPeerIsTheOnlyOneRetriedSixTimes() {
  bondTwoRemotesThenSwitchThemOff();
  CHECK(host().armSelectedPeerReconnect(kSecondRemote));
  pollOnce();
  CHECK(attemptsAt(kSecondRemote) == 1);  // at once, not after the first backoff
  fakeble::advanceMillis(3999);
  pollOnce();
  CHECK(attemptsAt(kSecondRemote) == 1);
  for (int i = 0; i < 12; ++i) {
    fakeble::advanceMillis(4000);
    pollOnce();
  }
  CHECK(attemptsAt(kSecondRemote) == 6);
  CHECK(attemptsAt(kRemote) == 0);  // no fallback to the other bond
}

void testSelectedPlanStartsNothingAfterItsWindow() {
  bondTwoRemotesThenSwitchThemOff();
  CHECK(host().armSelectedPeerReconnect(kSecondRemote));
  pollOnce();
  fakeble::advanceMillis(120000);
  pollOnce();
  CHECK(attemptsAt(kSecondRemote) == 1);
  CHECK(attemptsAt(kRemote) == 0);
}

void testArmIsRefusedWhenItCannotApply() {
  bondTwoRemotesThenSwitchThemOff();
  CHECK(!host().armSelectedPeerReconnect(nullptr));
  CHECK(!host().armSelectedPeerReconnect("AA:BB"));
  CHECK(!host().armSelectedPeerReconnect("AA:BB:CC:DD:EE:99"));  // not bonded
  host().startScan(1000);
  CHECK(!host().armSelectedPeerReconnect(kSecondRemote));
  CHECK(host().isScanning());
  host().stopScan();
  CHECK(host().armSelectedPeerReconnect(kSecondRemote));
  CHECK(!host().armSelectedPeerReconnect(kSecondRemote));  // one plan at a time

  fakeble::failConnects(false);
  CHECK(fakeble::connectTo(kRemote));
  CHECK(!host().armSelectedPeerReconnect(kSecondRemote));
  CHECK(host().isConnected());
}

void testConnectAndDisconnectCancelThePlan() {
  bondTwoRemotesThenSwitchThemOff();
  CHECK(host().armSelectedPeerReconnect(kSecondRemote));
  CHECK(host().connect(kRemote));
  CHECK(fakeble::waitForWorkerIdle());
  fakeble::advanceMillis(4001);
  pollOnce();
  // The default turn over the bonds again (first bond first), not the plan.
  CHECK(attemptsAt(kSecondRemote) == 0);
  CHECK(attemptsAt(kRemote) == 2);

  bondTwoRemotesThenSwitchThemOff();
  CHECK(host().armSelectedPeerReconnect(kSecondRemote));
  host().disconnect();
  fakeble::advanceMillis(4001);
  pollOnce();
  CHECK(attemptsAt(kSecondRemote) == 0);
  CHECK(attemptsAt(kRemote) == 0);  // disconnect() also pauses auto-reconnect
}

void testSelectedPeerThatDropsGetsAFreshPlan() {
  bondTwoRemotesThenSwitchThemOff();
  fakeble::failConnects(false);
  CHECK(host().armSelectedPeerReconnect(kSecondRemote));
  pollOnce();
  CHECK(host().isConnected());
  CHECK(std::strcmp(host().connectedAddr(), kSecondRemote) == 0);
  fakeble::advanceMillis(200000);  // long past the first plan's window
  fakeble::peerDisconnect();
  fakeble::advanceMillis(4000);
  pollOnce();
  CHECK(host().isConnected());
  CHECK(attemptsAt(kSecondRemote) == 2);
  CHECK(attemptsAt(kRemote) == 0);
}

// --- Raw button edges ------------------------------------------------------------

// A remote with no readable Report Map and one Input report declaring `reportId`.
int connectBareRemote(uint8_t reportId) {
  fakeble::resetWorld();
  const int in = fakeble::addInputReport(reportId);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));
  return in;
}

void frame(int in, std::initializer_list<uint8_t> bytes) {
  const std::vector<uint8_t> data(bytes);
  fakeble::notify(in, data.data(), data.size());
}

std::vector<freeink::RawButtonEvent> drainRaw() {
  std::vector<freeink::RawButtonEvent> edges;
  freeink::RawButtonEvent ev;
  while (host().popRawButton(ev)) edges.push_back(ev);
  return edges;
}

void testRawEdgeNamesTheReportAndTheByte() {
  const int in = connectBareRemote(3);
  freeink::RawButtonEvent ev;
  CHECK(!host().popRawButton(ev));
  frame(in, {0x00, 0x02, 0x00});
  frame(in, {0x00, 0x00, 0x00});
  const std::vector<freeink::RawButtonEvent> edges = drainRaw();
  CHECK(edges.size() == 2);
  if (edges.size() != 2) return;
  CHECK(edges[0].pressed);
  CHECK(edges[0].code() == 0x030102u);
  CHECK(edges[0].keycode == 0x02);  // what the key decode read from the same frame
  CHECK(!edges[1].pressed);
  CHECK(edges[1].code() == 0x030102u);
  CHECK(!edges[1].wasRest);
  freeink::KeyEvent key;
  CHECK(host().popKey(key) && key.keycode == 0x02);  // the key path is unchanged
}

void testKeyboardModifierByteIsNotTheButton() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));
  frame(g_inputReport, {0x02, 0, 0x04, 0, 0, 0, 0, 0});  // Shift + A
  frame(g_inputReport, {0x02, 0, 0, 0, 0, 0, 0, 0});     // A up, Shift still down
  frame(g_inputReport, {0, 0, 0, 0, 0, 0, 0, 0});
  const std::vector<freeink::RawButtonEvent> edges = drainRaw();
  CHECK(edges.size() == 2);
  if (edges.size() != 2) return;
  CHECK(edges[0].pressed && edges[0].code() == 0x000204u);
  CHECK(edges[0].keycode == 0x04 && edges[0].mods == 0x02);
  CHECK(!edges[1].pressed && edges[1].code() == 0x000204u);
}

// A keyboard map with no modifier keys: a bare key array, as the Flic Duo sends
// ("4F 00 .." for Right Arrow). Byte 0 is the key.
void testBareKeyArrayReadsByteZeroAsAKey() {
  // The Flic Duo's own Report Map: four 16-bit key slots, no modifier byte.
  constexpr uint8_t kBareKeyArrayMap[] = {
      0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,  // Generic Desktop, Keyboard, Report ID 1
      0x75, 0x10, 0x95, 0x04, 0x15, 0x00, 0x25, 0xFF,  //   4 x 16 bits
      0x05, 0x07, 0x19, 0x00, 0x29, 0xFF,              //   Keyboard page, usages 0..0xFF
      0x81, 0x00,                                      //   Input: key array
      0xC0,                                            // End Collection
  };
  fakeble::resetWorld();
  const int mapChar = fakeble::addCharacteristic(0x2A4B, true, false, false);
  fakeble::setValue(mapChar, kBareKeyArrayMap, sizeof kBareKeyArrayMap);
  const int in = fakeble::addInputReport(1);
  const int other = fakeble::addInputReport(2);  // the Flic also has one its map does not describe
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));
  frame(in, {0x4F, 0, 0, 0, 0, 0, 0, 0});  // Right Arrow, slot 1
  frame(in, {0, 0, 0, 0, 0, 0, 0, 0});

  freeink::KeyEvent ev;
  CHECK(host().popKey(ev) && ev.keycode == 0x4F && ev.special == freeink::SpecialKey::Right && ev.mods == 0);
  const std::vector<freeink::RawButtonEvent> edges = drainRaw();
  CHECK(edges.size() == 2);
  if (edges.size() != 2) return;
  CHECK(edges[0].pressed && edges[0].value == 0x4F && edges[0].byteIndex == 0 && edges[0].reportId == 1);
  CHECK(!edges[1].pressed && edges[1].value == 0x4F);

  frame(in, {0, 0, 0, 0, 0, 0, 0x50, 0});  // Left Arrow, slot 4
  frame(in, {0, 0, 0, 0, 0, 0, 0, 0});
  CHECK(host().popKey(ev) && ev.keycode == 0x50);
  frame(in, {0, 0x4F, 0, 0, 0, 0, 0, 0});  // 0x4F00 and 0x014F: above the map's
  frame(in, {0, 0, 0, 0, 0, 0, 0, 0});     // logical maximum, so not keys
  frame(in, {0x4F, 0x01, 0, 0, 0, 0, 0, 0});
  frame(in, {0, 0, 0, 0, 0, 0, 0, 0});
  CHECK(!host().popKey(ev));

  // Only the map's report id and length are bare keys; others keep the boot decode.
  drainRaw();
  frame(other, {0x4F, 0, 0, 0, 0, 0, 0, 0});
  frame(other, {0, 0, 0, 0, 0, 0, 0, 0});
  frame(in, {0x4F, 0, 0, 0, 0, 0, 0});
  frame(in, {0, 0, 0, 0, 0, 0, 0});
  CHECK(!host().popKey(ev));
  CHECK(drainRaw().empty());
}

// A map with a second Input (here a consumer array after the keyboard array) is
// not a bare key array: byte 0 of its keyboard report is not read as a key.
void testMapWithSecondInputKeepsBootDecode() {
  constexpr uint8_t kCompositeMap[] = {
      0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,  // Keyboard, Report ID 1
      0x75, 0x10, 0x95, 0x04, 0x15, 0x00, 0x25, 0xFF,  //   4 x 16 bits (the Flic's report)
      0x05, 0x07, 0x19, 0x00, 0x29, 0xFF, 0x81, 0x00,  //   Input: key array
      0xC0,                                            // End Collection
      0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,  // Consumer Control, Report ID 2
      0x75, 0x10, 0x95, 0x04, 0x15, 0x00, 0x26, 0xFF,  //   4 x 16 bits
      0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x81, 0x00,  //   Input: usage array
      0xC0,                                            // End Collection
  };
  fakeble::resetWorld();
  const int mapChar = fakeble::addCharacteristic(0x2A4B, true, false, false);
  fakeble::setValue(mapChar, kCompositeMap, sizeof kCompositeMap);
  const int in = fakeble::addInputReport(1);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));
  frame(in, {0x4F, 0, 0, 0, 0, 0, 0, 0});
  frame(in, {0, 0, 0, 0, 0, 0, 0, 0});
  freeink::KeyEvent ev;
  CHECK(!host().popKey(ev));
}

// The same keyboard with a vendor status byte declared ahead of its keys (an Input
// that is not a key array): byte 0 stays the boot layout's modifier byte.
void testKeyboardWithOtherInputsKeepsBootLayout() {
  constexpr uint8_t kStatusByteMap[] = {
      0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x75, 0x08, 0x95, 0x02,  // Keyboard, 2 x 8 bits
      0x81, 0x01,                                                  //   Input: constant
      0x95, 0x06, 0x05, 0x07, 0x19, 0x00, 0x29, 0xFF, 0x81, 0x00,  //   Input: 6 keys
      0xC0,
  };
  fakeble::resetWorld();
  serveRemote(kStatusByteMap, sizeof kStatusByteMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));
  frame(g_inputReport, {0x07, 0, 0x4F, 0, 0, 0, 0, 0});  // status 0x07, Right Arrow
  freeink::KeyEvent ev;
  CHECK(host().popKey(ev) && ev.keycode == 0x4F);
  CHECK(!host().popKey(ev));  // no phantom key 0x07
}

void testStreamedHoldIsOnePressAndOneRelease() {
  const int in = connectBareRemote(3);
  unsigned long lastFrameMs = 0;
  for (int i = 0; i < 6; ++i) {
    frame(in, {0x00, 0x02, 0x00});
    lastFrameMs = fakeble::clockMs();
    fakeble::advanceMillis(40);
    host().poll();
  }
  CHECK(drainRaw().size() == 1);  // the press only, while the frames keep coming
  fakeble::advanceMillis(200);
  host().poll();
  const std::vector<freeink::RawButtonEvent> edges = drainRaw();
  CHECK(edges.size() == 1);
  if (edges.size() != 1) return;
  CHECK(!edges[0].pressed);
  CHECK(edges[0].atMs == lastFrameMs);  // dated by the last frame, when it came up
}

void testSilentHoldKeepsItsReleaseForTheReleaseFrame() {
  const int in = connectBareRemote(3);
  frame(in, {0x00, 0x02, 0x00});
  fakeble::advanceMillis(1000);
  host().poll();
  CHECK(drainRaw().size() == 1);  // still held: one frame per edge, no stream
  frame(in, {0x00, 0x00, 0x00});
  const std::vector<freeink::RawButtonEvent> edges = drainRaw();
  CHECK(edges.size() == 1 && !edges[0].pressed);
}

void testPressOnlyFramesAfterSilenceAreNewPresses() {
  const int in = connectBareRemote(6);
  for (int i = 0; i < 3; ++i) {
    frame(in, {0x01});
    fakeble::advanceMillis(400);
    host().poll();
  }
  int presses = 0;
  int releases = 0;
  for (const freeink::RawButtonEvent& e : drainRaw()) {
    CHECK(e.code() == 0x060001u);
    (e.pressed ? presses : releases)++;
  }
  CHECK(presses == 3);
  CHECK(releases == 2);  // the third is still open
}

void testStatusByteAtConnectIsFlaggedAsTheRest() {
  // A remote whose idle frame is "10 00 00": its first frame reads as a press
  // against the zero guess until the next frame shows it was the rest.
  const int in = connectBareRemote(0);
  frame(in, {0x10, 0x00, 0x00});
  frame(in, {0x10, 0x02, 0x00});
  frame(in, {0x10, 0x00, 0x00});
  const std::vector<freeink::RawButtonEvent> edges = drainRaw();
  CHECK(edges.size() == 4);
  if (edges.size() != 4) return;
  CHECK(edges[0].pressed && edges[0].code() == 0x000010u);
  CHECK(!edges[1].pressed && edges[1].code() == 0x000010u && edges[1].wasRest);
  CHECK(edges[2].pressed && edges[2].code() == 0x000102u);
  CHECK(!edges[3].pressed && edges[3].code() == 0x000102u && !edges[3].wasRest);
}

void testFullRingDropsWholePressesNeverAReleaseAlone() {
  const int in = connectBareRemote(3);
  for (int i = 0; i < 9; ++i) {
    frame(in, {0x00, 0x02, 0x00});
    frame(in, {0x00, 0x00, 0x00});
  }
  std::vector<freeink::RawButtonEvent> edges = drainRaw();
  CHECK(edges.size() == 16);  // eight taps; the ninth is dropped whole
  for (size_t i = 0; i < edges.size(); ++i) CHECK(edges[i].pressed == (i % 2 == 0));

  // One slot free: a press would fit, its release would not, so the press stays out.
  for (int i = 0; i < 8; ++i) {
    frame(in, {0x00, 0x02, 0x00});
    frame(in, {0x00, 0x00, 0x00});
  }
  freeink::RawButtonEvent first;
  CHECK(host().popRawButton(first) && first.pressed);
  frame(in, {0x00, 0x02, 0x00});
  frame(in, {0x00, 0x00, 0x00});
  edges = drainRaw();
  CHECK(edges.size() == 15);
  CHECK(!edges.empty() && !edges.back().pressed);
}

void testAxisGamepadButtonComesAsOneTapNamedByItsZone() {
  const int in = connectBareRemote(0);
  frame(in, {0x13, 0xD0, 0x07, 0xD0, 0x07});  // pressed, axes still centred
  frame(in, {0x13, 0xD0, 0x07, 0x84, 0x03});  // axis 2 ramped low
  frame(in, {0x12, 0xD0, 0x07, 0x84, 0x03});  // released: the decoder reads the zone
  const std::vector<freeink::RawButtonEvent> edges = drainRaw();
  CHECK(edges.size() == 2);
  if (edges.size() != 2) return;
  CHECK(edges[0].pressed && edges[0].code() == 0xFFFF43u && edges[0].keycode == 0x43);
  CHECK(!edges[1].pressed && edges[1].code() == 0xFFFF43u);
}

void testNextLinkStartsWithNoButtonHeld() {
  const int in = connectBareRemote(3);
  frame(in, {0x00, 0x02, 0x00});
  fakeble::peerDisconnect();
  CHECK(fakeble::connectTo(kRemote));
  frame(in, {0x00, 0x02, 0x00});
  const std::vector<freeink::RawButtonEvent> edges = drainRaw();
  CHECK(edges.size() == 2);
  for (const freeink::RawButtonEvent& e : edges) CHECK(e.pressed);
}

}  // namespace

void testRemoteButtonProfiles() {
  using namespace freeink;
  CHECK(remoteButtonProfileIndex(nullptr) == -1);
  CHECK(remoteButtonProfileIndex("") == -1);
  CHECK(remoteButtonProfileIndex("Other") == -1);
  const int free3 = remoteButtonProfileIndex("Free3-R");
  CHECK(free3 >= 0);
  const auto &third = kRemoteButtonProfiles[free3];
  CHECK(third.count == 2);
  CHECK(third.buttons[0].code == 0x000102 &&
        third.buttons[0].key == RemoteKey::PageDown);
  CHECK(third.buttons[1].code == 0x000008 &&
        third.buttons[1].key == RemoteKey::PageUp);
  CHECK(third.buttons[0].anyReport && third.buttons[1].anyReport);
  const int boox = remoteButtonProfileIndex("BOOX Keypage");
  CHECK(boox >= 0);
  const auto &keys = kRemoteButtonProfiles[boox];
  CHECK(keys.count == 4);
  const uint32_t codes[] = {0x030002, 0x030001, 0x030004, 0x030008};
  for (size_t i = 0; i < 4; ++i) {
    CHECK(keys.buttons[i].code == codes[i]);
    CHECK(keys.buttons[i].key ==
          (i % 2 == 0 ? RemoteKey::Right : RemoteKey::Left));
    CHECK(!keys.buttons[i].anyReport);
  }
}

int main() {
  testRemoteButtonProfiles();
  testKeyboardPressIsOneKeyEvent();
  testHidFoundByListingWhenUuidSearchFails();
  testEndDuringPairingDoesNotDeleteTaskInsideNimble(fakeble::Stage::Security);
  testEndDuringPairingDoesNotDeleteTaskInsideNimble(fakeble::Stage::Discovery);
  testScanCancelsReconnectThatIsPairing();
  testDisconnectIsNotUndoneByAutoReconnect();
  testStreamedHeldKeyIsOnePress();
  testScanKeepsNoAdvertiserInNimble();
  testKeyHeldAcrossLinkDropIsPressedAgain();
  testConnectedAddressFollowsTheLink();
  testEndCancelsAPairingWaitBeforeDeletingTheTask();
  testEndLeavesAStuckTaskAloneAndFinishesLater();
  testEndWaitsForTheClientToFinishDisconnecting();
  testSecondEndDuringTeardownReportsStillStopping();
  testTwoTasksRetryingEndNeverOverlap();
  testConnectEndsAReconnectInFlight();
  testConnectWaitsForTheClientAfterEndingAPairing();
  testSelectedPeerIsTheOnlyOneRetriedSixTimes();
  testSelectedPlanStartsNothingAfterItsWindow();
  testArmIsRefusedWhenItCannotApply();
  testConnectAndDisconnectCancelThePlan();
  testSelectedPeerThatDropsGetsAFreshPlan();
  testRawEdgeNamesTheReportAndTheByte();
  testKeyboardModifierByteIsNotTheButton();
  testBareKeyArrayReadsByteZeroAsAKey();
  testKeyboardWithOtherInputsKeepsBootLayout();
  testMapWithSecondInputKeepsBootDecode();
  testStreamedHoldIsOnePressAndOneRelease();
  testSilentHoldKeepsItsReleaseForTheReleaseFrame();
  testPressOnlyFramesAfterSilenceAreNewPresses();
  testStatusByteAtConnectIsFlaggedAsTheRest();
  testFullRingDropsWholePressesNeverAReleaseAlone();
  testAxisGamepadButtonComesAsOneTapNamedByItsZone();
  testNextLinkStartsWithNoButtonHeld();
  fakeble::resetWorld();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  std::fflush(stdout);
  // Skip static destructors: a connection task the host deleted inside a NimBLE
  // wait is parked on purpose and must not see its fakes torn down.
  std::_Exit(checksFailed == 0 ? 0 : 1);
}
