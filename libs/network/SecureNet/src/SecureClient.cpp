#include "SecureClient.h"

// wolfSSL is only pulled in when explicitly enabled. This keeps the default SDK
// build free of the wolfSSL dependency while leaving a single, well-defined
// integration point for the TLS 1.3 transport.
#if defined(FREEINK_NET_WOLFSSL)
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#endif

namespace freeink {

bool SecureClient::tls13Available() {
#if defined(FREEINK_NET_WOLFSSL)
  return true;
#else
  return false;
#endif
}

SecureClient::~SecureClient() { stop(); }

void SecureClient::setCACert(const char* rootCA) { _rootCA = rootCA; }
void SecureClient::setInsecure() { _insecure = true; }

#if defined(FREEINK_NET_WOLFSSL)

namespace {
// Bridge wolfSSL's I/O to the underlying WiFiClient transport.
int wcSend(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
  auto* t = static_cast<WiFiClient*>(ctx);
  const int n = t->write(reinterpret_cast<const uint8_t*>(buf), sz);
  if (n <= 0) return WOLFSSL_CBIO_ERR_WANT_WRITE;
  return n;
}
int wcRecv(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
  auto* t = static_cast<WiFiClient*>(ctx);
  if (!t->connected() && t->available() == 0) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
  if (t->available() == 0) return WOLFSSL_CBIO_ERR_WANT_READ;
  const int n = t->read(reinterpret_cast<uint8_t*>(buf), sz);
  if (n <= 0) return WOLFSSL_CBIO_ERR_WANT_READ;
  return n;
}
}  // namespace

int SecureClient::connect(const char* host, uint16_t port) {
  stop();
  if (!_transport.connect(host, port)) return 0;

  WOLFSSL_METHOD* method = wolfTLSv1_3_client_method();
  auto* ctx = wolfSSL_CTX_new(method);
  if (!ctx) { _transport.stop(); return 0; }
  _ctx = ctx;

  if (_insecure) {
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, nullptr);
  } else if (_rootCA) {
    wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(_rootCA),
                                   strlen(_rootCA), WOLFSSL_FILETYPE_PEM);
  }
  wolfSSL_SetIORecv(ctx, wcRecv);
  wolfSSL_SetIOSend(ctx, wcSend);

  auto* ssl = wolfSSL_new(ctx);
  if (!ssl) { stop(); return 0; }
  _ssl = ssl;
  wolfSSL_SetIOReadCtx(ssl, &_transport);
  wolfSSL_SetIOWriteCtx(ssl, &_transport);
  wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, strlen(host));

  // The recv callback is non-blocking (returns WANT_READ when no bytes are
  // buffered), so wolfSSL_connect must be retried across handshake round-trips
  // rather than called once.
  const uint32_t deadline = millis() + 15000;
  int ret;
  while ((ret = wolfSSL_connect(ssl)) != WOLFSSL_SUCCESS) {
    const int err = wolfSSL_get_error(ssl, ret);
    if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) {
      if (Serial) Serial.printf("[SecureClient] wolfSSL_connect failed: %d\n", err);
      stop();
      return 0;
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      if (Serial) Serial.printf("[SecureClient] handshake timeout\n");
      stop();
      return 0;
    }
    delay(5);
  }
  _connected = true;
  return 1;
}

int SecureClient::connect(IPAddress ip, uint16_t port) {
  char host[16];
  snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return connect(host, port);
}

size_t SecureClient::write(const uint8_t* buf, size_t size) {
  if (!_connected) return 0;
  const int n = wolfSSL_write(static_cast<WOLFSSL*>(_ssl), buf, size);
  return n > 0 ? static_cast<size_t>(n) : 0;
}

int SecureClient::read(uint8_t* buf, size_t size) {
  if (!_connected) return -1;
  return wolfSSL_read(static_cast<WOLFSSL*>(_ssl), buf, size);
}

int SecureClient::available() {
  if (!_connected) return 0;
  return wolfSSL_pending(static_cast<WOLFSSL*>(_ssl)) + _transport.available();
}

void SecureClient::stop() {
  if (_ssl) { wolfSSL_free(static_cast<WOLFSSL*>(_ssl)); _ssl = nullptr; }
  if (_ctx) { wolfSSL_CTX_free(static_cast<WOLFSSL_CTX*>(_ctx)); _ctx = nullptr; }
  _transport.stop();
  _connected = false;
}

uint8_t SecureClient::connected() { return _connected && _transport.connected(); }

#else  // !FREEINK_NET_WOLFSSL — inert stub so the SDK builds without wolfSSL.

int SecureClient::connect(const char* host, uint16_t port) {
  (void)host; (void)port;
  if (Serial) Serial.println("[SecureClient] TLS 1.3 unavailable: build with -DFREEINK_NET_WOLFSSL=1");
  return 0;
}
int SecureClient::connect(IPAddress ip, uint16_t port) { (void)ip; (void)port; return 0; }
size_t SecureClient::write(const uint8_t* buf, size_t size) { (void)buf; (void)size; return 0; }
int SecureClient::read(uint8_t* buf, size_t size) { (void)buf; (void)size; return -1; }
int SecureClient::available() { return 0; }
void SecureClient::stop() { _transport.stop(); _connected = false; }
uint8_t SecureClient::connected() { return 0; }

#endif

// --- transport-agnostic single-byte helpers (shared) ---
size_t SecureClient::write(uint8_t b) { return write(&b, 1); }
int SecureClient::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}
int SecureClient::peek() { return -1; }
void SecureClient::flush() {}

}  // namespace freeink
