#pragma once

// Test driver for the fake NimBLE stack in stubs/. It drives the real
// BleKeyboardHost (src/BleKeyboardHost.cpp, FREEINK_CAP_BLE_HID_HOST=1):
//
//   fakeble::resetWorld();
//   int map = fakeble::addCharacteristic(0x2A4B, true, false, false);
//   fakeble::setValue(map, reportMap, sizeof reportMap);
//   int in = fakeble::addInputReport();
//   fakeble::beginHost();
//   fakeble::connectTo("AA:BB:CC:DD:EE:01");
//   fakeble::notify(in, report, sizeof report);
//   fakeble::host().popKey(ev);

#include <NimBLEDevice.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "BleKeyboardHost.h"

namespace fakeble {

// Where the connection task can be held. Connect is released by a GAP cancel or
// a disconnect; Security and Discovery wait on an established link, so only a
// disconnect (or the test) releases them, as in NimBLE.
enum class Stage : uint8_t { None, Connect, Security, Discovery };

freeink::BleKeyboardHost& host();

void resetWorld();
bool beginHost();
// Starts a connect and waits (real time) for the link or a failure.
bool connectTo(const char* addr, uint32_t timeoutMs = 2000);
// Waits until the connection task is back at its idle wait.
bool waitForWorkerIdle(uint32_t timeoutMs = 1000);
// The peripheral drops the link (out of range, powered off).
void peerDisconnect();

int addCharacteristic(uint16_t uuid, bool canRead, bool canWrite, bool canNotify);
// A notifiable Report characteristic with an Input Report Reference descriptor.
int addInputReport(uint8_t reportId = 0);
void setValue(int index, const uint8_t* data, size_t len);
bool notify(int index, const uint8_t* data, size_t len);

void advertise(const char* addr, const char* name, bool connectable = true);
// The scan in progress runs out of time, as a timed NimBLE scan does.
void endScan();
// Whether the last scan was started active (with scan requests).
bool scanIsActive();
// Whether the controller reports each advertiser once per scan.
bool scanFiltersDuplicates();
size_t retainedScanResults();

void holdAt(Stage stage);
// Holds at `stage` and ignores GAP cancels and disconnects there, like a NimBLE
// wait that does not come back in time; only releaseHold() ends it.
void holdStubbornlyAt(Stage stage);
void releaseHold();
// The next link teardown delivers the disconnect callback but leaves the client
// DISCONNECTING (not yet reported by getDisconnectedClient) until
// finishDisconnect().
void lingerOnDisconnect();
void finishDisconnect();
bool clientExists();
bool waitUntilHeld(Stage stage, uint32_t timeoutMs = 1000);
// True once vTaskDelete() was called on the connection task while it was held
// inside a NimBLE wait.
bool taskDeletedWhileHeld();

// Teardown instrumentation. NimBLEDevice::deleteClient() and deinit() count how
// many callers are inside them at once; maxTeardownOverlap() is the highest
// count seen since resetWorld(). stallTeardown(ms) makes both take that long
// (real time) so a second caller has a window to land in. holdNextDeinit()
// parks the next deinit() caller inside it until releaseDeinit(); a deinit()
// that arrives meanwhile is not held, so it shows up as overlap, not a hang.
size_t maxTeardownOverlap();
void stallTeardown(uint32_t ms);
void holdNextDeinit();
bool waitUntilDeinitHeld(uint32_t timeoutMs = 1000);
void releaseDeinit();

size_t connectCalls();
// Addresses the host asked NimBLE to connect to, in order.
std::vector<std::string> connectAddresses();
// While set, every GAP connect fails with a timeout (the peer is off or away).
void failConnects(bool fail);
// While set, the link comes up and then pairing fails (the remote lost its keys).
void failSecurity(bool fail);
// The peer returns nothing for a search for one service UUID; listing all services
// still finds them.
void failServiceSearchByUuid(bool fail);
unsigned long clockMs();
void advanceMillis(uint32_t ms);

}  // namespace fakeble
