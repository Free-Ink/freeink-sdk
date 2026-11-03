#pragma once

#include <BoardConfig.h>

#if !defined(SIMULATOR) && ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT && !FREEINK_DEVICE_STICKY && \
    SOC_USB_SERIAL_JTAG_SUPPORTED
#include <hal/usb_serial_jtag_ll.h>
#endif

namespace BoardConfig {
// Make pending native USB output visible to the host, without waiting or
// discarding queued bytes. HWCDC::flush() may discard its queue on timeout;
// request/response protocols must instead keep servicing TX while awaiting RX.
// UART and TinyUSB already schedule their output independently.
inline void serviceSerialTx() {
#if !defined(SIMULATOR) && ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT && !FREEINK_DEVICE_STICKY && \
    SOC_USB_SERIAL_JTAG_SUPPORTED
  usb_serial_jtag_ll_txfifo_flush();
#endif
}
}  // namespace BoardConfig
