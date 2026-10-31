#pragma once

// Metalio E-Ink4-Plus board support. A firmware includes this header (under
// FREEINK_DEVICE_METALIO_EINK4_PLUS) so its build links the library; the
// display driver reaches the config through FREEINK_EPDIY_CONFIG.
#include <BoardConfig.h>

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
#include <EpdiyConfig.h>

namespace freeink {
const EpdiyConfig& metalioEInk4PlusEpdiyConfig();
}  // namespace freeink
#endif
