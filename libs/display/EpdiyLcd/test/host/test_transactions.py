"""Compile the complete Read Pico drivers with recording hardware seams."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
SDK = HERE.parents[4]
LCD = SDK / "libs/display/EpdiyLcd"


def run():
    with tempfile.TemporaryDirectory(prefix="readpico-transactions-") as directory:
        root = Path(directory)
        os.environ["CCACHE_DIR"] = str(root / "ccache")
        stubs = {
            "Arduino.h": """#pragma once
#include <cstdint>
#include <cstddef>
#include <cassert>
#define INPUT_PULLUP 1
#define OUTPUT 2
#define LOW 0
#define HIGH 1
void delay(unsigned long);
inline void delayMicroseconds(unsigned long) {}
inline unsigned long millis() { return 100; }
inline void pinMode(int,int) {}
inline void digitalWrite(int,int) {}
inline int digitalRead(int) { return 1; }
struct SerialStub { template<class... T> void printf(const char*,T...) {} };
inline SerialStub Serial;
""",
            "esp_attr.h": "#pragma once\n#define IRAM_ATTR\n",
            "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n",
            "esp_types.h": "#pragma once\n#include <stdint.h>\n",
            "esp_log.h": "#pragma once\n#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n",
            "esp_timer.h": "#pragma once\n#include <stdint.h>\nstatic inline int64_t esp_timer_get_time(void) { return 0; }\n",
            "esp_heap_caps.h": """#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#ifdef __cplusplus
extern "C" {
#endif
extern int allocation_calls, fail_allocation;
#ifdef __cplusplus
}
#endif
static inline void* heap_caps_malloc(size_t size,int caps) {
  (void)caps; return ++allocation_calls == fail_allocation ? 0 : malloc(size);
}
static inline void* heap_caps_aligned_alloc(size_t align,size_t size,int caps) {
  if (++allocation_calls == fail_allocation) return 0;
  (void)caps; void* p=0; return posix_memalign(&p,align,size)==0 ? p : 0;
}
static inline void heap_caps_free(void* p) { free(p); }
""",
            "driver/gpio.h": "#pragma once\ntypedef int gpio_num_t;\n",
            "xtensa/core-macros.h": "#pragma once\n",
            "sdkconfig.h": "#pragma once\n#define CONFIG_IDF_TARGET_ESP32S3 1\n",
            "BoardConfig.h": """#pragma once
#define FREEINK_DRIVER_EPDIY_LCD 1
namespace BoardConfig {
struct Profile { unsigned short displayWidth,displayHeight; };
inline constexpr Profile ACTIVE{16,2};
}
""",
            "BatteryMonitor.h": "#pragma once\nnamespace BatteryMonitor { template<class T> void setPmuBatteryHook(T) {} }\n",
            "InputManager.h": "#pragma once\nnamespace InputManager { enum {BTN_UP,BTN_BACK,BTN_DOWN,BTN_POWER}; template<class T> void setButtonHook(T) {} }\n",
            "Rtc.h": "#pragma once\n#include <stdint.h>\nnamespace Rtc { struct Hooks { bool (*get)(uint32_t&,bool&); bool (*set)(uint32_t); }; inline void setPmuTimeHooks(Hooks) {} }\n",
            "freertos/FreeRTOS.h": "#pragma once\n#include <cassert>\n#define pdTRUE 1\n#define portMAX_DELAY 0\n#define configASSERT assert\n",
            "freertos/semphr.h": """#pragma once
#include <mutex>
using StaticSemaphore_t=std::recursive_mutex;
using SemaphoreHandle_t=StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* m) { return m; }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t m,int) { m->lock(); return 1; }
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t m) { m->unlock(); }
""",
        }
        for name, text in stubs.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        display = root / "display"
        shutil.copytree(SDK / "libs/display/FreeInkDisplay", display)
        (display / "src/bus/EpdBus.h").write_text(
            "#pragma once\n#include <cstdint>\nnamespace freeink { enum class BusyPolarity {ActiveLow}; class EpdBus {}; }\n")
        includes = [root, LCD / "include", LCD / "src", LCD / "src/epdiy/src",
                    LCD / "src/epdiy/include", LCD / "src/e0470/include",
                    display / "include", display / "src/driver"]
        flags = ["-I" + str(path) for path in includes]
        highlevel = root / "highlevel.o"
        queue = root / "queue.o"
        subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200112L", *flags, "-c",
                        str(LCD / "src/epdiy/src/highlevel.c"), "-o", str(highlevel)], check=True)
        subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200112L", *flags, "-c",
                        str(LCD / "src/epdiy/src/output_common/line_queue.c"), "-o", str(queue)], check=True)
        queue_check = root / "queue-check.o"
        subprocess.run(["cc", "-std=c11", *flags, "-c", str(HERE / "queue.c"),
                        "-o", str(queue_check)], check=True)
        for diagnostics in (0, 1):
            binary = root / f"display-test-{diagnostics}"
            subprocess.run(["c++", "-std=c++20", *flags, "-DFREEINK_EPDIY_LCD_CONFIG=testConfig",
                        f"-DFREEINK_READPICO_DIAGNOSTICS={diagnostics}",
                            str(HERE / "transactions.cpp"), str(LCD / "src/EpdiyLcd.cpp"),
                            str(display / "src/driver/EpdiyLcdDriver.cpp"), str(highlevel), str(queue), str(queue_check),
                            "-Wl,--wrap=calloc", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            subprocess.run([str(binary), "boot-power-failure"], check=True)
            subprocess.run([str(binary), "highlevel"], check=True)
            for allocation in range(1, 8):
                subprocess.run([str(binary), "oom", str(allocation)], check=True)
        (root / "Wire.h").write_text((HERE / "pmu_wire.h").read_text())
        (root / "esp_timer.h").write_text("""#pragma once
#include <stdint.h>
#ifdef __cplusplus
#include <chrono>
static inline int64_t esp_timer_get_time() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#else
static inline int64_t esp_timer_get_time(void) { return 0; }
#endif
""")
        board = SDK / "libs/hardware/BoardReadPico"
        binary = root / "pmu-test"
        subprocess.run(["c++", "-std=c++20", "-pthread", "-DFREEINK_READPICO_DIAGNOSTICS=1", "-I" + str(root),
                        "-DESP_PLATFORM", "-DFREEINK_DEVICE_READPICO=1",
                        "-I" + str(SDK / "libs/hardware/BoardConfig/include"),
                        "-I" + str(board / "include"), "-I" + str(board / "src"),
                        str(HERE / "pmu.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        from test_resources import run as run_resources
        run_resources(root, includes)
        from test_waveform_trim import run as run_waveform_trim
        run_waveform_trim()
        print("Read Pico display, resource failures, clear masks and concurrent PMU checks passed")


if __name__ == "__main__":
    run()
