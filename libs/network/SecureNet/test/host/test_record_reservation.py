"""Exercise production reservation/stop and pinned wolfSSL buffer functions.

WOLFSSL_INTERNAL_SOURCE must point at the consumer's Arduino-wolfSSL internal.c.
The buffer retention/release patches must already be applied. This is a host
allocator/lifetime test, not a TLS handshake or device memory measurement.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/SecureClient.cpp').read_text()
wolf = Path(os.environ['WOLFSSL_INTERNAL_SOURCE']).read_text()

def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]

harness = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
using byte = unsigned char;
using word32 = uint32_t;
#define STATIC_BUFFER_LEN 5
#define DTLS_RECORD_HEADER_SZ 13
#define MEMORY_E -125
#define BAD_FUNC_ARG -173
#define FORCED_FREE 1
#define MAX_RECORD_SIZE 16384
#define HAVE_MAX_FRAGMENT
#define DYNAMIC_TYPE_IN_BUFFER 0
#define WOLFSSL_MSG(x) ((void)0)
#define XMEMCPY memcpy
#define ForceZero(p, n) memset(p, 0, n)
#define IsEncryptionOn(s, r) 1
#define XMALLOC(n, h, t) alloc_buffer(n)
#define XFREE(p, h, t) free_buffer(p)
struct Header { size_t size; max_align_t alignment; };
static std::atomic<size_t> live{0}, allocations{0};
static size_t budget = 20000;
void* alloc_buffer(size_t n) {
  if (n > budget - live.load()) return nullptr;
  auto* h = static_cast<Header*>(malloc(sizeof(Header) + n));
  assert(h);
  h->size = n;
  live += n;
  ++allocations;
  return h + 1;
}
void free_buffer(void* p) {
  auto* h = static_cast<Header*>(p) - 1;
  live -= h->size;
  free(h);
}
struct Buffer {
  byte* buffer;
  byte staticBuffer[STATIC_BUFFER_LEN];
  word32 bufferSize, length, idx;
  byte dynamicFlag, offset;
};
struct WOLFSSL {
  struct { Buffer inputBuffer, clearOutputBuffer; } buffers{};
  struct { int dtls = WOLFSSL_GENERAL_ALIGNMENT != 0; } options;
  void* heap = nullptr;
  int max_fragment = MAX_RECORD_SIZE;
  WOLFSSL() {
    auto& b = buffers.inputBuffer;
    b.buffer = b.staticBuffer;
    b.bufferSize = STATIC_BUFFER_LEN;
  }
};
struct WOLFSSL_CTX {};
struct SerialStub {
  operator bool() const { return true; }
  template <typename... T> void printf(const char*, T...) {}
} Serial;
struct EspStub { unsigned getMaxAllocHeap() const { return budget - live; } } ESP;
void wolfSSL_CTX_free(WOLFSSL_CTX*) {}
class SecureClient {
 public:
  WOLFSSL session;
  void* _ssl = &session;
  void* _ctx = nullptr;
  bool _connected = true;
  struct { void stop() {} } _transport;
  void reserveFullRecord();
  void stop();
  ~SecureClient() { stop(); }
};
'''
checks = r'''
int main() {
  assert(kFullRecordCapacity == 16704);
  {
    SecureClient client;
#ifdef BASELINE
    // Before reservation: a medium record fits, then fragmentation leaves no
    // full-record block even after freeing the consumed medium record.
    client.reserveFullRecord();
    assert(live == 0);
    assert(GrowInputBuffer(&client.session, 3000, 0) == 0);
    client.session.buffers.inputBuffer.length = client.session.buffers.inputBuffer.idx = 3000;
    budget = 16000;
    assert(GrowInputBuffer(&client.session, 16416, 0) == MEMORY_E);
#else
    client.reserveFullRecord();
    const auto initialAllocs = allocations.load();
    auto* ptr = client.session.buffers.inputBuffer.buffer;
    assert(live >= 16704 && live <= 16720);
    // All medium and full records fit the same retained input buffer.
    for (word32 size : {2048u, 3966u, 16416u, 3000u}) {
      auto& b = client.session.buffers.inputBuffer;
      assert(size <= b.bufferSize);
      memset(b.buffer, 0x5a, size);
      b.length = b.idx = size;
      ShrinkInputBuffer(&client.session, 0);
      assert(b.buffer == ptr && allocations == initialAllocs);
    }
    client.reserveFullRecord();
    assert(allocations == initialAllocs);
    SecureClient other;
    other.reserveFullRecord();
    assert(!other.session.buffers.inputBuffer.dynamicFlag);
    other.stop(); // A non-owner cannot release another connection's admission.
    assert(recordReservationOwner.load() == &client);
#endif
    client.stop();
    assert(live == 0);
  }
#ifndef BASELINE
  budget = 1000;
  {
    SecureClient fail;
    fail.reserveFullRecord();
    assert(live == 0 && !recordReservationOwner.load());
    budget = 20000;
    fail.reserveFullRecord(); // Retry after allocation failure.
    assert(live >= 16704);
    fail._connected = false; // A read error/EOF must still clean up at stop.
    fail.stop();
    assert(live == 0 && !recordReservationOwner.load());
    fail.stop(); // Idempotent teardown.
  }
  for (int state = 0; state < 4; ++state) {
    SecureClient skip;
    if (state == 0) skip.session.max_fragment = 2048;
    if (state == 1) { skip.session.buffers.inputBuffer.length = 1; skip.session.buffers.inputBuffer.buffer[0] = 42; }
    if (state == 2) skip.session.buffers.clearOutputBuffer.length = 1;
    if (state == 3) skip._connected = false;
    skip.reserveFullRecord();
    assert(live == 0 && !recordReservationOwner.load());
    if (state == 1) assert(skip.session.buffers.inputBuffer.buffer[0] == 42);
  }
  // Two tasks racing admission can never both reserve. No global allocator is
  // installed, and a client on the same task cannot borrow another's memory.
  for (int trial = 0; trial < 100; ++trial) {
    SecureClient a, b;
    std::atomic<bool> start{false};
    std::thread t1([&] { while (!start.load()) {} a.reserveFullRecord(); });
    std::thread t2([&] { while (!start.load()) {} b.reserveFullRecord(); });
    start = true;
    t1.join(); t2.join();
    assert(!!a.session.buffers.inputBuffer.dynamicFlag != !!b.session.buffers.inputBuffer.dynamicFlag);
    a.stop(); b.stop();
    assert(live == 0 && !recordReservationOwner.load());
  }
#endif
}
'''
functions = function(wolf, 'void ShrinkInputBuffer(') + '\n' + function(wolf, 'int GrowInputBuffer(')
cleanup = '''
void wolfSSL_free(WOLFSSL* ssl) {
  if (ssl->buffers.inputBuffer.dynamicFlag) ShrinkInputBuffer(ssl, FORCED_FREE);
}
'''
start = source.index('std::atomic<SecureClient*> recordReservationOwner')
state = source[start:source.index('#endif', start)]
with tempfile.TemporaryDirectory(prefix='record-reservation-') as tmp:
    tmp = Path(tmp)
    for baseline in (True, False):
        for alignment in (0, 16):
            method = 'void SecureClient::reserveFullRecord() {}' if baseline else function(source, 'void SecureClient::reserveFullRecord()')
            (tmp / 'test.cpp').write_text(harness + functions + cleanup + state + method + function(source, 'void SecureClient::stop()') + checks)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-fsanitize=address,undefined', '-DFREEINK_WOLFSSL_RETAIN_INPUT_BUFFER',
                            '-DFREEINK_WOLFSSL_RELEASE_EMPTY_INPUT_BUFFER', f'-DWOLFSSL_GENERAL_ALIGNMENT={alignment}',
                            *(['-DBASELINE'] if baseline else []), str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True)
            print(f'record reservation: baseline={baseline}, alignment={alignment}: PASS')
