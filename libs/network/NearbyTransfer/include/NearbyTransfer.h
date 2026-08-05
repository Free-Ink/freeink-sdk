#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace freeink::nearby {

constexpr size_t MAC_BYTES = 6;
constexpr size_t MAX_PACKET_BYTES = 1100;
constexpr size_t PACKET_HEADER_BYTES = 16;
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint16_t V2_CHUNK_BYTES = 1024;
constexpr uint16_t COMPAT_CHUNK_BYTES = 224;

enum class PacketType : uint8_t {
  Discover = 1,
  Advertise,
  Offer,
  Accept,
  Reject,
  Data,
  Ack,
  Complete,
  Result,
  Cancel,
};

struct PacketView {
  PacketType type = PacketType::Cancel;
  uint32_t sessionId = 0;
  uint32_t sequence = 0;
  const uint8_t* payload = nullptr;
  uint16_t payloadLength = 0;
};

bool encodePacket(uint8_t* output, size_t capacity, PacketType type, uint32_t sessionId, uint32_t sequence,
                  const void* payload, uint16_t payloadLength, size_t& outputLength);
bool decodePacket(const uint8_t* data, size_t length, PacketView& output);

uint16_t readU16(const uint8_t* data);
uint32_t readU32(const uint8_t* data);
uint64_t readU64(const uint8_t* data);
void writeU16(uint8_t* data, uint16_t value);
void writeU32(uint8_t* data, uint32_t value);
void writeU64(uint8_t* data, uint64_t value);
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length);

class ReliableTransferSession {
 public:
  enum class Role : uint8_t { None, Sender, Receiver };

  void reset();
  void begin(Role role, uint32_t sessionId, uint64_t totalBytes, uint16_t chunkBytes);
  bool acceptReceivedChunk(uint32_t sequence, size_t length);
  bool acceptAcknowledgement(uint32_t nextSequence);
  void advanceSentBytes(size_t length);
  void includeBytes(const uint8_t* data, size_t length);

  Role role() const { return role_; }
  uint32_t id() const { return sessionId_; }
  uint32_t nextSequence() const { return nextSequence_; }
  uint64_t transferredBytes() const { return transferredBytes_; }
  uint64_t totalBytes() const { return totalBytes_; }
  uint16_t chunkBytes() const { return chunkBytes_; }
  uint32_t crc32() const { return crc_ ^ 0xFFFFFFFFu; }

 private:
  Role role_ = Role::None;
  uint32_t sessionId_ = 0;
  uint32_t nextSequence_ = 0;
  uint64_t transferredBytes_ = 0;
  uint64_t totalBytes_ = 0;
  uint16_t chunkBytes_ = COMPAT_CHUNK_BYTES;
  uint32_t crc_ = 0xFFFFFFFFu;
};

class EspNowTransport {
 public:
  struct Event {
    std::array<uint8_t, MAC_BYTES> sourceMac{};
    std::array<uint8_t, MAX_PACKET_BYTES> data{};
    uint16_t length = 0;
  };

  EspNowTransport() = default;
  ~EspNowTransport();
  EspNowTransport(const EspNowTransport&) = delete;
  EspNowTransport& operator=(const EspNowTransport&) = delete;

  bool begin(uint8_t channel);
  void end();
  bool send(const uint8_t* destinationMac, const uint8_t* data, size_t length);
  bool poll(Event& event);
  bool localMac(uint8_t* output) const;
  bool started() const { return started_.load(std::memory_order_acquire); }
  bool overflowed() const { return overflow_.load(std::memory_order_acquire); }

  // Called only by the platform receive callback.
  void enqueueFromCallback(const uint8_t* sourceMac, const uint8_t* data, int length);

 private:
  static constexpr size_t EVENT_COUNT = 4;
  static constexpr uint32_t POLL_SHUTDOWN = 1u << 31;
  static constexpr uint32_t POLL_COUNT_MASK = ~POLL_SHUTDOWN;
  std::array<Event, EVENT_COUNT> events_{};
  [[maybe_unused]] void* eventMutex_ = nullptr;
  std::atomic<uint32_t> pollState_{POLL_SHUTDOWN};
  uint8_t eventHead_ = 0;
  uint8_t eventCount_ = 0;
  uint8_t channel_ = 0;
  std::atomic<bool> overflow_{false};
  std::atomic<bool> started_{false};
};

}  // namespace freeink::nearby
