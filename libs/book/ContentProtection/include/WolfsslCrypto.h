#pragma once

// FreeInk — wolfSSL crypto backend.
//
// Compiled only when FREEINK_CONTENT_WOLFSSL is defined (CrossPoint sets it
// alongside FREEINK_NET_WOLFSSL). wolfSSL build needs: RSA (+ PKCS#8
// traditional decode), raw RSA ops, AES-CBC, SHA-1/SHA-256, PKCS#12 parse.
// No TLS, no cert store, no keygen certs — see the implementation notes.

#include "Crypto.h"

#ifdef FREEINK_CONTENT_WOLFSSL

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>

namespace freeink {
namespace content {

class WolfsslCrypto : public Crypto {
 public:
  WolfsslCrypto();
  ~WolfsslCrypto() override;

  // Diagnostics: set by the failing primitive (surfaced in firmware logs and
  // the web status page).
  std::string lastError;

  int32_t rsaPrivateRaw(const uint8_t* pkcs8Der, size_t pkcs8Len, const uint8_t* in, size_t inLen,
                        uint8_t* out, size_t outCap) override;
  bool aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len,
                        uint8_t* out) override;
  void sha1(const uint8_t* data, size_t len, uint8_t out[20]) override;
  void sha256(const uint8_t* data, size_t len, uint8_t out[32]) override;

  bool rsaGenerate(RsaKeyPairDer* out) override;
  bool rsaPublicEncrypt(const uint8_t* certDer, size_t certLen, const uint8_t* in, size_t inLen,
                        uint8_t* out, size_t outCap, size_t* outLen) override;
  bool rsaPrivateSignRaw(const uint8_t* pkcs8Der, size_t pkcs8Len, const uint8_t hash[20],
                         uint8_t out[128]) override;
  bool aes128CbcEncrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len,
                        uint8_t* out) override;
  bool pkcs12Extract(const uint8_t* p12, size_t len, const std::string& password,
                     std::vector<uint8_t>* keyPkcs8, std::vector<uint8_t>* certDer) override;
  void randomBytes(uint8_t* out, size_t len) override;

 private:
  WC_RNG rng_;
  bool rngOk_ = false;
};

}  // namespace content
}  // namespace freeink

#endif  // FREEINK_CONTENT_WOLFSSL
