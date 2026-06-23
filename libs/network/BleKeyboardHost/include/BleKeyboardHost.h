#pragma once

// FreeInk SDK — BLE HID host (singleton).
//
// Pairs with and connects to a Bluetooth Low Energy HID peripheral (central
// role) and exposes translated key events (printable chars + a SpecialKey enum)
// plus scan/pair/connect controls for a settings UI. One peripheral at a time.
//
// Capability-gated: the real NimBLE implementation compiles only when
// FREEINK_CAP_BLE_HID_HOST is set (and the firmware adds NimBLE-Arduino to its
// lib_deps); otherwise every method links a stub so callers need no #ifdefs and
// no BLE code is pulled in. This header is deliberately NimBLE-free so consumers
// (and host builds) never include the BLE stack just to see the API.
//
// Memory: all storage is fixed-capacity (no std::vector / heap in the hot path).
// BLE callbacks run on the NimBLE host task and hand data to the app through a
// small spinlock-guarded ring; drain it from the main loop with popKey().
//
// BLE-only — the ESP32-C3/S3 has no Bluetooth Classic radio, so Classic-only HID
// peripherals cannot connect.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace freeink {

// Non-character keys an editor/UI cares about. Printable keys arrive as `ch`.
enum class SpecialKey : uint8_t {
  None = 0,
  Enter,
  Backspace,
  Tab,
  Escape,
  Delete,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  PageUp,
  PageDown,
};

// One decoded key press (or auto-repeat). `pressed` is always true today — the
// host emits on the press edge and synthesizes repeats while a key is held; key
// releases are tracked internally for repeat but not surfaced.
struct KeyEvent {
  char ch = 0;                          // printable ASCII, or 0 for a special key
  uint8_t keycode = 0;                  // raw HID usage id
  uint8_t mods = 0;                     // HID modifier bitmask (ctrl/shift/alt/gui)
  SpecialKey special = SpecialKey::None;
  bool pressed = true;
};

// A BLE device seen during a scan.
struct DiscoveredDevice {
  char addr[18] = {0};  // "AA:BB:CC:DD:EE:FF"
  char name[32] = {0};  // falls back to the address when no name was received
  int rssi = 0;
  uint8_t addrType = 0;  // BLE address type, needed to reconnect
  bool hasName = false;  // true when the advertised name was actually received
  bool hid = false;      // advertises the HID service (0x1812)
  bool connectable = false;
};

// A BLE HID peripheral the host has bonded with (persisted in NVS for auto-reconnect).
struct PairedHidDevice {
  char addr[18] = {0};
  char name[32] = {0};
  uint8_t addrType = 0;
};
using PairedKeyboard = PairedHidDevice;  // Backward-compatible SDK name.

class BleKeyboardHost {
 public:
  static constexpr uint8_t kMaxDiscovered = 24;
  static constexpr uint8_t kMaxBonds = 4;
  static constexpr uint8_t kKeyQueueLen = 16;

  static BleKeyboardHost& getInstance();

  // Init the NimBLE central, security (Just Works bonding), and load the saved
  // pairing list. Safe to call once. Returns false if BLE init failed or the
  // capability is compiled out.
  bool begin(const char* hostName = "FreeInk");

  // Pump per main-loop iteration: drives auto-reconnect and key auto-repeat.
  // Cheap; never blocks.
  void poll();

  // --- Discovery -------------------------------------------------------------
  void startScan(uint32_t ms = 5000);
  void stopScan();
  bool isScanning() const { return scanning_; }
  uint8_t deviceCount() const { return deviceCount_; }
  const DiscoveredDevice& device(uint8_t i) const;
  // Free scan bookkeeping after connecting, to reclaim RAM while writing.
  void releaseScanResults();

  // --- Connection ------------------------------------------------------------
  // Begin an async connect to a scanned/bonded address. isConnected() flips once
  // the link is encrypted and the HID input report is subscribed.
  bool connect(const char* addr);
  void disconnect();
  bool isConnected() const { return connected_; }
  bool isConnecting() const { return connecting_; }
  const char* connectedName() const { return connName_; }
  bool takeConnectFailure(char* out, size_t outLen);
  bool takePairingPasskey(uint32_t& out);

  // --- Pairings (persisted) --------------------------------------------------
  uint8_t pairedCount() const { return bondCount_; }
  const PairedHidDevice& paired(uint8_t i) const;
  void forget(const char* addr);

  // --- Translated input ------------------------------------------------------
  // Pop the next key event. Returns false when the queue is empty.
  bool popKey(KeyEvent& out);

  // --- Internal: called by the NimBLE backend (not for app use). These keep the
  // public header free of NimBLE types — the .cpp translates BLE objects into
  // these plain calls. -------------------------------------------------------
  void onScanResultIngest(const char* addr, const char* name, int rssi, uint8_t type, bool hid, bool connectable);
  void onReportIngest(const uint8_t* data, size_t len);
  void onLinkUp(const char* addr, const char* name, uint8_t type);
  void onLinkDown();
  void onConnectFailed(const char* reason);
  void onPairingPasskey(uint32_t passkey);

 private:
  void enqueue(const KeyEvent& ev);    // ring push (spinlock-guarded)
  void emitUsage(uint8_t usage, uint8_t mods);  // translate + enqueue
  void persistBonds();
  void loadBonds();
  BleKeyboardHost() = default;
  BleKeyboardHost(const BleKeyboardHost&) = delete;
  BleKeyboardHost& operator=(const BleKeyboardHost&) = delete;

  // Plain, NimBLE-free state shared by both the real and stub builds. The NimBLE
  // objects, spinlock, and connection task live file-static in the .cpp so this
  // header pulls in nothing.
  DiscoveredDevice devices_[kMaxDiscovered];
  uint8_t deviceCount_ = 0;
  PairedHidDevice bonds_[kMaxBonds];
  uint8_t bondCount_ = 0;
  KeyEvent ring_[kKeyQueueLen];
  volatile uint8_t ringHead_ = 0;  // next write
  volatile uint8_t ringTail_ = 0;  // next read
  char connName_[32] = {0};
  char connectFailure_[48] = {0};
  volatile uint32_t pairingPasskey_ = 0;
  volatile bool connected_ = false;
  volatile bool connecting_ = false;
  volatile bool connectFailed_ = false;
  volatile bool pairingPasskeyReady_ = false;
  volatile bool scanning_ = false;
  bool begun_ = false;

  // Key auto-repeat: HID delivers one report per state change, so holding a key
  // (backspace, arrows) only sends a single press. The backend records the held
  // usage here and poll() synthesizes repeats after an initial delay.
  volatile uint8_t heldUsage_ = 0;
  volatile uint8_t heldMods_ = 0;
  volatile uint32_t heldSince_ = 0;
  volatile uint32_t lastRepeat_ = 0;
  uint8_t prevKeys_[6] = {0};  // backend-task only
};

}  // namespace freeink

// App-friendly accessors. BleKbd is kept for source compatibility.
#define BleHid ::freeink::BleKeyboardHost::getInstance()
#define BleKbd ::freeink::BleKeyboardHost::getInstance()
