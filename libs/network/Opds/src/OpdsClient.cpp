#include "OpdsClient.h"

#include <utility>

#include "OpdsAuthDoc.h"
#include "OpdsImplicitAuth.h"
#include "OpdsLog.h"
#include "OpdsSearchTemplate.h"
#include "OpenSearchDescParser.h"

namespace freeink::opds {

namespace {
// OPDS authentication documents are small; cap the 401-body capture.
constexpr size_t MAX_AUTH_DOC_BYTES = 8192;
// Publication / indirect-acquisition documents are bounded to keep heap low.
constexpr size_t MAX_PUB_DOC_BYTES = 16 * 1024;
// Prefer OPDS 2.0 JSON from servers that content-negotiate; Atom-only servers
// ignore this and serve their usual feed.
constexpr const char* FEED_ACCEPT = "application/opds+json,application/atom+xml;q=0.9,*/*;q=0.8";
constexpr const char* PUB_ACCEPT = "application/opds-publication+json,application/opds+json";

const char* orNull(const std::string& s) { return s.empty() ? nullptr : s.c_str(); }

// Primary language subtag of an Accept-Language value, region stripped
// ("fr,en;q=0.8" -> "fr", "en-US" -> "en"). Localized title keys may carry a
// region; matching a bare primary subtag against them covers both.
std::string primarySubtag(const std::string& acceptLang) {
  const size_t cut = acceptLang.find_first_of(",;-");
  return acceptLang.substr(0, cut == std::string::npos ? acceptLang.size() : cut);
}
}  // namespace

void OpdsClient::setServer(std::string url, std::string user, std::string pass) {
  serverUrl = std::move(url);
  username = std::move(user);
  password = std::move(pass);
}

void OpdsClient::setTokens(std::string access, std::string refresh, std::string refreshUrl) {
  bearerToken = std::move(access);
  refreshTokenValue = std::move(refresh);
  tokenRefreshUrl = std::move(refreshUrl);
}

void OpdsClient::resetAuthState() {
  basicAuthLatched = false;
  credentialsMissingFlag = false;
}

HttpAuth OpdsClient::currentAuth() const {
  HttpAuth a;
  if (basicAuthLatched) {
    a.username = username;
    a.password = password;
  }
  a.bearer = bearerToken;
  return a;
}

HttpAuth OpdsClient::downloadAuth() const { return currentAuth(); }

OpdsClient::FetchStatus OpdsClient::fetchFeed(const std::string& url, OpdsFeedParser& parser) {
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  // Resolve localized (language-map) titles to the UI language.
  parser.setPreferredLanguage(primarySubtag(acceptLang).c_str());
  authDirty = false;
  for (int authAttempt = 0;; ++authAttempt) {
    const HttpResult res =
        http.get(url, currentAuth(), FEED_ACCEPT, orNull(acceptLang), false,
                 [&parser](const uint8_t* data, const size_t len) {
                   parser.write(data, len);
                   return !parser.error();  // abort the transfer on a parse error
                 });
    parser.flush();

    if (!res.ok && res.status == 401) {
      // The 401 body is an authentication document describing the server's
      // flows. Try to renew an expired access token first (cheap); fall back to
      // a full login (several seconds of TLS handshakes across the SSO).
      credentialsMissingFlag = false;
      authDirty = true;  // token state may change or be cleared this attempt
      notify(ClientPhase::SigningIn);
      bool advanced = false;
      if (authAttempt == 0 && !refreshTokenValue.empty() && !tokenRefreshUrl.empty()) {
        if (tryRefreshToken()) {
          advanced = true;
        } else {
          refreshTokenValue.clear();  // dead refresh token; drop it and log in fresh
          tokenRefreshUrl.clear();
        }
      }
      if (!advanced && authAttempt < 2) {
        bearerToken.clear();
        basicAuthLatched = false;
        advanced = authenticate(url);
      }
      if (advanced) {
        parser.reset();  // drop any finalized backend before the retry
        notify(ClientPhase::Retrying);
        continue;
      }
      return credentialsMissingFlag ? FetchStatus::CredentialsMissing : FetchStatus::AuthFailed;
    }
    if (parser.error()) return FetchStatus::ParseFailed;
    if (!res.ok) return FetchStatus::FetchFailed;
    return FetchStatus::Ok;
  }
}

bool OpdsClient::fetchPublication(const std::string& docUrl, OpdsPublication& out) {
  std::string doc;
  doc.reserve(4096);
  http.get(docUrl, currentAuth(), PUB_ACCEPT, orNull(acceptLang), false,
           [&doc](const uint8_t* data, const size_t len) {
             const size_t room = doc.size() < MAX_PUB_DOC_BYTES ? MAX_PUB_DOC_BYTES - doc.size() : 0;
             doc.append(reinterpret_cast<const char*>(data), len < room ? len : room);
             return true;
           });
  parseOpdsPublicationDoc(doc.data(), doc.size(), out, primarySubtag(acceptLang).c_str());
  return out.valid;
}

bool OpdsClient::resolveIndirect(const std::string& docUrl, std::string& outDownloadUrl, bool& outIsEpub) {
  std::string doc;
  doc.reserve(4096);
  const bool fetched = http.get(docUrl, currentAuth(), nullptr, nullptr, false,
                                [&doc](const uint8_t* data, const size_t len) {
                                  const size_t room = doc.size() < MAX_PUB_DOC_BYTES ? MAX_PUB_DOC_BYTES - doc.size() : 0;
                                  doc.append(reinterpret_cast<const char*>(data), len < room ? len : room);
                                  return true;
                                })
                           .ok;
  std::string resolved;
  if (!fetched || !resolveOpdsIndirectAcquisition(doc.data(), doc.size(), resolved, outIsEpub)) {
    LOG_ERR("OPDS", "Could not resolve indirect acquisition");
    return false;
  }
  outDownloadUrl = http.resolveUrl(docUrl, resolved);
  LOG_DBG("OPDS", "Resolved indirect acquisition -> %s (epub=%d)", outDownloadUrl.c_str(), outIsEpub);
  return true;
}

bool OpdsClient::fetchSearchTemplate(const std::string& descUrl, std::string& outTemplate) {
  LOG_DBG("OPDS", "Fetching OpenSearch description: %s", descUrl.c_str());
  OpenSearchDescParser parser;
  const bool fetched = http.get(descUrl, currentAuth(), nullptr, nullptr, false,
                                [&parser](const uint8_t* data, const size_t len) {
                                  parser.write(data, len);
                                  return !parser.error();
                                })
                           .ok;
  parser.flush();
  if (!fetched || parser.error() || parser.getTemplate().empty()) {
    LOG_ERR("OPDS", "OpenSearch description unusable");
    return false;
  }
  outTemplate = parser.getTemplate();
  return true;
}

// Authentication for OPDS 1.0: re-request the resource capturing the 401 body
// (the authentication document), pick a flow the device can drive, and obtain
// credentials for retrying.
//  - basic: latched so subsequent requests send the stored credentials.
//  - oauth/password: POST the stored credentials to the "authenticate" link and
//    keep the returned access token as a Bearer header.
//  - oauth/implicit: an HTML login (possibly a federated SSO) driven headlessly.
bool OpdsClient::authenticate(const std::string& resourceUrl) {
  std::string body;
  body.reserve(1024);
  const HttpResult res = http.get(resourceUrl, HttpAuth{}, nullptr, nullptr, true,
                                  [&body](const uint8_t* data, const size_t len) {
                                    const size_t room = MAX_AUTH_DOC_BYTES - body.size();
                                    body.append(reinterpret_cast<const char*>(data), len < room ? len : room);
                                    return true;
                                  });
  if (res.status != 401) return false;  // not an auth challenge

  // An empty 401 body is a bare Basic challenge; a JSON body is an OPDS auth
  // document. Either way we proceed to pick a flow below.
  OpdsAuthDoc doc;
  const bool haveAuthDoc = parseOpdsAuthDocument(body.data(), body.size(), doc);
  LOG_DBG("OPDS", "Auth flows: doc=%d basic=%d password=%d implicit=%d", haveAuthDoc, doc.hasBasic,
          doc.hasOauthPassword, doc.hasOauthImplicit);
  if (username.empty() || password.empty()) {
    // Every flow needs the stored credentials; surface the real problem.
    LOG_ERR("OPDS", "Server requires login but no credentials are configured");
    credentialsMissingFlag = true;
    return false;
  }

  if (doc.hasOauthPassword && !doc.tokenUrl.empty()) {
    const std::string tokenUrl = http.resolveUrl(resourceUrl, doc.tokenUrl);
    const std::string form =
        "grant_type=password&username=" + opdsPercentEncode(username) + "&password=" + opdsPercentEncode(password);
    std::string response;
    int tokenStatus = 0;
    if (http.postForm(tokenUrl, form, response, tokenStatus)) {
      std::string token;
      if (extractJsonStringField(response.data(), response.size(), "access_token", token) && !token.empty()) {
        bearerToken = std::move(token);
        // Capture a refresh token so the next expiry renews without a full
        // login. Refresh goes to the auth document's `refresh` link when
        // present, otherwise back to the token endpoint (RFC 6749 6).
        refreshTokenValue.clear();
        extractJsonStringField(response.data(), response.size(), "refresh_token", refreshTokenValue);
        tokenRefreshUrl = refreshTokenValue.empty() ? std::string()
                          : doc.refreshUrl.empty()   ? tokenUrl
                                                     : http.resolveUrl(resourceUrl, doc.refreshUrl);
        LOG_INF("OPDS", "OAuth password grant succeeded");
        return true;
      }
    }
    LOG_ERR("OPDS", "OAuth token request failed (status %d)", tokenStatus);
    return false;
  }

  // Implicit grant: the server's login is an HTML form (possibly a federated
  // SSO on another host). Drive it headlessly with the stored credentials and
  // capture the opds://authorize callback token.
  if (doc.hasOauthImplicit && !doc.implicitUrl.empty()) {
    const std::string implicitUrl = http.resolveUrl(resourceUrl, doc.implicitUrl);
    std::string token;
    if (opdsImplicitAuthenticate(implicitUrl, username, password, token)) {
      bearerToken = std::move(token);
      // Implicit grant issues no refresh token; re-login on expiry.
      refreshTokenValue.clear();
      tokenRefreshUrl.clear();
      LOG_INF("OPDS", "Implicit OAuth login succeeded");
      return true;
    }
    return false;
  }

  // HTTP Basic: either the auth document offers it, or the 401 wasn't an OPDS
  // auth document at all (a plain Basic-protected server, e.g. calibre-web).
  // Latch it so subsequent requests send the credentials.
  if (doc.hasBasic || !haveAuthDoc) {
    LOG_INF("OPDS", "Using HTTP Basic authentication");
    basicAuthLatched = true;
    bearerToken.clear();  // Basic uses stored credentials, not a token
    refreshTokenValue.clear();
    tokenRefreshUrl.clear();
    return true;
  }
  return false;
}

bool OpdsClient::tryRefreshToken() {
  const std::string form = "grant_type=refresh_token&refresh_token=" + opdsPercentEncode(refreshTokenValue);
  std::string response;
  int tokenStatus = 0;
  if (!http.postForm(tokenRefreshUrl, form, response, tokenStatus)) {
    LOG_ERR("OPDS", "Token refresh failed (status %d)", tokenStatus);
    return false;
  }
  std::string token;
  if (!extractJsonStringField(response.data(), response.size(), "access_token", token) || token.empty()) {
    LOG_ERR("OPDS", "Token refresh response had no access_token");
    return false;
  }
  bearerToken = std::move(token);
  // A rotated refresh token replaces the old one; otherwise keep reusing it.
  std::string rotated;
  if (extractJsonStringField(response.data(), response.size(), "refresh_token", rotated) && !rotated.empty()) {
    refreshTokenValue = std::move(rotated);
  }
  LOG_INF("OPDS", "Access token refreshed");
  return true;
}

}  // namespace freeink::opds
