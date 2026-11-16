#pragma once

// FreeInk SDK — resumable HTTP GET over SecureHttpClient.
//
// A 2xx only means the headers arrived; the body can still be cut short by a
// transport drop, a server stall, or a TLS record OOM. fetchResumable() picks
// the transfer back up from the received byte count with a Range request on a
// fresh connection. A server that ignores the Range (200 instead of 206)
// restarts the body, so the sink is rewound before that body's first byte.
//
// Header-only, like SecureHttpClient.

#include <cctype>
#include <charconv>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

#include "SecureHttpClient.h"

namespace freeink {

struct FetchOptions {
  // Bytes the sink already holds from an earlier transfer; the first request
  // asks for the rest.
  size_t startOffset = 0;
  // Step followed https redirect targets down to http. For payloads that are
  // already content-encrypted, this skips a second TLS session and its record
  // buffer on low-heap boards; only the URL token loses transport security.
  bool redirectToHttp = false;
};

struct FetchSink {
  // Body bytes in order. Return false to stop the transfer.
  std::function<bool(const uint8_t* data, size_t len)> write;
  // The server ignored Range: drop everything held and restart from byte 0.
  // Unset, or returning false, stops the transfer.
  std::function<bool()> rewind;
  // After each write: (bytes held, whole-resource size or 0 when unknown).
  std::function<void(size_t bytes, size_t total)> progress;
};

struct FetchResult {
  int status = 0;         // last HTTP status; -1 on a bad URL or transport failure
  size_t bytes = 0;       // bytes the sink holds, including startOffset
  size_t total = 0;       // whole-resource size when the server reported it, else 0
  bool complete = false;  // the full body arrived
  bool stopped = false;   // the sink stopped the transfer (write/rewind returned false)
  bool aborted = false;   // shouldAbort fired
};

// scheme://host:port, lowercased, with the scheme's default port made explicit
// and any userinfo dropped: the unit credentials are scoped to.
inline std::string fetchOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return {};
  std::string scheme = url.substr(0, schemeEnd);
  const size_t hostStart = schemeEnd + 3;
  const size_t hostEnd = url.find_first_of("/?#", hostStart);
  std::string authority = url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority.erase(0, at + 1);
  for (auto* part : {&scheme, &authority}) {
    for (char& c : *part) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  const size_t bracket = authority.rfind(']');  // IPv6 literal: its colons are not a port
  if (authority.find(':', bracket == std::string::npos ? 0 : bracket) == std::string::npos) {
    authority += scheme == "https" ? ":443" : ":80";
  }
  return scheme + "://" + authority;
}

// A resumable response must identify both its byte interval and resource size.
inline bool parseFetchContentRange(std::string_view value, size_t& first, size_t& last, size_t& total) {
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  if (value.substr(0, 6) != "bytes ") return false;
  const char* cursor = value.data() + 6;
  const char* end = value.data() + value.size();
  const auto number = [&](size_t& out, const char separator) {
    const auto parsed = std::from_chars(cursor, end, out);
    if (parsed.ec != std::errc{}) return false;
    cursor = parsed.ptr;
    if (separator == '\0') return cursor == end;
    if (cursor == end || *cursor != separator) return false;
    ++cursor;
    return true;
  };
  return number(first, '-') && number(last, '/') && number(total, '\0') && first <= last && last < total;
}

// GETs url into sink. configure runs on each attempt's client after begin():
// user agent, timeout, trust, and (only while sameOrigin) credentials and other
// caller headers. sameOrigin is false once a redirect leaves the starting URL's
// scheme, host, or port, including an https->http step-down, so a caller's
// Authorization never reaches a different server or crosses in clear text.
// Non-2xx bodies never reach the sink.
inline FetchResult fetchResumable(const std::string& startUrl, const FetchOptions& options,
                                  const std::function<void(SecureHttpClient&, bool sameOrigin)>& configure,
                                  const FetchSink& sink, const SecureHttpClient::AbortCallback& shouldAbort = nullptr) {
  static constexpr int kMaxRedirects = 5;
  // Only consecutive zero-progress attempts count toward kMaxStalled;
  // kMaxAttempts is a backstop against a server that trickles forever.
  static constexpr int kMaxStalled = 3;
  static constexpr int kMaxAttempts = 20;
  FetchResult result;
  result.bytes = options.startOffset;
  std::string url = startUrl;
  const std::string startOrigin = fetchOrigin(startUrl);
  int redirects = 0;
  int stalled = 0;
  for (int attempt = 0; attempt < kMaxAttempts && stalled < kMaxStalled; ++attempt) {
    SecureHttpClient http;
    if (!http.begin(url)) {
      result.status = -1;
      return result;
    }
    // Bulk body: keep a full-record TLS buffer for the whole transfer.
    http.setReserveFullRecord(true);
    if (configure) configure(http, fetchOrigin(url) == startOrigin);
    size_t attemptStart = result.bytes;
    const bool resuming = attemptStart > 0;
    if (resuming) http.addHeader("Range", "bytes=" + std::to_string(attemptStart) + "-");

    bool firstChunk = true;
    bool invalidResponse = false;
    size_t responseEnd = 0;
    const auto beginResponse = [&] {
      firstChunk = false;
      const int status = http.getStatus();
      if (status == 206) {
        size_t first, last, total;
        if (!parseFetchContentRange(http.getHeader("content-range"), first, last, total) || first != attemptStart ||
            (result.total != 0 && result.total != total) ||
            (http.hasContentLength() && http.getContentLength() != last - first + 1)) {
          return false;
        }
        responseEnd = last + 1;
        result.total = total;
      } else {
        if (resuming) {
          if (status != 200) return false;
          if (!sink.rewind || !sink.rewind()) {
            result.stopped = true;
            return false;
          }
          result.bytes = attemptStart = 0;
        }
        result.total = http.hasContentLength() ? http.getContentLength() : 0;
        responseEnd = result.total;
      }
      return true;
    };
    result.status = http.GET(
        [&](const uint8_t* data, size_t len) {
          const int status = http.getStatus();
          if (status < 200 || status >= 300) return true;  // error page or redirect body: drain
          if ((firstChunk && !beginResponse()) || len > std::numeric_limits<size_t>::max() - result.bytes ||
              (responseEnd != 0 && (result.bytes > responseEnd || len > responseEnd - result.bytes))) {
            invalidResponse = true;
            return false;
          }
          if (!sink.write(data, len)) {
            result.stopped = true;
            return false;
          }
          result.bytes += len;
          if (sink.progress) sink.progress(result.bytes, result.total);
          return true;
        },
        shouldAbort);

    if (http.aborted()) {
      result.aborted = true;
      return result;
    }
    const int status = result.status;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      const std::string location = http.getHeader("location");
      if (redirects++ >= kMaxRedirects || location.empty() || !SecureHttpClient::resolveUrl(url, location, url)) {
        return result;
      }
      if (options.redirectToHttp && url.rfind("https://", 0) == 0) url.replace(0, 8, "http://");
      --attempt;  // a hop is not a transfer attempt
      continue;
    }
    if (invalidResponse || result.stopped || status < 200 || status >= 300) return result;
    // Empty bodies never invoke the write callback, but their headers still
    // need validation (and an ignored Range still needs to rewind the sink).
    if (firstChunk && !beginResponse()) return result;
    if (http.responseComplete()) {
      if (responseEnd != 0 && result.bytes != responseEnd) return result;
      if (status != 206 || result.bytes == result.total) {
        result.complete = true;
        return result;
      }
    }
    stalled = result.bytes > attemptStart ? 0 : stalled + 1;
  }
  return result;
}

}  // namespace freeink
