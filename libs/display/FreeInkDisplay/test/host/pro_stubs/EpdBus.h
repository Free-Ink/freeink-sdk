#pragma once
#include <cassert>
#include <cstdint>
#include <vector>
namespace freeink {
enum class BusyPolarity { ActiveHigh, ActiveLow, X3TwoPhase, UcIdleHigh, ActiveLowReady };
struct EpdPins { int8_t sclk, mosi, cs, dc, rst, busy, powerEnable=-1; };
class EpdBus {
 public:
  struct Write { uint8_t command; std::vector<uint8_t> bytes; unsigned transactions=0; };
  class Transaction {
   public:
    Transaction() = default;
    explicit Transaction(EpdBus& bus) : _bus(&bus) {
      assert(!_bus->transaction);
      _bus->transaction = true;
      if (!_bus->writes.empty()) ++_bus->writes.back().transactions;
    }
    ~Transaction() { end(); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept : _bus(other._bus) { other._bus = nullptr; }
    Transaction& operator=(Transaction&& other) noexcept {
      if (this != &other) {
        end();
        _bus = other._bus;
        other._bus = nullptr;
      }
      return *this;
    }
    void end() {
      if (_bus) {
        assert(_bus->transaction);
        _bus->transaction = false;
        _bus = nullptr;
      }
    }
    bool isActive() const { return _bus != nullptr; }
    void cmd(uint8_t c) { _bus->writes.push_back({c, {}, 1}); }
    void data(uint8_t d) { writeBytes(&d, 1); }
    void writeBytes(const uint8_t* p, uint16_t n) {
      assert(_bus && _bus->transaction && !_bus->writes.empty());
      _bus->writes.back().bytes.insert(_bus->writes.back().bytes.end(), p, p + n);
    }
    void writeDataChunk(const uint8_t* p, size_t n) {
      while (n > 0) {
        const uint16_t step = n > 32768u ? 32768u : static_cast<uint16_t>(n);
        writeBytes(p, step);
        p += step;
        n -= step;
      }
    }
   private:
    EpdBus* _bus = nullptr;
  };
  std::vector<Write> writes;
  bool transaction=false;
  unsigned waits=0;
  std::vector<size_t> busyWaitWriteCounts;
  using StreamChunkCallback = size_t (*)(void* ctx, const uint8_t** outChunk);
  void clear() { assert(!transaction); writes.clear(); waits=0; busyWaitWriteCounts.clear(); }
  void begin(const EpdPins&, uint32_t, BusyPolarity, int8_t=-1, int8_t=-1) {}
  void cmd(uint8_t c) { assert(!transaction); writes.push_back({c,{}}); }
  void data(uint8_t b) { data(&b,1); }
  void data(const uint8_t* p, uint16_t n) {
    assert(!transaction); ++writes.back().transactions;
    writes.back().bytes.insert(writes.back().bytes.end(),p,p+n);
  }
  Transaction beginTxn() { return Transaction(*this); }
  Transaction beginDataTxn(uint8_t c) { cmd(c); return beginTxn(); }
  size_t sendStream(uint8_t c, StreamChunkCallback nextChunk, void* ctx) {
    if (!nextChunk) return 0;
    auto txn = beginDataTxn(c);
    size_t total = 0;
    for (;;) {
      const uint8_t* chunk = nullptr;
      const size_t n = nextChunk(ctx, &chunk);
      if (n == 0 || !chunk) break;
      txn.writeDataChunk(chunk, n);
      total += n;
    }
    return total;
  }
  void fillPlane(uint8_t c, uint8_t v, uint16_t h, uint16_t wb) {
    cmd(c); auto txn = beginTxn(); writes.back().bytes.assign(size_t(h)*wb,v);
  }
  void cmdData2(uint8_t c, uint8_t a, uint8_t b) { cmd(c); data(a); data(b); }
  void cmdData(uint8_t c, const uint8_t* p, uint16_t n) { cmd(c); data(p,n); }
  void reset(uint16_t=0) {}
  void waitBusy(const char* =nullptr) { assert(!transaction); ++waits; busyWaitWriteCounts.push_back(writes.size()); }
  void waitBusy(BusyPolarity, const char* tag = nullptr) { waitBusy(tag); }
  void waitRefreshComplete(const char* =nullptr) { assert(!transaction); ++waits; }
  bool isBusy() const { return false; }
  EpdPins pins() const { return {}; }
  void setBusyWaitHooks(void (*)(), void (*)()) {}
  void setBusyWaitSliceHook(bool (*)(int8_t,uint8_t)) {}
};
}
