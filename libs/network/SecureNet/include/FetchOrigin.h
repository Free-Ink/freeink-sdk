#pragma once

// FreeInk SDK — the origin credentials are scoped to. Dependency-free so
// network clients that never open a connection themselves can share it.

#include <cctype>
#include <string>

namespace freeink {

// scheme://host:port, lowercased, with the scheme's default port made explicit
// and any userinfo dropped: the unit credentials are scoped to.
inline std::string fetchOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return {};
  std::string scheme = url.substr(0, schemeEnd);
  const size_t hostStart = schemeEnd + 3;
  const size_t hostEnd = url.find_first_of("/?#", hostStart);
  std::string authority = url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority.erase(0, at + 1);
  for (auto* part : {&scheme, &authority}) {
    for (char& c : *part) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  const size_t bracket = authority.rfind(']');  // IPv6 literal: its colons are not a port
  if (authority.find(':', bracket == std::string::npos ? 0 : bracket) == std::string::npos) {
    authority += scheme == "https" ? ":443" : ":80";
  }
  return scheme + "://" + authority;
}

}  // namespace freeink
