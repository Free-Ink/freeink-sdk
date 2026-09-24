#pragma once
#include <string>

#include "OpdsFeedParser.h"
#include "OpdsPublicationDoc.h"
#include "OpdsTransport.h"

/**
 * OPDS protocol client: owns the network orchestration for browsing an OPDS
 * catalog so the firmware only builds UI. Handles feed fetching with the OPDS
 * 1.0 authentication handshake (401 auth document -> Basic / OAuth password /
 * OAuth implicit), access-token refresh, indirect-acquisition resolution,
 * OpenSearch template discovery, and publication-document fetching.
 *
 * All HTTP and URL resolution goes through an injected OpdsTransport, so the
 * client is platform-agnostic and unit-testable off-device.
 */
namespace freeink::opds {

// Progress phases reported while fetchFeed() drives a multi-second login, so
// the UI can update its status line.
enum class ClientPhase {
  SigningIn,  // a 401 was received; authenticating
  Retrying,   // auth succeeded; re-fetching the feed
};
using ClientStatusFn = void (*)(void* ctx, ClientPhase phase);

class OpdsClient {
 public:
  enum class FetchStatus {
    Ok,
    AuthFailed,           // login attempted but failed
    CredentialsMissing,   // server needs login but none is configured
    ParseFailed,          // response was not a usable feed
    FetchFailed,          // transport error / non-auth HTTP failure
  };

  explicit OpdsClient(OpdsTransport& transport) : http(transport) {}

  // --- configuration -------------------------------------------------------
  void setServer(std::string url, std::string username, std::string password);
  // Restore a persisted token set (from the SD token store).
  void setTokens(std::string access, std::string refresh, std::string refreshUrl);
  // Accept-Language for content negotiation (localized catalogs); empty sends
  // none.
  void setAcceptLanguage(std::string acceptLanguage) { acceptLang = std::move(acceptLanguage); }
  // Reset the per-session auth latches (Basic, credentials-missing). Tokens are
  // kept; call at the start of a browsing session.
  void resetAuthState();
  void onStatus(ClientStatusFn fn, void* ctx) {
    statusFn = fn;
    statusCtx = ctx;
  }

  // --- feed / publication --------------------------------------------------
  // Fetch an absolute feed URL into `parser`, driving the 401 auth handshake
  // and token refresh. On Ok, `parser` holds the finalized feed.
  FetchStatus fetchFeed(const std::string& url, OpdsFeedParser& parser);

  // Fetch and parse a standalone publication document (detail page). Returns
  // false when the fetch or parse failed; `out.valid` reflects the parse.
  bool fetchPublication(const std::string& docUrl, OpdsPublication& out);

  // Fetch an indirect-acquisition publication document and resolve it to an
  // absolute download URL. `outIsEpub` reports whether the link is typed EPUB.
  bool resolveIndirect(const std::string& docUrl, std::string& outDownloadUrl, bool& outIsEpub);

  // Fetch an OpenSearch description document and return its search template
  // (empty on failure). OPDS 1.x servers that don't inline a template link one.
  bool fetchSearchTemplate(const std::string& descUrl, std::string& outTemplate);

  // --- auth/token state (read back to persist, or to drive an app-side
  //     download whose auth must match) ------------------------------------
  const std::string& accessToken() const { return bearerToken; }
  const std::string& refreshToken() const { return refreshTokenValue; }
  const std::string& refreshUrl() const { return tokenRefreshUrl; }
  bool useBasicAuth() const { return basicAuthLatched; }
  bool credentialsMissing() const { return credentialsMissingFlag; }
  // True when the last fetchFeed() drove a 401 auth handshake, so the token
  // state may have changed (or been cleared) and should be persisted. Lets the
  // caller skip an SD write on the common already-authorized path.
  bool tokensDirty() const { return authDirty; }
  // Auth block for an app-side request (e.g. the file download) that must carry
  // the same credentials the client negotiated.
  HttpAuth downloadAuth() const;

 private:
  OpdsTransport& http;
  std::string serverUrl;
  std::string username;
  std::string password;
  std::string acceptLang;
  std::string bearerToken;
  std::string refreshTokenValue;
  std::string tokenRefreshUrl;
  // Latched only after a 401 whose auth document offers Basic (or a bare Basic
  // challenge): sending Basic preemptively breaks OAuth-only servers.
  bool basicAuthLatched = false;
  bool credentialsMissingFlag = false;
  bool authDirty = false;
  ClientStatusFn statusFn = nullptr;
  void* statusCtx = nullptr;

  HttpAuth currentAuth() const;
  void notify(ClientPhase phase) const {
    if (statusFn) statusFn(statusCtx, phase);
  }
  // Re-request the resource capturing the 401 auth document, pick a flow the
  // device can drive, and obtain credentials. True when a retry should follow.
  bool authenticate(const std::string& resourceUrl);
  // Renew the access token from the refresh token; true on success.
  bool tryRefreshToken();
};

}  // namespace freeink::opds
