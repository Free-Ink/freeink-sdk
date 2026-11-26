"""Compile renderer lifecycle/clear code against recording FreeRTOS seams."""
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
LCD = HERE.parents[1]


def run(root, includes):
    stubs = {
        "freertos/FreeRTOS.h": '''#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0
#define configMAX_PRIORITIES 25
#define configASSERT assert
#define portYIELD_FROM_ISR() ((void)0)
typedef int BaseType_t;
typedef void* TaskHandle_t;
struct FakeSemaphore { bool available; };
typedef struct FakeSemaphore* SemaphoreHandle_t;
void* resource_allocate(size_t,size_t);
void resource_release(void*);
''',
        "freertos/semphr.h": '''#pragma once
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vSemaphoreDelete(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t,int);
int xSemaphoreGive(SemaphoreHandle_t);
static inline int xSemaphoreGiveFromISR(SemaphoreHandle_t p,BaseType_t* w) { *w=0;return xSemaphoreGive(p); }
''',
        "freertos/task.h": '''#pragma once
#include "FreeRTOS.h"
int xTaskCreatePinnedToCore(void(*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*,int);
void vTaskDelete(TaskHandle_t);
static inline unsigned ulTaskNotifyTake(int a,int b) { (void)a;(void)b;return 0; }
static inline void xTaskNotifyGive(TaskHandle_t p) { (void)p; }
static inline int xPortGetCoreID(void) { return 0; }
static inline void vTaskDelay(int t) { (void)t; }
''',
        "esp_heap_caps.h": '''#pragma once
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static inline void* heap_caps_malloc(size_t n,int caps) { (void)caps;return resource_allocate(n,1); }
static inline void* heap_caps_aligned_alloc(size_t a,size_t n,int caps) { (void)caps;return resource_allocate(n,a); }
static inline void heap_caps_free(void* p) { resource_release(p); }
''',
        "esp_idf_version.h": "#pragma once\n#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n#define ESP_IDF_VERSION 50400\n",
        "rom/cache.h": "#pragma once\n#define Cache_Start_DCache_Preload(...) ((void)0)\n",
    }
    for name, text in stubs.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    flags = ["-I" + str(p) for p in includes]
    sources = [HERE / "resources.c", LCD / "src/epdiy/src/render.c",
               LCD / "src/epdiy/src/output_lcd/render_lcd.c",
               LCD / "src/epdiy/src/output_common/render_context.c",
               LCD / "src/epdiy/src/output_common/line_queue.c",
               LCD / "src/epdiy/src/output_common/lut.c"]
    binary = root / "resources-test"
    subprocess.run(["cc", "-std=gnu11", "-ffunction-sections", "-fdata-sections", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast", *flags,
                    *(str(p) for p in sources), "-Wl,--gc-sections", "-Wl,--wrap=malloc",
                    "-Wl,--wrap=calloc", "-Wl,--wrap=free", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

    # Raw register operations are no-ops; allocation/IRQ/GDMA/GPIO APIs record
    # resource ownership and inject failures. Compile the entire real LCD driver.
    import re
    driver = LCD / "src/epdiy/src/output_lcd/lcd_driver.c"
    names = sorted(set(re.findall(r"\b((?:lcd_ll_|lcd_hal_|gpio_hal_|rmt_compat_|Cache_|esp_rom_)[A-Za-z0-9_]+)\s*\(", driver.read_text())))
    (root / "lcd_host_noops.h").write_text("#pragma once\n" + "".join(f"#define {name}(...) 0\n" for name in names))
    (root / "lcd_host.h").write_text((HERE / "lcd_host.h").read_text())
    headers = ["esp_intr_alloc.h", "hal/gpio_types.h", "esp_private/periph_ctrl.h",
               "soc/clk_tree_defs.h", "driver/gpio.h", "esp_check.h", "esp_lcd_panel_ops.h",
               "esp_lcd_panel_rgb.h", "esp_private/gdma.h", "hal/dma_types.h", "hal/gdma_ll.h",
               "hal/gpio_hal.h", "hal/lcd_hal.h", "hal/lcd_ll.h", "soc/lcd_periph.h"]
    for name in headers:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#pragma once\n#include "lcd_host.h"\n')
    heap = root / "esp_heap_caps.h"
    heap.write_text(heap.read_text() + """
#include <string.h>
#define MALLOC_CAP_DMA 4
static inline void* heap_caps_aligned_calloc(size_t a,size_t count,size_t n,int caps) {
    void* p=heap_caps_aligned_alloc(a,count*n,caps);if(p)memset(p,0,count*n);return p;
}
static inline void* heap_caps_calloc(size_t count,size_t n,int caps) { return heap_caps_aligned_calloc(1,count,n,caps); }
""")
    binary = root / "lcd-resources-test"
    subprocess.run(["cc", "-std=gnu11", "-ffunction-sections", "-fdata-sections", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast", *flags,
                    str(HERE / "lcd_resources.c"), str(driver), "-Wl,--gc-sections",
                    "-Wl,--wrap=malloc", "-Wl,--wrap=calloc", "-Wl,--wrap=free",
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
