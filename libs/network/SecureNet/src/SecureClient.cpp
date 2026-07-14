#include "SecureClient.h"

// wolfSSL is only pulled in when explicitly enabled. This keeps the default SDK
// build free of the wolfSSL dependency while leaving a single, well-defined
// integration point for the TLS 1.3 transport.
#if defined(FREEINK_NET_WOLFSSL)
#include <wolfssl/error-ssl.h>  // VERIFY_CERT_ERROR, DOMAIN_NAME_MISMATCH (SSL-layer codes)
#include <wolfssl/ssl.h>

// The Arduino-wolfSSL library's logging.c references this hook. It is normally
// defined in the library's wolfssl.h sketch glue, which is only compiled into
// sketch builds — a PlatformIO lib_deps build never compiles it and fails at
// link time with an undefined reference. Provide a weak default (routing to
// Serial) so SDK consumers link out of the box; an application that defines its
// own (e.g. routing into its logger) overrides this one. Signature must match
// wolfcrypt/logging.h exactly (int return).
extern "C" __attribute__((weak)) int wolfSSL_Arduino_Serial_Print(const char* const s) {
  if (s && Serial) Serial.printf("[wolfSSL] %s\n", s);
  return 0;
}
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
  if (n <= 0) {
    // A dead transport must surface as CONN_CLOSE: mapping it to WANT_WRITE
    // makes the handshake spin until the deadline instead of failing fast.
    if (!t->connected()) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WOLFSSL_CBIO_ERR_WANT_WRITE;
  }
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

bool isWantIo(const int err) {
  return err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE || err == WOLFSSL_CBIO_ERR_WANT_READ ||
         err == WOLFSSL_CBIO_ERR_WANT_WRITE;
}

// True if the wolfSSL error code is a peer-certificate-verification failure
// (as opposed to a transport/protocol failure). A verification failure is
// deterministic for a given server: retrying the handshake with a different
// TLS version (or any number of times) cannot change the outcome.
bool isVerificationError(const int err) {
  switch (err) {
    case ASN_NO_SIGNER_E:    // no trusted root for the chain
    case ASN_SIG_CONFIRM_E:  // signature check failed
    case ASN_BEFORE_DATE_E:  // notBefore in the future (device clock)
    case ASN_AFTER_DATE_E:   // expired
    case ASN_SELF_SIGNED_E:
    case CRL_CERT_DATE_ERR:
    case VERIFY_CERT_ERROR:  // generic certificate verification failure
    case DOMAIN_NAME_MISMATCH:
      return true;
    default:
      return false;
  }
}
}  // namespace

// One handshake attempt at a fixed TLS method and verification level.
int SecureClient::connectWithMethod(const char* host, uint16_t port, void* method, const char* label,
                                    bool verifyPeer) {
#if defined(FREEINK_WOLFSSL_DEBUG)
  // Routes wolfSSL's internal trace through wolfSSL_Arduino_Serial_Print (the
  // application provides that hook). Shows exactly where a handshake stalls.
  static bool debugEnabled = false;
  if (!debugEnabled) {
    wolfSSL_Debugging_ON();
    debugEnabled = true;
  }
#endif
  const uint32_t started = millis();
  stop();
  if (!_transport.connect(host, port)) {
    if (Serial) Serial.printf("[SecureClient] TCP connect failed (%s): %s:%u\n", label, host, port);
    return 0;
  }

  auto* ctx = wolfSSL_CTX_new(static_cast<WOLFSSL_METHOD*>(method));
  if (!ctx) {
    if (Serial) Serial.printf("[SecureClient] CTX alloc failed (%s), free heap %u\n", label, (unsigned)ESP.getFreeHeap());
    _transport.stop();
    return 0;
  }
  _ctx = ctx;

  if (!verifyPeer || _insecure) {
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, nullptr);
  } else if (_rootCA) {
    // A CA that fails to parse must fail the connect, not silently continue
    // against an empty trust store (every verified handshake would then fail
    // with a misleading no-signer error).
    if (wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(_rootCA), strlen(_rootCA),
                                       WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
      if (Serial) Serial.printf("[SecureClient] setCACert PEM did not parse (%s)\n", label);
      stop();
      return 0;
    }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, nullptr);
  }
  wolfSSL_SetIORecv(ctx, wcRecv);
  wolfSSL_SetIOSend(ctx, wcSend);

  auto* ssl = wolfSSL_new(ctx);
  if (!ssl) {
    if (Serial) Serial.printf("[SecureClient] SSL alloc failed (%s), free heap %u\n", label, (unsigned)ESP.getFreeHeap());
    stop();
    return 0;
  }
  _ssl = ssl;
  wolfSSL_SetIOReadCtx(ssl, &_transport);
  wolfSSL_SetIOWriteCtx(ssl, &_transport);
  wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, strlen(host));
  if (verifyPeer && !_insecure && _rootCA) {
    // Chain verification alone accepts ANY certificate signed by the trusted
    // roots, regardless of which server it was issued to. Also match the
    // hostname against the certificate's SAN/CN.
    wolfSSL_check_domain_name(ssl, host);
  }

  // The recv callback is non-blocking (returns WANT_READ when no bytes are
  // buffered), so wolfSSL_connect must be retried across handshake round-trips
  // rather than called once.
  const uint32_t deadline = millis() + 15000;
  int ret;
  while ((ret = wolfSSL_connect(ssl)) != WOLFSSL_SUCCESS) {
    const int err = wolfSSL_get_error(ssl, ret);
    if (!isWantIo(err)) {
      _lastConnectErr = err;
      if (Serial) Serial.printf("[SecureClient] wolfSSL_connect failed (%s): %d\n", label, err);
      stop();
      return 0;
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      _lastConnectErr = WOLFSSL_ERROR_WANT_READ;  // classify a timeout as transport, not verification
      if (Serial) {
        Serial.printf("[SecureClient] handshake timeout (%s): last err %d, transport %s, free heap %u\n", label, err,
                      _transport.connected() ? "up" : "down", (unsigned)ESP.getFreeHeap());
      }
      stop();
      return 0;
    }
    delay(5);
  }
  _connected = true;
  if (Serial) {
    Serial.printf("[SecureClient] handshake ok (%s): %s / %s in %lu ms\n", label, wolfSSL_get_version(ssl),
                  wolfSSL_get_cipher(ssl), (unsigned long)(millis() - started));
  }
  return 1;
}

// One connect attempt at a fixed verification level, including the TLS 1.2
// version-intolerance retry.
int SecureClient::connectAtVerify(const char* host, uint16_t port, bool verifyPeer) {
  // Negotiate the highest mutually supported version rather than pinning TLS 1.3:
  // self-hosted / Let's Encrypt nginx often tops out at TLS 1.2, and a 1.3-only
  // client fails those handshakes outright. v23 still selects 1.3 when the peer
  // offers it (WOLFSSL_TLS13 is enabled) and falls back to 1.2 otherwise.
  if (connectWithMethod(host, port, wolfSSLv23_client_method(), "auto", verifyPeer)) return 1;

  // A verification failure is deterministic: the same certificate fails the
  // same checks over TLS 1.2, so the retry below would only burn another
  // handshake (seconds of latency plus the ECC/RSA heap spike) to reach the
  // identical error.
  if (isVerificationError(_lastConnectErr)) return 0;

  // Some TLS 1.2-only servers are intolerant of a TLS 1.3-capable ClientHello
  // and abort with a fatal handshake_failure alert. Retry with an explicit
  // TLS 1.2 ClientHello before giving up.
  if (Serial) Serial.println("[SecureClient] retrying with TLS 1.2-only handshake");
  return connectWithMethod(host, port, wolfTLSv1_2_client_method(), "tls1.2", verifyPeer);
}

int SecureClient::connect(const char* host, uint16_t port) {
  // _lastConnectErr must not leak across connects: a TCP/DNS failure records no
  // handshake error, and a stale verification code from an earlier attempt
  // would misclassify it.
  _lastConnectErr = 0;
  _lastWasInsecure = false;

  // Explicitly insecure (setInsecure()): skip verification outright.
  if (_insecure) {
    const int ok = connectAtVerify(host, port, /*verifyPeer=*/false);
    _lastWasInsecure = ok == 1;
    return ok;
  }

  // Verified-first.
  if (connectAtVerify(host, port, /*verifyPeer=*/true)) return 1;

  // Only a verification-class failure can be helped by retrying without
  // verification; transport/protocol failures would fail the same way again.
  if (_allowInsecureFallback && isVerificationError(_lastConnectErr)) {
    if (Serial) {
      Serial.printf("[SecureClient] WARNING: certificate verify failed for %s (err %d); retrying WITHOUT verification\n",
                    host, _lastConnectErr);
    }
    const int ok = connectAtVerify(host, port, /*verifyPeer=*/false);
    _lastWasInsecure = ok == 1;
    return ok;
  }

  if (isVerificationError(_lastConnectErr) && Serial) {
    Serial.printf("[SecureClient] certificate verify failed for %s (err %d); failing closed\n", host, _lastConnectErr);
  }
  return 0;
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
  auto* ssl = static_cast<WOLFSSL*>(_ssl);
  const int n = wolfSSL_read(ssl, buf, size);
  if (n > 0) return n;

  const int err = wolfSSL_get_error(ssl, n);
  if (isWantIo(err)) return 0;
  if (err == WOLFSSL_ERROR_ZERO_RETURN) {
    _connected = false;
    return 0;
  }
  _connected = false;
  return -1;
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
