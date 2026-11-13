# SD stream host tests

Run `sh libs/hardware/SDCardManager/test/host/run.sh` from the SDK root.
The runner compiles the production `SDCardManager.cpp` and public header with
host stubs for Arduino, SdFat, board configuration and the task watchdog.
No implementation is copied or extracted into the tests.

Coverage includes exact output across buffer/chunk boundaries, positive short
writes, zero-progress writes, invalid output counts, SD read errors before and
after partial output, premature EOF, zero-progress reads, short reads, 64-bit
file sizes, file closure, uninitialized/open-failure paths, watchdog subscription
and time-budgeted yields during both ordinary transfers and partial writes.
The timer rollover case checks unsigned elapsed-time arithmetic.

For host sanitizers:

```sh
CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  sh libs/hardware/SDCardManager/test/host/run.sh
```

These tests verify transfer logic, not SD hardware or network timing. On a
device, transfer a multi-chunk file through a `Print`-derived network client and
compare its size and hash with the SD original. Then interrupt the connection
and check that the helper reports failure without restarting the device.

The runner also compiles SPI raw-access and SDMMC manager variants. Failed root
open, volume sync, and root close must keep the mount alive, reject USB raw
handoff, and prevent transport shutdown or bus isolation. Successful retry and
repeated shutdown are covered. Only the physical SDMMC transport is stubbed;
its production manager code and public API are compiled. These tests do not
simulate a real FAT volume or a physical power cut.
