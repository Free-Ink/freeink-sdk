#pragma once

// FreeInk SDK — minimal HTTPS client over SecureClient (wolfSSL TLS 1.3).
//
// Self-contained on purpose: it does NOT wrap Arduino HTTPClient. HTTPClient's
// begin() takes a NetworkClient&, but SecureClient is a plain Arduino Client
// (it owns a WiFiClient transport and runs wolfSSL on top), so it can't bind to
// that API. Instead this implements the small slice of HTTP/1.1 that firmware
// needs — GET/POST/PUT with custom headers and a buffered response body —
// directly over SecureClient, handling Content-Length, chunked, and
// connection-close-delimited responses. Connections are kept alive and reused
// across requests to the same scheme://host:port (see setReuse), so a burst of
// requests — an OPDS crawl, a sync exchange, a multi-file download — pays for
// one TLS handshake instead of one per request.
//
// Usage:
//   SecureHttpClient http;
//   http.setInsecure();                 // or http.setCACert(rootPem)
//   if (http.begin("https://host/path")) {
//     http.addHeader("Accept", "application/json");
//     int code = http.GET();            // < 0 on transport failure
//     const std::string& body = http.getString();
//     http.end();
//   }
//
// Header-only. Redirects are returned to the caller by default; opt in to
// following them with setFollowRedirects().
//
// OPT-IN: requires -DFREEINK_NET_WOLFSSL=1 for TLS. With the flag off,
// SecureClient is an inert stub and https requests fail at connect()
// (GET()/POST() return -1); plain-http URLs still work over the WiFiClient
// transport.

#include <Arduino.h>
#include <WiFiClient.h>
#include <base64.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "SecureClient.h"

namespace freeink {

class SecureHttpClient {
 public:
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
  using AbortCallback = std::function<bool()>;
  // (bytes so far, total from Content-Length or 0 when unknown).
  // Return false to abort the transfer.
  using ProgressCallback = std::function<bool(size_t downloaded, size_t total)>;

  SecureHttpClient() = default;
  ~SecureHttpClient() { end(); }
  // Non-copyable: owns a live connection and a Client* into its own members.
  SecureHttpClient(const SecureHttpClient&) = delete;
  SecureHttpClient& operator=(const SecureHttpClient&) = delete;

  // Skip peer verification (SecureClient does likewise). Required today because
  // the wolfSSL transport has no CA bundle wired up; see setCACert().
  void setInsecure() { _insecure = true; }
  // Verify against a single PEM root. Clears the insecure flag.
  void setCACert(const char* rootCA) {
    _rootCA = rootCA;
    _insecure = false;
  }
  void setTimeout(uint32_t ms) { _timeoutMs = ms; }
  // Progress reporting for response bodies (download meters). Called after
  // each delivered chunk with the running byte count; never called for
  // drained redirect bodies. Returning false aborts the transfer (reported
  // via callbackAborted()). Clear with nullptr.
  void setProgressCallback(const ProgressCallback& progress) { _progress = progress; }
  // Follow up to maxHops redirect hops (default 0: 3xx responses are returned
  // to the caller, matching the previous behavior). While following, the
  // intermediate 3xx bodies are drained and discarded; only the final
  // response reaches the caller. 303 — and, per long-standing convention,
  // 301/302 after a POST — continue as GET without the request body; 307/308
  // preserve method and body.
  void setFollowRedirects(int maxHops) { _followRedirects = maxHops < 0 ? 0 : maxHops; }
  // Allow a redirect to step down from https to http. Off by default, because
  // the downgrade silently drops transport security; when refused, following
  // stops and the caller sees the 3xx.
  void setAllowRedirectDowngrade(bool allow) { _allowRedirectDowngrade = allow; }
  // HTTP Basic authentication, sent on every request while set (OPDS servers
  // commonly protect their catalogs this way). An empty user disables it; an
  // empty password with a non-empty user is valid per RFC 7617.
  void setBasicAuth(const std::string& user, const std::string& pass) {
    _authUser = user;
    _authPass = pass;
  }
  void clearBasicAuth() {
    _authUser.clear();
    _authPass.clear();
  }
  // Identify the client. Sent on every request: several CDNs (notably
  // Cloudflare in front of OPDS catalogs) answer UA-less requests with 403.
  void setUserAgent(const std::string& ua) { _userAgent = ua; }
  // Keep the connection open between requests to the same scheme://host:port
  // (the default). Reusing the TLS session skips a full handshake per request —
  // seconds of latency plus the ECC/RSA heap spike on PSRAM-less boards.
  // setReuse(false) restores connection-per-request behavior.
  void setReuse(bool reuse) { _reuse = reuse; }

  // Parse the URL and reset per-request state. Returns false on a malformed
  // URL.
  bool begin(const std::string& url) {
    _headers.clear();
    _body.clear();
    _status = 0;
    return parseUrl(url, _scheme, _host, _path, _port);
  }
  // Closes the kept-alive connection (if any). Call when done with a server;
  // the next request transparently reconnects.
  void end() { closeConnection(); }

  void addHeader(const std::string& name, const std::string& value) {
    _headers.push_back(name + ": " + value + "\r\n");
  }

  int GET() { return sendRequest("GET", nullptr, 0); }
  int GET(const DataCallback& onData, const AbortCallback& shouldAbort = nullptr) {
    return sendRequest("GET", nullptr, 0, onData, shouldAbort);
  }
  int POST(const std::string& payload) {
    return sendRequest("POST", reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  }
  int sendRequest(const char* method, const std::string& payload) {
    return sendRequest(method, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  }

  // Performs the request and reads the full response body. Returns the HTTP
  // status code, or -1 if the connection or status line could not be read. The
  // body (which may be empty or, on a mid-stream drop, truncated) is available
  // via getString().
  int sendRequest(const char* method, const uint8_t* payload, size_t payloadLen) {
    return sendRequest(method, payload, payloadLen, [this](const uint8_t* data, size_t len) {
      _body.append(reinterpret_cast<const char*>(data), len);
      return true;
    });
  }

  // Performs the request and streams the response body to `onData` as chunks
  // arrive. Returns the HTTP status code, or -1 on transport/header failure.
  // Inspect responseComplete(), callbackAborted(), and aborted() to distinguish
  // an incomplete body from a caller-initiated stop.
  int sendRequest(const char* method, const uint8_t* payload, size_t payloadLen, const DataCallback& onData,
                  const AbortCallback& shouldAbort = nullptr) {
    std::string activeMethod = method;
    const uint8_t* activePayload = payload;
    size_t activePayloadLen = payloadLen;
    for (int hop = 0;; ++hop) {
      const int status = sendRequestOnce(activeMethod.c_str(), activePayload, activePayloadLen, onData, shouldAbort);
      if (status < 0 || hop >= _followRedirects || !isRedirectStatus(status)) return status;

      const std::string location = getHeader("location");
      if (location.empty()) return status;
      const std::string base = _scheme + "://" + hostHeader() + _path;
      std::string next;
      if (!resolveUrl(base, location, next)) return status;
      std::string scheme;
      std::string host;
      std::string path;
      uint16_t port = 0;
      if (!parseUrl(next, scheme, host, path, port)) return status;
      // Refuse to silently drop transport security: a https -> http redirect
      // stops here (the caller sees the 3xx) unless explicitly allowed.
      if (_scheme == "https" && scheme == "http" && !_allowRedirectDowngrade) return status;
      _scheme = scheme;
      _host = host;
      _path = path;
      _port = port;

      // 303 always continues as GET; after 301/302, long-standing convention
      // downgrades POST to GET too. 307/308 preserve method and body.
      if ((status == 301 || status == 302 || status == 303) && activeMethod != "GET" && activeMethod != "HEAD") {
        activeMethod = "GET";
        activePayload = nullptr;
        activePayloadLen = 0;
      }
    }
  }

  // One request/response transaction against the URL state — never follows
  // redirects. When following is enabled and the response is a 3xx, its body
  // is drained but NOT delivered to onData (only the final response's body
  // reaches the caller's sink).
  int sendRequestOnce(const char* method, const uint8_t* payload, size_t payloadLen, const DataCallback& onData,
                      const AbortCallback& shouldAbort = nullptr) {
    _status = 0;
    _body.clear();
    _responseHeaders.clear();
    _contentLength = 0;
    _haveContentLength = false;
    _bodyComplete = false;
    _callbackAborted = false;
    _aborted = false;

    // One transparent retry: a keep-alive server may close the connection
    // between requests at any time, and the race surfaces as a write failure
    // or a missing status line on a socket that looked connected. That is a
    // property of the reused connection, not of the request, so it earns one
    // attempt on a fresh connection. A request that failed on a fresh
    // connection is a real error and is not retried.
    for (int attempt = 0; attempt < 2; ++attempt) {
      const bool reusing = connectionMatches();
      if (isAborted(shouldAbort)) return -1;
      if (!ensureConnected()) return -1;

      if (!writeRequest(method, payload, payloadLen)) {
        closeConnection();
        if (reusing && attempt == 0) continue;
        return -1;
      }

      const unsigned long headerDeadline = millis() + _timeoutMs;
      std::string line;
      if (!readLine(*_conn, line, headerDeadline, shouldAbort)) {
        closeConnection();
        if (reusing && attempt == 0 && !_aborted) continue;
        return -1;
      }
      // "HTTP/1.1 200 OK" — the status code starts at offset 9.
      _status = line.size() >= 12 ? atoi(line.c_str() + 9) : 0;
      // HTTP/1.0 peers default to connection-per-request; only an explicit
      // Connection: keep-alive header (below) overrides that.
      bool keepAlive = line.compare(0, 9, "HTTP/1.0 ") != 0;

      std::string transferEncoding;
      while (readLine(*_conn, line, headerDeadline, shouldAbort)) {
        if (line.empty()) break;  // end of headers
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(tolower(c)); });
        _responseHeaders.push_back(Header{name, value});
        if (name == "content-length") {
          _contentLength = static_cast<size_t>(strtoul(value.c_str(), nullptr, 10));
          _haveContentLength = true;
        } else if (name == "transfer-encoding") {
          std::transform(value.begin(), value.end(), value.begin(),
                         [](unsigned char c) { return static_cast<char>(tolower(c)); });
          transferEncoding = value;
        } else if (name == "connection") {
          std::string v = value;
          std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
          if (v.find("close") != std::string::npos) keepAlive = false;
          else if (v.find("keep-alive") != std::string::npos) keepAlive = true;
        }
      }
      if (_aborted) {
        closeConnection();
        return -1;
      }

      // A close-delimited body (no framing) ends WITH the connection, so it can
      // never leave a reusable socket behind.
      // A 3xx body that is about to be followed is protocol plumbing, not
      // payload: drain it (keeping the connection reusable for the next hop)
      // without touching the caller's sink.
      const bool discardBody = _followRedirects > 0 && isRedirectStatus(_status);
      const DataCallback discard = [](const uint8_t*, size_t) { return true; };
      const DataCallback& bodySink = discardBody ? discard : onData;
      _downloaded = 0;
      _reportProgress = !discardBody && static_cast<bool>(_progress);

      bool reusableFraming = true;
      if (transferEncoding.find("chunked") != std::string::npos) {
        _bodyComplete = readChunked(*_conn, bodySink, shouldAbort);
      } else if (transferEncoding.empty() || transferEncoding == "identity") {
        if (_haveContentLength) {
          _bodyComplete = readFixed(*_conn, _contentLength, bodySink, shouldAbort);
        } else {
          _bodyComplete = readUntilClose(*_conn, bodySink, shouldAbort);
          reusableFraming = false;
        }
      } else {
        _bodyComplete = false;
      }

      // Reuse only a provably clean connection. An aborted/truncated body
      // leaves undrained bytes on the socket, and the next request would parse
      // leftover body data as its status line.
      if (!_reuse || !keepAlive || !_bodyComplete || !reusableFraming) closeConnection();
      return _status;
    }
    return -1;
  }

  const std::string& getString() const { return _body; }
  int getStatus() const { return _status; }
  int getSize() const { return static_cast<int>(_body.size()); }
  bool responseComplete() const { return _bodyComplete; }
  bool callbackAborted() const { return _callbackAborted; }
  bool aborted() const { return _aborted; }
  bool hasContentLength() const { return _haveContentLength; }
  size_t getContentLength() const { return _contentLength; }

  std::string getHeader(const std::string& headerName) const {
    std::string normalized = headerName;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    const auto found = std::find_if(_responseHeaders.begin(), _responseHeaders.end(),
                                    [&normalized](const Header& header) { return header.name == normalized; });
    return found == _responseHeaders.end() ? "" : found->value;
  }

  static bool resolveUrl(const std::string& baseUrl, const std::string& location, std::string& resolved) {
    if (location.find("://") != std::string::npos) {
      resolved = location;
      return true;
    }
    std::string scheme;
    std::string host;
    std::string path;
    uint16_t port = 0;
    if (!parseUrl(baseUrl, scheme, host, path, port)) return false;
    const std::string authority = hostHeaderFor(scheme, host, port);
    if (!location.empty() && location[0] == '/') {
      resolved = scheme + "://" + authority + location;
      return true;
    }
    std::string parent = path;
    const size_t slash = parent.rfind('/');
    parent = slash == std::string::npos ? "/" : parent.substr(0, slash + 1);
    resolved = scheme + "://" + authority + parent + location;
    return true;
  }

  // True if the library was built with wolfSSL TLS 1.3 support enabled.
  static bool tls13Available() { return SecureClient::tls13Available(); }

 private:
  struct Header {
    std::string name;
    std::string value;
  };

  static bool isRedirectStatus(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
  }

  static bool parseUrl(const std::string& url, std::string& scheme, std::string& host, std::string& path,
                       uint16_t& port) {
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    // URL schemes are case-insensitive (RFC 3986 §3.1): "HTTPS://..." from a
    // server's Location header or user input must parse like "https://...".
    scheme = url.substr(0, schemeEnd);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    const size_t hostStart = schemeEnd + 3;
    const size_t pathStart = url.find('/', hostStart);
    const std::string hostPort =
        pathStart == std::string::npos ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
    path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    const size_t portSep = hostPort.rfind(':');
    if (portSep != std::string::npos) {
      host = hostPort.substr(0, portSep);
      port = static_cast<uint16_t>(atoi(hostPort.substr(portSep + 1).c_str()));
    } else {
      host = hostPort;
      port = scheme == "https" ? 443 : 80;
    }
    return !host.empty() && (scheme == "http" || scheme == "https");
  }

  std::string hostHeader() const {
    return hostHeaderFor(_scheme, _host, _port);
  }

  static std::string hostHeaderFor(const std::string& scheme, const std::string& host, uint16_t port) {
    const uint16_t defaultPort = scheme == "https" ? 443 : 80;
    if (port == 0 || port == defaultPort) return host;
    return host + ":" + std::to_string(port);
  }

  // True when the kept-alive connection matches the target of the next request
  // and still looks alive. "Looks" is best-effort: the server may have closed
  // it already, which the send path handles with its one-shot retry.
  bool connectionMatches() const {
    return _conn && _conn->connected() && _connHttps == (_scheme == "https") && _connHost == _host &&
           _connPort == _port;
  }

  // Reuse the kept-alive connection when it matches, else (re)connect.
  bool ensureConnected() {
    if (connectionMatches()) return true;
    closeConnection();
    if (_scheme == "https") {
      if (_insecure) {
        _secure.setInsecure();
      } else if (_rootCA) {
        _secure.setCACert(_rootCA);
      }
      _secure.setTimeout(_timeoutMs / 1000);
      if (!_secure.connect(_host.c_str(), _port)) return false;
      _conn = &_secure;
      _connHttps = true;
    } else {
      _plain.setTimeout(_timeoutMs / 1000);
      if (!_plain.connect(_host.c_str(), _port)) return false;
      _conn = &_plain;
      _connHttps = false;
    }
    _connHost = _host;
    _connPort = _port;
    return true;
  }

  void closeConnection() {
    if (_conn) {
      _conn->stop();
      _conn = nullptr;
    }
    _connHost.clear();
    _connPort = 0;
    _connHttps = false;
  }

  // Request line + headers (+ body). Write results are checked: a stale
  // keep-alive socket often surfaces first as a failed write.
  bool writeRequest(const char* method, const uint8_t* payload, size_t payloadLen) {
    std::string req = std::string(method) + " " + _path + " HTTP/1.1\r\nHost: " + hostHeader() + "\r\n";
    req += "User-Agent: " + _userAgent + "\r\n";
    req += _reuse ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    if (!_authUser.empty()) {
      const std::string creds = _authUser + ":" + _authPass;
      req += "Authorization: Basic " + std::string(base64::encode(creds.c_str()).c_str()) + "\r\n";
    }
    for (const std::string& h : _headers) req += h;
    if (payload && payloadLen) req += "Content-Length: " + std::to_string(payloadLen) + "\r\n";
    req += "\r\n";
    if (_conn->write(reinterpret_cast<const uint8_t*>(req.data()), req.size()) != req.size()) return false;
    if (payload && payloadLen && _conn->write(payload, payloadLen) != payloadLen) return false;
    return true;
  }

  bool isAborted(const AbortCallback& shouldAbort) {
    if (shouldAbort && shouldAbort()) {
      _aborted = true;
      return true;
    }
    return false;
  }

  bool emitBody(const DataCallback& onData, const uint8_t* data, size_t len) {
    if (!onData(data, len)) {
      _callbackAborted = true;
      return false;
    }
    _downloaded += len;
    if (_reportProgress && !_progress(_downloaded, _haveContentLength ? _contentLength : 0)) {
      _callbackAborted = true;
      return false;
    }
    return true;
  }

  // Reads one CRLF-terminated line (CR stripped). Returns false on timeout, a
  // closed connection with no pending data, or an over-long line.
  bool readLine(Client& c, std::string& line, unsigned long deadline, const AbortCallback& shouldAbort = nullptr) {
    line.clear();
    while (static_cast<int32_t>(millis() - deadline) < 0) {
      if (isAborted(shouldAbort)) return false;
      while (c.available() > 0) {
        const int ch = c.read();
        if (ch < 0) break;
        if (ch == '\n') {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          return true;
        }
        if (line.size() >= MAX_LINE) return false;
        line += static_cast<char>(ch);
      }
      if (!c.connected() && c.available() == 0) return false;
      delay(1);
    }
    return false;
  }

  // Streams exactly `count` body bytes.
  bool readFixed(Client& c, size_t count, const DataCallback& onData, const AbortCallback& shouldAbort = nullptr) {
    uint8_t buf[READ_CHUNK];
    size_t remaining = count;
    unsigned long deadline = millis() + _timeoutMs;
    while (remaining > 0) {
      if (isAborted(shouldAbort)) return false;
      const size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
      const int n = c.read(buf, want);
      if (n <= 0) {
        if (!c.connected() && c.available() == 0) return false;
        if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
        delay(2);
        continue;
      }
      deadline = millis() + _timeoutMs;
      if (!emitBody(onData, buf, static_cast<size_t>(n))) return false;
      remaining -= static_cast<size_t>(n);
    }
    return true;
  }

  // Streams body bytes until the peer closes (Connection: close, no length).
  bool readUntilClose(Client& c, const DataCallback& onData, const AbortCallback& shouldAbort = nullptr) {
    uint8_t buf[READ_CHUNK];
    unsigned long deadline = millis() + _timeoutMs;
    for (;;) {
      if (isAborted(shouldAbort)) return false;
      const int n = c.read(buf, sizeof(buf));
      if (n <= 0) {
        if (!c.connected() && c.available() == 0) return true;
        if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
        delay(2);
        continue;
      }
      deadline = millis() + _timeoutMs;
      if (!emitBody(onData, buf, static_cast<size_t>(n))) return false;
    }
  }

  // Decodes a chunked body. Stops at the zero-size chunk and drains trailers.
  bool readChunked(Client& c, const DataCallback& onData, const AbortCallback& shouldAbort = nullptr) {
    std::string line;
    for (;;) {
      if (isAborted(shouldAbort)) return false;
      if (!readLine(c, line, millis() + _timeoutMs, shouldAbort)) return false;
      const size_t ext = line.find(';');
      const std::string sizeText = ext == std::string::npos ? line : line.substr(0, ext);
      char* parseEnd = nullptr;
      const unsigned long size = strtoul(sizeText.c_str(), &parseEnd, 16);
      if (parseEnd == sizeText.c_str()) return false;
      if (size == 0) {
        while (readLine(c, line, millis() + _timeoutMs, shouldAbort) && !line.empty()) {
        }
        return !_aborted;
      }
      if (!readFixed(c, size, onData, shouldAbort)) return false;
      if (!readLine(c, line, millis() + _timeoutMs, shouldAbort)) return false;  // consume trailing CRLF
    }
  }

  // Body read buffer. 2 KB drains wolfSSL's decrypted TLS records in few
  // enough read() calls to keep large downloads moving: at 512 B a consuming
  // firmware measured ~30 KB/s and slow CDNs (Cloudflare) dropped the
  // connection mid-stream; 2 KB removed the stall. Stack-allocated in the
  // body readers, so kept modest.
  static constexpr size_t READ_CHUNK = 2048;
  static constexpr size_t MAX_LINE = 4096;  // header / chunk-size line cap

  // Kept-alive connection state. _conn points at _secure or _plain while a
  // connection is held open, and null otherwise.
  WiFiClient _plain;
  SecureClient _secure;
  Client* _conn = nullptr;
  std::string _connHost;
  uint16_t _connPort = 0;
  bool _connHttps = false;
  bool _reuse = true;

  std::string _scheme;
  std::string _host;
  std::string _path;
  std::string _body;
  std::vector<Header> _responseHeaders;
  uint16_t _port = 0;
  int _status = 0;
  size_t _contentLength = 0;
  const char* _rootCA = nullptr;
  std::string _userAgent = "FreeInk-ESP32";
  std::string _authUser;
  std::string _authPass;
  bool _insecure = false;
  int _followRedirects = 0;
  bool _allowRedirectDowngrade = false;
  ProgressCallback _progress;
  size_t _downloaded = 0;
  bool _reportProgress = false;
  bool _haveContentLength = false;
  bool _bodyComplete = false;
  bool _callbackAborted = false;
  bool _aborted = false;
  uint32_t _timeoutMs = 15000;
  std::vector<std::string> _headers;
};

}  // namespace freeink
