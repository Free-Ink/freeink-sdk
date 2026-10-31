// FreeInk SDK — BLE HID host implementation.
//
// The real NimBLE central path compiles only under FREEINK_CAP_BLE_HID_HOST; the
// #else branch links stub bodies (and references no BLE code). Flow: scan ->
// connect on a dedicated FreeRTOS task -> bond -> set Report
// Protocol -> subscribe to the HID input report -> diff reports into key events.
// Connection runs on its own task so neither the main loop nor the NimBLE host
// task ever blocks on pairing.

#include "BleKeyboardHost.h"

#include <BoardConfig.h>  // FREEINK_CAP_BLE_HID_HOST

namespace freeink {

BleKeyboardHost& BleKeyboardHost::getInstance() {
  static BleKeyboardHost instance;
  return instance;
}

}  // namespace freeink

#if FREEINK_CAP_BLE_HID_HOST

#include <NimBLEDevice.h>
#include <Preferences.h>

#include <atomic>
#include <cstring>
#include <string>

#include "HidKeymap.h"

namespace freeink {
namespace {

constexpr uint16_t kHidService = 0x1812;
constexpr uint16_t kAppearanceKeyboard = 0x03C1;
constexpr uint16_t kCharReport = 0x2A4D;
constexpr uint16_t kCharProtocolMode = 0x2A4E;
constexpr uint16_t kCharBootKbdInput = 0x2A22;
constexpr uint16_t kCharReportMap = 0x2A4B;
constexpr uint16_t kDescReportReference = 0x2908;
// Input report characteristics whose Report Reference id is kept for raw edges.
constexpr uint8_t kMaxInputReportChars = 4;

// Page-turner remotes often stream a held key (or omit a clean release frame). If no
// report arrives within this window, treat the key as released so one physical press
// yields one event and the next press re-triggers.
constexpr uint32_t kReleaseTimeoutMs = 150;
constexpr uint32_t kReconnectBackoffMs = 4000;
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint32_t kTeardownConnectWaitMs = kConnectTimeoutMs + 500;
constexpr uint32_t kMaxTeardownTimeoutMs = 2000;
constexpr uint32_t kTeardownPollMs = 10;
constexpr size_t kScanDebugPayloadMax = 31;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

NimBLEClient* g_client = nullptr;

// Connection task: connect() stores a target and notifies it; it runs the
// blocking connect+pair+discover sequence off the main and NimBLE host tasks.
TaskHandle_t g_connTask = nullptr;
volatile bool g_connecting = false;
char g_targetAddr[18] = {0};
uint8_t g_targetType = 0;
bool g_targetTryAltType = false;
// Set by an end() that could not finish; cleared by the one that does.
std::atomic<bool> g_teardownPending{false};
// Held by the one end() that is running. end() is called from more than one task
// (an app may stop the radio from its main loop and from a render task), and its
// teardown (connection task delete, client delete, NimBLEDevice::deinit) is not
// re-entrant: two at once can double-free NimBLE state. A second caller does not
// wait; it gets false, which reads as "still stopping" and is retried like a
// teardown that ran out of time.
std::atomic<bool> g_endBusy{false};
// False while a link attempt or a link exists; set again by NimBLE's disconnect
// callback. NimBLE only lets go of the client once that callback has run.
std::atomic<bool> g_disconnectObserved{true};

uint32_t g_lastReconnectMs = 0;
uint8_t g_reconnectIdx = 0;
// Off once the app calls disconnect(), so a link the user closed stays closed.
// connect() and begin() turn it back on; a link the peer drops leaves it on.
bool g_autoReconnect = true;

// armSelectedPeerReconnect(): while an address is set, auto-reconnect tries only
// that peer, and only while attempts and the window last. Guarded by g_mux.
constexpr uint8_t kSelectedReconnectAttempts = 6;
constexpr uint32_t kSelectedReconnectWindowMs = 120000;
char g_selectedAddr[18] = {0};
uint8_t g_selectedAttemptsLeft = 0;
uint32_t g_selectedStartedMs = 0;

// armBondedReconnect(): while armed, poll() scans and connects to a bond heard
// advertising, and only within the window from arming or from the last drop.
// Guarded by g_mux.
constexpr uint32_t kPriorityWaitMs = 3000;
constexpr uint32_t kBondedScanMs = 5000;
bool g_bondedArmed = false;
uint32_t g_bondedStartedMs = 0;
PickPolicy g_pickPolicy = PickPolicy::First;
char g_priorityAddr[18] = {0};
char g_candidateAddr[18] = {0};
uint8_t g_candidateType = 0;
uint32_t g_candidateFirstMs = 0;
bool g_candidateReady = false;
// A bond whose link came up and then failed this many times in a row is not picked
// again until the window reopens, once another bond has been heard, so it cannot
// starve a remote that works. With no other bond around it keeps its turns.
constexpr uint8_t kBondedFailLimit = 2;
char g_failedAddr[18] = {0};
uint8_t g_failedCount = 0;
bool g_otherBondHeard = false;
// The connect being made got a link before it failed (service, pairing or HID setup).
// A connect timeout does not count: the remote may only be waking. Connection task only.
bool g_failedAfterLink = false;

// Caller holds g_mux.
void clearCandidateLocked() {
  g_candidateAddr[0] = '\0';
  g_candidateReady = false;
}

// Caller holds g_mux. A new scan window: every bond gets its turns again.
void openBondedWindowLocked() {
  g_bondedStartedMs = millis();
  g_failedAddr[0] = '\0';
  g_failedCount = 0;
  g_otherBondHeard = false;
}

// Caller holds g_mux.
void cancelBondedLocked() {
  g_bondedArmed = false;
  g_priorityAddr[0] = '\0';
  clearCandidateLocked();
}

// Caller holds g_mux.
void clearReconnectPlansLocked() {
  g_selectedAddr[0] = '\0';
  g_selectedAttemptsLeft = 0;
  cancelBondedLocked();
}

// HID Report Map hints (parsed once per connection in setupHid). Many BLE
// page-turner remotes are NOT plain boot keyboards: they place their code on the
// Consumer Control page (0x0C) or at a non-standard byte offset. These hints, plus
// the generic-extraction fallback in onReportIngest, let those remotes surface a
// stable key code so the host (capture-then-assign UI) can bind it. g_lastGenericCode
// edge-detects the generic path so one physical press = one key event.
bool g_hasKeyboardPage = false;
bool g_hasConsumerPage = false;
uint8_t g_preferredByteIndex = 0xFF;  // byte the report map suggests holds the code
uint8_t g_lastGenericCode = 0;        // last non-zero code seen on the generic path
volatile uint32_t g_lastReportMs = 0;  // millis() of the last HID notification (stale-release)
// A keyboard whose Report Map has one Input item, a Keyboard-page array of 8- or
// 16-bit slots (Flic Duo: four 16-bit slots, "4F 00 .." for Right Arrow), has no
// modifier byte: its keys start at byte 0. len 0 = no such keyboard.
struct BareKeys {
  uint8_t reportId;
  uint8_t slotBytes;
  uint8_t len;
};
BareKeys g_bareKeys{};

bool isBareKeys(uint8_t reportId, size_t len) {
  return g_bareKeys.len != 0 && reportId == g_bareKeys.reportId && len == g_bareKeys.len;
}

// Report id each subscribed Input characteristic declares in its Report Reference
// descriptor, so a report keeps its identity when its payload has no id byte.
struct InputReportChar {
  const NimBLERemoteCharacteristic* chr;
  uint8_t reportId;
};
InputReportChar g_inputChars[kMaxInputReportChars];
uint8_t g_inputCharCount = 0;
uint8_t g_notifyReportId = 0;  // report id of the characteristic that just notified

// The part of a report raw edges read: its id and up to 8 payload bytes, with the
// modifier byte of a keyboard-shaped report cleared (a held Shift is not a button).
// Same shapes as the key decode: a 9-byte frame leads with its report id, a 7- or
// 8-byte payload with its modifiers.
struct RawFrame {
  uint8_t id;
  uint8_t bytes[8];
};

RawFrame rawFrameOf(const uint8_t* data, size_t len) {
  RawFrame f{};
  size_t offset = 0;
  f.id = g_notifyReportId;
  if (len == 9) {
    f.id = data[0];
    offset = 1;
  }
  const size_t n = len - offset;
  for (size_t i = 0; i < n && i < sizeof(f.bytes); ++i) f.bytes[i] = data[offset + i];
  if (n >= 7 && !isBareKeys(f.id, n)) f.bytes[0] = 0;
  return f;
}

// The button a frame holds: the first byte that differs from `rest`, as
// value | index << 8 | id << 16. 0 = the frame is the rest frame (a release).
uint32_t rawButtonCode(const RawFrame& f, const uint8_t* rest) {
  for (uint32_t i = 0; i < sizeof(f.bytes); ++i) {
    if (f.bytes[i] != rest[i]) return f.bytes[i] | i << 8 | static_cast<uint32_t>(f.id) << 16;
  }
  return 0;
}

BleKeyboardHost& self() { return BleKeyboardHost::getInstance(); }

// The client can be deleted: its disconnect callback ran and NimBLE reports it
// DISCONNECTED. deleteClient() on a CONNECTED or DISCONNECTING client only
// defers the delete, and a deinit() after that leaks NimBLE's client slot.
bool clientReleased() {
  return g_disconnectObserved.load() && NimBLEDevice::getDisconnectedClient() == g_client;
}

// The BareKeys layout of a Report Map, or {}. Walks whole items (HID 1.11 6.2.2),
// so item data is never read as a tag; the hint scan in parseReportMapHints stays
// a byte scan so other remotes keep their behaviour. A second Input, Push/Pop, a
// long item or a map cut mid-item gives {}; so does a report shorter than the
// 7 bytes the boot layout decode starts at.
BareKeys bareKeysOf(const uint8_t* map, size_t len) {
  BareKeys found{};
  uint32_t page = 0, size = 0, count = 0, id = 0;
  for (size_t i = 0; i < len;) {
    const uint8_t prefix = map[i];
    const size_t n = (prefix & 3) == 3 ? 4 : (prefix & 3);
    if (prefix == 0xFE || i + n >= len) return {};
    uint32_t value = 0;
    for (size_t b = 0; b < n; ++b) value |= static_cast<uint32_t>(map[i + 1 + b]) << (8 * b);
    switch (prefix & 0xFC) {
      case 0x04:  // Usage Page
        page = value;
        break;
      case 0x74:  // Report Size
        size = value;
        break;
      case 0x94:  // Report Count
        count = value;
        break;
      case 0x84:  // Report ID
        id = value;
        break;
      case 0xA4:  // Push
      case 0xB4:  // Pop
        return {};
      case 0x80:  // Input: must be the only one, a Data,Array on the Keyboard page
        // count <= 64 keeps the report length in a uint8_t.
        if (found.len || value != 0 || page != 0x07 || (size != 8 && size != 16) || count > 64 ||
            count * size / 8 < 7) {
          return {};
        }
        found = {static_cast<uint8_t>(id), static_cast<uint8_t>(size / 8), static_cast<uint8_t>(count * size / 8)};
        break;
    }
    i += 1 + n;
  }
  return found;
}

// Scan a HID Report Map descriptor for Usage Page (0x05 nn) items and note whether
// a keyboard (0x07) or consumer (0x0C) page is present, plus a heuristic byte index
// where the active code tends to live (keyboard reports: byte[2]; compact consumer
// reports: byte[1]). This is a hint, not a full descriptor parse.
void parseReportMapHints(const uint8_t* map, size_t len) {
  g_hasKeyboardPage = false;
  g_hasConsumerPage = false;
  g_preferredByteIndex = 0xFF;
  if (!map || len < 2) return;
  for (size_t i = 0; i + 1 < len; ++i) {
    if (map[i] == 0x05) {  // Usage Page (1-byte value follows)
      if (map[i + 1] == 0x07) g_hasKeyboardPage = true;
      else if (map[i + 1] == 0x0C) g_hasConsumerPage = true;
    }
  }
  if (g_hasKeyboardPage) g_preferredByteIndex = 2;
  else if (g_hasConsumerPage) g_preferredByteIndex = 1;
}

// Pick a representative "primary" code from a report that the standard keyboard
// slot decode did not handle (consumer / compact / non-standard layouts). Prefers
// the report-map-hinted byte, else the first meaningful non-zero byte. Returns 0
// when the report carries no code (a release frame).
uint8_t extractPrimaryCode(const uint8_t* p, size_t n, size_t* codeIdx = nullptr) {
  const size_t lim = n < 8 ? n : 8;
  // NOTE: do NOT skip 0x01 here. In a keyboard report 0x01 is ErrorRollOver, but in
  // a consumer / vendor report it is a valid button code (e.g. a 3-byte page-turner
  // report of "01 00 00" on press, "00 00 00" on release). Only zero means "no code".
  if (g_preferredByteIndex != 0xFF && g_preferredByteIndex < n && p[g_preferredByteIndex] != 0) {
    if (codeIdx) *codeIdx = g_preferredByteIndex;
    return p[g_preferredByteIndex];
  }
  for (size_t i = 0; i < lim; ++i) {
    if (p[i] != 0) {
      if (codeIdx) *codeIdx = i;
      return p[i];
    }
  }
  return 0;
}

#ifndef FREEINK_BLE_HID_SCAN_DEBUG
#ifdef FREEINK_BLE_KEYBOARD_SCAN_DEBUG
#define FREEINK_BLE_HID_SCAN_DEBUG FREEINK_BLE_KEYBOARD_SCAN_DEBUG
#else
#define FREEINK_BLE_HID_SCAN_DEBUG 0
#endif
#endif

// Raw HID report logging. Define FREEINK_BLE_HID_REPORT_DEBUG=1 in the firmware to
// dump every notification's bytes — the fastest way to learn what a new remote sends.
#ifndef FREEINK_BLE_HID_REPORT_DEBUG
#define FREEINK_BLE_HID_REPORT_DEBUG 0
#endif

#if FREEINK_BLE_HID_SCAN_DEBUG
void printPayloadHex(const NimBLEAdvertisedDevice* dev) {
  if (!dev) return;
  const std::vector<uint8_t>& payload = dev->getPayload();
  const size_t n = payload.size() < kScanDebugPayloadMax ? payload.size() : kScanDebugPayloadMax;
  Serial.print("  payload=");
  for (size_t i = 0; i < n; ++i) {
    if (payload[i] < 0x10) Serial.print('0');
    Serial.printf("%x", payload[i]);
  }
  if (payload.size() > n) Serial.print("...");
}
#endif

void onHidNotify(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool) {
  g_notifyReportId = 0;
  for (uint8_t i = 0; i < g_inputCharCount; ++i) {
    if (g_inputChars[i].chr == chr) g_notifyReportId = g_inputChars[i].reportId;
  }
  self().onReportIngest(data, len);
}

bool setupHid(NimBLEClient* client) {
  NimBLERemoteService* hid = client->getService(NimBLEUUID(kHidService));
  if (!hid) return false;

  // Prefer Report Protocol so we get full reports (and report ids) like a host.
  NimBLERemoteCharacteristic* proto = hid->getCharacteristic(NimBLEUUID(kCharProtocolMode));
  if (proto && proto->canWrite()) {
    uint8_t mode = 1;  // 1 = Report Protocol, 0 = Boot Protocol
    proto->writeValue(&mode, 1, false);
  }

  // Parse the HID Report Map for usage-page / byte-offset hints, so non-keyboard
  // page-turner remotes can be decoded by the generic fallback in onReportIngest.
  g_hasKeyboardPage = false;
  g_hasConsumerPage = false;
  g_preferredByteIndex = 0xFF;
  g_bareKeys = {};
  g_lastGenericCode = 0;
  if (NimBLERemoteCharacteristic* rmap = hid->getCharacteristic(NimBLEUUID(kCharReportMap))) {
    if (rmap->canRead()) {
      NimBLEAttValue v = rmap->readValue();
      parseReportMapHints(v.data(), v.size());
      g_bareKeys = bareKeysOf(v.data(), v.size());
#if FREEINK_BLE_HID_REPORT_DEBUG
      Serial.printf("[BleHid] report map: kbd=%d consumer=%d preferredByte=%d bareKeysLen=%u len=%u\n",
                    g_hasKeyboardPage, g_hasConsumerPage, (int)g_preferredByteIndex, (unsigned)g_bareKeys.len,
                    (unsigned)v.size());
#endif
    }
  }

  bool subscribed = false;
  g_inputCharCount = 0;
  const std::vector<NimBLERemoteCharacteristic*>& chars = hid->getCharacteristics(true);
  for (NimBLERemoteCharacteristic* c : chars) {
    if (!c) continue;
    if (c->getUUID() != NimBLEUUID(kCharReport) || !c->canNotify()) continue;
    // Report Reference descriptor (0x2908): byte[0] is the report id, byte[1] the
    // report type (1 = Input).
    bool isInput = true;
    uint8_t reportId = 0;
    NimBLERemoteDescriptor* ref = c->getDescriptor(NimBLEUUID(kDescReportReference));
    if (ref) {
      NimBLEAttValue v = ref->readValue();
      if (v.size() >= 2 && v[1] != 0x01) isInput = false;
      if (v.size() >= 1) reportId = v[0];
    }
    if (isInput && c->subscribe(true, onHidNotify)) {
      subscribed = true;
      if (g_inputCharCount < kMaxInputReportChars) g_inputChars[g_inputCharCount++] = {c, reportId};
    }
  }

  if (!subscribed) {  // fallback: boot keyboard input report
    // Boot reports always lead with the modifier byte. One byte store: a report
    // already notifying must never see a half-cleared layout.
    g_bareKeys.len = 0;
    NimBLERemoteCharacteristic* boot = hid->getCharacteristic(NimBLEUUID(kCharBootKbdInput));
    if (boot && boot->canNotify() && boot->subscribe(true, onHidNotify)) subscribed = true;
  }
  return subscribed;
}

bool hasHidService(NimBLEClient* client) {
  if (!client) return false;
  const NimBLEUUID hid(kHidService);
  if (client->getService(hid)) return true;
  // Some peripherals (Flic) return nothing for a search for one service UUID (ATT
  // Find By Type Value). Android and BlueZ list every service instead; do that too.
  // setupHid's getService() then finds HID in that list.
  for (const NimBLERemoteService* svc : client->getServices(true)) {
    if (svc->getUUID() == hid) return true;
  }
  return false;
}

// Ends an in-flight connection attempt at whatever stage it has reached.
// cancelConnect() stops only the GAP connect. Once the link is up the worker
// waits inside NimBLE for pairing or GATT discovery, and only a disconnect ends
// that wait. Deleting the worker there leaves NimBLE holding its task handle.
void cancelPendingConnect() {
  if (!g_client) return;
  g_client->cancelConnect();
  if (g_client->isConnected()) g_client->disconnect();
}

void doConnect(const char* addrStr, uint8_t type) {
  g_failedAfterLink = false;
  if (!g_client) {
    self().onConnectFailed("BLE client unavailable");
    return;
  }
  NimBLEDevice::getScan()->stop();
  NimBLEAddress addr(std::string(addrStr), type);
  g_disconnectObserved.store(false);
  if (!g_client->connect(addr)) {
    g_disconnectObserved.store(true);
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.printf("[BleHid] connect failed: %s type=%u err=%d\n", addrStr, type, g_client->getLastError());
#endif
    if (!g_targetTryAltType) {
      self().onConnectFailed(g_client->getLastError() == BLE_HS_ETIMEOUT ? "Connect timeout" : "Connection failed");
      return;
    }
    type = type == 0 ? 1 : 0;
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.printf("[BleHid] retry connect: %s type=%u\n", addrStr, type);
#endif
    addr = NimBLEAddress(std::string(addrStr), type);
    g_disconnectObserved.store(false);
    if (!g_client->connect(addr)) {
      g_disconnectObserved.store(true);
#if FREEINK_BLE_HID_SCAN_DEBUG
      Serial.printf("[BleHid] connect failed: %s type=%u err=%d\n", addrStr, type, g_client->getLastError());
#endif
      self().onConnectFailed(g_client->getLastError() == BLE_HS_ETIMEOUT ? "Connect timeout" : "Connection failed");
      return;
    }
  }
  g_failedAfterLink = true;  // the link is up: a failure from here counts toward the skip
  if (!hasHidService(g_client)) {
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.printf("[BleHid] no HID service: %s\n", addrStr);
#endif
    g_client->disconnect();
    self().onConnectFailed("Not a HID device");
    return;
  }
  if (!g_client->secureConnection()) {
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.printf("[BleHid] security failed: %s err=%d\n", addrStr, g_client->getLastError());
#endif
    g_client->disconnect();
    self().onConnectFailed("Pairing failed");
    return;
  }
  if (!setupHid(g_client)) {
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.printf("[BleHid] HID setup failed: %s\n", addrStr);
#endif
    g_client->disconnect();
    self().onConnectFailed("No HID input report");
    return;
  }
  self().onLinkUp(addrStr, nullptr, type);  // name resolved from scan/bond lists
}

void connTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    doConnect(g_targetAddr, g_targetType);
    g_connecting = false;
  }
}

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!dev) return;
    // Store named/HID advertisers by default; optionally keep anonymous
    // non-HID probe candidates during bring-up. HID is still validated at
    // connect time. The name falls back to the address. Keep the callback cheap
    // so heavy logging can't choke the C3's advertisement-report queue.
    const std::string a = dev->getAddress().toString();
    // A bonded-reconnect scan only asks whether a bond advertises: skip the name work.
    portENTER_CRITICAL(&g_mux);
    const bool bondedScan = g_bondedArmed;
    portEXIT_CRITICAL(&g_mux);
    if (bondedScan) {
      self().onScanResultIngest(a.c_str(), nullptr, 0, dev->getAddress().getType(), false, dev->isConnectable());
      return;
    }
    const bool named = dev->haveName();
    const std::string nm = named ? dev->getName() : a;
    const uint8_t type = dev->getAddress().getType();
    const int rssi = dev->getRSSI();
    const uint16_t appearance = dev->haveAppearance() ? dev->getAppearance() : 0;
    const bool keyboardAppearance = appearance == kAppearanceKeyboard;
    const bool connectable = dev->isConnectable();
    const bool hid = dev->isAdvertisingService(NimBLEUUID(kHidService)) || keyboardAppearance;
#if FREEINK_BLE_HID_SCAN_DEBUG
    if (named || hid || connectable) {
      Serial.printf("[BLE adv] %s  name='%s'  rssi=%d  hid=%d  app=0x%04x  conn=%d  addrType=%u",
                    a.c_str(), nm.c_str(), rssi, hid ? 1 : 0, appearance, connectable ? 1 : 0, type);
#if CONFIG_BT_NIMBLE_EXT_ADV
      Serial.printf("  legacy=%d  advType=0x%02x  data=%u  phy=%u/%u  len=%u", dev->isLegacyAdvertisement() ? 1 : 0,
                    dev->getAdvType(), dev->getDataStatus(), dev->getPrimaryPhy(), dev->getSecondaryPhy(),
                    dev->getAdvLength());
#else
      Serial.printf("  advType=0x%02x  len=%u", dev->getAdvType(), dev->getAdvLength());
#endif
      printPayloadHex(dev);
      Serial.println();
    }
#endif
    self().onScanResultIngest(a.c_str(), nm.c_str(), rssi, type, hid, connectable);
  }
};

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int) override {
    g_disconnectObserved.store(true);
    self().onLinkDown();
  }
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override { NimBLEDevice::injectPassKey(connInfo, 123456); }
  uint32_t onPassKeyDisplay(NimBLEConnInfo&) override {
    const uint32_t passkey = NimBLEDevice::getSecurityPasskey();
    self().onPairingPasskey(passkey);
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.printf("[BleHid] pairing passkey: %06lu\n", static_cast<unsigned long>(passkey));
#endif
    return passkey;
  }
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t) override {
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }
  // Reject peripheral connection-parameter updates — some keyboards request one
  // on the first keypress and drop the link if it's negotiated.
  bool onConnParamsUpdateRequest(NimBLEClient*, const ble_gap_upd_params*) override { return false; }
};

ScanCB g_scanCb;
ClientCB g_clientCb;

}  // namespace

// --- Lifecycle ---------------------------------------------------------------
bool BleKeyboardHost::begin(const char* hostName) {
  if (begun_) return true;
  // An unfinished end() left the connection task, the client or the stack in
  // place: re-initializing now would pull NimBLE out from under them.
  if (g_teardownPending.load()) return false;
  g_autoReconnect = true;
  portENTER_CRITICAL(&g_mux);
  clearReconnectPlansLocked();
  portEXIT_CRITICAL(&g_mux);

#if FREEINK_BLE_HID_SCAN_DEBUG
  Serial.printf("[BleHid] begin: host='%s' bonds=%u\n", hostName ? hostName : "FreeInk", bondCount_);
#endif

  // Self-heal a partial teardown: if NimBLE still reports initialized (a previous
  // deinit raced and didn't complete), NimBLEDevice::init() would no-op and hand back
  // a dead stack. Force a clean deinit first so the init below actually runs.
  if (NimBLEDevice::isInitialized()) {
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.println("[BleHid] begin: NimBLE was already initialized; forcing deinit");
#endif
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  loadBonds();
#if FREEINK_BLE_HID_SCAN_DEBUG
  Serial.printf("[BleHid] begin: loaded bonds=%u\n", bondCount_);
#endif
  if (!NimBLEDevice::init(hostName ? hostName : "FreeInk")) {
    Serial.println("[BleHid] begin: NimBLEDevice::init() failed");
    return false;
  }

  // HID page-turner reports are a few bytes; request the minimum ATT MTU so the
  // host doesn't reserve large per-connection GATT buffers. On RAM-constrained
  // targets (ESP32-C3, ~16 KB max contiguous block once the stack is up) every KB
  // back to the heap matters for the app (e.g. EPUB inflate windows).
  NimBLEDevice::setMTU(23);

  // Bonding for HID remotes. Default to Just Works because page-turners commonly
  // have no input/display capability; mandatory MITM makes those devices reject
  // pairing. Firmware that specifically needs host-display keyboard pairing can
  // opt in with FREEINK_BLE_HID_REQUIRE_MITM=1.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/FREEINK_BLE_HID_REQUIRE_MITM, /*sc=*/false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(123456);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);

  NimBLEScan* scan = NimBLEDevice::getScan();
  // wantDuplicates = true: DON'T filter duplicates. A device's name/HID often
  // rides in the scan response (a separate PDU after the first advertisement);
  // with duplicate filtering on, the C3 controller drops that follow-up, so the
  // device shows up nameless or not at all. This (plus the windowed interval
  // below and the no-filter onResult) keeps scan response and extended adv data.
  scan->setScanCallbacks(&g_scanCb, true);
  // onResult() copies what the UI needs into devices_, and nothing reads NimBLE's
  // own result list. Callback-only mode frees each advertiser once it has been
  // reported instead of keeping every one heard on the heap until the next scan.
  scan->setMaxResults(0);
  scan->setActiveScan(true);  // send scan requests -> receive scan responses (names)
  // CONTINUOUS listening (window == interval, 100% duty; values are ms).
  // Extended advertising splits data into an AUX packet on a secondary
  // channel that the controller must catch at a precise moment after the primary
  // — if the scan window is closed when it lands, the name/HID UUID is lost. A
  // windowed (low-duty) scan is fine for legacy keyboards but starves AUX
  // reception, which is the only data this keyboard exposes. Duplicate filtering
  // is OFF (above) so the AUX packet (same address as the primary) isn't dropped.
  scan->setInterval(160);
  scan->setWindow(160);
#if CONFIG_BT_NIMBLE_EXT_ADV
  // Some BLE 5.x peripherals advertise on LE Coded. Scan both PHYs so the pairing
  // UI sees the same devices a desktop Bluetooth stack reports.
  scan->setPhy(NimBLEScan::SCAN_ALL);
#endif

  g_client = NimBLEDevice::createClient();
  if (!g_client) {
    // NimBLE can refuse a new client if a previous one wasn't reclaimed (e.g. rapid
    // deinit/init cycles). Don't dereference null — unwind cleanly so a later begin()
    // can retry from a clean state.
    Serial.println("[BleHid] begin: createClient() returned null");
    NimBLEDevice::deinit(true);
    return false;
  }
  g_client->setConnectTimeout(kConnectTimeoutMs);
  // Widen the link supervision timeout. The host runs on a single RISC-V core and
  // some operations block it for several seconds (e.g. the reader rebuilding a whole
  // chapter to reach its last page on a backward page turn across a section boundary).
  // NimBLE's default supervision timeout is only 2.56s, so those long renders overrun
  // it and the controller drops the link — the peripheral then fails to re-encrypt on
  // auto-reconnect (HCI 0x08 "Connection Timeout" -> BLE_HS err 520). Request a low
  // interval for keypress responsiveness with an 8s supervision timeout (units:
  // interval 1.25ms, timeout 10ms) so a slow page render can't sever the connection.
  // Peripheral-initiated updates are still rejected (see onConnParamsUpdateRequest), so
  // these host-chosen params hold for the life of the link.
  g_client->setConnectionParams(/*minInterval=*/12, /*maxInterval=*/24, /*latency=*/0, /*timeout=*/800);
  g_client->setClientCallbacks(&g_clientCb, false);

  xTaskCreate(connTaskFn, "ble-conn", 4096, nullptr, 3, &g_connTask);
  begun_ = true;
#if FREEINK_BLE_HID_SCAN_DEBUG
  Serial.println("[BleHid] begin: ok");
#endif
  return true;
}

bool BleKeyboardHost::end(uint32_t timeoutMs) {
  if (g_endBusy.exchange(true)) return false;
  struct EndBusyRelease {
    ~EndBusyRelease() { g_endBusy.store(false); }
  } endBusyRelease;
  // Read under g_endBusy: no other end() can be between its begun_ and
  // g_teardownPending writes, so "both false" really means stopped.
  if (!begun_ && !g_teardownPending.load()) return true;
  const uint32_t budgetMs = timeoutMs > kMaxTeardownTimeoutMs ? kMaxTeardownTimeoutMs : timeoutMs;
  const uint32_t startMs = millis();
  portENTER_CRITICAL(&g_mux);
  clearReconnectPlansLocked();
  portEXIT_CRITICAL(&g_mux);
  if (begun_) {
    // Stopping before not-running: an observer of isRunning()/isStopping() never
    // sees both false while the stack is still up.
    g_teardownPending.store(true);
    begun_ = false;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) scan->stop();
  }

  // If the connection task is inside a connect, pairing or GATT wait, do not
  // delete it or deinit NimBLE under it: killing it there leaves NimBLE holding a
  // dead task and can crash on Bluetooth-off, sleep, or the next begin(). Cancel
  // on every pass: the task can bring the link up after the first cancel.
  while (g_connecting) {
    cancelPendingConnect();
    if (millis() - startMs >= budgetMs) return false;
    vTaskDelay(pdMS_TO_TICKS(kTeardownPollMs));
  }
  if (g_connTask) {
    vTaskDelete(g_connTask);
    g_connTask = nullptr;
  }

  // Close the link and delete the client BEFORE deinit. NimBLE keeps a fixed-size
  // client array that survives deinit/init, and deleteClient() only DEFERS the
  // delete while the client is CONNECTED/DISCONNECTING. deinit() would then tear
  // the host down before that deferred delete runs, the slot leaks, and the next
  // begin()'s createClient() returns null for good.
  if (g_client) {
    while (!clientReleased()) {
      if (g_client->isConnected()) g_client->disconnect();
      if (millis() - startMs >= budgetMs) return false;
      vTaskDelay(pdMS_TO_TICKS(kTeardownPollMs));
    }
    NimBLEDevice::deleteClient(g_client);
    g_client = nullptr;
  }

  // Free the NimBLE host + BT controller memory back to the heap. Bonds live in
  // NVS and survive this. A deinit that did not complete is retried by the next
  // end(), so the next begin() never inherits a half-initialized stack.
  NimBLEDevice::deinit(true);
  if (NimBLEDevice::isInitialized()) return false;
  g_lastGenericCode = 0;
  g_lastReportMs = 0;

  portENTER_CRITICAL(&g_mux);
  connected_ = false;
  connecting_ = false;
  scanning_ = false;
  deviceCount_ = 0;
  ringHead_ = 0;
  ringTail_ = 0;
  heldUsage_ = 0;
  clearReconnectPlansLocked();
  rawHead_ = 0;
  rawTail_ = 0;
  rawCode_ = 0;
  rawReports_ = 0;
  rawDropped_ = false;
  portEXIT_CRITICAL(&g_mux);
  rawRestCount_ = 0;
  connAddr_[0] = '\0';
  g_teardownPending.store(false);
  return true;
}

bool BleKeyboardHost::isStopping() const { return g_teardownPending.load(); }

void BleKeyboardHost::poll() {
  if (!begun_) return;

  // Reflect the live scanner state.
  scanning_ = NimBLEDevice::getScan()->isScanning();

  // Held-key release. Page-turner remotes stream a held key (and many omit a clean
  // release frame), so we do NOT synthesize host-side auto-repeat — that turned one
  // tap into dozens of page turns. Instead, when reports stop arriving, age the held
  // key / last generic code out so one physical press == one event and the next press
  // (even of the same button) re-triggers. Covers both the keyboard and generic paths.
  portENTER_CRITICAL(&g_mux);
  const uint8_t held = heldUsage_;
  portEXIT_CRITICAL(&g_mux);
  if ((held != 0 || g_lastGenericCode != 0) && (millis() - g_lastReportMs) > kReleaseTimeoutMs) {
    portENTER_CRITICAL(&g_mux);
    heldUsage_ = 0;
    portEXIT_CRITICAL(&g_mux);
    memset(prevKeys_, 0, sizeof(prevKeys_));
    g_lastGenericCode = 0;
  }

  // Silence ends a raw button only on a remote that was streaming it; one that
  // sends a frame per edge is still holding and sends its own release. The
  // release is dated by the last frame, which is when the button came up.
  portENTER_CRITICAL(&g_mux);
  if (rawCode_ != 0 && rawReports_ > 1 && (millis() - g_lastReportMs) > kReleaseTimeoutMs) {
    if (!rawDropped_) pushRawLocked(rawCode_, false, g_lastReportMs, 0, 0, 0);
    rawDropped_ = false;
    rawCode_ = 0;
    rawReports_ = 0;
  }
  portEXIT_CRITICAL(&g_mux);

  // Auto-reconnect to a bonded HID peripheral: the selected one while a plan is
  // armed (never another bond), otherwise each bond in turn.
  char selected[sizeof(g_selectedAddr)] = {0};
  portENTER_CRITICAL(&g_mux);
  const bool planArmed = g_selectedAddr[0] != '\0';
  if (planArmed && g_selectedAttemptsLeft > 0 && millis() - g_selectedStartedMs < kSelectedReconnectWindowMs &&
      millis() - g_lastReconnectMs >= kReconnectBackoffMs && !connected_ && !g_connecting && !scanning_) {
    memcpy(selected, g_selectedAddr, sizeof(selected));
    --g_selectedAttemptsLeft;
  }
  portEXIT_CRITICAL(&g_mux);

  // Bonded pick: scan, and connect to the bond heard advertising.
  char picked[sizeof(g_candidateAddr)] = {0};
  bool scanForBonds = false;
  portENTER_CRITICAL(&g_mux);
  const bool bondedArmed = g_bondedArmed;
  if (bondedArmed && !connected_ && !g_connecting) {
    const uint32_t now = millis();
    if (g_candidateAddr[0] && (g_candidateReady || now - g_candidateFirstMs >= kPriorityWaitMs)) {
      g_candidateReady = true;
      if (now - g_lastReconnectMs >= kReconnectBackoffMs) memcpy(picked, g_candidateAddr, sizeof(picked));
    }
    scanForBonds = !picked[0] && !scanning_ && now - g_bondedStartedMs < kSelectedReconnectWindowMs;
  }
  portEXIT_CRITICAL(&g_mux);
  if (picked[0]) {
    stopScan();
    connectInternal(picked, /*explicitRequest=*/false);
  } else if (scanForBonds && begun_ && clientReleased()) {  // end() may run on another task
    NimBLEScan* scan = NimBLEDevice::getScan();
    // Passive: an active scan holds a scannable bond's report until its scan response.
    scan->setActiveScan(false);
    scan->setDuplicateFilter(1);  // one report per advertiser and 5 s scan
    scan->start(kBondedScanMs, false, true);
    scanning_ = scan->isScanning();
  }

  if (selected[0]) {
    connectInternal(selected, /*explicitRequest=*/false);
  } else if (!planArmed && !bondedArmed && g_autoReconnect && !connected_ && !g_connecting && !scanning_ &&
             bondCount_ > 0) {
    const uint32_t now = millis();
    if (now - g_lastReconnectMs > kReconnectBackoffMs) {
      g_lastReconnectMs = now;
      g_reconnectIdx = static_cast<uint8_t>(g_reconnectIdx % bondCount_);
      connect(bonds_[g_reconnectIdx].addr);
      g_reconnectIdx++;
    }
  }
}

// --- Discovery ---------------------------------------------------------------
void BleKeyboardHost::startScan(uint32_t ms) {
  if (!begun_) {
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.println("[BleHid] scan start ignored: host not begun");
#endif
    return;
  }
  if (g_connecting && g_client) {
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.println("[BleHid] scan start: cancelling pending reconnect");
#endif
    const uint32_t waitStart = millis();
    while (g_connecting && millis() - waitStart < kTeardownConnectWaitMs) {
      cancelPendingConnect();
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  portENTER_CRITICAL(&g_mux);
  cancelBondedLocked();  // a scan the app starts is a pairing scan
  deviceCount_ = 0;
  portEXIT_CRITICAL(&g_mux);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->clearResults();
  scan->setActiveScan(true);    // names ride in scan responses
  scan->setDuplicateFilter(0);  // and in later reports of the same advertiser
#if FREEINK_BLE_HID_SCAN_DEBUG
  Serial.printf("[BleHid] scan start: active, continuous, %lu ms wasScanning=%d\n", static_cast<unsigned long>(ms),
                scan->isScanning() ? 1 : 0);
#endif
  const bool started = scan->start(ms, false, true);
  scanning_ = scan->isScanning();
#if FREEINK_BLE_HID_SCAN_DEBUG
  Serial.printf("[BleHid] scan start result: started=%d scanning=%d\n", started ? 1 : 0, scanning_ ? 1 : 0);
#endif
}

void BleKeyboardHost::stopScan() {
  if (!begun_) return;
#if FREEINK_BLE_HID_SCAN_DEBUG
  Serial.printf("[BleHid] scan stop: wasScanning=%d devices=%u\n", NimBLEDevice::getScan()->isScanning() ? 1 : 0,
                deviceCount_);
#endif
  NimBLEDevice::getScan()->stop();
  scanning_ = false;
}

const DiscoveredDevice& BleKeyboardHost::device(uint8_t i) const {
  static const DiscoveredDevice kEmpty{};
  return i < deviceCount_ ? devices_[i] : kEmpty;
}

void BleKeyboardHost::releaseScanResults() {
  if (begun_) NimBLEDevice::getScan()->clearResults();
  portENTER_CRITICAL(&g_mux);
  deviceCount_ = 0;
  portEXIT_CRITICAL(&g_mux);
}

// --- Connection --------------------------------------------------------------
bool BleKeyboardHost::connect(const char* addr) { return connectInternal(addr, /*explicitRequest=*/true); }

bool BleKeyboardHost::armSelectedPeerReconnect(const char* addr) {
  if (!addr || strnlen(addr, sizeof(g_selectedAddr)) != sizeof(g_selectedAddr) - 1) return false;
  if (!begun_ || !g_connTask || connected_ || g_connecting) return false;
  bool bonded = false;
  for (uint8_t i = 0; i < bondCount_; ++i) {
    if (strncmp(bonds_[i].addr, addr, sizeof(bonds_[i].addr)) == 0) bonded = true;
  }
  if (!bonded) return false;
  // The bonded pick's own scan does not refuse the plan: it gives way. The app's scan does.
  portENTER_CRITICAL(&g_mux);
  const bool bondedScan = g_bondedArmed;
  cancelBondedLocked();
  portEXIT_CRITICAL(&g_mux);
  if (bondedScan) stopScan();
  if (scanning_) return false;
  portENTER_CRITICAL(&g_mux);
  const bool armed = g_selectedAddr[0] == '\0';
  if (armed) {
    cancelBondedLocked();
    memcpy(g_selectedAddr, addr, sizeof(g_selectedAddr));
    g_selectedAttemptsLeft = kSelectedReconnectAttempts;
    g_selectedStartedMs = millis();
    g_lastReconnectMs = g_selectedStartedMs - kReconnectBackoffMs;  // the first poll tries at once
  }
  portEXIT_CRITICAL(&g_mux);
  return armed;
}

bool BleKeyboardHost::armBondedReconnect(const PickPolicy policy, const char* priorityAddr) {
  if (!begun_ || !g_connTask) return false;
  portENTER_CRITICAL(&g_mux);
  bool bondedPriority = false;
  for (uint8_t i = 0; policy == PickPolicy::Priority && priorityAddr && i < bondCount_; ++i) {
    if (strncmp(bonds_[i].addr, priorityAddr, sizeof(bonds_[i].addr)) == 0) bondedPriority = true;
  }
  if (bondCount_ == 0 || (scanning_ && !g_bondedArmed)) {  // nothing to find, or the app's own scan
    portEXIT_CRITICAL(&g_mux);
    return false;
  }
  clearReconnectPlansLocked();
  g_bondedArmed = true;
  g_pickPolicy = bondedPriority ? PickPolicy::Priority : PickPolicy::First;
  if (bondedPriority) memcpy(g_priorityAddr, priorityAddr, sizeof(g_priorityAddr) - 1);
  openBondedWindowLocked();
  g_lastReconnectMs = g_bondedStartedMs - kReconnectBackoffMs;  // the first pick connects at once
  portEXIT_CRITICAL(&g_mux);
  return true;
}

bool BleKeyboardHost::connectInternal(const char* addr, const bool explicitRequest) {
  if (!begun_ || !addr || g_connecting) return false;
  if (explicitRequest) {
    portENTER_CRITICAL(&g_mux);
    clearReconnectPlansLocked();
    g_autoReconnect = true;
    portEXIT_CRITICAL(&g_mux);
  }

  uint8_t type = 0;
  bool knownType = false;
  for (uint8_t i = 0; i < deviceCount_; ++i) {
    if (strncmp(devices_[i].addr, addr, sizeof(devices_[i].addr)) == 0) {
      type = devices_[i].addrType;
      knownType = true;
      break;
    }
  }
  for (uint8_t i = 0; i < bondCount_; ++i) {
    if (strncmp(bonds_[i].addr, addr, sizeof(bonds_[i].addr)) == 0) {
      type = bonds_[i].addrType;
      knownType = true;
      break;
    }
  }

  if (!explicitRequest) {
    // poll() copied the pick outside the lock: a drop, a failure or forget() may have cleared it since.
    portENTER_CRITICAL(&g_mux);
    const bool bonded = g_bondedArmed;
    const bool stillPicked =
        bonded && g_candidateReady && !connected_ && strncmp(g_candidateAddr, addr, sizeof(g_candidateAddr)) == 0;
    if (stillPicked) {
      type = g_candidateType;
      knownType = true;
    }
    portEXIT_CRITICAL(&g_mux);
    if (bonded && !stillPicked) return false;
  }

  strncpy(g_targetAddr, addr, sizeof(g_targetAddr) - 1);
  g_targetAddr[sizeof(g_targetAddr) - 1] = '\0';
  g_targetType = type;
  g_targetTryAltType = !knownType;
  g_connecting = true;
  connecting_ = true;
  connectFailed_ = false;
  pairingPasskeyReady_ = false;
  connectFailure_[0] = '\0';
  if (g_connTask) xTaskNotifyGive(g_connTask);
  return true;
}

void BleKeyboardHost::disconnect() {
  g_autoReconnect = false;
  portENTER_CRITICAL(&g_mux);
  const bool bondedScan = g_bondedArmed;
  clearReconnectPlansLocked();
  portEXIT_CRITICAL(&g_mux);
  if (bondedScan) stopScan();
  if (g_client && g_client->isConnected()) g_client->disconnect();
}

// --- Pairings ----------------------------------------------------------------
const PairedHidDevice& BleKeyboardHost::paired(uint8_t i) const {
  static const PairedHidDevice kEmpty{};
  return i < bondCount_ ? bonds_[i] : kEmpty;
}

void BleKeyboardHost::forget(const char* addr) {
  if (!addr) return;
  for (uint8_t i = 0; i < bondCount_; ++i) {
    if (strncmp(bonds_[i].addr, addr, sizeof(bonds_[i].addr)) != 0) continue;
    NimBLEDevice::deleteBond(NimBLEAddress(std::string(bonds_[i].addr), bonds_[i].addrType));
    // Under the lock, for the bonded scan's reads on the NimBLE task. onLinkUp() adds
    // bonds outside it; the scan skips the table while a connect is in progress.
    portENTER_CRITICAL(&g_mux);
    if (strncmp(g_selectedAddr, addr, sizeof(g_selectedAddr)) == 0) clearReconnectPlansLocked();
    if (strncmp(g_candidateAddr, addr, sizeof(g_candidateAddr)) == 0) clearCandidateLocked();
    if (strncmp(g_priorityAddr, addr, sizeof(g_priorityAddr)) == 0) {
      g_priorityAddr[0] = '\0';
      g_pickPolicy = PickPolicy::First;
    }
    for (uint8_t j = i + 1; j < bondCount_; ++j) bonds_[j - 1] = bonds_[j];
    bondCount_--;
    portEXIT_CRITICAL(&g_mux);
    persistBonds();
    return;
  }
}

// --- Translated input --------------------------------------------------------
bool BleKeyboardHost::popKey(KeyEvent& out) {
  bool got = false;
  portENTER_CRITICAL(&g_mux);
  if (ringHead_ != ringTail_) {
    out = ring_[ringTail_];
    ringTail_ = static_cast<uint8_t>((ringTail_ + 1) % kKeyQueueLen);
    got = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return got;
}

bool BleKeyboardHost::popRawButton(RawButtonEvent& out) {
  bool got = false;
  portENTER_CRITICAL(&g_mux);
  if (rawHead_ != rawTail_) {
    out = rawRing_[rawTail_];
    rawTail_ = static_cast<uint8_t>((rawTail_ + 1) % kRawQueueLen);
    got = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return got;
}

bool BleKeyboardHost::pushRawLocked(const uint32_t code, const bool pressed, const uint32_t atMs,
                                    const uint8_t keycode, const uint8_t mods, const uint8_t keep,
                                    const bool wasRest) {
  const uint8_t used = static_cast<uint8_t>((rawHead_ - rawTail_ + kRawQueueLen) % kRawQueueLen);
  if (kRawQueueLen - 1 - used <= keep) return false;  // drop rather than block, like the key ring
  RawButtonEvent& e = rawRing_[rawHead_];
  e.value = static_cast<uint8_t>(code);
  e.byteIndex = static_cast<uint8_t>(code >> 8);
  e.reportId = static_cast<uint8_t>(code >> 16);
  e.pressed = pressed;
  e.keycode = keycode;
  e.mods = mods;
  e.wasRest = wasRest;
  e.atMs = atMs;
  rawHead_ = static_cast<uint8_t>((rawHead_ + 1) % kRawQueueLen);
  return true;
}

void BleKeyboardHost::enqueue(const KeyEvent& ev) {
  portENTER_CRITICAL(&g_mux);
  const uint8_t next = static_cast<uint8_t>((ringHead_ + 1) % kKeyQueueLen);
  if (next != ringTail_) {  // drop on overflow rather than block
    ring_[ringHead_] = ev;
    ringHead_ = next;
  }
  portEXIT_CRITICAL(&g_mux);
}

void BleKeyboardHost::emitUsage(uint8_t usage, uint8_t mods) {
  if (usage == 0) return;
  char ch;
  SpecialKey special;
  // Best-effort translation: known keyboard usages get a char / SpecialKey. Unknown
  // codes (page-turner consumer codes, vendor layouts) still surface with keycode set
  // so a capture-then-assign UI can bind them — hidTranslate already zeroes ch/special.
  hidTranslate(usage, mods, ch, special);
  KeyEvent ev;
  ev.ch = ch;
  ev.keycode = usage;
  ev.mods = mods;
  ev.special = special;
  ev.pressed = true;
  if (framePressUsage_ == 0) {  // the key a raw press of this frame carries
    framePressUsage_ = usage;
    framePressMods_ = mods;
  }
  enqueue(ev);
}

// --- Internal hooks from the BLE backend -------------------------------------
void BleKeyboardHost::onReportIngest(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  const uint32_t now = millis();
  const uint32_t lastMs = g_lastReportMs;
  g_lastReportMs = now;  // freshness for the stale-release timeout in poll()
  framePressUsage_ = 0;
  framePressMods_ = 0;
  frameAxisPad_ = false;

#if FREEINK_BLE_HID_REPORT_DEBUG
  {
    char buf[64];
    size_t off = 0;
    const size_t dump = len < 16 ? len : 16;
    for (size_t i = 0; i < dump && off + 3 < sizeof(buf); ++i) {
      off += snprintf(buf + off, sizeof(buf) - off, "%02X ", data[i]);
    }
    Serial.printf("[BleHid] report len=%u %s\n", (unsigned)len, buf);
  }
#endif

  // Normalize: strip a leading report id (len 9). Boot/report-protocol keyboard
  // reports are [mod][reserved][k0..k5] (8 bytes) or a compact [mod][k0..k5] (7).
  const uint8_t* p = data;
  size_t n = len;
  if (n == 9) {
    p += 1;
    n -= 1;
  }

  // --- Standard keyboard report path -----------------------------------------
  uint8_t mod = 0;
  uint8_t keys[6] = {0};
  bool keyboardShaped = false;
  if (isBareKeys(g_notifyReportId, n)) {
    // Every slot from byte 0; the first six keys are kept. Keyboard usages end at
    // 0xFF, so a 16-bit slot whose high byte is set holds no key.
    const size_t w = g_bareKeys.slotBytes;
    size_t k = 0;
    for (size_t i = 0; i + w <= n && k < 6; i += w) {
      if (p[i] && (w == 1 || !p[i + 1])) keys[k++] = p[i];
    }
    keyboardShaped = true;
  } else if (n >= 8) {
    mod = p[0];
    for (int i = 0; i < 6; ++i) keys[i] = p[2 + i];
    keyboardShaped = true;
  } else if (n == 7) {
    mod = p[0];
    for (int i = 0; i < 6; ++i) keys[i] = p[1 + i];
    keyboardShaped = true;
  }

  bool emittedKb = false;
  bool keyHeld = false;  // the key slots still hold a key from an earlier report
  if (keyboardShaped) {
    // Emit a press for every key newly present versus the previous report.
    for (int i = 0; i < 6; ++i) {
      const uint8_t k = keys[i];
      if (k == 0 || k == 0x01 /*ErrorRollOver*/) continue;
      bool wasDown = false;
      for (int j = 0; j < 6; ++j) {
        if (prevKeys_[j] == k) {
          wasDown = true;
          break;
        }
      }
      if (!wasDown) {
        emitUsage(k, mod);
        emittedKb = true;
      }
    }

    // Track the last held key for auto-repeat.
    uint8_t cur = 0;
    for (int i = 0; i < 6; ++i) {
      if (keys[i] != 0 && keys[i] != 0x01) cur = keys[i];
    }
    keyHeld = cur != 0;
    portENTER_CRITICAL(&g_mux);
    if (cur == 0) {
      heldUsage_ = 0;
    } else if (cur != heldUsage_) {
      heldUsage_ = cur;
      heldMods_ = mod;
      heldSince_ = millis();
      lastRepeat_ = millis();
    }
    portEXIT_CRITICAL(&g_mux);

    memcpy(prevKeys_, keys, sizeof(prevKeys_));
  }

  // --- Generic fallback for non-keyboard remotes -----------------------------
  // Many page turners are not boot keyboards: they emit on the Consumer Control
  // page or place the code at a non-standard byte. When the keyboard slots produced
  // nothing and the device doesn't look like a pure keyboard, scan the report for a
  // representative code and surface it (edge-detected so one press == one event).
  // A report whose key slots still hold a key belongs to the keyboard path: a
  // remote that streams a held key repeats it, and each repeat would read here
  // as a new press.
  const bool tryGeneric = !emittedKb && !keyHeld && (g_hasConsumerPage || !g_hasKeyboardPage || n < 7);
  if (tryGeneric) {
    size_t codeIdx = 0;
    const uint8_t code = extractPrimaryCode(p, n, &codeIdx);
    // Gamepad-style modes keep constant bits in the button byte and only clear
    // the pressed bit on release (ino gamebrick mode T: 0x13 pressed -> 0x12
    // released, bit0 is the button). "code changed" alone emits a phantom
    // second press on that release frame. A press must SET at least one bit
    // the previous code didn't have; a bit-subset of the previous code is a
    // (partial) release, never a new press. Value-coded remotes are unaffected
    // in practice: their release frame is all-zero (code 0), and the 150 ms
    // stale-release timeout in poll() zeroes g_lastGenericCode between any two
    // human-separated presses.
    const bool pressEdge = code != 0 && code != g_lastGenericCode && (code & ~g_lastGenericCode) != 0;
    const bool releaseEdge = code != 0 && code != g_lastGenericCode && (code & ~g_lastGenericCode) == 0;
    if (codeIdx == 0 && n >= 5) {
      frameAxisPad_ = true;
      // Gamepad/axis-pair mode (ino gamebrick T): byte 0 is a status byte whose
      // bit0 is "pressed" (0x13 press -> 0x12 release), bytes 1-4 are two 16-bit
      // LE axes centered at ~2000. Every button raises the same pressed bit; the
      // identity lives in where the axes sit. The FIRST press frame is not
      // trustworthy (a direction press starts near center and ramps toward its
      // extreme over several frames), but the release frame still carries the
      // final held values ("12 D0 07 84 03" = up, "12 D0 07 10 0E" = down,
      // "12 B8 0B D0 07" = A, "12 B8 0B C4 09" = B). So classify on the release
      // edge: quantize each axis into low/center/high and emit a synthetic code
      // from the zone pair. Both-axes-centered carries no identity (bare
      // release) and emits nothing.
      if (releaseEdge) {
        uint16_t axis1, axis2;  // memcpy: p may be unaligned (RISC-V faults on unaligned loads)
        memcpy(&axis1, p + 1, sizeof(axis1));
        memcpy(&axis2, p + 3, sizeof(axis2));
        constexpr uint16_t kAxisCenter = 2000;
        constexpr uint16_t kAxisDeadband = 300;  // B rests at center+500; center noise stays inside +/-300
        const auto zone = [](uint16_t v) -> uint8_t {
          if (v < kAxisCenter - kAxisDeadband) return 0;
          if (v > kAxisCenter + kAxisDeadband) return 2;
          return 1;
        };
        const uint8_t zonePair = zone(axis1) * 3 + zone(axis2);
        if (zonePair != 4) {  // 4 = both centered: nothing to bind
          emitUsage(0x40 + zonePair, 0);
        }
      }
    } else if (pressEdge) {
      // Value-coded remotes (e.g. "01 00 00" press, all-zero release): the code
      // byte IS the identity; emit on the press edge as before.
      emitUsage(code, 0);
    }
    g_lastGenericCode = code;
  } else if (emittedKb) {
    g_lastGenericCode = 0;
  }
  ingestRawEdge(data, len, now, lastMs);
}

void BleKeyboardHost::ingestRawEdge(const uint8_t* data, size_t len, const uint32_t now, const uint32_t lastMs) {
  // Each report id is read against its rest frame. A frame that only clears bits
  // of the one before is a release and becomes the rest; when the first frame of
  // a report is followed by one that only adds bits, that first frame was the rest.
  const RawFrame f = rawFrameOf(data, len);
  uint8_t slot = 0;
  while (slot < rawRestCount_ && rawRest_[slot].id != f.id) ++slot;
  if (slot == rawRestCount_) {
    if (rawRestCount_ < 4) ++rawRestCount_;
    slot = rawRestCount_ - 1;
    rawRest_[slot] = RawRest{};
    rawRest_[slot].id = f.id;
  }
  RawRest& r = rawRest_[slot];
  uint8_t adds = 0;
  uint8_t clears = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    adds |= f.bytes[i] & ~r.prev[i];
    clears |= r.prev[i] & ~f.bytes[i];
  }
  if (clears && !adds) {
    memcpy(r.rest, f.bytes, sizeof(r.rest));
    r.known = true;
  }
  if (adds && !clears && r.frames == 1) {
    memcpy(r.rest, r.prev, sizeof(r.rest));
    r.known = true;
  }
  memcpy(r.prev, f.bytes, sizeof(r.prev));
  if (r.frames < 2) ++r.frames;
  // An axis-pair gamepad names its button only through the zone the decoder reads;
  // its axis bytes ramp while held and would read as a new button on every frame.
  const uint32_t code = frameAxisPad_ ? 0 : rawButtonCode(f, r.rest);

  portENTER_CRITICAL(&g_mux);
  bool pressTried = false;
  if (code != rawCode_) {
    // Release before press, so a swap of one button for another reads in that
    // order. A button held until now that was read against the zero guess, whose
    // byte the rest learned since carries, was the rest frame, not a button.
    bool heldWasRest = false;
    for (uint8_t i = 0; rawCode_ != 0 && rawGuessed_ && i < rawRestCount_; ++i) {
      if (rawRest_[i].id != static_cast<uint8_t>(rawCode_ >> 16)) continue;
      const uint8_t at = static_cast<uint8_t>(rawCode_ >> 8);
      heldWasRest = at < 8 && rawRest_[i].known && rawRest_[i].rest[at] == static_cast<uint8_t>(rawCode_);
      break;
    }
    if (rawCode_ != 0 && !rawDropped_) pushRawLocked(rawCode_, false, now, 0, 0, 0, heldWasRest);
    pressTried = code != 0;
    rawDropped_ = pressTried && !pushRawLocked(code, true, now, framePressUsage_, framePressMods_, 1);
    rawCode_ = code;
    rawGuessed_ = !r.known;
    rawReports_ = 1;
  } else if (code != 0) {
    if (now - lastMs > kReleaseTimeoutMs) {
      // The same frame after silence, with no release frame between: a remote that
      // only reports presses. Each such frame is a new press; the one before it is
      // settled first so every press keeps exactly one release.
      if (!rawDropped_) pushRawLocked(code, false, lastMs, 0, 0, 0);
      pressTried = true;
      rawDropped_ = !pushRawLocked(code, true, now, framePressUsage_, framePressMods_, 1);
      rawReports_ = 1;
    } else if (rawReports_ < 255) {
      ++rawReports_;
    }
  }
  if (framePressUsage_ != 0 && !pressTried) {
    // The decoder read a key that no byte edge carries (the zone of an axis gamepad,
    // a key added while another is held). It gets an identity of its own, byte index
    // 0xFF (never a payload byte), as one tap.
    const uint8_t open = rawCode_ != 0 && !rawDropped_ ? 1 : 0;
    const uint32_t k = 0xFFFF00u | framePressUsage_;
    if (pushRawLocked(k, true, now, framePressUsage_, framePressMods_, static_cast<uint8_t>(1 + open))) {
      pushRawLocked(k, false, now, 0, 0, open);
    }
  }
  portEXIT_CRITICAL(&g_mux);
}

void BleKeyboardHost::onScanResultIngest(const char* addr, const char* name, int rssi, uint8_t type, bool hid,
                                         bool connectable) {
  if (!addr) return;
  portENTER_CRITICAL(&g_mux);
  if (g_bondedArmed) {
    // Only a bond that can take a connection counts; nothing enters the device list.
    for (uint8_t i = 0; connectable && !connected_ && !g_connecting && i < bondCount_; ++i) {
      if (strncmp(bonds_[i].addr, addr, sizeof(bonds_[i].addr)) != 0) continue;
      const bool failedOne = g_failedAddr[0] && strncmp(g_failedAddr, addr, sizeof(g_failedAddr)) == 0;
      if (g_failedAddr[0] && !failedOne) g_otherBondHeard = true;
      if (failedOne && g_failedCount >= kBondedFailLimit && g_otherBondHeard) break;
      const bool priority =
          g_pickPolicy == PickPolicy::Priority && strncmp(g_priorityAddr, addr, sizeof(g_priorityAddr)) == 0;
      if (!g_candidateAddr[0] || (priority && !g_candidateReady)) {
        memcpy(g_candidateAddr, bonds_[i].addr, sizeof(g_candidateAddr));
        g_candidateType = type;
        g_candidateFirstMs = millis();
        g_candidateReady = g_pickPolicy == PickPolicy::First || priority;
      }
      break;
    }
    portEXIT_CRITICAL(&g_mux);
    return;
  }
  portEXIT_CRITICAL(&g_mux);
  if (!name) return;  // a bonded-scan report that arrived after the pick was cancelled
  // A "real" name (not the address fallback) should never be downgraded back to
  // the address on a later primary-only advertisement.
  const bool realName = name && name[0] && strcmp(name, addr) != 0;
#if !FREEINK_BLE_HID_SHOW_UNNAMED_DEVICES
  if (!realName && !hid) {
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.printf("[BleHid] scan filtered: %s name='%s' rssi=%d hid=%d conn=%d type=%u\n", addr, name ? name : "",
                  rssi, hid ? 1 : 0, connectable ? 1 : 0, type);
#endif
    return;
  }
#endif
  portENTER_CRITICAL(&g_mux);

  // Upsert by address.
  uint8_t idx = deviceCount_;
  for (uint8_t i = 0; i < deviceCount_; ++i) {
    if (strncmp(devices_[i].addr, addr, sizeof(devices_[i].addr)) == 0) {
      idx = i;
      break;
    }
  }

  if (idx != deviceCount_) {
    // Known device: refresh RSSI; upgrade to a real name / HID flag if we just
    // learned one (these can arrive in a later advertisement or scan response).
    DiscoveredDevice& d = devices_[idx];
    d.rssi = rssi;
    if (hid) d.hid = true;
    if (connectable) d.connectable = true;
    if (realName) {
      strncpy(d.name, name, sizeof(d.name) - 1);
      d.name[sizeof(d.name) - 1] = '\0';
      d.hasName = true;
    }
    portEXIT_CRITICAL(&g_mux);
    return;
  }

  // New device.
  if (deviceCount_ < kMaxDiscovered) {
    idx = deviceCount_++;
  } else {
    // Full: evict the weakest-RSSI entry if this one is stronger, so nearby
    // input peripherals are not crowded out by distant beacons.
    uint8_t weakest = 0;
    for (uint8_t i = 1; i < deviceCount_; ++i) {
      if (devices_[i].rssi < devices_[weakest].rssi) weakest = i;
    }
    if (rssi <= devices_[weakest].rssi) {
      portEXIT_CRITICAL(&g_mux);
      return;
    }
    idx = weakest;
  }

  DiscoveredDevice& d = devices_[idx];
  strncpy(d.addr, addr, sizeof(d.addr) - 1);
  d.addr[sizeof(d.addr) - 1] = '\0';
  strncpy(d.name, name && name[0] ? name : addr, sizeof(d.name) - 1);
  d.name[sizeof(d.name) - 1] = '\0';
  d.rssi = rssi;
  d.addrType = type;
  d.hasName = realName;
  d.hid = hid;
  d.connectable = connectable;
#if FREEINK_BLE_HID_SCAN_DEBUG
  Serial.printf("[BleHid] scan accepted: idx=%u count=%u addr=%s name='%s' rssi=%d hid=%d conn=%d type=%u\n", idx,
                deviceCount_, d.addr, d.name, d.rssi, d.hid ? 1 : 0, d.connectable ? 1 : 0, d.addrType);
#endif
  portEXIT_CRITICAL(&g_mux);
}

void BleKeyboardHost::onLinkUp(const char* addr, const char* name, uint8_t type) {
  // Resolve a friendly name from the scan or bond lists if the caller has none.
  const char* resolved = (name && name[0] && (!addr || strcmp(name, addr) != 0)) ? name : nullptr;
  if (!resolved && addr) {
    for (uint8_t i = 0; i < deviceCount_; ++i) {
      if (strncmp(devices_[i].addr, addr, sizeof(devices_[i].addr)) == 0 && devices_[i].hasName &&
          devices_[i].name[0] && strcmp(devices_[i].name, addr) != 0) {
        resolved = devices_[i].name;
        break;
      }
    }
  }
  if (!resolved && addr) {
    for (uint8_t i = 0; i < bondCount_; ++i) {
      if (strncmp(bonds_[i].addr, addr, sizeof(bonds_[i].addr)) == 0 && bonds_[i].name[0]) {
        resolved = bonds_[i].name;
        break;
      }
    }
  }
  connAddr_[0] = '\0';
  if (addr) {
    strncpy(connAddr_, addr, sizeof(connAddr_) - 1);
    connAddr_[sizeof(connAddr_) - 1] = '\0';
  }
  connName_[0] = '\0';
  if (resolved) {
    strncpy(connName_, resolved, sizeof(connName_) - 1);
    connName_[sizeof(connName_) - 1] = '\0';
  } else if (addr) {
    strncpy(connName_, addr, sizeof(connName_) - 1);
    connName_[sizeof(connName_) - 1] = '\0';
  }

  // Persist the pairing if new.
  if (addr) {
    bool known = false;
    for (uint8_t i = 0; i < bondCount_; ++i) {
      if (strncmp(bonds_[i].addr, addr, sizeof(bonds_[i].addr)) == 0) {
        if (resolved && strcmp(bonds_[i].name, resolved) != 0) {
          strncpy(bonds_[i].name, resolved, sizeof(bonds_[i].name) - 1);
          bonds_[i].name[sizeof(bonds_[i].name) - 1] = '\0';
          bonds_[i].addrType = type;
          persistBonds();
        }
        known = true;
        break;
      }
    }
    if (!known && bondCount_ < kMaxBonds) {
      PairedHidDevice& b = bonds_[bondCount_++];
      strncpy(b.addr, addr, sizeof(b.addr) - 1);
      b.addr[sizeof(b.addr) - 1] = '\0';
      b.name[0] = '\0';
      if (resolved) {
        strncpy(b.name, resolved, sizeof(b.name) - 1);
        b.name[sizeof(b.name) - 1] = '\0';
      }
      b.addrType = type;
      persistBonds();
    }
  }

  g_reconnectIdx = 0;
  connecting_ = false;
  connected_ = true;
}

void BleKeyboardHost::onLinkDown() {
  portENTER_CRITICAL(&g_mux);
  // The selected peer's link dropped on its own (disconnect() clears the plan
  // first): a fresh bounded plan brings it back.
  if (connected_ && g_selectedAddr[0] && strncmp(g_selectedAddr, connAddr_, sizeof(g_selectedAddr)) == 0) {
    g_selectedAttemptsLeft = kSelectedReconnectAttempts;
    g_selectedStartedMs = millis();
    g_lastReconnectMs = g_selectedStartedMs;
  }
  if (g_bondedArmed) {
    clearCandidateLocked();
    if (connected_) openBondedWindowLocked();  // a drop opens a fresh scan window
  }
  connected_ = false;
  connecting_ = false;
  connAddr_[0] = '\0';
  heldUsage_ = 0;
  // No button is held across a link drop, and the next remote may rest on other bytes.
  rawCode_ = 0;
  rawReports_ = 0;
  rawDropped_ = false;
  portEXIT_CRITICAL(&g_mux);
  rawRestCount_ = 0;
  // A link that drops while a key is down never delivers its release. Forget the
  // key, so its first press on the next link reads as a press.
  memset(prevKeys_, 0, sizeof(prevKeys_));
  g_lastGenericCode = 0;
}

void BleKeyboardHost::onConnectFailed(const char* reason) {
  connected_ = false;
  connecting_ = false;
  portENTER_CRITICAL(&g_mux);
  strncpy(connectFailure_, reason && reason[0] ? reason : "Connection failed", sizeof(connectFailure_) - 1);
  connectFailure_[sizeof(connectFailure_) - 1] = '\0';
  connectFailed_ = true;
  heldUsage_ = 0;
  if (g_selectedAddr[0] || g_bondedArmed) g_lastReconnectMs = millis();  // the next attempt waits from the failure
  if (g_bondedArmed && g_failedAfterLink) {
    if (strncmp(g_failedAddr, g_targetAddr, sizeof(g_failedAddr)) == 0) {
      ++g_failedCount;
    } else {
      memcpy(g_failedAddr, g_targetAddr, sizeof(g_failedAddr));
      g_failedCount = 1;
    }
  }
  if (g_bondedArmed) clearCandidateLocked();
  portEXIT_CRITICAL(&g_mux);
}

bool BleKeyboardHost::takeConnectFailure(char* out, size_t outLen) {
  if (!out || outLen == 0) return false;
  bool failed = false;
  portENTER_CRITICAL(&g_mux);
  if (connectFailed_) {
    strncpy(out, connectFailure_, outLen - 1);
    out[outLen - 1] = '\0';
    connectFailed_ = false;
    failed = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return failed;
}

void BleKeyboardHost::onPairingPasskey(uint32_t passkey) {
  portENTER_CRITICAL(&g_mux);
  pairingPasskey_ = passkey;
  pairingPasskeyReady_ = true;
  portEXIT_CRITICAL(&g_mux);
}

bool BleKeyboardHost::takePairingPasskey(uint32_t& out) {
  bool ready = false;
  portENTER_CRITICAL(&g_mux);
  if (pairingPasskeyReady_) {
    out = pairingPasskey_;
    pairingPasskeyReady_ = false;
    ready = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return ready;
}

// --- NVS persistence ---------------------------------------------------------
bool BleKeyboardHost::firstSavedBond(PairedHidDevice& out) {
  if (!begun_) loadBonds();  // the table in flash; begin() loads it again
  if (bondCount_ == 0 || bonds_[0].addr[0] == '\0') return false;
  out = bonds_[0];
  return true;
}

void BleKeyboardHost::loadBonds() {
  Preferences p;
  if (!p.begin("freeink-hid", true)) return;
  bondCount_ = p.getUChar("n", 0);
  if (bondCount_ > kMaxBonds) bondCount_ = kMaxBonds;
  if (bondCount_ > 0) p.getBytes("b", bonds_, bondCount_ * sizeof(PairedHidDevice));
  p.end();
  bool cleaned = false;
  for (uint8_t i = 0; i < bondCount_; ++i) {
    if (bonds_[i].name[0] && strcmp(bonds_[i].name, bonds_[i].addr) == 0) {
      bonds_[i].name[0] = '\0';
      cleaned = true;
    }
  }
  if (cleaned) persistBonds();
}

void BleKeyboardHost::persistBonds() {
  Preferences p;
  if (!p.begin("freeink-hid", false)) return;
  p.putUChar("n", bondCount_);
  if (bondCount_ > 0) p.putBytes("b", bonds_, bondCount_ * sizeof(PairedHidDevice));
  p.end();
}

}  // namespace freeink

#else  // !FREEINK_CAP_BLE_HID_HOST — stub bodies, no BLE code linked.

namespace freeink {

bool BleKeyboardHost::begin(const char*) { return false; }
bool BleKeyboardHost::end(uint32_t) { return true; }
bool BleKeyboardHost::isStopping() const { return false; }
void BleKeyboardHost::poll() {}
void BleKeyboardHost::startScan(uint32_t) {}
void BleKeyboardHost::stopScan() {}
const DiscoveredDevice& BleKeyboardHost::device(uint8_t) const {
  static const DiscoveredDevice kEmpty{};
  return kEmpty;
}
void BleKeyboardHost::releaseScanResults() {}
bool BleKeyboardHost::connect(const char*) { return false; }
bool BleKeyboardHost::armSelectedPeerReconnect(const char*) { return false; }
bool BleKeyboardHost::armBondedReconnect(PickPolicy, const char*) { return false; }
void BleKeyboardHost::disconnect() {}
const PairedHidDevice& BleKeyboardHost::paired(uint8_t) const {
  static const PairedHidDevice kEmpty{};
  return kEmpty;
}
void BleKeyboardHost::forget(const char*) {}
bool BleKeyboardHost::popKey(KeyEvent&) { return false; }
bool BleKeyboardHost::popRawButton(RawButtonEvent&) { return false; }
void BleKeyboardHost::onScanResultIngest(const char*, const char*, int, uint8_t, bool, bool) {}
void BleKeyboardHost::onReportIngest(const uint8_t*, size_t) {}
void BleKeyboardHost::onLinkUp(const char*, const char*, uint8_t) {}
void BleKeyboardHost::onLinkDown() {}
void BleKeyboardHost::onConnectFailed(const char*) {}
bool BleKeyboardHost::takeConnectFailure(char*, size_t) { return false; }
void BleKeyboardHost::onPairingPasskey(uint32_t) {}
bool BleKeyboardHost::takePairingPasskey(uint32_t&) { return false; }
void BleKeyboardHost::enqueue(const KeyEvent&) {}
void BleKeyboardHost::emitUsage(uint8_t, uint8_t) {}
void BleKeyboardHost::persistBonds() {}
bool BleKeyboardHost::firstSavedBond(PairedHidDevice&) { return false; }
void BleKeyboardHost::loadBonds() {}

}  // namespace freeink

#endif  // FREEINK_CAP_BLE_HID_HOST
