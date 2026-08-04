// FreeInk — wolfSSL crypto backend.
//
// Notes on the wolfSSL pieces:
//  - PKCS#8 is decoded via ToTraditional (in-place PKCS#8 -> PKCS#1), then
//    wc_RsaPrivateKeyDecode. Buffers must be writable.
//  - "Raw" RSA is wc_RsaFunction(RSA_PRIVATE_ENCRYPT) = m^d mod n; signing is a manual PKCS#1
//    type-1 pad of the 20-byte hash followed by that raw op — byte-identical
//    to the reference implementation.
//  - SPKI/PKCS#8 wrappers for locally generated keys are small fixed-shape
//    DER encodings (rsaEncryption OID) — built by hand here.
//  - The X.509 authentication certificate's public key is extracted with
//    wc_ParseCert + wc_GetPubKeyDerFromCert, then the PKCS#1 key is unwrapped
//    from the SPKI by a minimal DER walk.
//  - PKCS#12 parsing (wc_PKCS12_parse) needs WOLFSSL_PKCS12-capable build;
//    Arduino-wolfSSL 5.7.x has it unless NO_PKCS12 is set — check
//    scripts/patch_wolfssl.py in the firmware if this fails to link.

#include "WolfsslCrypto.h"

#ifdef FREEINK_CONTENT_WOLFSSL

#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/pkcs12.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sha256.h>

#if defined(ESP_PLATFORM)
#include <mbedtls/esp_mbedtls_random.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#endif

#include <stdlib.h>
#include <string.h>

namespace freeink {
namespace content {

namespace {

// RsaKey is ~8.4 KB with wolfSSL's embedded math configuration. Keeping one
// as a local overflows CrossPoint's 8 KB render/loop task stacks before
// wc_InitRsaKey() even returns. Own it on the heap and centralize cleanup.
class ScopedRsaKey {
 public:
  ScopedRsaKey() : key_(static_cast<RsaKey*>(malloc(sizeof(RsaKey)))) {
    if (key_ && wc_InitRsaKey(key_, nullptr) != 0) {
      free(key_);
      key_ = nullptr;
    }
  }
  ~ScopedRsaKey() {
    if (key_) {
      wc_FreeRsaKey(key_);
      free(key_);
    }
  }

  RsaKey* get() const { return key_; }

 private:
  RsaKey* key_;
};

// --- tiny DER helpers -------------------------------------------------------

// Number of bytes DER uses to encode `len`, in minimal (definite) form:
// short form (1 byte) for <128, else one 0x8N prefix + N length octets. Must
// stay in lockstep with derWriteLen — a mismatch under-counts the parent
// length field and corrupts the whole structure.
size_t derLenLen(size_t len) {
  if (len < 128) return 1;
  if (len < 0x100) return 2;    // 0x81 <lo>
  return 3;                     // 0x82 <hi> <lo>  (key material never exceeds 64 KB)
}

uint8_t* derWriteLen(uint8_t* p, size_t len) {
  if (len < 128) {
    *p++ = static_cast<uint8_t>(len);
  } else if (len < 0x100) {
    *p++ = 0x81;
    *p++ = static_cast<uint8_t>(len);
  } else {
    *p++ = 0x82;
    *p++ = static_cast<uint8_t>(len >> 8);
    *p++ = static_cast<uint8_t>(len);
  }
  return p;
}

// Wraps a PKCS#1 RSAPublicKey blob as SubjectPublicKeyInfo.
bool wrapSpki(const uint8_t* pkcs1, size_t pkcs1Len, std::vector<uint8_t>* out) {
  static const uint8_t kAlgId[15] = {0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86,
                                     0xF7, 0x0D, 0x01, 0x01, 0x01, 0x05, 0x00};
  const size_t bitStrLen = 1 + pkcs1Len;
  const size_t contentLen = sizeof(kAlgId) + 1 + derLenLen(bitStrLen) + bitStrLen;
  out->clear();
  out->reserve(2 + derLenLen(contentLen) + contentLen);
  out->push_back(0x30);
  uint8_t scratch[4];
  uint8_t* end = derWriteLen(scratch, contentLen);
  out->insert(out->end(), scratch, end);
  out->insert(out->end(), kAlgId, kAlgId + sizeof(kAlgId));
  out->push_back(0x03);
  end = derWriteLen(scratch, bitStrLen);
  out->insert(out->end(), scratch, end);
  out->push_back(0x00);
  out->insert(out->end(), pkcs1, pkcs1 + pkcs1Len);
  return true;
}

// Wraps a PKCS#1 RSAPrivateKey blob as PKCS#8 PrivateKeyInfo.
bool wrapPkcs8(const uint8_t* pkcs1, size_t pkcs1Len, std::vector<uint8_t>* out) {
  static const uint8_t kHead[7] = {0x02, 0x01, 0x00, 0x30, 0x0D, 0x06, 0x09};  // version + algid start
  static const uint8_t kAlgTail[6] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D};
  static const uint8_t kAlgEnd[4] = {0x01, 0x01, 0x01, 0x05};
  // algid: 30 0D 06 09 <oid 9 bytes> 05 00
  const size_t contentLen = 3 + 15 + 1 + derLenLen(pkcs1Len) + pkcs1Len;
  out->clear();
  out->reserve(2 + derLenLen(contentLen) + contentLen);
  out->push_back(0x30);
  uint8_t scratch[4];
  uint8_t* end = derWriteLen(scratch, contentLen);
  out->insert(out->end(), scratch, end);
  out->insert(out->end(), kHead, kHead + sizeof(kHead));
  out->insert(out->end(), kAlgTail, kAlgTail + sizeof(kAlgTail));
  out->insert(out->end(), kAlgEnd, kAlgEnd + sizeof(kAlgEnd));
  out->push_back(0x00);
  out->push_back(0x04);
  end = derWriteLen(scratch, pkcs1Len);
  out->insert(out->end(), scratch, end);
  out->insert(out->end(), pkcs1, pkcs1 + pkcs1Len);
  return true;
}

// Locates the SubjectPublicKeyInfo inside an X.509 cert by scanning for the
// rsaEncryption algorithm identifier and backing up to the enclosing
// SEQUENCE. Works without wolfSSL's cert parser (kept as a fallback for
// feature-trimmed builds). Returns nullptr when not found.
const uint8_t* spkiFromX509(const uint8_t* cert, size_t len, size_t* spkiLen) {
  static const uint8_t kRsaAlgId[13] = {0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48,
                                        0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
  for (size_t i = 0; i + sizeof(kRsaAlgId) + 4 < len; i++) {
    if (memcmp(cert + i, kRsaAlgId, sizeof(kRsaAlgId)) != 0) continue;
    // SPKI header immediately precedes the algid: 30 81 <len> (3 bytes) or
    // 30 82 <len16> (4 bytes).
    for (int hdr = 3; hdr <= 4; hdr++) {
      if (i < static_cast<size_t>(hdr) || cert[i - hdr] != 0x30) continue;
      size_t total = 0;
      if (hdr == 3 && cert[i - 2] == 0x81) {
        total = 3 + cert[i - 1];
      } else if (hdr == 4 && cert[i - 3] == 0x82) {
        total = 4 + ((static_cast<size_t>(cert[i - 2]) << 8) | cert[i - 1]);
      }
      if (total == 0) continue;
      const uint8_t* spki = cert + i - hdr;
      if (spki + total <= cert + len) {
        *spkiLen = total;
        return spki;
      }
    }
  }
  return nullptr;
}

bool decodePrivateKey(const uint8_t* pkcs8Der, size_t pkcs8Len, RsaKey* key) {
  // ToTraditional converts PKCS#8 to PKCS#1 in place; hand it a scratch copy.
  std::vector<uint8_t> buf(pkcs8Der, pkcs8Der + pkcs8Len);
  int len = ToTraditional(buf.data(), static_cast<word32>(buf.size()));
  if (len < 0) {
    // Maybe already traditional.
    len = static_cast<int>(pkcs8Len);
  }
  word32 idx = 0;
  return wc_RsaPrivateKeyDecode(buf.data(), &idx, key, static_cast<word32>(len)) == 0;
}

}  // namespace

WolfsslCrypto::WolfsslCrypto() { rngOk_ = wc_InitRng(&rng_) == 0; }

WolfsslCrypto::~WolfsslCrypto() {
  if (rngOk_) wc_FreeRng(&rng_);
}

void WolfsslCrypto::randomBytes(uint8_t* out, size_t len) {
  if (rngOk_) wc_RNG_GenerateBlock(&rng_, out, static_cast<word32>(len));
}

void WolfsslCrypto::sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
  wc_ShaHash(data, static_cast<word32>(len), out);
}

void WolfsslCrypto::sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  wc_Sha256Hash(data, static_cast<word32>(len), out);
}

int32_t WolfsslCrypto::rsaPrivateRaw(const uint8_t* pkcs8Der, size_t pkcs8Len, const uint8_t* in,
                                     size_t inLen, uint8_t* out, size_t outCap) {
  if (!rngOk_) return -1;
  ScopedRsaKey key;
  if (!key.get()) return -2;
  int32_t result = -3;
  if (decodePrivateKey(pkcs8Der, pkcs8Len, key.get())) {
    word32 outLen = static_cast<word32>(outCap);
    // The raw private op is m^d mod n (used for both request signing and
    // content-key decryption). wc_RsaFunction's `type` is the *_ENCRYPT/_DECRYPT
    // set: RSA_PRIVATE_ENCRYPT==2 is m^d. Do NOT pass RSA_PRIVATE (==1) — that
    // aliases RSA_PUBLIC_DECRYPT and computes m^e with the public exponent,
    // yielding signatures the server rejects as BadPadding.
    if (wc_RsaFunction(in, static_cast<word32>(inLen), out, &outLen, RSA_PRIVATE_ENCRYPT, key.get(),
                       &rng_) == 0) {
      result = static_cast<int32_t>(outLen);
    }
  }
  return result;
}

bool WolfsslCrypto::rsaPrivateSignRaw(const uint8_t* pkcs8Der, size_t pkcs8Len,
                                      const uint8_t hash[20], uint8_t out[128]) {
  uint8_t padded[128];
  memset(padded, 0xFF, sizeof(padded));
  padded[0] = 0x00;
  padded[1] = 0x01;
  padded[sizeof(padded) - 20 - 1] = 0x00;
  memcpy(padded + sizeof(padded) - 20, hash, 20);
  return rsaPrivateRaw(pkcs8Der, pkcs8Len, padded, sizeof(padded), out, 128) == 128;
}

bool WolfsslCrypto::rsaGenerate(RsaKeyPairDer* out) {
  lastError.clear();
  if (!rngOk_) {
    lastError = "rng not initialized";
    return false;
  }
#if defined(ESP_PLATFORM)
  // wolfSSL 5.7.2's Tom's Fast Math key-generation path is unstable on the
  // ESP32-C3 (the Miller-Rabin test faults in fp_mul_comba). ESP-IDF already
  // ships mbedTLS with RSA key generation enabled, so use it for this one
  // operation. The output remains the same SPKI public key + PKCS#8 private
  // key consumed by the rest of the content-protection code.
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);

  int rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (rc == 0) {
    rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_esp_random, nullptr, 1024, 65537);
  }

  uint8_t pubBuf[192];
  uint8_t privBuf[1024];
  int pubLen = 0;
  int privLen = 0;
  if (rc == 0) pubLen = mbedtls_pk_write_pubkey_der(&key, pubBuf, sizeof(pubBuf));
  if (pubLen > 0) privLen = mbedtls_pk_write_key_der(&key, privBuf, sizeof(privBuf));

  bool ok = pubLen > 0 && privLen > 0;
  if (ok) {
    out->spki.assign(pubBuf + sizeof(pubBuf) - pubLen, pubBuf + sizeof(pubBuf));
    ok = wrapPkcs8(privBuf + sizeof(privBuf) - privLen, static_cast<size_t>(privLen), &out->pkcs8);
    if (!ok) lastError = "PKCS#8 DER wrap failed";
  } else {
    lastError = "mbedTLS RSA keygen rc=" + std::to_string(rc) + " pub=" + std::to_string(pubLen) +
                " priv=" + std::to_string(privLen);
  }

  mbedtls_pk_free(&key);
  return ok;
#else
  ScopedRsaKey key;
  if (!key.get()) {
    lastError = "wc_InitRsaKey failed";
    return false;
  }
  const int mk = wc_MakeRsaKey(key.get(), 1024, 65537, &rng_);
  bool ok = mk == 0;
  if (!ok) {
    // Surface the code: negative wolfSSL error (e.g. -125 MP_MEM = out of heap,
    // -173 NOT_COMPILED_IN, -101 RNG_FAILURE_E, -173/-174 config/size).
    lastError = "wc_MakeRsaKey rc=" + std::to_string(mk);
  } else {
    uint8_t pubBuf[192];
    uint8_t privBuf[1024];
    // with_header=0 → bare PKCS#1 RSAPublicKey. wc_RsaKeyToPublicDer() forces a
    // SubjectPublicKeyInfo header (~162 B, overflowed the old buffer), but
    // wrapSpki() below adds the SPKI wrapper itself — mirrors the private side
    // (bare wc_RsaKeyToDer + wrapPkcs8).
    const int pubLen = wc_RsaKeyToPublicDer_ex(key.get(), pubBuf, sizeof(pubBuf), 0);
    const int privLen = wc_RsaKeyToDer(key.get(), privBuf, sizeof(privBuf));
    if (pubLen <= 0 || privLen <= 0) {
      lastError = "keyToDer pub=" + std::to_string(pubLen) + " priv=" + std::to_string(privLen);
      ok = false;
    } else {
      ok = wrapSpki(pubBuf, static_cast<size_t>(pubLen), &out->spki) &&
           wrapPkcs8(privBuf, static_cast<size_t>(privLen), &out->pkcs8);
      if (!ok) lastError = "der wrap failed";
    }
  }
  return ok;
#endif
}

bool WolfsslCrypto::rsaPublicEncrypt(const uint8_t* certDer, size_t certLen, const uint8_t* in,
                                     size_t inLen, uint8_t* out, size_t outCap, size_t* outLen) {
  lastError.clear();
  if (!rngOk_) {
    lastError = "rng init failed";
    return false;
  }

  // Preferred: wolfSSL's cert parser. Fallback: hand-scan the SPKI (trimmed
  // wolfSSL builds fail real-world cert parses in surprising ways).
  const uint8_t* spki = nullptr;
  size_t spkiLen = 0;
  uint8_t spkiBuf[512];
  {
    DecodedCert cert;
    wc_InitDecodedCert(&cert, certDer, static_cast<word32>(certLen), nullptr);
    const int rc = wc_ParseCert(&cert, CERT_TYPE, NO_VERIFY, nullptr);
    if (rc == 0) {
      word32 n = sizeof(spkiBuf);
      if (wc_GetPubKeyDerFromCert(&cert, spkiBuf, &n) == 0) {
        spki = spkiBuf;
        spkiLen = n;
      }
    } else {
      lastError = "wc_ParseCert rc=" + std::to_string(rc);
    }
    wc_FreeDecodedCert(&cert);
  }
  if (!spki) {
    spki = spkiFromX509(certDer, certLen, &spkiLen);
    if (!spki) {
      lastError += "; spki scan failed";
      return false;
    }
  }

  ScopedRsaKey key;
  if (!key.get()) {
    lastError = "wc_InitRsaKey failed";
    return false;
  }
  // wc_RsaPublicKeyDecode accepts either a bare PKCS#1 RSAPublicKey (what
  // wc_GetPubKeyDerFromCert returns in this build) or a full
  // SubjectPublicKeyInfo (what the spkiFromX509 fallback yields) — it tries
  // PKCS#1 first, then SPKI. So hand it the DER directly; no manual unwrap.
  word32 idx = 0;
  bool ok = wc_RsaPublicKeyDecode(spki, &idx, key.get(), static_cast<word32>(spkiLen)) == 0;
  if (!ok) {
    lastError = "wc_RsaPublicKeyDecode failed";
  } else {
    const int n = wc_RsaPublicEncrypt(in, static_cast<word32>(inLen), out,
                                      static_cast<word32>(outCap), key.get(), &rng_);
    if (n > 0) {
      *outLen = static_cast<size_t>(n);
    } else {
      ok = false;
      const int modBits = wc_RsaEncryptSize(key.get()) * 8;
#if defined(WOLFSSL_ESP32_CRYPT_RSA_PRI_EXPTMOD)
      const char* path = "HW";
#else
      const char* path = "SW";
#endif
      lastError = "wc_RsaPublicEncrypt rc=" + std::to_string(n) + " modbits=" + std::to_string(modBits) +
                  " " + path;
    }
  }
  return ok;
}

bool WolfsslCrypto::aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in,
                                     size_t len, uint8_t* out) {
  if (len % 16 != 0) return false;
  Aes aes;
  if (wc_AesSetKey(&aes, key, 16, iv, AES_DECRYPTION) != 0) return false;
  return wc_AesCbcDecrypt(&aes, out, in, static_cast<word32>(len)) == 0;
}

bool WolfsslCrypto::aes128CbcEncrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in,
                                     size_t len, uint8_t* out) {
  // PKCS#7 pad first (wolfSSL does no padding).
  const size_t paddedLen = ((len / 16) + 1) * 16;
  std::vector<uint8_t> padded(paddedLen);
  memcpy(padded.data(), in, len);
  const uint8_t pad = static_cast<uint8_t>(paddedLen - len);
  for (size_t i = len; i < paddedLen; i++) padded[i] = pad;

  Aes aes;
  if (wc_AesSetKey(&aes, key, 16, iv, AES_ENCRYPTION) != 0) return false;
  return wc_AesCbcEncrypt(&aes, out, padded.data(), static_cast<word32>(paddedLen)) == 0;
}

bool WolfsslCrypto::pkcs12Extract(const uint8_t* p12, size_t len, const std::string& password,
                                  std::vector<uint8_t>* keyPkcs8, std::vector<uint8_t>* certDer) {
  lastError.clear();
  WC_PKCS12* bundle = wc_PKCS12_new();
  if (!bundle) {
    lastError = "wc_PKCS12_new failed";
    return false;
  }
  const int d2i = wc_d2i_PKCS12(p12, static_cast<word32>(len), bundle);
  if (d2i != 0) {
    // Bad DER — e.g. the base64 decode picked up whitespace, or `len` is wrong.
    lastError = "wc_d2i_PKCS12 rc=" + std::to_string(d2i) + " len=" + std::to_string(len);
    wc_PKCS12_free(bundle);
    return false;
  }

  byte* key = nullptr;
  byte* cert = nullptr;
  word32 keyLen = 0, certLen = 0;
  const int rc = wc_PKCS12_parse(bundle, password.c_str(), &key, &keyLen, &cert, &certLen, nullptr);
  wc_PKCS12_free(bundle);
  // A bundle missing either bag can parse "successfully" with one output null;
  // free whatever was returned before bailing.
  if (rc < 0 || !key || !cert) {
    // rc<0 with a valid bundle usually means the passphrase is wrong.
    lastError = "wc_PKCS12_parse rc=" + std::to_string(rc) + " keyLen=" + std::to_string(keyLen) +
                " certLen=" + std::to_string(certLen) + " pwLen=" + std::to_string(password.size());
    if (key) XFREE(key, NULL, DYNAMIC_TYPE_PUBLIC_KEY);
    if (cert) XFREE(cert, NULL, DYNAMIC_TYPE_PKCS);
    return false;
  }

  // The parsed private key is traditional (PKCS#1) — wrap as PKCS#8.
  const bool ok = wrapPkcs8(key, keyLen, keyPkcs8);
  if (ok) certDer->assign(cert, cert + certLen);
  XFREE(key, NULL, DYNAMIC_TYPE_PUBLIC_KEY);
  XFREE(cert, NULL, DYNAMIC_TYPE_PKCS);
  return ok;
}

}  // namespace content
}  // namespace freeink

#endif  // FREEINK_CONTENT_WOLFSSL
