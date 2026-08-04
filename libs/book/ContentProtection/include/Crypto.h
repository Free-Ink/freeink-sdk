#pragma once

// FreeInk — crypto backend interface.
//
// On device: wolfSSL. Host tests: OpenSSL. Kept behind an interface so the
// freestanding core carries no crypto dependency and stays unit-testable.
//
// Freestanding C++17.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace freeink {
namespace content {

struct RsaKeyPairDer {
  std::vector<uint8_t> spki;   // SubjectPublicKeyInfo DER (wire format)
  std::vector<uint8_t> pkcs8;  // PKCS#8 DER (private key storage format)
};

class Crypto {
 public:
  virtual ~Crypto() = default;

  // --- read path ------------------------------------------------------------

  // Raw RSA private-key operation (no padding) with a PKCS#8 DER key.
  // `in` is one modulus-sized block; `out` must have room for the same.
  // Returns output length (== modulus size) or a negative value on error.
  virtual int32_t rsaPrivateRaw(const uint8_t* pkcs8Der, size_t pkcs8Len, const uint8_t* in,
                                size_t inLen, uint8_t* out, size_t outCap) = 0;

  // AES-128-CBC decrypt, no padding handling (caller unpads). `len` must be
  // a multiple of 16. In-place operation is allowed (out == in).
  virtual bool aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in,
                                size_t len, uint8_t* out) = 0;

  virtual void sha1(const uint8_t* data, size_t len, uint8_t out[20]) = 0;
  virtual void sha256(const uint8_t* data, size_t len, uint8_t out[32]) = 0;

  // --- client (off-device credential setup) -----------------------------------

  // RSA-1024 keypair (e=65537), exported as SPKI + PKCS#8 DER.
  virtual bool rsaGenerate(RsaKeyPairDer* out) = 0;

  // RSAES-PKCS1-v1_5 with an X.509 certificate's public key (DER).
  virtual bool rsaPublicEncrypt(const uint8_t* certDer, size_t certLen, const uint8_t* in,
                                size_t inLen, uint8_t* out, size_t outCap, size_t* outLen) = 0;

  // request signature: PKCS#1 v1.5 type-1 padding + raw RSA private op
  // over the 20-byte canonicalization hash. `out` is one modulus (128 bytes).
  virtual bool rsaPrivateSignRaw(const uint8_t* pkcs8Der, size_t pkcs8Len, const uint8_t hash[20],
                                 uint8_t out[128]) = 0;

  // AES-128-CBC encrypt WITH PKCS#7 padding (used for device-key encryption).
  // `out` must hold len rounded up to 16 + 16.
  virtual bool aes128CbcEncrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in,
                                size_t len, uint8_t* out) = 0;

  // Extracts the private key (as PKCS#8 DER) and certificate (DER) from a
  // PKCS#12 bundle. note: the signing bundle's passphrase is
  // base64(device key).
  virtual bool pkcs12Extract(const uint8_t* p12, size_t len, const std::string& password,
                             std::vector<uint8_t>* keyPkcs8, std::vector<uint8_t>* certDer) = 0;

  virtual void randomBytes(uint8_t* out, size_t len) = 0;
};

}  // namespace content
}  // namespace freeink
