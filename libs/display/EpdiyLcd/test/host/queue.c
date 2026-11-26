#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "output_common/line_queue.h"

static size_t table_bytes;
void* __real_calloc(size_t, size_t);
void* __wrap_calloc(size_t count, size_t size) {
  table_bytes = count * size;
  return __real_calloc(count, size);
}

void check_queue(void) {
  LineQueue_t queue = lq_init(64, 304);
  assert(table_bytes == 64 * sizeof(uint8_t*));
  memset(lq_current(&queue), 0x5A, 304);
  lq_commit(&queue);
  uint8_t line[304];
  assert(lq_read(&queue, line) == 0 && line[303] == 0x5A);
  lq_free(&queue);
}
