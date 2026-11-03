#pragma once

// FreeInk SDK — TLS 1.3 secure client.
//
// WHY: the precompiled mbedTLS shipped in the ESP-IDF/pioarduino package has
// TLS 1.3 compiled out as empty stubs (PSA crypto prerequisites disabled), so
// WiFiClientSecure / esp_http_client cannot reach TLS-1.3-only servers
// (e.g. KOSync at kosync.ak-team.com:3042 — handshake fails with
// -0x7780 MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE). A -D Kconfig flag can't change a
// precompiled .a, and a custom_sdkconfig rebuild fails on managed-component
// dependencies. The only fix that doesn't rebuild ESP-IDF is to bring our own
// TLS stack compiled from source: wolfSSL, which supports TLS 1.3 + PSA.
//
// SecureClient is an Arduino Client wrapping a wolfSSL session over a plain
// WiFiClient transport, independent of system mbedTLS.
//
// OPT-IN: enable with -DFREEINK_NET_WOLFSSL=1 and add wolfSSL to lib_deps. With
// the flag off, this compiles to an inert no-op (connectSecure() returns false)
// so the rest of the SDK builds without the wolfSSL dependency present.

#include <Arduino.h>
#include <Client.h>
#include <WiFiClient.h>

#include <functional>
#include <utility>

namespace freeink {

// True when `host` (a name or an IP literal) resolves; logs a failure.
// Arduino-ESP32 3.3.x NetworkClient::connect(host) treats a failed lookup as
// success and waits out the connect timeout on 0.0.0.0, so check this first.
bool hostResolves(const char* host);

class SecureClient : public Client {
 public:
  SecureClient() = default;
  ~SecureClient() override;

  // Return true to abandon an in-progress connect.
  using AbortCallback = std::function<bool()>;

  // Certificate / verification configuration (applied before connect()).
  void setCACert(const char* rootCA);
  void setInsecure();  // skip peer verification (testing only)

  // Polled during connect(): between the TCP connect and the handshake, on
  // every handshake round-trip, and between the two handshake attempts. A
  // caller that drives its own input loop can use this both to cancel and to
  // keep that loop running, the same way the read paths already do.
  //
  // It cannot interrupt the blocking TCP connect itself, so the longest
  // uninterruptible stretch is one setConnectionTimeout() window rather than
  // the four that a failing connect() can otherwise spend.
  void setAbortCallback(AbortCallback cb) { _shouldAbort = std::move(cb); }

  // Bulk downloads: pre-grow this connection's wolfSSL input buffer after
  // request text has been sent/freed, before reading response headers. Requires
  // the consumer's verified FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER patch; otherwise
  // a no-op. Skips negotiated small records, pending data, and a concurrent
  // reservation. OOM falls back to ordinary record growth. wolfSSL owns/frees
  // the buffer, including on cancellation/error. Client calls must be serialized.
#if defined(FREEINK_NET_WOLFSSL)
  void reserveFullRecord();
#else
  void reserveFullRecord() {}
#endif

  // True when the last connect() returned because the abort callback fired
  // rather than because the connection failed.
  bool aborted() const { return _aborted; }

  // Connect and perform a TLS 1.3 handshake to host:port (uses the SNI host).
  int connect(IPAddress ip, uint16_t port) override;
  int connect(const char* host, uint16_t port) override;

  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buf, size_t size) override;
  int available() override;
  int read() override;
  int read(uint8_t* buf, size_t size) override;
  int peek() override;
  void flush() override;
  void stop() override;
  uint8_t connected() override;
  operator bool() override { return connected(); }

  // True if the library was built with wolfSSL TLS 1.3 support enabled.
  static bool tls13Available();

 private:
  int connectWithMethod(const char* host, uint16_t port, void* method, const char* label);

  // Polls _shouldAbort and latches _aborted so callers can tell a cancel from
  // a failure after connect() returns 0.
  bool abortRequested();

  WiFiClient _transport;
  AbortCallback _shouldAbort;
  bool _aborted = false;
  bool _tcpConnected = false;  // the last connectWithMethod() reached the handshake
  const char* _rootCA = nullptr;
  bool _insecure = false;
  void* _ssl = nullptr;  // WOLFSSL* (opaque to keep wolfSSL headers out of here)
  void* _ctx = nullptr;  // WOLFSSL_CTX*
  bool _connected = false;
};

}  // namespace freeink
