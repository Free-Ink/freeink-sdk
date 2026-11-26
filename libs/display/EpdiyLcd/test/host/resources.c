#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "epd_lcd.h"
#include "output_common/render_context.h"
#include "output_lcd/lcd_driver.h"
#include "output_lcd/render_lcd.h"
#include "render.h"

static int tasks, semaphores, board_live;
static bool fail_board;
static line_cb_func_t line_callback;
static frame_done_func_t frame_callback;
static void *line_payload, *frame_payload;
static unsigned char scan[2][4];
#include "resource_heap.h"
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
  SemaphoreHandle_t p = resource_allocate(sizeof(*p), 1);
  if (p) {
    ++semaphores;
    p->available = false;
  }
  return p;
}
void vSemaphoreDelete(SemaphoreHandle_t p) {
  assert(p);
  --semaphores;
  resource_release(p);
}
int xSemaphoreTake(SemaphoreHandle_t p, int timeout) {
  (void)timeout;
  assert(p && p->available);
  p->available = false;
  return 1;
}
int xSemaphoreGive(SemaphoreHandle_t p) {
  assert(p);
  p->available = true;
  return 1;
}
int xTaskCreatePinnedToCore(void (*fn)(void*), const char* name, unsigned stack, void* arg, unsigned prio,
                            TaskHandle_t* out, int core) {
  (void)fn;
  (void)name;
  (void)stack;
  (void)arg;
  (void)prio;
  (void)core;
  *out = resource_allocate(1, 1);
  if (!*out) return 0;
  ++tasks;
  return 1;
}
void vTaskDelete(TaskHandle_t p) {
  assert(p);
  --tasks;
  resource_release(p);
}
static bool init_board(uint32_t width) {
  assert(width == 16);
  board_live = 1;
  return !fail_board;
}
static void deinit_board(void) { board_live = 0; }
static void poweroff(epd_ctrl_state_t* state) { (void)state; }
static EpdBoardDefinition board = {.init = init_board, .deinit = deinit_board, .poweroff = poweroff};
static const EpdDisplay_t display = {16, 2, 16, 18, NULL};
const EpdBoardDefinition* epd_current_board(void) { return &board; }
const EpdDisplay_t* epd_get_display(void) { return &display; }
int epd_width(void) { return 16; }
int epd_height(void) { return 2; }
epd_ctrl_state_t* epd_ctrl_state(void) { return NULL; }
void epd_control_reg_init(void) {}
void epd_control_reg_deinit(void) {}
void epd_set_mode(bool mode) { (void)mode; }
void epd_lcd_line_source_cb(line_cb_func_t cb, void* arg) {
  line_callback = cb;
  line_payload = arg;
}
void epd_lcd_frame_done_cb(frame_done_func_t cb, void* arg) {
  frame_callback = cb;
  frame_payload = arg;
}
void epd_lcd_start_frame(void) {
  // Completion is synchronous here; production ISR uses exactly these callbacks.
  for (int i = 0; i < 2; i++) line_callback(line_payload, scan[i]);
  frame_callback(frame_payload);
}
void epd_apply_line_mask_VE(uint8_t* buf, const uint8_t* mask, int len) { epd_apply_line_mask(buf, mask, len); }
static void assert_empty(void) { assert(!live && !tasks && !semaphores && !board_live); }
int main(void) {
  fail_board = true;
  assert(!epd_renderer_init(EPD_OPTIONS_DEFAULT));
  epd_renderer_deinit();
  assert_empty();
  fail_board = false;
  assert(epd_renderer_init(EPD_OPTIONS_DEFAULT));
  const int allocations = calls;
  epd_renderer_deinit();
  assert_empty();
  for (int failure = 1; failure <= allocations; failure++) {
    calls = 0;
    fail_at = failure;
    assert(!epd_renderer_init(EPD_OPTIONS_DEFAULT));
    epd_renderer_deinit();
    assert_empty();
    calls = 0;
    fail_at = 0;
    assert(epd_renderer_init(EPD_OPTIONS_DEFAULT));
    // Exercise every horizontal crop, including both out-of-bounds edges.
    for (int x = -2; x <= 16; x++)
      for (int width = 0; width <= 18; width++) {
        const int before = calls;
        epd_push_pixels((EpdRect){x, 0, width, 2}, 15, 0);
        assert(calls == before);
        for (int row = 0; row < 2; row++)
          for (int pixel = 0; pixel < 16; pixel++) {
            const unsigned expected = (pixel >= x && pixel < x + width) ? 1 : 0;
            assert(((scan[row][pixel / 4] >> (2 * (pixel % 4))) & 3) == expected);
          }
      }
    epd_renderer_deinit();
    epd_renderer_deinit();
    assert_empty();
  }
}
