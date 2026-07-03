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

// Page-turner remotes often stream a held key (or omit a clean release frame). If no
// report arrives within this window, treat the key as released so one physical press
// yields one event and the next press re-triggers.
constexpr uint32_t kReleaseTimeoutMs = 150;
constexpr uint32_t kReconnectBackoffMs = 4000;
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint32_t kTeardownConnectWaitMs = kConnectTimeoutMs + 500;
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

uint32_t g_lastReconnectMs = 0;
uint8_t g_reconnectIdx = 0;

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

BleKeyboardHost& self() { return BleKeyboardHost::getInstance(); }

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
uint8_t extractPrimaryCode(const uint8_t* p, size_t n) {
  const size_t lim = n < 8 ? n : 8;
  // NOTE: do NOT skip 0x01 here. In a keyboard report 0x01 is ErrorRollOver, but in
  // a consumer / vendor report it is a valid button code (e.g. a 3-byte page-turner
  // report of "01 00 00" on press, "00 00 00" on release). Only zero means "no code".
  if (g_preferredByteIndex != 0xFF && g_preferredByteIndex < n && p[g_preferredByteIndex] != 0) {
    return p[g_preferredByteIndex];
  }
  for (size_t i = 0; i < lim; ++i) {
    if (p[i] != 0) return p[i];
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

void onHidNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
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
  g_lastGenericCode = 0;
  if (NimBLERemoteCharacteristic* rmap = hid->getCharacteristic(NimBLEUUID(kCharReportMap))) {
    if (rmap->canRead()) {
      NimBLEAttValue v = rmap->readValue();
      parseReportMapHints(v.data(), v.size());
#if FREEINK_BLE_HID_REPORT_DEBUG
      Serial.printf("[BleHid] report map: kbd=%d consumer=%d preferredByte=%d len=%u\n", g_hasKeyboardPage,
                    g_hasConsumerPage, (int)g_preferredByteIndex, (unsigned)v.size());
#endif
    }
  }

  bool subscribed = false;
  const std::vector<NimBLERemoteCharacteristic*>& chars = hid->getCharacteristics(true);
  for (NimBLERemoteCharacteristic* c : chars) {
    if (!c) continue;
    if (c->getUUID() != NimBLEUUID(kCharReport) || !c->canNotify()) continue;
    // Report Reference descriptor (0x2908) byte[1] is the report type: 1=Input.
    bool isInput = true;
    NimBLERemoteDescriptor* ref = c->getDescriptor(NimBLEUUID(kDescReportReference));
    if (ref) {
      NimBLEAttValue v = ref->readValue();
      if (v.size() >= 2 && v[1] != 0x01) isInput = false;
    }
    if (isInput && c->subscribe(true, onHidNotify)) subscribed = true;
  }

  if (!subscribed) {  // fallback: boot keyboard input report
    NimBLERemoteCharacteristic* boot = hid->getCharacteristic(NimBLEUUID(kCharBootKbdInput));
    if (boot && boot->canNotify() && boot->subscribe(true, onHidNotify)) subscribed = true;
  }
  return subscribed;
}

bool hasHidService(NimBLEClient* client) {
  return client && client->getService(NimBLEUUID(kHidService)) != nullptr;
}

void doConnect(const char* addrStr, uint8_t type) {
  if (!g_client) {
    self().onConnectFailed("BLE client unavailable");
    return;
  }
  NimBLEDevice::getScan()->stop();
  NimBLEAddress addr(std::string(addrStr), type);
  if (!g_client->connect(addr)) {
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
    if (!g_client->connect(addr)) {
#if FREEINK_BLE_HID_SCAN_DEBUG
      Serial.printf("[BleHid] connect failed: %s type=%u err=%d\n", addrStr, type, g_client->getLastError());
#endif
      self().onConnectFailed(g_client->getLastError() == BLE_HS_ETIMEOUT ? "Connect timeout" : "Connection failed");
      return;
    }
  }
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
  void onDisconnect(NimBLEClient*, int) override { self().onLinkDown(); }
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
  g_client->setClientCallbacks(&g_clientCb, false);

  xTaskCreate(connTaskFn, "ble-conn", 4096, nullptr, 3, &g_connTask);
  begun_ = true;
#if FREEINK_BLE_HID_SCAN_DEBUG
  Serial.println("[BleHid] begin: ok");
#endif
  return true;
}

void BleKeyboardHost::end() {
  if (!begun_) return;
  begun_ = false;

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan && scan->isScanning()) scan->stop();

  // If auto-reconnect is in the middle of g_client->connect(), do not delete the
  // worker or deinit NimBLE under it. Let the blocking connect path unwind first;
  // killing it inside NimBLE leaves host/controller state inconsistent and can
  // crash on Bluetooth-off, sleep, or the next begin().
  if (g_connecting && g_client) g_client->cancelConnect();
  const uint32_t waitStart = millis();
  while (g_connecting && millis() - waitStart < kTeardownConnectWaitMs) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  // Kill the connection worker once it is idle so it can't run doConnect() against
  // the stack while we tear it down.
  if (g_connTask) {
    vTaskDelete(g_connTask);
    g_connTask = nullptr;
  }
  g_connecting = false;

  // Close the link and explicitly delete the client BEFORE deinit. This is critical:
  // NimBLE keeps a fixed-size client array (m_pClients) that survives deinit/init, and
  // deleteClient() DEFERS deletion while the client is CONNECTED/DISCONNECTING (it sets
  // a flag and disconnects async). deinit() then tears down the host before that
  // deferred delete runs, so the slot leaks — and the next begin()'s createClient()
  // returns null forever (BLE can't restart). Waiting for a real disconnect, then
  // deleting while DISCONNECTED, frees the slot for good.
  if (g_client) {
    if (g_client->isConnected()) g_client->disconnect();
    for (int i = 0; i < 60 && g_client->isConnected(); ++i) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(150));  // let DISCONNECTING settle to DISCONNECTED
    NimBLEDevice::deleteClient(g_client);
    g_client = nullptr;
  }

  // Free the NimBLE host + BT controller memory back to the heap. Bonds live in
  // NVS and survive this; begin() re-initializes cleanly. Retry once if the stack
  // didn't fully tear down (stop raced something), so re-init isn't a no-op.
  NimBLEDevice::deinit(true);
  if (NimBLEDevice::isInitialized()) {
    vTaskDelay(pdMS_TO_TICKS(50));
    NimBLEDevice::deinit(true);
  }
  g_connecting = false;
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
  portEXIT_CRITICAL(&g_mux);
}

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

  // Auto-reconnect to a bonded HID peripheral.
  if (!connected_ && !g_connecting && !scanning_ && bondCount_ > 0) {
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
    g_client->cancelConnect();
    const uint32_t waitStart = millis();
    while (g_connecting && millis() - waitStart < kTeardownConnectWaitMs) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  portENTER_CRITICAL(&g_mux);
  deviceCount_ = 0;
  portEXIT_CRITICAL(&g_mux);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->clearResults();
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
bool BleKeyboardHost::connect(const char* addr) {
  if (!begun_ || !addr || g_connecting) return false;

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
    for (uint8_t j = i + 1; j < bondCount_; ++j) bonds_[j - 1] = bonds_[j];
    bondCount_--;
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
  enqueue(ev);
}

// --- Internal hooks from the BLE backend -------------------------------------
void BleKeyboardHost::onReportIngest(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  g_lastReportMs = millis();  // freshness for the stale-release timeout in poll()

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
  if (n >= 8) {
    mod = p[0];
    for (int i = 0; i < 6; ++i) keys[i] = p[2 + i];
    keyboardShaped = true;
  } else if (n == 7) {
    mod = p[0];
    for (int i = 0; i < 6; ++i) keys[i] = p[1 + i];
    keyboardShaped = true;
  }

  bool emittedKb = false;
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
  const bool tryGeneric = !emittedKb && (g_hasConsumerPage || !g_hasKeyboardPage || n < 7);
  if (tryGeneric) {
    const uint8_t code = extractPrimaryCode(p, n);
    if (code != 0 && code != g_lastGenericCode) emitUsage(code, 0);
    g_lastGenericCode = code;
  } else if (emittedKb) {
    g_lastGenericCode = 0;
  }
}

void BleKeyboardHost::onScanResultIngest(const char* addr, const char* name, int rssi, uint8_t type, bool hid,
                                         bool connectable) {
  if (!addr) return;
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
  connected_ = false;
  connecting_ = false;
  portENTER_CRITICAL(&g_mux);
  heldUsage_ = 0;
  portEXIT_CRITICAL(&g_mux);
}

void BleKeyboardHost::onConnectFailed(const char* reason) {
  connected_ = false;
  connecting_ = false;
  portENTER_CRITICAL(&g_mux);
  strncpy(connectFailure_, reason && reason[0] ? reason : "Connection failed", sizeof(connectFailure_) - 1);
  connectFailure_[sizeof(connectFailure_) - 1] = '\0';
  connectFailed_ = true;
  heldUsage_ = 0;
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
void BleKeyboardHost::end() {}
void BleKeyboardHost::poll() {}
void BleKeyboardHost::startScan(uint32_t) {}
void BleKeyboardHost::stopScan() {}
const DiscoveredDevice& BleKeyboardHost::device(uint8_t) const {
  static const DiscoveredDevice kEmpty{};
  return kEmpty;
}
void BleKeyboardHost::releaseScanResults() {}
bool BleKeyboardHost::connect(const char*) { return false; }
void BleKeyboardHost::disconnect() {}
const PairedHidDevice& BleKeyboardHost::paired(uint8_t) const {
  static const PairedHidDevice kEmpty{};
  return kEmpty;
}
void BleKeyboardHost::forget(const char*) {}
bool BleKeyboardHost::popKey(KeyEvent&) { return false; }
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
void BleKeyboardHost::loadBonds() {}

}  // namespace freeink

#endif  // FREEINK_CAP_BLE_HID_HOST
