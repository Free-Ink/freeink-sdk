#pragma once
#include <array>
#include <cassert>
#include <cstring>
#include <vector>

struct RecordingWire {
  std::vector<uint8_t> tx;
  std::array<uint8_t, 64> response{};
  std::array<uint8_t, 64> received{};
  unsigned index = 0, commands = 0;
  uint8_t reg = 0;
  bool inFlight = false;
  void begin(int, int) {}
  void setClock(int) {}
  void setTimeOut(int) {}
  void beginTransmission(uint8_t) { tx.clear(); }
  void write(uint8_t byte) { tx.push_back(byte); }
  void write(const uint8_t* p, size_t n) { tx.insert(tx.end(), p, p + n); }
  uint8_t endTransmission(bool = true) {
    reg = tx[0];
    if (reg != 0x80) return 0;
    assert(!inFlight);  // a second command must wait until the response is consumed
    inFlight = true;
    ++commands;
    response.fill(0);
    response[0] = 0xA5;
    response[6] = tx[7];
    response[7] = tx[8];
    if (commands == 1) response[10] = 0x17;  // stale session exercises recovery
    const uint16_t code = tx[9] | (uint16_t(tx[10]) << 8);
    if (code == 0x0007) {
      response[12] = 8;
      response[18] = 0x78;
      response[19] = 0x56;
      response[20] = 0x34;
      response[21] = 0x12;
      response[24] = 1;
    } else if (code == 0x0510) {
      response[12] = 4;
      response[18] = 0xDC;
      response[19] = 0x05;  // 1500 mV
      response[20] = 1;
    }
    appendCrc(response, 62);
    return 0;
  }
  uint8_t requestFrom(uint8_t, uint8_t n) {
    index = 0;
    received = response;
    if (reg == 0x00) {
      received.fill(0);
      received[0] = 'P';
      received[1] = 'M';
      received[2] = 'U';
      received[12] = 1;
      appendCrc(received, 30);
    } else if (reg == 0x20) {
      received.fill(0);
      received[2] = 3;  // RUNNING, power key held
      received[4] = 1 << 5;
      received[40] = uint8_t(commands);
      appendCrc(received, 62);
    }
    return n;
  }
  int available() { return 64 - index; }
  int read() {
    const int value = received[index++];
    if (reg == 0x81 && index == 64) inFlight = false;
    return value;
  }
  static void appendCrc(std::array<uint8_t, 64>& data, unsigned length) {
    uint16_t crc = 0xFFFF;
    for (unsigned i = 0; i < length; ++i) {
      crc ^= uint16_t(data[i]) << 8;
      for (int bit = 0; bit < 8; ++bit) crc = crc & 0x8000 ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    data[length] = uint8_t(crc);
    data[length + 1] = uint8_t(crc >> 8);
  }
};
inline RecordingWire Wire;
