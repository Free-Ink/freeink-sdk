#include "NearbyTransfer.h"

#include <algorithm>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace freeink::nearby {
namespace {
constexpr uint8_t MAGIC[4] = {'C', 'I', 'F', 'T'};

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
EspNowTransport* activeTransport = nullptr;

void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
  if (activeTransport && info) activeTransport->enqueueFromCallback(info->src_addr, data, length);
}
#endif
}  // namespace

uint16_t readU16(const uint8_t* data) { return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8); }

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readU64(const uint8_t* data) {
  return static_cast<uint64_t>(readU32(data)) | (static_cast<uint64_t>(readU32(data + 4)) << 32);
}

void writeU16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t* data, const uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) data[i] = static_cast<uint8_t>(value >> (i * 8));
}

void writeU64(uint8_t* data, const uint64_t value) {
  writeU32(data, static_cast<uint32_t>(value));
  writeU32(data + 4, static_cast<uint32_t>(value >> 32));
}

bool encodePacket(uint8_t* output, const size_t capacity, const PacketType type, const uint32_t sessionId,
                  const uint32_t sequence, const void* payload, const uint16_t payloadLength, size_t& outputLength) {
  outputLength = 0;
  if (!output || capacity < PACKET_HEADER_BYTES + payloadLength || (payloadLength > 0 && payload == nullptr) ||
      PACKET_HEADER_BYTES + payloadLength > MAX_PACKET_BYTES) {
    return false;
  }
  memcpy(output, MAGIC, sizeof(MAGIC));
  output[4] = PROTOCOL_VERSION;
  output[5] = static_cast<uint8_t>(type);
  writeU16(output + 6, payloadLength);
  writeU32(output + 8, sessionId);
  writeU32(output + 12, sequence);
  if (payloadLength > 0) memcpy(output + PACKET_HEADER_BYTES, payload, payloadLength);
  outputLength = PACKET_HEADER_BYTES + payloadLength;
  return true;
}

bool decodePacket(const uint8_t* data, const size_t length, PacketView& output) {
  if (!data || length < PACKET_HEADER_BYTES || length > MAX_PACKET_BYTES || memcmp(data, MAGIC, sizeof(MAGIC)) != 0 ||
      data[4] != PROTOCOL_VERSION) {
    return false;
  }
  const uint16_t payloadLength = readU16(data + 6);
  if (length != PACKET_HEADER_BYTES + payloadLength) return false;
  const uint8_t rawType = data[5];
  if (rawType < static_cast<uint8_t>(PacketType::Discover) || rawType > static_cast<uint8_t>(PacketType::Cancel)) {
    return false;
  }
  output.type = static_cast<PacketType>(rawType);
  output.sessionId = readU32(data + 8);
  output.sequence = readU32(data + 12);
  output.payload = data + PACKET_HEADER_BYTES;
  output.payloadLength = payloadLength;
  return true;
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, const size_t length) {
  if (!data) return crc;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

void ReliableTransferSession::reset() { *this = ReliableTransferSession{}; }

void ReliableTransferSession::begin(const Role role, const uint32_t sessionId, const uint64_t totalBytes,
                                    const uint16_t chunkBytes) {
  reset();
  role_ = role;
  sessionId_ = sessionId;
  totalBytes_ = totalBytes;
  chunkBytes_ = std::clamp<uint16_t>(chunkBytes, COMPAT_CHUNK_BYTES, V2_CHUNK_BYTES);
}

bool ReliableTransferSession::acceptReceivedChunk(const uint32_t sequence, const size_t length) {
  if (role_ != Role::Receiver || sequence != nextSequence_ || transferredBytes_ + length > totalBytes_) return false;
  transferredBytes_ += length;
  ++nextSequence_;
  return true;
}

bool ReliableTransferSession::acceptAcknowledgement(const uint32_t nextSequence) {
  if (role_ != Role::Sender || nextSequence != nextSequence_ + 1) return false;
  ++nextSequence_;
  return true;
}

void ReliableTransferSession::advanceSentBytes(const size_t length) {
  if (role_ == Role::Sender && transferredBytes_ + length <= totalBytes_) transferredBytes_ += length;
}

void ReliableTransferSession::includeBytes(const uint8_t* data, const size_t length) {
  crc_ = crc32Update(crc_, data, length);
}

EspNowTransport::~EspNowTransport() { end(); }

bool EspNowTransport::begin(const uint8_t channel) {
  end();
  channel_ = channel;
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  eventMutex_ = xSemaphoreCreateMutex();
  if (!eventMutex_) return false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  WiFi.setSleep(false);
  if (esp_wifi_set_channel(channel_, WIFI_SECOND_CHAN_NONE) != ESP_OK || esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK ||
      esp_now_init() != ESP_OK) {
    end();
    return false;
  }
  started_ = true;
  activeTransport = this;
  if (esp_now_register_recv_cb(onReceive) != ESP_OK) {
    end();
    return false;
  }
  pollState_.store(0, std::memory_order_release);
  return true;
#else
  return false;
#endif
}

void EspNowTransport::end() {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  const bool wasStarted = started_.load(std::memory_order_acquire);
#endif
  started_.store(false, std::memory_order_release);
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  pollState_.fetch_or(POLL_SHUTDOWN, std::memory_order_acq_rel);
  if (activeTransport == this) activeTransport = nullptr;
  if (wasStarted) {
    esp_now_unregister_recv_cb();
    esp_now_deinit();
  }
  if (eventMutex_) {
    while ((pollState_.load(std::memory_order_acquire) & POLL_COUNT_MASK) != 0) taskYIELD();
    const auto eventMutex = static_cast<SemaphoreHandle_t>(eventMutex_);
    if (xSemaphoreTake(eventMutex, portMAX_DELAY) == pdTRUE) xSemaphoreGive(eventMutex);
    vSemaphoreDelete(eventMutex);
    eventMutex_ = nullptr;
  }
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
#endif
  eventHead_ = 0;
  eventCount_ = 0;
  overflow_ = false;
}

bool EspNowTransport::send(const uint8_t* destinationMac, const uint8_t* data, const size_t length) {
  if (!started_ || !destinationMac || !data || length == 0 || length > MAX_PACKET_BYTES) return false;
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  if (!esp_now_is_peer_exist(destinationMac)) {
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, destinationMac, MAC_BYTES);
    peer.channel = channel_;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    const esp_err_t addResult = esp_now_add_peer(&peer);
    if (addResult != ESP_OK && addResult != ESP_ERR_ESPNOW_EXIST) return false;
  }
  return esp_now_send(destinationMac, data, length) == ESP_OK;
#else
  return true;
#endif
}

bool EspNowTransport::poll(Event& event) {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  uint32_t pollState = pollState_.load(std::memory_order_acquire);
  do {
    if ((pollState & POLL_SHUTDOWN) != 0) return false;
  } while (!pollState_.compare_exchange_weak(pollState, pollState + 1, std::memory_order_acquire,
                                             std::memory_order_relaxed));

  const auto eventMutex = static_cast<SemaphoreHandle_t>(eventMutex_);
  if (!eventMutex || xSemaphoreTake(eventMutex, portMAX_DELAY) != pdTRUE) {
    pollState_.fetch_sub(1, std::memory_order_release);
    return false;
  }
  if (!started_.load(std::memory_order_acquire)) {
    xSemaphoreGive(eventMutex);
    pollState_.fetch_sub(1, std::memory_order_release);
    return false;
  }
#endif
  const bool available = eventCount_ > 0;
  if (available) {
    event = events_[eventHead_];
    eventHead_ = static_cast<uint8_t>((eventHead_ + 1) % EVENT_COUNT);
    --eventCount_;
  }
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  xSemaphoreGive(eventMutex);
  pollState_.fetch_sub(1, std::memory_order_release);
#endif
  return available;
}

bool EspNowTransport::localMac(uint8_t* output) const {
  if (!output) return false;
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  return esp_wifi_get_mac(WIFI_IF_STA, output) == ESP_OK;
#else
  memset(output, 0, MAC_BYTES);
  return true;
#endif
}

void EspNowTransport::enqueueFromCallback(const uint8_t* sourceMac, const uint8_t* data, const int length) {
  if (!sourceMac || !data || length <= 0 || length > static_cast<int>(MAX_PACKET_BYTES)) return;
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  if (!eventMutex_ || xSemaphoreTake(static_cast<SemaphoreHandle_t>(eventMutex_), 0) != pdTRUE) {
    overflow_ = true;
    return;
  }
#endif
  if (eventCount_ >= EVENT_COUNT) {
    overflow_ = true;
  } else {
    Event& event = events_[(eventHead_ + eventCount_) % EVENT_COUNT];
    memcpy(event.sourceMac.data(), sourceMac, MAC_BYTES);
    memcpy(event.data.data(), data, static_cast<size_t>(length));
    event.length = static_cast<uint16_t>(length);
    ++eventCount_;
  }
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(eventMutex_));
#endif
}

}  // namespace freeink::nearby
