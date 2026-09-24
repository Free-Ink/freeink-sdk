#pragma once
#include <Print.h>

#include <memory>
#include <string>
#include <vector>

#include "Opds2Parser.h"
#include "OpdsParser.h"

/**
 * Format-sniffing front-end for OPDS feeds. Buffers nothing: the first
 * non-whitespace byte of the body picks the backend ('{' selects the OPDS 2.0
 * JSON parser, anything else the OPDS 1.x Atom parser) and only that backend
 * is instantiated. A UTF-8 BOM is skipped.
 */
class OpdsFeedParser final : public Print {
 public:
  OpdsFeedParser() = default;
  OpdsFeedParser(const OpdsFeedParser&) = delete;
  OpdsFeedParser& operator=(const OpdsFeedParser&) = delete;

  // Preferred UI language (primary subtag, e.g. "en"), forwarded to the OPDS
  // 2.0 backend so localized title objects resolve to the reader's language
  // instead of the feed's first translation. Set before writing.
  void setPreferredLanguage(const char* lang);

  // Discard all state so the instance can parse a fresh response (e.g. the
  // authenticated retry after a 401).
  void reset();

  size_t write(uint8_t c) override;
  size_t write(const uint8_t* data, size_t length) override;
  void flush() override;

  bool error() const;
  bool truncated() const;

  // True when the feed parsed as OPDS 2.0 JSON.
  bool isOpds2() const { return jsonParser != nullptr; }

  std::vector<OpdsEntry> takeEntries();
  // Facet rows, one section heading per facet group.
  std::vector<OpdsEntry> takeFacetEntries();
  const std::string& getFeedTitle() const;
  const std::string& getSearchTemplate() const;
  const std::string& getSearchDescriptionUrl() const;
  const std::string& getNextPageUrl() const;
  const std::string& getPrevPageUrl() const;
  const std::string& getFirstPageUrl() const;
  const std::string& getLastPageUrl() const;
  const std::string& getShelfUrl() const;
  const std::string& getWishlistUrl() const;
  const std::string& getHistoryUrl() const;

 private:
  bool selectBackend(uint8_t firstByte);

  std::unique_ptr<OpdsParser> xmlParser;
  std::unique_ptr<Opds2Parser> jsonParser;
  uint8_t bomBytesSkipped = 0;
  bool allocFailed = false;
  char preferredLang[12] = {0};
};
