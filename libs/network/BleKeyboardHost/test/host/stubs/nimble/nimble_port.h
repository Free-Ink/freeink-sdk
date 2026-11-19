#pragma once

// Host-test stand-in for NimBLE's event queue. The default queue stands for the
// host task: an event put on it runs at once, in order, on the caller's thread.
// The fake counts events initialised and not yet deinitialised, as ESP-IDF 6's
// event pool does (fakeble::eventsOutstanding()).

struct ble_npl_event;
typedef void ble_npl_event_fn(ble_npl_event* ev);

struct ble_npl_event {
  ble_npl_event_fn* fn = nullptr;
  void* arg = nullptr;
  bool inPool = false;
};

struct ble_npl_eventq {};

ble_npl_eventq* nimble_port_get_dflt_eventq();
void ble_npl_event_init(ble_npl_event* ev, ble_npl_event_fn* fn, void* arg);
void ble_npl_event_deinit(ble_npl_event* ev);
void ble_npl_eventq_put(ble_npl_eventq* evq, ble_npl_event* ev);
