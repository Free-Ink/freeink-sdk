# BLE HID Host

`BleKeyboardHost` (`libs/network/BleKeyboardHost`) is a Bluetooth Low Energy HID
**host**: it pairs with and connects to one BLE HID peripheral at a time
(central role) and hands firmware translated key events. It is useful for
keyboards, page turners, remote buttons, and similar devices that expose the HID
service (`0x1812`).

The class and include path keep their original `BleKeyboardHost` name for source
compatibility. New code can use the `BleHid` accessor; `BleKbd` remains as an
alias for existing firmware.

> **BLE only.** The ESP32-C3/S3 has no Bluetooth Classic (BR/EDR) radio, so
> Classic-only HID devices cannot connect.

## Enabling it

The library is a **capability**, gated on `FREEINK_CAP_BLE_HID_HOST` (default
off). When off it links stub bodies and pulls in **no** BLE code. To turn it on,
a firmware:

```ini
build_flags =
  -DFREEINK_CAP_BLE_HID_HOST=1
  ; Hide anonymous non-HID advertisers from deviceCount()/device() by default.
  ; Set to 1 during bring-up if you want unnamed probe candidates in the list.
  -DFREEINK_BLE_HID_SHOW_UNNAMED_DEVICES=0
  ; Optional but recommended for BLE 5.x devices that use extended advertising:
  -DCONFIG_BT_NIMBLE_EXT_ADV=1
  ; Minimal NimBLE footprint (central-only, 1 connection):
  -DCONFIG_BT_NIMBLE_ROLE_CENTRAL=1
  -DCONFIG_BT_NIMBLE_ROLE_OBSERVER=1
  -DCONFIG_BT_NIMBLE_ROLE_PERIPHERAL=0
  -DCONFIG_BT_NIMBLE_ROLE_BROADCASTER=0
  -DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
  -DCONFIG_BT_NIMBLE_MAX_BONDS=4

lib_deps =
  BleKeyboardHost=symlink://freeink-sdk/libs/network/BleKeyboardHost
  h2zero/NimBLE-Arduino@^2.3.8
```

The older `FREEINK_CAP_BLE_KEYBOARD` flag still maps to the HID host capability
for compatibility.

## API

```cpp
#include <BleKeyboardHost.h>

void setup() {
  BleHid.begin("FreeInk");   // init NimBLE central + bonding, load NVS bonds
}

void loop() {
  BleHid.poll();             // drive auto-reconnect + key auto-repeat

  freeink::KeyEvent ev;
  while (BleHid.popKey(ev)) {
    if (ev.special == freeink::SpecialKey::PageDown) { /* page turner */ }
    else if (ev.ch) { /* printable keyboard input */ }
  }
}
```

Pairing UI:

```cpp
BleHid.startScan(5000);
for (uint8_t i = 0; i < BleHid.deviceCount(); ++i) {
  const auto& d = BleHid.device(i);  // d.addr, d.name, d.rssi, d.hid, d.connectable
}
BleHid.connect(addr);                // async; isConnected() flips when ready
BleHid.releaseScanResults();         // reclaim scan RAM once connected

BleHid.pairedCount(); BleHid.paired(i);
BleHid.forget(addr);
```

`FREEINK_BLE_HID_SHOW_UNNAMED_DEVICES` controls scan-list noise at SDK level.
The default `0` keeps named devices and devices advertising HID, but drops
anonymous non-HID advertisers. Set it to `1` to include unnamed connectable
devices while debugging a new peripheral. Each `DiscoveredDevice` also exposes
`hasName` if a firmware wants to apply a runtime UI filter.

`begin()` configures bonded pairing with host-display passkey support. When a
peripheral requires passkey pairing, the host exposes the six-digit code through
`takePairingPasskey()` so firmware can show it to the user.

## How It Works

- **Scan** — active scan with duplicate reports enabled; every advertiser is
  upserted into a fixed array (`kMaxDiscovered`) and the HID service UUID is
  recorded when present. HID is validated at connect time so devices that put
  their name or services in scan response / extended advertising fragments can
  still appear in the pairing UI.
- **Connect** — runs on a dedicated FreeRTOS task: connect → discover HID →
  `secureConnection()` (bond) → write **Report Protocol** mode → subscribe to
  Input reports, falling back to Boot Keyboard Input (`0x2A22`) for devices that
  only expose the boot characteristic.
- **Reports** — normalized to `[mod][k0..k5]`, diffed against the previous
  report, then translated (US QWERTY HID usages) into `KeyEvent`s. Page turners
  commonly send arrow or page-up/page-down usages, which arrive as `SpecialKey`.
- **Auto-repeat** — HID sends one report per state change, so `poll()`
  synthesizes repeats for the held key after an initial delay.

## Memory

All storage is fixed-capacity: discovered devices (`kMaxDiscovered` = 24), bonds
(`kMaxBonds` = 4), and the key ring (`kKeyQueueLen` = 16). One active connection.
No `std::vector`/heap in the hot path. Configure NimBLE for a single connection,
central-only, to keep its static footprint small on the C3.
