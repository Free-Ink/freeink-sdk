#pragma once
#include <string>
#include <vector>

/**
 * Pure helpers for driving an OPDS OAuth implicit-grant login without a
 * browser (Cantook/De Marque servers such as Lirtuel): scraping the HTML
 * login form, carrying session cookies across the redirect chain, and
 * extracting the access token from the opds://authorize callback.
 * The network sequencing lives in src/network/OpdsImplicitAuth.
 */

struct OpdsLoginForm {
  bool found = false;
  bool hasPassword = false;  // false for an identifier-first step (email page)
  std::string action;        // raw action attribute; empty means "post to the same URL"
  std::string body;          // x-www-form-urlencoded fields with credentials filled in
};

/**
 * Scan an HTML document for the first <form> containing a password input.
 * Hidden fields (and pre-checked checkboxes/radios) are passed through, the
 * first text-like input receives the username, password inputs the password.
 */
OpdsLoginForm buildOpdsLoginForm(const char* html, size_t len, const std::string& username,
                                 const std::string& password);

/**
 * Minimal cookie jar for the login redirect chain. Cookies are keyed by the
 * response host (or an explicit Domain attribute) and matched by domain
 * suffix; attributes such as Path, Secure and Expires are ignored — the jar
 * lives for one authentication attempt only.
 */
class OpdsCookieJar {
 public:
  void store(const std::string& host, const std::string& setCookieHeader);
  std::string headerFor(const std::string& host) const;

 private:
  struct Cookie {
    std::string domain;
    std::string name;
    std::string value;
  };
  std::vector<Cookie> cookies;
};

/**
 * Extract the percent-decoded access_token parameter from an OAuth callback
 * URL (query or fragment form). Empty string when absent.
 */
std::string extractOpdsAccessToken(const std::string& callbackUrl);
