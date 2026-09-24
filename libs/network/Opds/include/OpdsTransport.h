#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

/**
 * Network + URL abstraction the OpdsClient drives. The firmware implements it
 * over its HTTP stack (esp_http_client via HttpDownloader) and URL helper; a
 * host test provides an in-memory mock. Keeping it behind this interface lets
 * the OPDS orchestration (feed fetch, 401 auth, token refresh, indirect
 * acquisition, OpenSearch) live in the SDK and be unit-tested off-device.
 */
namespace freeink::opds {

// Credentials for a single request. Empty fields are omitted from the request:
// Basic auth is sent only when username is non-empty, Bearer only when bearer
// is non-empty (bearer takes precedence, matching the HTTP layer).
struct HttpAuth {
  std::string username;
  std::string password;
  std::string bearer;
};

// Outcome of a streaming GET: ok is true for a 2xx response, status carries the
// final HTTP code (0 when the request never reached a response).
struct HttpResult {
  bool ok = false;
  int status = 0;
};

// Body-chunk sink for a streaming GET; return false to abort the transfer.
using OpdsDataSink = std::function<bool(const uint8_t* data, size_t len)>;

class OpdsTransport {
 public:
  virtual ~OpdsTransport() = default;

  // Streaming GET. `accept`/`acceptLanguage` are sent when non-null. When
  // `captureErrorBody` is set, a 401 body is also streamed to `onChunk` (OPDS
  // authentication documents are served as 401 bodies).
  virtual HttpResult get(const std::string& url, const HttpAuth& auth, const char* accept,
                         const char* acceptLanguage, bool captureErrorBody, const OpdsDataSink& onChunk) = 0;

  // POST an application/x-www-form-urlencoded body (an OAuth token request) and
  // collect the response. Returns true on a 2xx status; fills outStatus.
  virtual bool postForm(const std::string& url, const std::string& body, std::string& outResponse,
                        int& outStatus) = 0;

  // Resolve a possibly-relative href against a base URL, applying whatever URL
  // normalisation the HTTP client requires.
  virtual std::string resolveUrl(const std::string& base, const std::string& ref) = 0;
};

}  // namespace freeink::opds
