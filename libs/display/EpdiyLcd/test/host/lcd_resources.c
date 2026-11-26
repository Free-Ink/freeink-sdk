#include <lcd_host.h>

#include "epd_lcd.h"
#include "resource_heap.h"
static int peripheral_live;
void periph_module_enable(int id) {
  (void)id;
  assert(!peripheral_live);
  peripheral_live = 1;
}
void periph_module_disable(int id) {
  (void)id;
  assert(peripheral_live);
  peripheral_live = 0;
}
void periph_module_reset(int id) { (void)id; }
int esp_intr_alloc_intrstatus(int source, int flags, uint32_t status, uint32_t mask, void (*isr)(void*), void* data,
                              intr_handle_t* out) {
  (void)source;
  (void)flags;
  (void)status;
  (void)mask;
  (void)isr;
  (void)data;
  *out = resource_allocate(1, 1);
  return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
int esp_intr_enable(intr_handle_t p) {
  assert(p);
  return ESP_OK;
}
int esp_intr_disable(intr_handle_t p) {
  assert(p);
  return ESP_OK;
}
int esp_intr_free(intr_handle_t p) {
  assert(p);
  resource_release(p);
  return ESP_OK;
}
int gdma_new_channel(const gdma_channel_alloc_config_t* cfg, gdma_channel_handle_t* out) {
  (void)cfg;
  *out = resource_allocate(sizeof(**out), 1);
  if (*out) (*out)->connected = false;
  return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
int gdma_connect(gdma_channel_handle_t p, gdma_trigger_t t) {
  (void)t;
  assert(p);
  if (!resource_step()) return ESP_FAIL;
  p->connected = true;
  return ESP_OK;
}
int gdma_reset(gdma_channel_handle_t p) {
  assert(p);
  return ESP_OK;
}
int gdma_disconnect(gdma_channel_handle_t p) {
  assert(p && p->connected);
  p->connected = false;
  return ESP_OK;
}
int gdma_del_channel(gdma_channel_handle_t p) {
  assert(p && !p->connected);
  resource_release(p);
  return ESP_OK;
}
int gdma_set_transfer_ability(gdma_channel_handle_t p, const gdma_transfer_ability_t* a) {
  (void)a;
  assert(p);
  return resource_step() ? ESP_OK : ESP_FAIL;
}
int gdma_register_tx_event_callbacks(gdma_channel_handle_t p, const gdma_tx_event_callbacks_t* cb, void* data) {
  (void)cb;
  (void)data;
  assert(p);
  return resource_step() ? ESP_OK : ESP_FAIL;
}
int gdma_start(gdma_channel_handle_t p, intptr_t address) {
  (void)address;
  assert(p);
  return ESP_OK;
}
int gpio_set_direction(int pin, int mode) {
  (void)pin;
  (void)mode;
  return resource_step() ? ESP_OK : ESP_FAIL;
}
int gpio_config(const gpio_config_t* cfg) {
  (void)cfg;
  return resource_step() ? ESP_OK : ESP_FAIL;
}
int gpio_set_level(int pin, int level) {
  (void)pin;
  (void)level;
  return resource_step() ? ESP_OK : ESP_FAIL;
}
int gpio_reset_pin(int pin) {
  (void)pin;
  return ESP_OK;
}
int main(void) {
  LcdEpdConfig_t cfg = {.pixel_clock = 24000000, .bus_width = 16, .line = {2, 2, 2, 1}};
  assert(epd_lcd_init(&cfg, 1216, 684) == ESP_OK);
  int total = calls;
  epd_lcd_deinit();
  epd_lcd_deinit();
  assert(!live && !peripheral_live);
  for (int i = 1; i <= total; i++) {
    calls = 0;
    fail_at = i;
    assert(epd_lcd_init(&cfg, 1216, 684) != ESP_OK);
    epd_lcd_deinit();
    epd_lcd_deinit();
    assert(!live && !peripheral_live);
    fail_at = 0;
    calls = 0;
    assert(epd_lcd_init(&cfg, 1216, 684) == ESP_OK);
    epd_lcd_deinit();
    assert(!live && !peripheral_live);
  }
}
