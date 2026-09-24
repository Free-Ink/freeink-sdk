#pragma once
#include <string>

/**
 * Drive an OPDS OAuth implicit-grant login without a browser. Follows the
 * authentication document's "authenticate" URL, fills the first HTML login
 * form encountered with the stored credentials (carrying session cookies
 * across redirect hops, including federated SSO on another host), and
 * captures the access token from the opds://authorize callback redirect.
 *
 * Verified against Cantook/De Marque catalogs (Lirtuel + Samarcande SSO).
 * Only available on wolfSSL builds; returns false elsewhere.
 */
bool opdsImplicitAuthenticate(const std::string& authenticateUrl, const std::string& username,
                              const std::string& password, std::string& outToken);
