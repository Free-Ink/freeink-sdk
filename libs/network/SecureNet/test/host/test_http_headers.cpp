// Exercise the production HTTP parser. TLS is a scripted transport, not wolfSSL.
#include <ResumableFetch.h>

#include <cassert>
#include <cstdio>

namespace freeink {
SecureClient::~SecureClient() = default;
void SecureClient::setCACert(const char*) {}
void SecureClient::setInsecure() {}
int SecureClient::connect(IPAddress ip, uint16_t port) { return _transport.connect(ip, port); }
int SecureClient::connect(const char* host, uint16_t port) { return _transport.connect(host, port); }
size_t SecureClient::write(uint8_t b) { return _transport.write(b); }
size_t SecureClient::write(const uint8_t* p, size_t n) { return _transport.write(p, n); }
int SecureClient::available() { return _transport.available(); }
int SecureClient::read() { return _transport.read(); }
int SecureClient::read(uint8_t* p, size_t n) { return _transport.read(p, n); }
int SecureClient::peek() { return _transport.peek(); }
void SecureClient::flush() {}
void SecureClient::stop() { _transport.stop(); }
uint8_t SecureClient::connected() { return _transport.connected(); }
bool SecureClient::tls13Available() { return false; }
bool hostResolves(const char*) { return true; }
}  // namespace freeink

static void reset(std::initializer_list<std::string> replies) {
  FakeNet::replies().assign(replies);
  FakeNet::requests().clear();
  FakeNet::hosts().clear();
  FakeNet::readDelay() = 0;
  FakeNet::bytesRead() = 0;
  FakeNet::holdOpen() = false;
  fakeNow() = 0;
}
static const std::string status = "HTTP/1.1 200 OK\r\n";
static const std::string end = "Content-Length: 2\r\n\r\nok";
static std::string field(const std::string& name, size_t length) {
  assert(length >= name.size() + 2);
  return name + ": " + std::string(length - name.size() - 2, 'x');
}
static void check(const std::string& reply, bool succeeds) {
  reset({reply});
  freeink::SecureHttpClient http;
  assert(http.begin("https://a/font"));
  const int code = http.GET();
  assert(code == (succeeds ? 200 : -1));
  assert(http.responseComplete() == succeeds);
  assert(http.getString() == (succeeds ? "ok" : ""));
  assert(FakeNet::bytesRead() <= 8192);
}
int main() {
  // Buffered cap includes CR, excludes LF, preserving the existing boundary.
  for (size_t n : {4094, 4095, 4096, 4097, 4173}) {
    check(status + field("X-Ordinary", n) + "\r\n" + end, n <= 4095);
    check(status + field("CoNtEnT-SeCuRiTy-PoLiCy", n) + "\r\n" + end, true);
  }
  reset({status + field("Content-Security-Policy", 4095) + "\r\n" + end});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/f") && http.GET() == 200);
    assert(http.getHeader("content-security-policy").size() == 4095 - 25);
  }
  check(status + field("X-Ordinary", 4096) + "\n" + end, true);
  const auto csp = field("Content-Security-Policy", 4173) + "\r\n";
  // Similar names, leading whitespace, and all critical fields still fail.
  for (const char* name :
       {"Content-Security-Policy-Report-Only", " Content-Security-Policy", "Content-Security-Policy ", "Location",
        "Content-Length", "Transfer-Encoding", "Content-Disposition", "WWW-Authenticate", "Set-Cookie", "Connection"}) {
    check(status + field(name, 4200) + "\r\n" + end, false);
  }
  check(std::string("HTTP/1.1 200 ") + std::string(4100, 'x') + "\r\n" + end, false);
  for (bool before : {false, true}) {
    const std::string length = "Content-Length: 2\r\n";
    check(status + (before ? length + csp : csp + length) + "\r\nok", true);
    const std::string critical =
        "Content-Disposition: attachment; filename=font.bin\r\n"
        "WWW-Authenticate: Basic realm=fonts\r\nConnection: close\r\n";
    reset({status + (before ? critical + csp : csp + critical) + end});
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/font") && http.GET() == 200);
    assert(http.responseComplete() && http.getString() == "ok");
    assert(http.getHeader("content-disposition") == "attachment; filename=font.bin");
    assert(http.getHeader("www-authenticate") == "Basic realm=fonts");
    assert(http.getHeader("content-security-policy").empty());
    const std::string chunked = "Transfer-Encoding: chunked\r\n";
    check(status + (before ? chunked + csp : csp + chunked) + "\r\n2\r\nok\r\n0\r\n\r\n", true);
  }
  // Exact aggregate boundary includes status, every CR/LF and final blank line.
  const size_t overhead = status.size() + 2 + end.size() - 2;
  check(status + field("Content-Security-Policy", 8192 - overhead) + "\r\n" + end, true);
  check(status + field("Content-Security-Policy", 8193 - overhead) + "\r\n" + end, false);
  check(status + csp + csp + end, false);
  check(status + field("X-A", 4000) + "\r\n" + field("X-B", 4000) + "\r\n" + end, true);
  check(status + field("X-A", 4095) + "\r\n" + field("X-B", 4095) + "\r\n" + end, false);
  for (const std::string& tail : {std::string(""), std::string("Content-Length: 0\r\n"), csp,
                                  csp.substr(0, csp.size() - 1), field("Content-Security-Policy", 6000)}) {
    check(status + tail, false);
  }
  // Incomplete headers never follow a redirect or deliver a body.
  reset({"HTTP/1.1 302 Found\r\nLocation: https://b/f\r\n" + csp});
  {
    freeink::SecureHttpClient http;
    http.setFollowRedirects(3);
    assert(http.begin("https://a/f") && http.GET() == -1);
    assert(FakeNet::requests().size() == 1);
  }
  for (size_t n : {63, 64}) {
    std::string headers;
    for (size_t i = 0; i < n; ++i) headers += "X: y\r\n";
    check(status + headers + end, n == 63);  // Content-Length is one more field.
  }
  // Slow but continuously available data cannot bypass the absolute deadline.
  reset({status + csp + end});
  FakeNet::readDelay() = 1;
  {
    freeink::SecureHttpClient http;
    http.setTimeout(100);
    assert(http.begin("https://a/f") && http.GET() == -1);
    assert(FakeNet::bytesRead() == 100 && fakeNow() == 100);
  }
  reset({status + csp});
  FakeNet::holdOpen() = true;
  {
    freeink::SecureHttpClient http;
    http.setTimeout(100);
    assert(http.begin("https://a/f") && http.GET() == -1 && fakeNow() == 100);
  }
  reset({status + csp + end});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("https://a/f"));
    assert(http.GET(
               [](const uint8_t*, size_t) {
                 assert(false);
                 return true;
               },
               [] { return FakeNet::bytesRead() >= 4200; }) == -1);
    assert(http.aborted() && FakeNet::bytesRead() == 4200);
  }
  // ResumableFetch owns origin-scoped authentication. Location survives before
  // or after discarded CSP, including long signed query strings.
  for (bool before : {false, true}) {
    for (const std::string& target : {std::string("https://a"), std::string("https://b")}) {
      const std::string path = "/asset?token=" + std::string(900, 'q');
      const std::string location = "Location: " + target + path + "\r\n";
      reset({"HTTP/1.1 302 Found\r\n" + (before ? location + csp : csp + location) + "Content-Length: 0\r\n\r\n",
             status + end});
      std::string body;
      freeink::FetchSink sink;
      sink.write = [&](const uint8_t* p, size_t n) {
        body.append(reinterpret_cast<const char*>(p), n);
        return true;
      };
      auto result = freeink::fetchResumable(
          "https://a/f", {},
          [](freeink::SecureHttpClient& http, bool sameOrigin) {
            if (sameOrigin) http.addHeader("Authorization", "Bearer secret");
          },
          sink);
      assert(result.complete && body == "ok" && FakeNet::requests().size() == 2);
      assert(FakeNet::requests()[1].find("GET " + path + " ") == 0);
      assert((FakeNet::requests()[1].find("Authorization:") != std::string::npos) == (target == "https://a"));
    }
  }
  for (bool allow : {false, true}) {
    reset({"HTTP/1.1 302 Found\r\n" + csp + "Location: http://b/f\r\nContent-Length: 0\r\n\r\n", status + end});
    freeink::SecureHttpClient http;
    http.setFollowRedirects(2);
    http.setAllowRedirectDowngrade(allow);
    assert(http.begin("https://a/f"));
    assert(http.GET() == (allow ? 200 : 302));
    assert(FakeNet::requests().size() == (allow ? 2 : 1));
  }
  puts("bounded response headers, deadlines, redirects and origin auth ok");
}
