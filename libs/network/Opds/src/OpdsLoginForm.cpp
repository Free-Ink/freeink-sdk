#include "OpdsLoginForm.h"

#include <cctype>
#include <cstring>

#include "OpdsSearchTemplate.h"

namespace {

bool iequals(const std::string& a, const char* b) {
  const size_t blen = strlen(b);
  if (a.size() != blen) return false;
  for (size_t i = 0; i < blen; ++i) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

// Case-insensitive search for a tag opener ("<form", "<input") that is not a
// prefix of a longer name (e.g. "<formation" must not match).
size_t findTag(const char* html, size_t len, size_t from, const char* tag) {
  const size_t tagLen = strlen(tag);
  for (size_t i = from; i + tagLen <= len; ++i) {
    if (html[i] != '<') continue;
    size_t j = 0;
    while (j < tagLen && i + j < len &&
           tolower(static_cast<unsigned char>(html[i + j])) == static_cast<unsigned char>(tag[j])) {
      ++j;
    }
    if (j == tagLen) {
      const char next = i + tagLen < len ? html[i + tagLen] : '>';
      if (isspace(static_cast<unsigned char>(next)) || next == '>' || next == '/') return i;
    }
  }
  return std::string::npos;
}

// Decode the handful of HTML entities that appear in attribute values.
std::string decodeEntities(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '&') {
      if (in.compare(i, 5, "&amp;") == 0) {
        out += '&';
        i += 4;
        continue;
      }
      if (in.compare(i, 6, "&quot;") == 0) {
        out += '"';
        i += 5;
        continue;
      }
      if (in.compare(i, 5, "&#39;") == 0) {
        out += '\'';
        i += 4;
        continue;
      }
      if (in.compare(i, 4, "&lt;") == 0) {
        out += '<';
        i += 3;
        continue;
      }
      if (in.compare(i, 4, "&gt;") == 0) {
        out += '>';
        i += 3;
        continue;
      }
    }
    out += in[i];
  }
  return out;
}

// Attribute lookup inside a single tag's text (between '<' and '>').
std::string attrValue(const std::string& tag, const char* name) {
  const size_t nameLen = strlen(name);
  for (size_t i = 0; i + nameLen < tag.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < nameLen; ++j) {
      if (tolower(static_cast<unsigned char>(tag[i + j])) != static_cast<unsigned char>(name[j])) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    // Must be a standalone attribute name.
    if (i > 0 && !isspace(static_cast<unsigned char>(tag[i - 1]))) continue;
    size_t p = i + nameLen;
    while (p < tag.size() && isspace(static_cast<unsigned char>(tag[p]))) ++p;
    if (p >= tag.size() || tag[p] != '=') continue;
    ++p;
    while (p < tag.size() && isspace(static_cast<unsigned char>(tag[p]))) ++p;
    if (p >= tag.size()) return "";
    if (tag[p] == '"' || tag[p] == '\'') {
      const char quote = tag[p++];
      const size_t end = tag.find(quote, p);
      return decodeEntities(tag.substr(p, end == std::string::npos ? std::string::npos : end - p));
    }
    size_t end = p;
    while (end < tag.size() && !isspace(static_cast<unsigned char>(tag[end])) && tag[end] != '>') ++end;
    return decodeEntities(tag.substr(p, end - p));
  }
  return "";
}

bool attrPresent(const std::string& tag, const char* name) {
  const size_t nameLen = strlen(name);
  for (size_t i = 0; i + nameLen <= tag.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < nameLen; ++j) {
      if (tolower(static_cast<unsigned char>(tag[i + j])) != static_cast<unsigned char>(name[j])) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    if (i > 0 && !isspace(static_cast<unsigned char>(tag[i - 1]))) continue;
    const char next = i + nameLen < tag.size() ? tag[i + nameLen] : '>';
    if (isspace(static_cast<unsigned char>(next)) || next == '=' || next == '>' || next == '/') return true;
  }
  return false;
}

void appendField(std::string& body, const std::string& name, const std::string& value) {
  if (!body.empty()) body += '&';
  body += opdsPercentEncode(name);
  body += '=';
  body += opdsPercentEncode(value);
}

bool looksLikeLogin(const std::string& s) {
  std::string l = s;
  for (auto& c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return l.find("email") != std::string::npos || l.find("user") != std::string::npos ||
         l.find("login") != std::string::npos || l.find("ident") != std::string::npos ||
         l.find("account") != std::string::npos || l.find("signin") != std::string::npos;
}

}  // namespace

OpdsLoginForm buildOpdsLoginForm(const char* html, const size_t len, const std::string& username,
                                 const std::string& password) {
  OpdsLoginForm form;
  // First identifier-only form (email page of a two-step login), used only if
  // the document has no password form.
  OpdsLoginForm identityStep;
  size_t searchFrom = 0;

  // Walk every form. A form with a password input is the login form. Pages may
  // carry a search form first, and identifier-first logins (e.g. ebooks.com)
  // show an email-only page before the password page.
  while (true) {
    const size_t formStart = findTag(html, len, searchFrom, "<form");
    if (formStart == std::string::npos) break;
    const size_t formTagEnd = std::string(html + formStart, std::min<size_t>(len - formStart, 2048)).find('>');
    if (formTagEnd == std::string::npos) break;
    const std::string formTag(html + formStart, formTagEnd + 1);

    size_t formEnd = findTag(html, len, formStart + 1, "</form");
    if (formEnd == std::string::npos) formEnd = len;
    searchFrom = formEnd;

    std::string body;
    bool haveUser = false;
    bool havePassword = false;
    bool strongIdentity = false;  // the username field is clearly a login field
    size_t pos = formStart;
    while (true) {
      const size_t inputStart = findTag(html, len, pos, "<input");
      if (inputStart == std::string::npos || inputStart >= formEnd) break;
      const size_t inputEnd = std::string(html + inputStart, std::min<size_t>(len - inputStart, 2048)).find('>');
      if (inputEnd == std::string::npos) break;
      const std::string tag(html + inputStart, inputEnd + 1);
      pos = inputStart + inputEnd;

      const std::string name = attrValue(tag, "name");
      if (name.empty()) continue;
      std::string type = attrValue(tag, "type");
      if (type.empty()) type = "text";

      if (iequals(type, "password")) {
        appendField(body, name, password);
        havePassword = true;
      } else if (iequals(type, "text") || iequals(type, "email")) {
        if (!haveUser) {
          appendField(body, name, username);
          haveUser = true;
          strongIdentity = iequals(type, "email") || looksLikeLogin(name) || looksLikeLogin(attrValue(tag, "id")) ||
                           iequals(attrValue(tag, "autocomplete"), "username");
        }
      } else if (iequals(type, "hidden")) {
        appendField(body, name, attrValue(tag, "value"));
      } else if ((iequals(type, "checkbox") || iequals(type, "radio")) && attrPresent(tag, "checked")) {
        const std::string value = attrValue(tag, "value");
        appendField(body, name, value.empty() ? "on" : value);
      }
      // submit/button/reset/image inputs are not sent
    }

    if (havePassword) {
      form.found = true;
      form.hasPassword = true;
      form.action = attrValue(formTag, "action");
      form.body = std::move(body);
      return form;  // a password form always wins
    }
    // Remember the first credible identifier-only page (gated on a strong
    // login signal so a plain search box is not mistaken for a login step).
    if (haveUser && strongIdentity && !identityStep.found) {
      identityStep.found = true;
      identityStep.hasPassword = false;
      identityStep.action = attrValue(formTag, "action");
      identityStep.body = std::move(body);
    }
  }

  return identityStep.found ? identityStep : form;
}

void OpdsCookieJar::store(const std::string& host, const std::string& setCookieHeader) {
  const size_t attrsStart = setCookieHeader.find(';');
  const std::string pair = setCookieHeader.substr(0, attrsStart);
  const size_t eq = pair.find('=');
  if (eq == std::string::npos || eq == 0) return;
  Cookie cookie;
  cookie.name = pair.substr(0, eq);
  cookie.value = pair.substr(eq + 1);
  // Trim surrounding whitespace from the name.
  while (!cookie.name.empty() && isspace(static_cast<unsigned char>(cookie.name.front()))) cookie.name.erase(0, 1);
  if (cookie.name.empty()) return;

  cookie.domain = host;
  if (attrsStart != std::string::npos) {
    // A Domain attribute widens the cookie to that domain's subdomains.
    std::string attrs = setCookieHeader.substr(attrsStart);
    std::string lower = attrs;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const size_t dom = lower.find("domain=");
    if (dom != std::string::npos) {
      size_t start = dom + 7;
      size_t end = attrs.find(';', start);
      std::string domain = attrs.substr(start, end == std::string::npos ? std::string::npos : end - start);
      while (!domain.empty() && (domain.front() == '.' || isspace(static_cast<unsigned char>(domain.front())))) {
        domain.erase(0, 1);
      }
      while (!domain.empty() && isspace(static_cast<unsigned char>(domain.back()))) domain.pop_back();
      if (!domain.empty()) cookie.domain = domain;
    }
  }

  for (auto& existing : cookies) {
    if (existing.domain == cookie.domain && existing.name == cookie.name) {
      existing.value = std::move(cookie.value);
      return;
    }
  }
  cookies.push_back(std::move(cookie));
}

std::string OpdsCookieJar::headerFor(const std::string& host) const {
  std::string header;
  for (const auto& cookie : cookies) {
    const bool match = host == cookie.domain ||
                       (host.size() > cookie.domain.size() &&
                        host.compare(host.size() - cookie.domain.size(), cookie.domain.size(), cookie.domain) == 0 &&
                        host[host.size() - cookie.domain.size() - 1] == '.');
    if (!match) continue;
    if (!header.empty()) header += "; ";
    header += cookie.name;
    header += '=';
    header += cookie.value;
  }
  return header;
}

std::string extractOpdsAccessToken(const std::string& callbackUrl) {
  static constexpr char PARAM[] = "access_token=";
  size_t pos = 0;
  while ((pos = callbackUrl.find(PARAM, pos)) != std::string::npos) {
    const char before = pos > 0 ? callbackUrl[pos - 1] : '?';
    if (before == '?' || before == '&' || before == '#') {
      const size_t start = pos + sizeof(PARAM) - 1;
      size_t end = callbackUrl.find('&', start);
      if (end == std::string::npos) end = callbackUrl.size();
      // Percent-decode the token.
      std::string token;
      token.reserve(end - start);
      for (size_t i = start; i < end; ++i) {
        if (callbackUrl[i] == '%' && i + 2 < end && isxdigit(static_cast<unsigned char>(callbackUrl[i + 1])) &&
            isxdigit(static_cast<unsigned char>(callbackUrl[i + 2]))) {
          token += static_cast<char>(strtol(callbackUrl.substr(i + 1, 2).c_str(), nullptr, 16));
          i += 2;
        } else {
          token += callbackUrl[i];
        }
      }
      return token;
    }
    pos += sizeof(PARAM) - 1;
  }
  return "";
}
