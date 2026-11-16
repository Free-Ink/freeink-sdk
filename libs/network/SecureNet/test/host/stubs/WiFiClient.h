#pragma once
// Host stub: each connect() takes the next scripted server reply; the reply's
// bytes are served and then the peer "closes", so a reply shorter than its
// Content-Length is a mid-body drop. Requests are recorded for assertions.
#include <Client.h>

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

struct FakeNet {
  static unsigned long& readDelay() {
    static unsigned long ms = 0;
    return ms;
  }
  static size_t& bytesRead() {
    static size_t n = 0;
    return n;
  }
  static bool& holdOpen() {
    static bool hold = false;
    return hold;
  }
  static std::deque<std::string>& replies() {
    static std::deque<std::string> r;
    return r;
  }
  static std::vector<std::string>& requests() {
    static std::vector<std::string> r;
    return r;
  }
  static std::vector<std::string>& hosts() {
    static std::vector<std::string> h;
    return h;
  }
};

class WiFiClient : public Client {
 public:
  int connect(IPAddress, uint16_t) override { return 0; }
  int connect(const char* host, uint16_t) override {
    if (FakeNet::replies().empty()) return 0;
    FakeNet::hosts().push_back(host);
    reply_ = FakeNet::replies().front();
    FakeNet::replies().pop_front();
    pos_ = 0;
    open_ = true;
    FakeNet::requests().emplace_back();
    return 1;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t size) override {
    FakeNet::requests().back().append(reinterpret_cast<const char*>(buf), size);
    return size;
  }
  int available() override { return open_ ? static_cast<int>(reply_.size() - pos_) : 0; }
  int read() override {
    if (available() <= 0) return -1;
    delay(FakeNet::readDelay());
    ++FakeNet::bytesRead();
    return static_cast<uint8_t>(reply_[pos_++]);
  }
  int read(uint8_t* buf, size_t size) override {
    const size_t n = std::min(size, static_cast<size_t>(available()));
    memcpy(buf, reply_.data() + pos_, n);
    pos_ += n;
    return n ? static_cast<int>(n) : -1;
  }
  int peek() override { return available() > 0 ? static_cast<uint8_t>(reply_[pos_]) : -1; }
  void flush() override {}
  void stop() override { open_ = false; }
  uint8_t connected() override { return open_ && (FakeNet::holdOpen() || pos_ < reply_.size()); }
  operator bool() override { return connected(); }
  void setConnectionTimeout(unsigned long) {}

 private:
  std::string reply_;
  size_t pos_ = 0;
  bool open_ = false;
};
