#pragma once

// FreeInk SDK — lightweight UI primitives.
//
// FreeInkUI is intentionally not a retained DOM. It provides small value types,
// fixed-capacity interaction routing, and simple row/column slot layout so apps
// can build reusable e-paper components without hardcoding panel coordinates.
// Labels and asset pointers are borrowed; the caller owns lifetime.
//
// The library is freestanding C++ (no Arduino/ESP-IDF dependency) so the same
// code runs on firmware and in host-side unit tests.

#include "FreeInkUICore.h"

#include "components/controls/button.h"
#include "components/controls/header.h"
#include "components/controls/progress-bar.h"
#include "components/controls/slider.h"
#include "components/controls/checkbox.h"
#include "components/bars/status-bar.h"
#include "components/bars/gesture-bar.h"
#include "components/bars/tab-bar.h"
#include "components/bars/battery-indicator.h"
#include "components/bars/reader-chrome.h"
#include "components/bars/tap-zones.h"
#include "components/text/text-field.h"
#include "components/text/text-area.h"
#include "components/lists/list.h"
#include "components/lists/dropdown.h"
#include "components/lists/table.h"
#include "components/lists/setting-row.h"
#include "components/lists/toggle-row.h"
#include "components/lists/stepper-row.h"
#include "components/lists/radio-group.h"
#include "components/overlays/popup.h"
#include "components/overlays/context-menu.h"
#include "components/overlays/toast.h"
#include "components/overlays/message-panel.h"
#include "components/overlays/option-dialog.h"
#include "components/media/cover-carousel.h"
#include "components/media/cover-grid.h"
#include "components/media/book-card.h"
#include "components/media/metric-card.h"
#include "components/keyboard/key-grid.h"
#include "components/keyboard/keyboard.h"
#include "components/keyboard/qwerty-keyboard.h"
