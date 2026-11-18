// Production HTTP/resume control flow over scripted TLS transport, with a
// reservation spy. Actual wolfSSL ownership is tested by test_record_reservation.py.
#include <ResumableFetch.h>

#include <cassert>
#include <cstdio>

namespace {
int reservations = 0, liveReservations = 0;
bool failWrite = false;
bool peerClosesNow = false;  // the server closes the open connection at the next connected() check
}  // namespace
namespace freeink {
SecureClient::~SecureClient() { stop(); }
void SecureClient::setCACert(const char*) {}
void SecureClient::setInsecure() {}
int SecureClient::connect(IPAddress, uint16_t) { return 0; }
int SecureClient::connect(const char* host, uint16_t port) { return _transport.connect(host, port); }
size_t SecureClient::write(uint8_t b) { return write(&b, 1); }
size_t SecureClient::write(const uint8_t* data, size_t n) {
  if (failWrite) return 0;
  return _transport.write(data, n);
}
void SecureClient::reserveFullRecord() {
  assert(!FakeNet::requests().empty());
  const auto& req = FakeNet::requests().back();
  assert(req.size() >= 4 && req.substr(req.size() - 4) == "\r\n\r\n");
  assert(_transport.peek() == 'H');  // No response read before reservation.
  ++reservations;
  if (!_ctx) {
    _ctx = this;
    ++liveReservations;
  }
}
int SecureClient::available() { return _transport.available(); }
int SecureClient::read() { return _transport.read(); }
int SecureClient::read(uint8_t* buf, size_t size) { return _transport.read(buf, size); }
int SecureClient::peek() { return _transport.peek(); }
void SecureClient::flush() {}
void SecureClient::stop() {
  if (_ctx) {
    --liveReservations;
    _ctx = nullptr;
  }
  _transport.stop();
}
uint8_t SecureClient::connected() {
  if (peerClosesNow) {
    peerClosesNow = false;
    _transport.stop();
  }
  return _transport.connected();
}
bool SecureClient::tls13Available() { return true; }
bool hostResolves(const char*) { return true; }
}  // namespace freeink

void reset(std::initializer_list<const char*> replies) {
  assert(liveReservations == 0);
  reservations = 0;
  failWrite = false;
  peerClosesNow = false;
  FakeNet::replies().assign(replies.begin(), replies.end());
  FakeNet::requests().clear();
  FakeNet::hosts().clear();
}
int main() {
  for (bool reserve : {false, true}) {
    for (const char* url : {"http://a/book", "https://a/book"}) {
      reset({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"});
      freeink::SecureHttpClient http;
      http.setReserveFullRecord(reserve);
      http.setReuse(false);
      assert(http.begin(url));
      http.addHeader("Cookie", std::string(6000, 'x'));
      assert(http.GET() == 200 && http.responseComplete());
      assert(reservations == (reserve && std::string(url).find("https:") == 0 ? 1 : 0));
      assert(liveReservations == 0);
    }
  }
  // Failed sends must not consume the reserve; pre-request cancellation too.
  for (bool cancel : {false, true}) {
    reset({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"});
    freeink::SecureHttpClient http;
    http.setReserveFullRecord(true);
    assert(http.begin("https://a/book"));
    failWrite = !cancel;
    assert(http.GET([](const uint8_t*, size_t) { return true; }, [=] { return cancel; }) < 0);
    assert(reservations == 0 && liveReservations == 0);
  }
  // Body cancellation and malformed headers close and release the connection.
  for (const char* reply :
       {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"}) {
    reset({reply});
    freeink::SecureHttpClient http;
    http.setReserveFullRecord(true);
    assert(http.begin("https://a/book"));
    http.GET([](const uint8_t*, size_t) { return false; });
    assert(reservations == 1 && !http.responseComplete() && liveReservations == 0);
  }
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nabc",
         "HTTP/1.1 206 Partial Content\r\nContent-Length: 3\r\nContent-Range: bytes 3-5/6\r\n\r\ndef"});
  std::string body;
  freeink::FetchSink sink;
  sink.write = [&](const uint8_t* data, size_t size) {
    body.append(reinterpret_cast<const char*>(data), size);
    return true;
  };
  auto result = freeink::fetchResumable("https://a/book", {}, nullptr, sink);
  assert(result.complete && body == "abcdef" && reservations == 2 && liveReservations == 0);
  assert(FakeNet::requests()[1].find("Range: bytes=3-") != std::string::npos);

  // A kept-alive connection that the server closed between requests: the
  // request is sent again once, on a fresh connection.
  reset(
      {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcHTTP/1.1 2", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ndef"});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/one"));
    assert(http.GET() == 200 && http.getString() == "abc");
    assert(http.begin("https://a/two"));
    assert(http.GET() == 200 && http.getString() == "def");
    assert(FakeNet::hosts().size() == 2);
    assert(FakeNet::requests()[0].find("GET /two ") != std::string::npos);  // sent on the kept-alive connection
    assert(FakeNet::requests()[1].find("GET /two ") != std::string::npos);  // and again on a fresh one
  }
  // A connection opened by connect() has carried no request yet. A request
  // that fails on it is not sent again: the server may already have acted on it.
  reset({"HTTP/1.1 2", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/order"));
    assert(http.connect());
    assert(http.POST("pay once") < 0);
    assert(FakeNet::hosts().size() == 1 && FakeNet::replies().size() == 1);
    assert(FakeNet::requests().size() == 1 && FakeNet::requests()[0].find("\r\n\r\npay once") != std::string::npos);
  }
  // The server closes a kept-alive connection just before the next request
  // starts (here at the request's first abort check). The request goes out on
  // a fresh connection, so a drop before its status line is not retried.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc-", "HTTP/1.1 2",
         "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/one"));
    assert(http.GET() == 200);
    assert(http.begin("https://a/order"));
    bool first = true;
    const auto closeBeforeRequest = [&] {
      if (first) peerClosesNow = true;
      first = false;
      return false;
    };
    const std::string payload = "pay once";
    assert(http.sendRequest(
               "POST", reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
               [](const uint8_t*, size_t) { return true; }, closeBeforeRequest) < 0);
    assert(FakeNet::hosts().size() == 2 && FakeNet::replies().size() == 1);
  }
  // GitHub release assets redirect with a >4KB Content-Security-Policy header.
  // Discard that header without losing the Location (including signed query).
  const std::string cspPrefix = "CoNtEnT-SeCuRiTy-PoLiCy: ";
  const std::string longCsp = cspPrefix + std::string(4173 - cspPrefix.size(), 'x');
  assert(longCsp.size() == 4173);
  const std::string location = "https://cdn.example/asset?" + std::string(900, 'q');
  const std::string redirect =
      "HTTP/1.1 302 Found\r\n" + longCsp + "\r\nLocation: " + location + "\r\nContent-Length: 0\r\n\r\n";
  reset({redirect.c_str(), "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
  {
    freeink::FetchSink fontSink;
    std::string downloaded;
    fontSink.write = [&](const uint8_t* data, size_t size) {
      downloaded.append(reinterpret_cast<const char*>(data), size);
      return true;
    };
    auto font = freeink::fetchResumable("https://github.com/fonts.json", {}, nullptr, fontSink);
    assert(font.complete && font.status == 200 && downloaded == "ok");
    assert(FakeNet::hosts().size() == 2 && FakeNet::hosts()[1] == "cdn.example");
    assert(FakeNet::requests()[1].find("GET /asset?") == 0);
    assert(liveReservations == 0);
  }

  // Oversized framing/redirect headers still fail rather than being discarded.
  for (const char* name : {"Location", "Content-Length", "Transfer-Encoding"}) {
    const std::string malformed =
        std::string("HTTP/1.1 200 OK\r\n") + name + ": " + std::string(4200, 'x') + "\r\nContent-Length: 2\r\n\r\nok";
    reset({malformed.c_str()});
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/font"));
    assert(http.GET() == -1);
    assert(liveReservations == 0);
  }
  // Even discardable headers must fit within the total header budget.
  const std::string tooLongCsp =
      "HTTP/1.1 200 OK\r\n" + cspPrefix + std::string(8200, 'x') + "\r\nContent-Length: 2\r\n\r\nok";
  reset({tooLongCsp.c_str()});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/font"));
    assert(http.GET() == -1);
    assert(liveReservations == 0);
  }

  // Streamed bodies are exact, bounded, and rewind on a stale-connection retry.
  for (bool retry : {false, true}) {
    if (retry)
      reset({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcHTTP/1.1 2",
             "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
    else
      reset({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/upload"));
    if (retry) assert(http.GET() == 200);
    std::string payload(25000, 'x');
    const auto reader = [](void* context, size_t offset, uint8_t* out, size_t length) -> size_t {
      assert(length <= 256);
      auto& body = *static_cast<std::string*>(context);
      memcpy(out, body.data() + offset, length);
      return length;
    };
    assert(http.sendRequestStream("PUT", payload.size(), reader, &payload,
                                  [](const uint8_t*, size_t) { return true; }) == 200);
    assert(http.responseComplete());
    assert(FakeNet::requests().back().find("Content-Length: 25000\r\n") != std::string::npos);
    assert(FakeNet::requests().back().substr(FakeNet::requests().back().size() - payload.size()) == payload);
  }
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/upload"));
    assert(http.sendRequestStream(
               "PUT", 1000, [](void*, size_t, uint8_t*, size_t) -> size_t { return 0; }, nullptr,
               [](const uint8_t*, size_t) { return true; }) < 0);
    assert(!http.responseComplete());
  }

  const auto uploadReader = [](void*, size_t, uint8_t* out, size_t length) -> size_t {
    memset(out, 'x', length);
    return length;
  };
  const auto acceptResponse = [](const uint8_t*, size_t) { return true; };

  // A one-shot cancel after the first chunk must not replay a reused request.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokX", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/upload"));
    assert(http.GET() == 200);
    const size_t previousRequestSize = FakeNet::requests().back().size();
    bool cancelled = false;
    const auto cancel = [&] {
      if (!cancelled && FakeNet::requests().back().size() >= previousRequestSize + 256) {
        cancelled = true;
        return true;
      }
      return false;
    };
    assert(http.sendRequestStream("PUT", 512, uploadReader, nullptr, acceptResponse, cancel) == -1);
    assert(cancelled && http.aborted() && !http.responseComplete());
    assert(FakeNet::hosts().size() == 1 && FakeNet::replies().size() == 1);
    const auto& request = FakeNet::requests().back();
    const size_t bodyStart = request.find("\r\n\r\n", previousRequestSize) + 4;
    assert(request.substr(bodyStart) == std::string(256, 'x'));
  }

  // Streaming never follows redirects, even if ordinary requests are configured to.
  for (int hops : {0, 3}) {
    reset({"HTTP/1.1 307 Temporary Redirect\r\nLocation: https://b/upload\r\nContent-Length: 6\r\n\r\nreason"});
    freeink::SecureHttpClient http;
    http.setFollowRedirects(hops);
    assert(http.begin("https://a/upload"));
    std::string body;
    assert(http.sendRequestStream("PUT", 1, uploadReader, nullptr, [&](const uint8_t* data, size_t length) {
      body.append(reinterpret_cast<const char*>(data), length);
      return true;
    }) == 307);
    assert(body == "reason" && http.responseComplete());
    assert(http.getHeader("location") == "https://b/upload");
    assert(FakeNet::hosts().size() == 1);
  }

  // Rejecting a missing source clears the previous response without sending anything.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Old: previous\r\n\r\nok"});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/upload"));
    assert(http.GET() == 200 && http.responseComplete());
    assert(http.getString() == "ok" && http.getHeader("x-old") == "previous");
    const size_t requestSize = FakeNet::requests().back().size();
    assert(http.sendRequestStream("PUT", 1, nullptr, nullptr, acceptResponse) == -1);
    assert(http.getStatus() == 0 && !http.responseComplete());
    assert(http.getString().empty() && http.getHeaders().empty());
    assert(!http.hasContentLength() && http.getContentLength() == 0);
    assert(!http.aborted() && !http.callbackAborted());
    assert(FakeNet::hosts().size() == 1 && FakeNet::requests().back().size() == requestSize);
  }
  puts("bulk request timing, failure, cancellation and retry ok");
}
