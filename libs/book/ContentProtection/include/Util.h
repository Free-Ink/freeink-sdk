#pragma once

// FreeInk — small shared helpers (header-only).

#include <stdint.h>
#include <string.h>

#include <string>

namespace freeink {
namespace content {

// FNV-1a 64-bit. Entry paths are tracked as hashes (8 bytes each instead of a
// heap string apiece); at 64 bits a collision within one container's few
// hundred names is negligible.
inline uint64_t fnv1a64(const char* s, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= static_cast<uint8_t>(s[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

inline int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Decodes base64 (ignoring whitespace) into out. Returns decoded length, or
// negative on malformed input / insufficient capacity.
inline int32_t base64Decode(const char* in, size_t inLen, uint8_t* out, size_t outCap) {
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  for (size_t i = 0; i < inLen; i++) {
    const char c = in[i];
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    const int v = b64Val(c);
    if (v < 0) return -1;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= outCap) return -1;
      out[n++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
    }
  }
  return static_cast<int32_t>(n);
}

inline std::string base64Decode(const std::string& in) {
  std::string out;
  out.resize((in.size() * 3) / 4 + 3);
  const int32_t n = base64Decode(in.data(), in.size(), reinterpret_cast<uint8_t*>(out.data()), out.size());
  if (n < 0) return std::string();
  out.resize(static_cast<size_t>(n));
  return out;
}

// Extracts the 16 raw bytes of a UUID from "urn:uuid:xxxxxxxx-xxxx-..." (or a
// bare/dashed hex form). Returns false if fewer than 32 hex digits.
inline bool uuidBytes(const std::string& urn, uint8_t out[16]) {
  uint8_t nibbles[32];
  size_t n = 0;
  for (const char c : urn) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else continue;
    if (n < 32) nibbles[n++] = static_cast<uint8_t>(v);
  }
  if (n < 32) return false;
  for (size_t i = 0; i < 16; i++) out[i] = static_cast<uint8_t>((nibbles[2 * i] << 4) | nibbles[2 * i + 1]);
  return true;
}

// Parses an ISO-8601 date-time ("2026-08-15T23:59:59-04:00" or "...Z") into
// epoch seconds. Returns false on malformed input. Date math is civil-days
// based; no <time.h> dependency (freestanding).
inline bool isoToEpoch(const std::string& iso, int64_t* epochOut) {
  int Y, M, D, h, m, s;
  if (iso.size() < 19) return false;
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return false;
  // Days from civil (Howard Hinnant's algorithm).
  const int y = M <= 2 ? Y - 1 : Y;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097ll + static_cast<int64_t>(doe) - 719468ll;
  int64_t epoch = days * 86400ll + h * 3600ll + m * 60ll + s;
  // Optional numeric timezone offset (+/-HH:MM); Z or absent means UTC.
  const size_t tzPos = iso.find_first_of("+-", 19);
  if (tzPos != std::string::npos && tzPos + 5 < iso.size()) {
    int th, tm;
    if (sscanf(iso.c_str() + tzPos + 1, "%d:%d", &th, &tm) == 2) {
      const int64_t off = th * 3600ll + tm * 60ll;
      epoch += (iso[tzPos] == '-') ? off : -off;
    }
  }
  *epochOut = epoch;
  return true;
}

}  // namespace content
}  // namespace freeink
