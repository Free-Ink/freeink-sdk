// Exercise facade selection independently of static board metadata.
#include <cassert>
#include <cstring>
#include <type_traits>
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Ssd1677Driver.h"
#include "src/driver/Uc8179Driver.h"
#include "src/driver/Uc8279X4Driver.h"
#undef private
#include "src/driver/Ed2208M5Driver.h"
#include "src/driver/It8951Driver.h"
#include "src/driver/LgfxEpdDriver.h"
#include "src/driver/M5OfficialDriver.h"
#include "src/driver/PaperMonoDriver.h"
#include "src/driver/Ssd2677Driver.h"
#include "src/driver/Uc8253MurphyDriver.h"
#include "src/driver/Uc8253X3Driver.h"
#include "src/driver/Uc8279Driver.h"
#include "src/driver/Uc8279cA4Driver.h"

using namespace freeink;

template <typename Driver>
constexpr bool ownsControllerIdentity =
    std::is_same_v<decltype(&Driver::controllerName), const char* (Driver::*)() const>;

static_assert(ownsControllerIdentity<Ssd1677Driver>);
static_assert(ownsControllerIdentity<Uc8179Driver>);
static_assert(ownsControllerIdentity<Uc8279X4Driver>);
static_assert(ownsControllerIdentity<Ed2208M5Driver>);
static_assert(ownsControllerIdentity<It8951Driver>);
static_assert(ownsControllerIdentity<LgfxEpdDriver>);
static_assert(ownsControllerIdentity<M5OfficialDriver>);
static_assert(ownsControllerIdentity<PaperMonoDriver>);
static_assert(ownsControllerIdentity<Ssd2677Driver>);
static_assert(ownsControllerIdentity<Uc8253MurphyDriver>);
static_assert(ownsControllerIdentity<Uc8253X3Driver>);
static_assert(ownsControllerIdentity<Uc8279Driver>);
static_assert(ownsControllerIdentity<Uc8279cA4Driver>);

int main() {
  struct Case {
    BoardConfig::DisplayController controller;
    const char* name;
  };
  for (const auto& item : {Case{BoardConfig::DisplayController::SSD1677, "SSD1677"},
                           Case{BoardConfig::DisplayController::UC8179, "UC8179"},
                           Case{BoardConfig::DisplayController::UC8279, "UC8279"}}) {
    BoardConfig::ACTIVE.displayController = item.controller;
    FreeInkDisplay display(1, 2, 3, 4, 5, 6);
    assert(display.getControllerName() == nullptr);
    display.selectDriver();
    assert(!strcmp(display.getControllerName(), item.name));
    // A query reports the standing driver without reselecting or touching the bus.
    BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::SSD1677;
    assert(!strcmp(display.getControllerName(), item.name));
    assert(display._bus.writes.empty());
  }
}
