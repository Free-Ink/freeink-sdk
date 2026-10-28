#pragma once

// Host-test stand-in for NimBLE-Arduino's <NimBLEDevice.h>.
//
// src/BleKeyboardHost.cpp compiles unmodified against this header, so the tests
// drive the real central-role code: GATT walk of the HID service, report decode,
// edge detection, the key ring, auto-reconnect and teardown. Only the calls that
// file makes are modelled (NimBLE-Arduino 2.x names and shapes). There is no
// radio: a link comes up at once unless a test holds the connection task at the
// connect, security or discovery wait, which only a GAP cancel (connect) or a
// disconnect (all three) releases, as in NimBLE.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#define BLE_HS_ETIMEOUT 13
#define BLE_HS_IO_DISPLAY_ONLY 0
#define BLE_SM_PAIR_KEY_DIST_ENC 0x01

struct ble_gap_upd_params;

namespace fakeble {
struct FakeState;
}  // namespace fakeble

typedef void (*NimBLENotifyCallback)(class NimBLERemoteCharacteristic*, uint8_t*, size_t, bool);

class NimBLEUUID {
 public:
  NimBLEUUID() = default;
  explicit NimBLEUUID(uint16_t value16) : value16_(value16) {}
  uint16_t value16() const { return value16_; }
  bool operator==(const NimBLEUUID& other) const { return value16_ == other.value16_; }
  bool operator!=(const NimBLEUUID& other) const { return !(*this == other); }

 private:
  uint16_t value16_ = 0;
};

class NimBLEAddress {
 public:
  NimBLEAddress() = default;
  NimBLEAddress(const std::string& addr, uint8_t type) : addr_(addr), type_(type) {}
  const std::string& toString() const { return addr_; }
  uint8_t getType() const { return type_; }

 private:
  std::string addr_;
  uint8_t type_ = 0;
};

// A view onto the fake peripheral's bytes; the library copies values out at once.
class NimBLEAttValue {
 public:
  NimBLEAttValue() = default;
  NimBLEAttValue(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  uint8_t operator[](size_t index) const { return data_[index]; }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

class NimBLERemoteDescriptor {
 public:
  NimBLEAttValue readValue() const { return NimBLEAttValue(bytes_, len_); }

 private:
  uint8_t bytes_[2] = {0, 0};
  size_t len_ = 0;
  friend struct fakeble::FakeState;
};

class NimBLERemoteCharacteristic {
 public:
  NimBLEUUID getUUID() const { return uuid_; }
  bool canRead() const { return canRead_; }
  bool canWrite() const { return canWrite_; }
  bool canNotify() const { return canNotify_; }
  bool writeValue(const uint8_t* data, size_t length, bool response = false);
  NimBLEAttValue readValue() const { return NimBLEAttValue(value_.data(), value_.size()); }
  NimBLERemoteDescriptor* getDescriptor(const NimBLEUUID& uuid);
  bool subscribe(bool notifications, NimBLENotifyCallback callback, bool response = false);

 private:
  NimBLEUUID uuid_;
  bool canRead_ = false;
  bool canWrite_ = false;
  bool canNotify_ = false;
  std::vector<uint8_t> value_;
  NimBLERemoteDescriptor reportReference_;
  bool hasReportReference_ = false;
  bool subscribed_ = false;
  NimBLENotifyCallback notifyCallback_ = nullptr;
  friend struct fakeble::FakeState;
};

class NimBLERemoteService {
 public:
  const NimBLEUUID& getUUID() const { return uuid_; }
  NimBLERemoteCharacteristic* getCharacteristic(const NimBLEUUID& uuid) const;
  const std::vector<NimBLERemoteCharacteristic*>& getCharacteristics(bool refresh = false) const;

 private:
  NimBLEUUID uuid_;
  friend struct fakeble::FakeState;
};

struct NimBLEConnInfo {};

class NimBLEClientCallbacks;

class NimBLEClient {
 public:
  bool connect(const NimBLEAddress& address);
  bool disconnect();
  bool isConnected() const;
  bool secureConnection();
  bool cancelConnect();
  int getLastError() const;
  NimBLERemoteService* getService(const NimBLEUUID& uuid);
  const std::vector<NimBLERemoteService*>& getServices(bool refresh = false);
  void setConnectTimeout(uint32_t timeoutMs);
  void setConnectionParams(uint16_t minInterval, uint16_t maxInterval, uint16_t latency, uint16_t timeout);
  void setClientCallbacks(NimBLEClientCallbacks* callbacks, bool deleteOnDisconnect = true);

 private:
  bool connected_ = false;
  bool disconnecting_ = false;  // callback delivered, NimBLE not yet DISCONNECTED
  NimBLEClientCallbacks* callbacks_ = nullptr;
  friend struct fakeble::FakeState;
};

class NimBLEClientCallbacks {
 public:
  virtual ~NimBLEClientCallbacks() = default;
  virtual void onDisconnect(NimBLEClient*, int) {}
  virtual void onPassKeyEntry(NimBLEConnInfo&) {}
  virtual uint32_t onPassKeyDisplay(NimBLEConnInfo&) { return 0; }
  virtual void onConfirmPasskey(NimBLEConnInfo&, uint32_t) {}
  virtual bool onConnParamsUpdateRequest(NimBLEClient*, const ble_gap_upd_params*) { return true; }
};

class NimBLEAdvertisedDevice {
 public:
  NimBLEAddress getAddress() const { return address_; }
  bool haveName() const { return !name_.empty(); }
  std::string getName() const { return name_; }
  int getRSSI() const { return rssi_; }
  bool haveAppearance() const { return false; }
  uint16_t getAppearance() const { return 0; }
  bool isAdvertisingService(const NimBLEUUID& uuid) const { return uuid.value16() == 0x1812; }
  bool isConnectable() const { return connectable_; }

 private:
  NimBLEAddress address_;
  std::string name_;
  int rssi_ = -60;
  bool connectable_ = true;
  friend struct fakeble::FakeState;
};

class NimBLEScanCallbacks {
 public:
  virtual ~NimBLEScanCallbacks() = default;
  virtual void onResult(const NimBLEAdvertisedDevice*) {}
};

// NimBLE keeps every advertiser it reports in the scan's result list unless
// setMaxResults(0) puts the scan in callback-only mode; the fake counts them.
class NimBLEScan {
 public:
  void setScanCallbacks(NimBLEScanCallbacks* callbacks, bool wantDuplicates = false);
  void setActiveScan(bool active);
  void setDuplicateFilter(uint8_t enabled) { duplicateFilter_ = enabled != 0; }
  void setInterval(uint16_t intervalMs);
  void setWindow(uint16_t windowMs);
  void setMaxResults(uint8_t maxResults);
  bool start(uint32_t durationMs, bool isContinue = false, bool restart = true);
  bool isScanning() const;
  bool isActiveScan() const { return active_; }                // fake only
  bool filtersDuplicates() const { return duplicateFilter_; }  // fake only
  bool stop();
  void clearResults();

 private:
  bool scanning_ = false;
  bool active_ = false;
  bool duplicateFilter_ = false;
  NimBLEScanCallbacks* callbacks_ = nullptr;
  uint8_t maxResults_ = 0xFF;
  std::vector<std::string> retained_;
  friend struct fakeble::FakeState;
};

class NimBLEDevice {
 public:
  static bool init(const std::string& deviceName);
  static bool deinit(bool clearAll = false);
  static bool isInitialized();
  static bool setMTU(uint16_t mtu);
  static void setSecurityAuth(bool bonding, bool mitm, bool sc);
  static void setSecurityIOCap(uint8_t iocap);
  static void setSecurityPasskey(uint32_t passkey);
  static void setSecurityInitKey(uint8_t key);
  static void setSecurityRespKey(uint8_t key);
  static uint32_t getSecurityPasskey();
  static bool injectPassKey(const NimBLEConnInfo& connInfo, uint32_t passkey);
  static bool injectConfirmPasskey(const NimBLEConnInfo& connInfo, bool accept);
  static NimBLEScan* getScan();
  static NimBLEClient* createClient();
  static bool deleteClient(NimBLEClient* client);
  static NimBLEClient* getDisconnectedClient();
  static bool deleteBond(const NimBLEAddress& address);
};
