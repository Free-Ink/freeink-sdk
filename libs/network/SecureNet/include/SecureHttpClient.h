#pragma once

// FreeInk SDK — minimal HTTPS client over SecureClient (wolfSSL TLS 1.3).
//
// Self-contained on purpose: it does NOT wrap Arduino HTTPClient. HTTPClient's
// begin() takes a NetworkClient&, but SecureClient is a plain Arduino Client
// (it owns a WiFiClient transport and runs wolfSSL on top), so it can't bind to
// that API. Instead this implements the small slice of HTTP/1.1 that firmware
// needs — GET/POST/PUT with custom headers and a buffered response body —
// directly over SecureClient, handling Content-Length, chunked, and
// connection-close-delimited responses.
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
// Header-only. Callers must pass a final URL (no redirect following here).
//
// OPT-IN: requires -DFREEINK_NET_WOLFSSL=1 for TLS. With the flag off,
// SecureClient is an inert stub and https requests fail at connect()
// (GET()/POST() return -1); plain-http URLs still work over the WiFiClient
// transport.

#include <Arduino.h>
#include <WiFiClient.h>

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

  // Skip peer verification (SecureClient does likewise). Required today because
  // the wolfSSL transport has no CA bundle wired up; see setCACert().
  void setInsecure() { _insecure = true; }
  // Verify against a single PEM root. Clears the insecure flag.
  void setCACert(const char* rootCA) {
    _rootCA = rootCA;
    _insecure = false;
  }
  void setTimeout(uint32_t ms) { _timeoutMs = ms; }

  // Parse the URL and reset per-request state. Returns false on a malformed
  // URL.
  bool begin(const std::string& url) {
    _headers.clear();
    _body.clear();
    _status = 0;
    return parseUrl(url, _scheme, _host, _path, _port);
  }
  // Each request uses Connection: close, so there is no socket to release here;
  // present for symmetry with HTTPClient call sites.
  void end() {}

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
    _status = 0;
    _body.clear();
    _responseHeaders.clear();
    _contentLength = 0;
    _haveContentLength = false;
    _bodyComplete = false;
    _callbackAborted = false;
    _aborted = false;

    WiFiClient plain;
    SecureClient secure;
    Client* client;
    if (_scheme == "https") {
      if (_insecure) {
        secure.setInsecure();
      } else if (_rootCA) {
        secure.setCACert(_rootCA);
      }
      client = &secure;
    } else {
      client = &plain;
    }
    client->setTimeout(_timeoutMs / 1000);
    if (isAborted(shouldAbort)) return -1;
    if (!client->connect(_host.c_str(), _port)) return -1;

    std::string req =
        std::string(method) + " " + _path + " HTTP/1.1\r\nHost: " + hostHeader() + "\r\nConnection: close\r\n";
    for (const std::string& h : _headers) req += h;
    if (payload && payloadLen) req += "Content-Length: " + std::to_string(payloadLen) + "\r\n";
    req += "\r\n";
    client->write(reinterpret_cast<const uint8_t*>(req.data()), req.size());
    if (payload && payloadLen) client->write(payload, payloadLen);

    const unsigned long headerDeadline = millis() + _timeoutMs;
    std::string line;
    if (!readLine(*client, line, headerDeadline, shouldAbort)) {
      client->stop();
      return -1;
    }
    // "HTTP/1.1 200 OK" — the status code starts at offset 9.
    _status = line.size() >= 12 ? atoi(line.c_str() + 9) : 0;

    std::string transferEncoding;
    while (readLine(*client, line, headerDeadline, shouldAbort)) {
      if (line.empty()) break;  // end of headers
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ') value.erase(value.begin());
      for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      _responseHeaders.push_back(Header{name, value});
      if (name == "content-length") {
        _contentLength = static_cast<size_t>(strtoul(value.c_str(), nullptr, 10));
        _haveContentLength = true;
      } else if (name == "transfer-encoding") {
        for (char& c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        transferEncoding = value;
      }
    }
    if (_aborted) {
      client->stop();
      return -1;
    }

    if (transferEncoding.find("chunked") != std::string::npos) {
      _bodyComplete = readChunked(*client, onData, shouldAbort);
    } else if (transferEncoding.empty() || transferEncoding == "identity") {
      if (_haveContentLength) {
        _bodyComplete = readFixed(*client, _contentLength, onData, shouldAbort);
      } else {
        _bodyComplete = readUntilClose(*client, onData, shouldAbort);
      }
    } else {
      _bodyComplete = false;
    }
    client->stop();
    return _status;
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
    for (char& c : normalized) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (const Header& header : _responseHeaders) {
      if (header.name == normalized) return header.value;
    }
    return "";
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

  static bool parseUrl(const std::string& url, std::string& scheme, std::string& host, std::string& path,
                       uint16_t& port) {
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    scheme = url.substr(0, schemeEnd);
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
      char* end = nullptr;
      const unsigned long size = strtoul(sizeText.c_str(), &end, 16);
      if (end == sizeText.c_str()) return false;
      if (size == 0) {
        while (readLine(c, line, millis() + _timeoutMs, shouldAbort) && !line.empty()) {
        }
        return !_aborted;
      }
      if (!readFixed(c, size, onData, shouldAbort)) return false;
      if (!readLine(c, line, millis() + _timeoutMs, shouldAbort)) return false;  // consume trailing CRLF
    }
  }

  static constexpr size_t READ_CHUNK = 128;  // body read buffer (small: kept on stack)
  static constexpr size_t MAX_LINE = 4096;   // header / chunk-size line cap

  std::string _scheme;
  std::string _host;
  std::string _path;
  std::string _body;
  std::vector<Header> _responseHeaders;
  uint16_t _port = 0;
  int _status = 0;
  size_t _contentLength = 0;
  const char* _rootCA = nullptr;
  bool _insecure = false;
  bool _haveContentLength = false;
  bool _bodyComplete = false;
  bool _callbackAborted = false;
  bool _aborted = false;
  uint32_t _timeoutMs = 15000;
  std::vector<std::string> _headers;
};

}  // namespace freeink
