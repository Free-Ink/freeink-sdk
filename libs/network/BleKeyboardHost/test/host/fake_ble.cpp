// Fakes behind the stub headers (NimBLE, FreeRTOS, Arduino, Preferences) and
// the test driver in fake_ble.h. Only what src/BleKeyboardHost.cpp calls is
// modelled; everything else the tests observe is the library's own code.

#include "fake_ble.h"

#include <Preferences.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// --- Arduino ------------------------------------------------------------------
SerialStub Serial;
static std::atomic<unsigned long> g_clockMs{1000};
unsigned long millis() { return g_clockMs.load(); }

// --- FreeRTOS -----------------------------------------------------------------
namespace {

std::recursive_mutex& criticalMutex() {
  static std::recursive_mutex m;
  return m;
}

struct TaskStop {};

struct Task {
  std::thread thread;
  std::mutex m;
  std::condition_variable cv;
  bool notified = false;
  bool stop = false;
  bool waiting = false;
  bool abandoned = false;
};

Task* g_task = nullptr;
thread_local Task* t_task = nullptr;
bool g_deletedWhileHeld = false;

struct Gate {
  std::mutex m;
  std::condition_variable cv;
  fakeble::Stage configured = fakeble::Stage::None;
  fakeble::Stage active = fakeble::Stage::None;
  bool released = false;
  bool stubborn = false;
  Task* heldTask = nullptr;
};

Gate& gate() {
  static Gate g;
  return g;
}

void parkForever() {
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

// Holds the calling task at `stage` when a test asked for it. Returns true when
// the wait was ended by a cancel or disconnect (the NimBLE call then fails).
bool holdHere(fakeble::Stage stage) {
  Gate& g = gate();
  std::unique_lock<std::mutex> lock(g.m);
  if (g.configured != stage) return false;
  g.active = stage;
  g.heldTask = t_task;
  g.released = false;
  g.cv.notify_all();
  g.cv.wait(lock, [&g] { return g.released; });
  g.active = fakeble::Stage::None;
  g.configured = fakeble::Stage::None;
  g.stubborn = false;
  g.heldTask = nullptr;
  lock.unlock();
  // A task deleted while it sat in this wait must never run again: on the device
  // its stack and handle are gone.
  if (t_task != nullptr && t_task->abandoned) parkForever();
  return true;
}

void releaseHeld(bool onlyConnectStage) {
  Gate& g = gate();
  {
    std::lock_guard<std::mutex> guard(g.m);
    if (g.active == fakeble::Stage::None || g.stubborn) return;
    if (onlyConnectStage && g.active != fakeble::Stage::Connect) return;
    g.released = true;
  }
  g.cv.notify_all();
}

}  // namespace

void portEnterCritical(portMUX_TYPE*) { criticalMutex().lock(); }
void portExitCritical(portMUX_TYPE*) { criticalMutex().unlock(); }

BaseType_t xTaskCreate(void (*fn)(void*), const char*, uint32_t, void* arg, UBaseType_t, TaskHandle_t* outHandle) {
  Task* task = new Task();
  g_task = task;
  task->thread = std::thread([task, fn, arg] {
    t_task = task;
    try {
      fn(arg);
    } catch (const TaskStop&) {
    }
  });
  if (outHandle != nullptr) *outHandle = task;
  return pdTRUE;
}

void vTaskDelete(TaskHandle_t handle) {
  Task* task = static_cast<Task*>(handle);
  if (task == nullptr) return;
  bool held = false;
  {
    std::lock_guard<std::mutex> guard(gate().m);
    held = gate().heldTask == task;
  }
  if (g_task == task) g_task = nullptr;
  if (held) {
    g_deletedWhileHeld = true;
    task->abandoned = true;
    task->thread.detach();  // leaked on purpose: it can never be joined safely
    return;
  }
  {
    std::lock_guard<std::mutex> guard(task->m);
    task->stop = true;
  }
  task->cv.notify_all();
  if (task->thread.joinable()) task->thread.join();
  delete task;
}

void vTaskDelay(uint32_t ticks) {
  g_clockMs += ticks;
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void xTaskNotifyGive(TaskHandle_t handle) {
  Task* task = static_cast<Task*>(handle);
  if (task == nullptr) return;
  {
    std::lock_guard<std::mutex> guard(task->m);
    task->notified = true;
  }
  task->cv.notify_all();
}

uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, uint32_t) {
  Task* task = t_task;
  if (task == nullptr) return 0;
  std::unique_lock<std::mutex> lock(task->m);
  task->waiting = true;
  task->cv.notify_all();
  task->cv.wait(lock, [task] { return task->notified || task->stop; });
  task->waiting = false;
  if (task->stop) throw TaskStop{};
  if (clearOnExit == pdTRUE) task->notified = false;
  return 1;
}

// --- Fake stack state -----------------------------------------------------------
namespace fakeble {

void resetTeardownInstrumentation();  // with the NimBLEDevice fakes below

struct FakeState {
  std::atomic<bool> initialized{false};
  int lastError = 0;
  size_t connectCalls = 0;
  std::vector<std::string> connectAddresses;
  bool failConnects = false;
  std::vector<std::unique_ptr<NimBLERemoteCharacteristic>> owned;
  std::vector<NimBLERemoteCharacteristic*> chars;
  NimBLERemoteService service;
  NimBLERemoteService gapService;
  // What NimBLE holds after discovery on this link; connect() clears it.
  std::vector<NimBLERemoteService*> discovered;
  bool uuidSearchFails = false;
  NimBLEClient* client = nullptr;
  NimBLEScan scan;
  std::map<std::string, std::vector<uint8_t>> nvs;
  bool lingerOnDisconnect = false;

  FakeState() {
    service.uuid_ = NimBLEUUID(0x1812);
    gapService.uuid_ = NimBLEUUID(0x1800);
  }

  NimBLERemoteCharacteristic* at(int index) {
    if (index < 0 || static_cast<size_t>(index) >= chars.size()) return nullptr;
    return chars[static_cast<size_t>(index)];
  }

  int add(uint16_t uuid, bool canRead, bool canWrite, bool canNotify) {
    auto chr = std::make_unique<NimBLERemoteCharacteristic>();
    chr->uuid_ = NimBLEUUID(uuid);
    chr->canRead_ = canRead;
    chr->canWrite_ = canWrite;
    chr->canNotify_ = canNotify;
    chars.push_back(chr.get());
    owned.push_back(std::move(chr));
    return static_cast<int>(chars.size()) - 1;
  }

  int addInput(uint8_t reportId) {
    const int index = add(0x2A4D, true, false, true);
    NimBLERemoteCharacteristic* chr = at(index);
    chr->reportReference_.bytes_[0] = reportId;
    chr->reportReference_.bytes_[1] = 0x01;  // Input
    chr->reportReference_.len_ = 2;
    chr->hasReportReference_ = true;
    return index;
  }

  void set(int index, const uint8_t* data, size_t len) {
    if (NimBLERemoteCharacteristic* chr = at(index)) chr->value_.assign(data, data + len);
  }

  bool notify(int index, const uint8_t* data, size_t len) {
    NimBLERemoteCharacteristic* chr = at(index);
    if (chr == nullptr || !chr->subscribed_ || chr->notifyCallback_ == nullptr) return false;
    chr->notifyCallback_(chr, const_cast<uint8_t*>(data), len, false);
    return true;
  }

  void peerDrop() {
    if (client == nullptr || !client->connected_) return;
    client->connected_ = false;
    if (client->callbacks_ != nullptr) client->callbacks_->onDisconnect(client, 0x08);
  }

  void advertise(const char* addr, const char* name) {
    if (!scan.scanning_ || scan.callbacks_ == nullptr) return;
    if (scan.maxResults_ != 0 &&
        std::find(scan.retained_.begin(), scan.retained_.end(), addr) == scan.retained_.end()) {
      scan.retained_.emplace_back(addr);
    }
    NimBLEAdvertisedDevice dev;
    dev.address_ = NimBLEAddress(std::string(addr), 0);
    dev.name_ = name;
    scan.callbacks_->onResult(&dev);
  }

  size_t retained() const { return scan.retained_.size(); }

  void finishDisconnect() {
    lingerOnDisconnect = false;
    if (client != nullptr) client->disconnecting_ = false;
  }

  NimBLEClient* disconnectedClient() const {
    return client != nullptr && !client->connected_ && !client->disconnecting_ ? client : nullptr;
  }

  void reset() {
    owned.clear();
    chars.clear();
    initialized = false;
    lastError = 0;
    connectCalls = 0;
    connectAddresses.clear();
    failConnects = false;
    delete client;
    client = nullptr;
    scan = NimBLEScan();
    nvs.clear();
    lingerOnDisconnect = false;
    discovered.clear();
    uuidSearchFails = false;
  }
};

FakeState& state() {
  static FakeState s;
  return s;
}

freeink::BleKeyboardHost& host() { return freeink::BleKeyboardHost::getInstance(); }

void resetWorld() {
  releaseHold();
  finishDisconnect();
  if (host().isRunning() || host().isStopping()) host().end();
  {
    std::lock_guard<std::mutex> guard(gate().m);
    gate().configured = Stage::None;
    gate().stubborn = false;
  }
  g_deletedWhileHeld = false;
  state().reset();
  resetTeardownInstrumentation();
  g_clockMs = 1000;
}

bool beginHost() { return host().begin("FreeInk"); }

bool connectTo(const char* addr, uint32_t timeoutMs) {
  if (!host().connect(addr)) return false;
  char failure[64];
  for (uint32_t waited = 0; waited < timeoutMs; ++waited) {
    {
      std::lock_guard<std::recursive_mutex> fence(criticalMutex());
      if (host().isConnected()) return waitForWorkerIdle();
    }
    if (host().takeConnectFailure(failure, sizeof failure)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

bool waitForWorkerIdle(uint32_t timeoutMs) {
  Task* task = g_task;
  if (task == nullptr) return true;
  std::unique_lock<std::mutex> lock(task->m);
  return task->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [task] { return task->waiting && !task->notified; });
}

void peerDisconnect() { state().peerDrop(); }

int addCharacteristic(uint16_t uuid, bool canRead, bool canWrite, bool canNotify) {
  return state().add(uuid, canRead, canWrite, canNotify);
}
int addInputReport(uint8_t reportId) { return state().addInput(reportId); }
void setValue(int index, const uint8_t* data, size_t len) { state().set(index, data, len); }
bool notify(int index, const uint8_t* data, size_t len) { return state().notify(index, data, len); }

void advertise(const char* addr, const char* name) { state().advertise(addr, name); }
size_t retainedScanResults() { return state().retained(); }

void holdAt(Stage stage) {
  std::lock_guard<std::mutex> guard(gate().m);
  gate().configured = stage;
}

bool waitUntilHeld(Stage stage, uint32_t timeoutMs) {
  Gate& g = gate();
  std::unique_lock<std::mutex> lock(g.m);
  return g.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&g, stage] { return g.active == stage; });
}

void holdStubbornlyAt(Stage stage) {
  std::lock_guard<std::mutex> guard(gate().m);
  gate().configured = stage;
  gate().stubborn = true;
}

void releaseHold() {
  {
    std::lock_guard<std::mutex> guard(gate().m);
    gate().stubborn = false;
  }
  releaseHeld(false);
}

void lingerOnDisconnect() { state().lingerOnDisconnect = true; }

void finishDisconnect() { state().finishDisconnect(); }

bool clientExists() { return state().client != nullptr; }

bool taskDeletedWhileHeld() { return g_deletedWhileHeld; }
size_t connectCalls() { return state().connectCalls; }
std::vector<std::string> connectAddresses() { return state().connectAddresses; }
void failConnects(bool fail) { state().failConnects = fail; }
void failServiceSearchByUuid(bool fail) { state().uuidSearchFails = fail; }
unsigned long clockMs() { return g_clockMs.load(); }
void advanceMillis(uint32_t ms) { g_clockMs += ms; }

}  // namespace fakeble

// --- NimBLE -----------------------------------------------------------------------
using fakeble::Stage;
using fakeble::state;

bool NimBLERemoteCharacteristic::writeValue(const uint8_t* data, size_t length, bool) {
  value_.assign(data, data + length);
  return true;
}

NimBLERemoteDescriptor* NimBLERemoteCharacteristic::getDescriptor(const NimBLEUUID& uuid) {
  return hasReportReference_ && uuid.value16() == 0x2908 ? &reportReference_ : nullptr;
}

bool NimBLERemoteCharacteristic::subscribe(bool notifications, NimBLENotifyCallback callback, bool) {
  subscribed_ = notifications;
  notifyCallback_ = notifications ? callback : nullptr;
  return true;
}

NimBLERemoteCharacteristic* NimBLERemoteService::getCharacteristic(const NimBLEUUID& uuid) const {
  for (NimBLERemoteCharacteristic* chr : state().chars) {
    if (chr->getUUID() == uuid) return chr;
  }
  return nullptr;
}

const std::vector<NimBLERemoteCharacteristic*>& NimBLERemoteService::getCharacteristics(bool) const {
  return state().chars;
}

bool NimBLEClient::connect(const NimBLEAddress& address) {
  state().connectCalls++;
  state().connectAddresses.push_back(address.toString());
  if (connected_ || disconnecting_) {  // like NimBLE: only a DISCONNECTED client connects
    state().lastError = BLE_HS_EREJECT;
    return false;
  }
  if (holdHere(Stage::Connect) || state().failConnects) {
    state().lastError = BLE_HS_ETIMEOUT;
    return false;
  }
  state().lastError = 0;
  state().discovered.clear();
  connected_ = true;
  return true;
}

bool NimBLEClient::disconnect() {
  releaseHeld(false);
  if (!connected_) return true;
  connected_ = false;
  disconnecting_ = state().lingerOnDisconnect;
  if (callbacks_ != nullptr) callbacks_->onDisconnect(this, 0x16);
  return true;
}

bool NimBLEClient::isConnected() const { return connected_; }

bool NimBLEClient::secureConnection() { return !holdHere(Stage::Security); }

bool NimBLEClient::cancelConnect() {
  releaseHeld(true);
  return true;
}

int NimBLEClient::getLastError() const { return state().lastError; }

// Like NimBLE: a service already discovered on this link is found without a
// request; otherwise the peer is searched for that one UUID.
NimBLERemoteService* NimBLEClient::getService(const NimBLEUUID& uuid) {
  if (holdHere(Stage::Discovery)) return nullptr;
  for (NimBLERemoteService* svc : state().discovered) {
    if (svc->getUUID() == uuid) return svc;
  }
  if (state().uuidSearchFails || uuid != state().service.getUUID()) return nullptr;
  state().discovered.push_back(&state().service);
  return &state().service;
}

const std::vector<NimBLERemoteService*>& NimBLEClient::getServices(bool refresh) {
  // A dropped link lists nothing, as NimBLE's retrieveServices() fails at once.
  if (refresh) state().discovered.clear();
  if (refresh && connected_) state().discovered = {&state().gapService, &state().service};
  return state().discovered;
}

void NimBLEClient::setConnectTimeout(uint32_t) {}
void NimBLEClient::setConnectionParams(uint16_t, uint16_t, uint16_t, uint16_t) {}
void NimBLEClient::setClientCallbacks(NimBLEClientCallbacks* callbacks, bool) { callbacks_ = callbacks; }

void NimBLEScan::setScanCallbacks(NimBLEScanCallbacks* callbacks, bool) { callbacks_ = callbacks; }
void NimBLEScan::setActiveScan(bool) {}
void NimBLEScan::setInterval(uint16_t) {}
void NimBLEScan::setWindow(uint16_t) {}
void NimBLEScan::setMaxResults(uint8_t maxResults) { maxResults_ = maxResults; }
bool NimBLEScan::start(uint32_t, bool, bool) {
  scanning_ = true;
  return true;
}
bool NimBLEScan::isScanning() const { return scanning_; }
bool NimBLEScan::stop() {
  scanning_ = false;
  return true;
}
void NimBLEScan::clearResults() { retained_.clear(); }

namespace {

std::atomic<size_t> g_inTeardown{0};
std::atomic<size_t> g_maxTeardownOverlap{0};
std::atomic<uint32_t> g_teardownStallMs{0};
std::atomic<bool> g_deinitHoldArmed{false};
std::atomic<bool> g_deinitHeld{false};
std::atomic<bool> g_deinitReleased{false};

struct TeardownScope {
  TeardownScope() {
    const size_t now = ++g_inTeardown;
    size_t seen = g_maxTeardownOverlap.load();
    while (now > seen && !g_maxTeardownOverlap.compare_exchange_weak(seen, now)) {
    }
    const uint32_t stall = g_teardownStallMs.load();
    if (stall != 0) std::this_thread::sleep_for(std::chrono::milliseconds(stall));
  }
  ~TeardownScope() { --g_inTeardown; }
};

}  // namespace

namespace fakeble {

size_t maxTeardownOverlap() { return g_maxTeardownOverlap.load(); }
void stallTeardown(uint32_t ms) { g_teardownStallMs = ms; }
void holdNextDeinit() {
  g_deinitReleased = false;
  g_deinitHeld = false;
  g_deinitHoldArmed = true;
}
bool waitUntilDeinitHeld(uint32_t timeoutMs) {
  for (uint32_t waited = 0; waited < timeoutMs; ++waited) {
    if (g_deinitHeld.load()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
void releaseDeinit() {
  g_deinitHoldArmed = false;
  g_deinitReleased = true;
}
void resetTeardownInstrumentation() {
  releaseDeinit();
  g_teardownStallMs = 0;
  g_maxTeardownOverlap = 0;
}

}  // namespace fakeble

bool NimBLEDevice::init(const std::string&) {
  state().initialized = true;
  return true;
}
bool NimBLEDevice::deinit(bool) {
  TeardownScope scope;
  if (g_deinitHoldArmed.exchange(false)) {
    g_deinitHeld = true;
    while (!g_deinitReleased.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    g_deinitHeld = false;
  }
  state().initialized = false;
  return true;
}
bool NimBLEDevice::isInitialized() { return state().initialized; }
bool NimBLEDevice::setMTU(uint16_t) { return true; }
void NimBLEDevice::setSecurityAuth(bool, bool, bool) {}
void NimBLEDevice::setSecurityIOCap(uint8_t) {}
void NimBLEDevice::setSecurityPasskey(uint32_t) {}
void NimBLEDevice::setSecurityInitKey(uint8_t) {}
void NimBLEDevice::setSecurityRespKey(uint8_t) {}
uint32_t NimBLEDevice::getSecurityPasskey() { return 123456; }
bool NimBLEDevice::injectPassKey(const NimBLEConnInfo&, uint32_t) { return true; }
bool NimBLEDevice::injectConfirmPasskey(const NimBLEConnInfo&, bool) { return true; }
NimBLEScan* NimBLEDevice::getScan() { return &state().scan; }
NimBLEClient* NimBLEDevice::createClient() {
  if (state().client == nullptr) state().client = new NimBLEClient();
  return state().client;
}
bool NimBLEDevice::deleteClient(NimBLEClient* client) {
  TeardownScope scope;
  if (client == nullptr || client != state().client) return false;
  delete client;
  state().client = nullptr;
  return true;
}
NimBLEClient* NimBLEDevice::getDisconnectedClient() { return state().disconnectedClient(); }
bool NimBLEDevice::deleteBond(const NimBLEAddress&) { return true; }

// --- Preferences (in-memory NVS) ------------------------------------------------
bool Preferences::begin(const char* name, bool) {
  ns_ = name != nullptr ? name : "";
  return true;
}

uint8_t Preferences::getUChar(const char* key, uint8_t defaultValue) {
  const auto it = state().nvs.find(ns_ + "/" + key);
  return it == state().nvs.end() || it->second.empty() ? defaultValue : it->second[0];
}

size_t Preferences::getBytes(const char* key, void* buf, size_t maxLen) {
  const auto it = state().nvs.find(ns_ + "/" + key);
  if (it == state().nvs.end() || buf == nullptr) return 0;
  const size_t len = std::min(maxLen, it->second.size());
  std::memcpy(buf, it->second.data(), len);
  return len;
}

size_t Preferences::putUChar(const char* key, uint8_t value) {
  state().nvs[ns_ + "/" + key].assign(1, value);
  return 1;
}

size_t Preferences::putBytes(const char* key, const void* buf, size_t len) {
  const uint8_t* bytes = static_cast<const uint8_t*>(buf);
  state().nvs[ns_ + "/" + key].assign(bytes, bytes + len);
  return len;
}
