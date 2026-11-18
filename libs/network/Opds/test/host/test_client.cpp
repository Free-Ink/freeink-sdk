#include "OpdsClient.h"

#include <cassert>
#include <cstdio>
#include <deque>

using namespace freeink::opds;

const std::string ROOT = "https://catalog.test";
const std::string FEED = R"(<feed xmlns="http://www.w3.org/2005/Atom"><title>Books</title></feed>)";
const std::string BASIC_DOC = R"({"authentication":[{"type":"http://opds-spec.org/auth/basic"}]})";
const std::string OAUTH_DOC = R"({"authentication":[{"type":"http://opds-spec.org/auth/oauth/password","links":[{"rel":"authenticate","href":"/token"}]}]})";

struct Reply {
  int status;
  std::string body;
  std::string challenge;
  bool redirected = false;
};

struct Transport : OpdsTransport {
  std::deque<Reply> replies;
  std::deque<Reply> posts;
  std::vector<HttpAuth> requests;
  std::vector<std::string> forms;

  HttpResult get(const std::string&, const HttpAuth& auth, const char*, const char*, bool capture,
                 const OpdsDataSink& sink) override {
    assert(!replies.empty());
    Reply reply = replies.front();
    replies.pop_front();
    requests.push_back(auth);
    const bool ok = reply.status >= 200 && reply.status < 300;
    if (ok || (capture && reply.status == 401))
      sink(reinterpret_cast<const uint8_t*>(reply.body.data()), reply.body.size());
    return {ok, reply.status, reply.challenge, reply.redirected};
  }
  bool postForm(const std::string& url, const std::string& form, std::string& response, int& status) override {
    assert(url == ROOT + "/token");
    assert(!posts.empty());
    Reply reply = posts.front();
    posts.pop_front();
    forms.push_back(form);
    response = reply.body;
    status = reply.status;
    return status == 200;
  }
  std::string resolveUrl(const std::string&, const std::string& ref) override { return ROOT + ref; }
};

void configure(OpdsClient& client) { client.setServer(ROOT, "reader", "secret", {{"X-Proxy", "value"}}); }

int main() {
  // Public Atom catalog, followed by an acquisition-only Basic challenge.
  {
    Transport http;
    OpdsClient client(http);
    configure(client);
    http.replies.push_back({200, FEED, ""});
    OpdsFeedParser parser;
    assert(client.fetchFeed(ROOT, parser) == OpdsClient::FetchStatus::Ok);
    assert(client.downloadAuth().username.empty());
    http.replies.push_back({401, "<html>Login required</html>", "Basic realm=books"});
    assert(client.authenticateDownload(ROOT + "/book.epub"));
    const auto auth = client.downloadAuth();
    assert(auth.username == "reader" && auth.password == "secret" && auth.bearer.empty());
    assert(auth.headers.size() == 1);
    assert(http.requests.back().username.empty());
  }
  // Challenge matching must ignore quoted commas and auth-params named Basic.
  for (const auto& challenge : {"Basic realm=books", "bAsIc realm=books",
                               "Digest realm=books, Basic realm=books", "Bearer realm=\"a,b\", Basic realm=books"}) {
    Transport http;
    OpdsClient client(http);
    configure(client);
    http.replies.push_back({401, "", challenge});
    assert(client.authenticateDownload(ROOT));
  }
  for (const auto& challenge : {"", "Bearer realm=books", "Digest realm=\"x, Basic realm=y\"",
                               "Bearer realm=books, Basic =value", "BasicOther realm=books"}) {
    Transport http;
    OpdsClient client(http);
    configure(client);
    http.replies.push_back({401, "", challenge});
    assert(!client.authenticateDownload(ROOT));
    assert(!client.useBasicAuth());
  }
  for (const auto& reply : {Reply{403, "", "Basic realm=books"}, Reply{401, BASIC_DOC, "", true}}) {
    Transport http;
    OpdsClient client(http);
    configure(client);
    http.replies.push_back(reply);
    assert(!client.authenticateDownload(ROOT));
    assert(!client.useBasicAuth());
  }
  // Existing feed authentication and OPDS Basic documents still work.
  {
    Transport http;
    OpdsClient client(http);
    configure(client);
    http.replies = {{401, "", ""}, {401, BASIC_DOC, ""}, {200, FEED, ""}};
    OpdsFeedParser parser;
    assert(client.fetchFeed(ROOT, parser) == OpdsClient::FetchStatus::Ok);
    assert(http.requests.back().username == "reader");
    assert(client.tokensDirty());
  }
  // Missing and rejected credentials terminate without unbounded retries.
  {
    Transport http;
    OpdsClient client(http);
    http.replies = {{401, "", "Basic realm=books"}};
    assert(!client.authenticateDownload(ROOT));
    assert(client.credentialsMissing());
  }
  {
    Transport http;
    OpdsClient client(http);
    configure(client);
    for (int i = 0; i < 5; ++i) http.replies.push_back({401, "", "Basic realm=books"});
    OpdsFeedParser parser;
    assert(client.fetchFeed(ROOT, parser) == OpdsClient::FetchStatus::AuthFailed);
    assert(http.replies.empty());
  }
  // OAuth retains precedence over Basic and download-time tokens can be persisted.
  {
    Transport http;
    OpdsClient client(http);
    configure(client);
    http.replies = {{401, OAUTH_DOC, "Basic realm=books"}};
    http.posts = {{200, R"({"access_token":"access","refresh_token":"refresh"})", ""}};
    assert(client.authenticateDownload(ROOT));
    assert(client.downloadAuth().bearer == "access");
    assert(client.downloadAuth().username.empty());
    assert(client.tokensDirty());
    assert(http.forms.front().find("grant_type=password") == 0);
    http.posts = {{200, R"({"access_token":"renewed"})", ""}};
    assert(client.authenticateDownload(ROOT));
    assert(client.accessToken() == "renewed");
    assert(http.forms.back().find("grant_type=refresh_token") == 0);
    assert(http.replies.empty());
    // Failed refresh falls back to the resource's challenge.
    http.posts = {{401, "", ""}};
    http.replies = {{401, "", "Basic realm=books"}};
    assert(client.authenticateDownload(ROOT));
    assert(client.useBasicAuth());
    assert(client.accessToken().empty() && client.refreshToken().empty());
  }
  std::puts("OPDS client authentication tests passed");
}
