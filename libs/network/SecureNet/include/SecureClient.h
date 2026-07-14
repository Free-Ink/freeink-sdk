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

namespace freeink {

class SecureClient : public Client {
 public:
  SecureClient() = default;
  ~SecureClient() override;

  // Certificate / verification configuration (applied before connect()).
  void setCACert(const char* rootCA);
  void setInsecure();  // skip peer verification (testing only)
  // Opt-in: when a CA is set and the handshake fails with a verification-class
  // error (untrusted/expired/self-signed/mismatched certificate), retry once
  // with verification disabled, logging a warning. Off by default — security-
  // critical callers (OTA) must leave this off so downloads fail closed.
  // Transport/protocol failures never trigger the fallback.
  void setAllowInsecureFallback(bool allow) { _allowInsecureFallback = allow; }
  // True if the last successful connect() ended on an unverified handshake
  // (via setInsecure() or the fallback above) — an audit hook for callers that
  // surface a "connection not verified" indicator.
  bool lastConnectWasInsecure() const { return _lastWasInsecure; }

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
  // One handshake attempt at a fixed TLS method and verification level.
  int connectWithMethod(const char* host, uint16_t port, void* method, const char* label, bool verifyPeer);
  // One connect attempt at a fixed verification level, incl. the TLS 1.2 retry.
  int connectAtVerify(const char* host, uint16_t port, bool verifyPeer);

  WiFiClient _transport;
  const char* _rootCA = nullptr;
  bool _insecure = false;
  bool _allowInsecureFallback = false;
  bool _lastWasInsecure = false;
  int _lastConnectErr = 0;  // wolfSSL_get_error() from the last failed handshake; 0 = none
  void* _ssl = nullptr;  // WOLFSSL* (opaque to keep wolfSSL headers out of here)
  void* _ctx = nullptr;  // WOLFSSL_CTX*
  bool _connected = false;
};

}  // namespace freeink
