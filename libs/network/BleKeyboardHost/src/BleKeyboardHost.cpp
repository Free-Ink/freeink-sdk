// FreeInk SDK — BLE HID host implementation.
//
// The real NimBLE central path compiles only under FREEINK_CAP_BLE_HID_HOST; the
// #else branch links stub bodies (and references no BLE code). Flow: scan ->
// connect on a dedicated FreeRTOS task -> Just-Works bonding -> set Report
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
constexpr uint16_t kDescReportReference = 0x2908;

constexpr uint32_t kRepeatDelayMs = 450;
constexpr uint32_t kRepeatIntervalMs = 45;
constexpr uint32_t kReconnectBackoffMs = 4000;
constexpr uint32_t kConnectTimeoutMs = 8000;
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

BleKeyboardHost& self() { return BleKeyboardHost::getInstance(); }

#ifndef FREEINK_BLE_HID_SCAN_DEBUG
#ifdef FREEINK_BLE_KEYBOARD_SCAN_DEBUG
#define FREEINK_BLE_HID_SCAN_DEBUG FREEINK_BLE_KEYBOARD_SCAN_DEBUG
#else
#define FREEINK_BLE_HID_SCAN_DEBUG 0
#endif
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
  loadBonds();
  if (!NimBLEDevice::init(hostName ? hostName : "FreeInk")) return false;

  // Bonding with passkey display support. Keyboards often pair like macOS: the
  // host displays a six-digit passkey and the user types it on the peripheral.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/true, /*sc=*/false);
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
  g_client->setConnectTimeout(kConnectTimeoutMs);
  g_client->setClientCallbacks(&g_clientCb, false);

  xTaskCreate(connTaskFn, "ble-conn", 4096, nullptr, 3, &g_connTask);
  begun_ = true;
  return true;
}

void BleKeyboardHost::poll() {
  if (!begun_) return;

  // Reflect the live scanner state.
  scanning_ = NimBLEDevice::getScan()->isScanning();

  // Key auto-repeat.
  uint8_t usage, mods;
  uint32_t since, last;
  portENTER_CRITICAL(&g_mux);
  usage = heldUsage_;
  mods = heldMods_;
  since = heldSince_;
  last = lastRepeat_;
  portEXIT_CRITICAL(&g_mux);
  if (usage != 0) {
    const uint32_t now = millis();
    if (now - since > kRepeatDelayMs && now - last > kRepeatIntervalMs) {
      portENTER_CRITICAL(&g_mux);
      lastRepeat_ = now;
      portEXIT_CRITICAL(&g_mux);
      emitUsage(usage, mods);
    }
  }

  // Auto-reconnect to a bonded HID peripheral.
  if (!connected_ && !g_connecting && bondCount_ > 0) {
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
  if (!begun_) return;
  portENTER_CRITICAL(&g_mux);
  deviceCount_ = 0;
  portEXIT_CRITICAL(&g_mux);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->clearResults();
#if FREEINK_BLE_HID_SCAN_DEBUG
  Serial.printf("[BleHid] scan start: active, continuous, %lu ms\n", static_cast<unsigned long>(ms));
#endif
  scan->start(ms, false, true);
  scanning_ = true;
}

void BleKeyboardHost::stopScan() {
  if (!begun_) return;
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
  char ch;
  SpecialKey special;
  if (!hidTranslate(usage, mods, ch, special)) return;
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

  // Normalize to [mod][k0..k5]: strip a leading report id (len 9), or handle the
  // compact 7-byte form (no reserved byte); boot/report protocol is 8 bytes.
  const uint8_t* p = data;
  size_t n = len;
  if (n == 9) {
    p += 1;
    n -= 1;
  }
  uint8_t mod = 0;
  uint8_t keys[6] = {0};
  if (n >= 8) {
    mod = p[0];
    for (int i = 0; i < 6; ++i) keys[i] = p[2 + i];
  } else if (n == 7) {
    mod = p[0];
    for (int i = 0; i < 6; ++i) keys[i] = p[1 + i];
  } else {
    return;
  }

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
    if (!wasDown) emitUsage(k, mod);
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

void BleKeyboardHost::onScanResultIngest(const char* addr, const char* name, int rssi, uint8_t type, bool hid,
                                         bool connectable) {
  if (!addr) return;
  // A "real" name (not the address fallback) should never be downgraded back to
  // the address on a later primary-only advertisement.
  const bool realName = name && name[0] && strcmp(name, addr) != 0;
#if !FREEINK_BLE_HID_SHOW_UNNAMED_DEVICES
  if (!realName && !hid) return;
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
