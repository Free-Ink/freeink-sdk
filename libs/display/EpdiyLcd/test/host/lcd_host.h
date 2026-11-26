#pragma once
#include <assert.h>
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int gpio_num_t;
typedef struct {
  void* dev;
} gpio_hal_context_t;
typedef struct {
  void* dev;
} lcd_hal_context_t;
typedef struct {
  unsigned integer, numerator, denominator;
} hal_utils_clk_div_t;
typedef struct {
  int mode;
  uint64_t pin_bit_mask;
} gpio_config_t;
typedef struct {
  struct {
    unsigned suc_eof, size, length, owner;
  } dw0;
  void* buffer;
  void* next;
} dma_descriptor_t;
typedef struct FakeChannel {
  bool connected;
}* gdma_channel_handle_t;
typedef struct {
  int direction;
} gdma_channel_alloc_config_t;
typedef struct {
  int psram_trans_align, sram_trans_align;
} gdma_transfer_ability_t;
typedef int gdma_trigger_t;
typedef int gdma_event_data_t;
typedef struct {
  bool (*on_trans_eof)(gdma_channel_handle_t, gdma_event_data_t*, void*);
} gdma_tx_event_callbacks_t;
typedef void* intr_handle_t;
typedef int portMUX_TYPE;
static const struct {
  struct {
    int irq_id;
  } panels[1];
} lcd_periph_rgb_signals = {0};
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(...) ((void)0)
#define taskEXIT_CRITICAL(...) ((void)0)
#define GPIO_HAL_GET_HW(...) 0
#define GPIO_PORT_0 0
#define GPIO_MODE_OUTPUT 1
#define PIN_FUNC_GPIO 0
#define PERIPH_LCD_CAM_MODULE 0
#define LCD_CLK_SRC_PLL240M 0
#define ESP_INTR_FLAG_IRAM 1
#define ESP_INTR_FLAG_INTRDISABLED 2
#define ESP_INTR_FLAG_SHARED 4
#define ESP_INTR_FLAG_LOWMED 8
#define LCD_LL_EVENT_VSYNC_END 1
#define LCD_LL_EVENT_TRANS_DONE 2
#define DMA_DESCRIPTOR_BUFFER_OWNER_CPU 0
#define DMA_DESCRIPTOR_BUFFER_MAX_SIZE 4092
#define GDMA_CHANNEL_DIRECTION_TX 0
#define GDMA_MAKE_TRIGGER(...) 0
#define ESP_RETURN_ON_ERROR(expr, tag, message) \
  do {                                          \
    int err_ = (expr);                          \
    if (err_ != ESP_OK) return err_;            \
  } while (0)
#define ESP_RETURN_ON_FALSE(cond, err, tag, message) \
  do {                                               \
    if (!(cond)) return err;                         \
  } while (0)
#define ESP_GOTO_ON_ERROR(expr, label, tag, message) \
  do {                                               \
    if ((expr) != ESP_OK) goto label;                \
  } while (0)
void periph_module_enable(int);
void periph_module_disable(int);
void periph_module_reset(int);
int esp_intr_alloc_intrstatus(int, int, uint32_t, uint32_t, void (*)(void*), void*, intr_handle_t*);
int esp_intr_disable(intr_handle_t);
int esp_intr_enable(intr_handle_t);
int esp_intr_free(intr_handle_t);
int gdma_new_channel(const gdma_channel_alloc_config_t*, gdma_channel_handle_t*);
int gdma_connect(gdma_channel_handle_t, gdma_trigger_t);
int gdma_reset(gdma_channel_handle_t);
int gdma_disconnect(gdma_channel_handle_t);
int gdma_del_channel(gdma_channel_handle_t);
int gdma_set_transfer_ability(gdma_channel_handle_t, const gdma_transfer_ability_t*);
int gdma_register_tx_event_callbacks(gdma_channel_handle_t, const gdma_tx_event_callbacks_t*, void*);
int gdma_start(gdma_channel_handle_t, intptr_t);
int gpio_set_direction(int, int);
int gpio_config(const gpio_config_t*);
int gpio_set_level(int, int);
int gpio_reset_pin(int);
// Declare RMT APIs before replacing raw register operations with test macros.
// clang-format off
#include "output_common/rmt_compat.h"
#include "lcd_host_noops.h"
// clang-format on
