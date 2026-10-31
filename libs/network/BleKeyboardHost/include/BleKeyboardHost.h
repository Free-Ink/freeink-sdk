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

#include <atomic>

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

// One button edge read from the report BYTES, next to the key decode. Its
// identity is where the report first differs from its rest frame (the frame the
// remote sends with nothing pressed): the report id (the id byte of a 9-byte
// frame, else the Report Reference id of the characteristic, else 0), the payload
// byte index and the byte's value. It keeps apart what the key decode folds
// together or drops: a button on a byte the decoder does not read, two reports
// that share a code. `keycode`/`mods` on a press are the key the decoder read from
// the same frame (0 when none), so an app that routes raw edges can fall back to
// key bindings for a button it has not learned.
struct RawButtonEvent {
  uint8_t reportId = 0;
  uint8_t byteIndex = 0;
  uint8_t value = 0;
  bool pressed = false;
  uint8_t keycode = 0;
  uint8_t mods = 0;
  // Release only: the press it ends was read before the report's rest frame was
  // known, and the rest learned since holds that very byte, so the "press" was the
  // remote idling on a non-zero status byte. A button-learning screen drops it.
  bool wasRest = false;
  uint32_t atMs = 0;  // millis() when the frame arrived, for hold timing
  // value | byteIndex << 8 | reportId << 16; never 0 for an edge. A key the decoder
  // read that no byte edge carries (an axis gamepad's zone) comes as one tap with
  // byteIndex 0xFF and reportId 0xFF.
  uint32_t code() const {
    return static_cast<uint32_t>(value) | static_cast<uint32_t>(byteIndex) << 8 |
           static_cast<uint32_t>(reportId) << 16;
  }
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
  // Eight presses with their releases, plus the slot that tells a full ring from
  // an empty one.
  static constexpr uint8_t kRawQueueLen = 17;

  static BleKeyboardHost& getInstance();

  // Init the NimBLE central, security (Just Works bonding), and load the saved
  // pairing list. Safe to call once. Returns false if BLE init failed or the
  // capability is compiled out.
  bool begin(const char* hostName = "FreeInk");

  // Fully tear down the BLE stack: stop scanning, drop the link, delete the
  // connection task, and NimBLEDevice::deinit() so the NimBLE host + controller
  // RAM (tens of KB) is returned to the heap. Use this — not disconnect() — when
  // the user turns Bluetooth off, so memory-hungry work (e.g. EPUB inflate) can
  // allocate again. Bonds persist in NVS; begin() re-inits cleanly afterwards.
  // Must run at normal CPU frequency (controller deinit), like begin().
  //
  // Returns true once the stack is down. It waits at most timeoutMs (capped at
  // 2 s) for the connection task to leave a connect, pairing or discovery wait
  // and for the link to finish closing. When that takes longer, nothing is
  // deleted under NimBLE: end() returns false, isStopping() stays true and a
  // later end() carries on from there. end(0) never waits. Safe to call from
  // more than one task: while one end() runs, another returns false at once
  // (still stopping) instead of tearing down alongside it.
  bool end(uint32_t timeoutMs = 1000);

  // True from an end() that returned false until one returns true. begin() is
  // refused meanwhile.
  bool isStopping() const;

  // Pump per main-loop iteration: drives auto-reconnect and key auto-repeat.
  // Cheap; never blocks.
  void poll();

  // True while the NimBLE stack is initialized (between a successful begin() and
  // end()). Lets the app gate CPU-frequency and lifecycle decisions on whether BLE
  // is actually resident, independent of the user's on/off preference.
  bool isRunning() const { return begun_; }

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
  // the link is encrypted and the HID input report is subscribed. An attempt still in
  // progress (an automatic reconnect or an earlier connect()) is ended first; the
  // call waits for that, at most about 8.5 s (usually milliseconds), and returns
  // false if it could not end it.
  bool connect(const char* addr);
  // Reconnect to one bonded peer only, for a bounded time: at most six attempts,
  // each four seconds after the last one failed, none started later than 120
  // seconds after arming, and no fallback to the other bonds. For an app that
  // knows which remote it wants and should not spend the radio on the others.
  // Refused (false) for an address that is not bonded, before begin(), while
  // scanning, connecting or connected, or while a plan is already armed.
  // connect(), disconnect(), end(), and forget() of that peer cancel the plan; a
  // link to the peer that later drops on its own starts a fresh one.
  bool armSelectedPeerReconnect(const char* addr);
  // Drop the link and pause auto-reconnect until the next connect() or begin().
  void disconnect();
  bool isConnected() const { return connected_; }
  bool isConnecting() const { return connecting_; }
  const char* connectedName() const { return connName_; }
  // Address of the peer on the live link ("" when none). Settings kept per remote
  // must use this rather than the address the app asked for: auto-reconnect can
  // bring up another bonded peer.
  const char* connectedAddr() const { return connAddr_; }
  bool takeConnectFailure(char* out, size_t outLen);
  bool takePairingPasskey(uint32_t& out);

  // --- Pairings (persisted) --------------------------------------------------
  uint8_t pairedCount() const { return bondCount_; }
  const PairedHidDevice& paired(uint8_t i) const;
  void forget(const char* addr);

  // --- Translated input ------------------------------------------------------
  // Pop the next key event. Returns false when the queue is empty.
  bool popKey(KeyEvent& out);
  // Pop the next raw button edge (see RawButtonEvent). Filled from the same
  // reports as popKey(), in a ring of its own, so an app that never calls it sees
  // no change. A press only goes in with room left for its release; when the
  // ring is full the whole press is dropped, never a release alone.
  bool popRawButton(RawButtonEvent& out);

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
  bool connectInternal(const char* addr, bool explicitRequest);
  // Raw ring push; the caller holds the ring lock. Refused (false) unless `keep`
  // slots stay free after it: a press keeps one for its own release.
  bool pushRawLocked(uint32_t code, bool pressed, uint32_t atMs, uint8_t keycode, uint8_t mods, uint8_t keep,
                     bool wasRest = false);
  void ingestRawEdge(const uint8_t* data, size_t len, uint32_t now, uint32_t lastMs);
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
  char connAddr_[18] = {0};
  char connectFailure_[48] = {0};
  volatile uint32_t pairingPasskey_ = 0;
  volatile bool connected_ = false;
  volatile bool connecting_ = false;
  volatile bool connectFailed_ = false;
  volatile bool pairingPasskeyReady_ = false;
  volatile bool scanning_ = false;
  // Atomic: read from other tasks (isRunning) while end() clears it after
  // raising the stopping flag.
  std::atomic<bool> begun_{false};

  // Key auto-repeat: HID delivers one report per state change, so holding a key
  // (backspace, arrows) only sends a single press. The backend records the held
  // usage here and poll() synthesizes repeats after an initial delay.
  volatile uint8_t heldUsage_ = 0;
  volatile uint8_t heldMods_ = 0;
  volatile uint32_t heldSince_ = 0;
  volatile uint32_t lastRepeat_ = 0;
  uint8_t prevKeys_[6] = {0};  // backend-task only

  // Raw button edges (RawButtonEvent), their own ring. rawCode_ is the button the
  // last frame held (0 = none); rawReports_ counts frames that repeated it, which
  // tells a remote that streams a held button (silence = release) from one that
  // sends one frame per edge (silence = still held). Guarded by the ring lock.
  RawButtonEvent rawRing_[kRawQueueLen];
  volatile uint8_t rawHead_ = 0;
  volatile uint8_t rawTail_ = 0;
  uint32_t rawCode_ = 0;
  uint8_t rawReports_ = 0;
  bool rawDropped_ = false;  // rawCode_'s press was dropped: its release is not sent either
  bool rawGuessed_ = false;  // rawCode_ was read against the all-zero guess, its rest not yet known
  // The rest frame and the last frame of each report id. Backend task only.
  struct RawRest {
    uint8_t id;
    uint8_t frames;  // frames seen, capped at 2: the first one may itself be the rest state
    bool known;      // rest is a frame the remote sent, not the all-zero guess
    uint8_t rest[8];
    uint8_t prev[8];
  };
  RawRest rawRest_[4];
  uint8_t rawRestCount_ = 0;
  // What the key decode read from the frame being ingested. Backend task only.
  bool frameAxisPad_ = false;
  uint8_t framePressUsage_ = 0;
  uint8_t framePressMods_ = 0;
};

}  // namespace freeink

// App-friendly accessors. BleKbd is kept for source compatibility.
#define BleHid ::freeink::BleKeyboardHost::getInstance()
#define BleKbd ::freeink::BleKeyboardHost::getInstance()
