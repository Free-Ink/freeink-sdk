#pragma once
#include <Print.h>
#include <expat.h>

#include <string>
#include <vector>

#include "OpdsEntry.h"

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsParser parser;
 *   if (parser.parse(xmlData, xmlLength)) {
 *     for (const auto& entry : parser.getEntries()) {
 *       if (entry.type == OpdsEntryType::BOOK) {
 *         // Downloadable book
 *       } else {
 *         // Navigation link to another catalog
 *       }
 *     }
 *   }
 */
class OpdsParser final : public Print {
 public:
  OpdsParser();
  ~OpdsParser();

  // Disable copy
  const std::string& getSearchTemplate() const { return searchTemplate; }
  // rel="search" link without an inline template: an OpenSearch description
  // document the caller must fetch and parse to obtain the template.
  const std::string& getSearchDescriptionUrl() const { return searchDescriptionUrl; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  const std::string& getFirstPageUrl() const { return firstPageUrl; }
  const std::string& getLastPageUrl() const { return lastPageUrl; }
  const std::string& getShelfUrl() const { return shelfUrl; }
  const std::string& getWishlistUrl() const { return wishlistUrl; }
  const std::string& getHistoryUrl() const { return historyUrl; }
  const std::string& getFeedTitle() const { return feedTitle; }
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;

  void flush() override;

  bool error() const;
  bool truncated() const { return feedTruncated; }

  operator bool() { return !error(); }

  /**
   * Get the parsed entries (both navigation and book entries).
   * @return Vector of OpdsEntry entries
   */
  const std::vector<OpdsEntry>& getEntries() const& { return entries; }
  std::vector<OpdsEntry> getEntries() && { return std::move(entries); }
  // Facet links (rel http://opds-spec.org/facet), one section per
  // opds:facetGroup, with thr:count as the row detail.
  std::vector<OpdsEntry> takeFacetEntries() { return std::move(facetEntries); }

  /**
   * Get only book entries (legacy compatibility).
   * @return Vector of book entries
   */
  std::vector<OpdsEntry> getBooks() const;

  /**
   * Clear all parsed entries.
   */
  void clear();

 private:
  // Expat callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  std::string searchTemplate;
  std::string searchDescriptionUrl;
  std::string nextPageUrl;
  std::string prevPageUrl;
  std::string firstPageUrl;
  std::string lastPageUrl;
  std::string shelfUrl;
  std::string wishlistUrl;
  std::string historyUrl;
  std::string feedTitle;
  // Helper to find attribute value
  static const char* findAttribute(const XML_Char** atts, const char* name);
  static void assignBounded(std::string& target, const char* value, size_t maxLen);
  static void appendBounded(std::string& target, const char* value, size_t len, size_t maxLen);

  XML_Parser parser = nullptr;
  std::vector<OpdsEntry> entries;
  std::vector<OpdsEntry> facetEntries;
  // facetGroup of the last collected facet link; a change starts a new
  // section heading.
  std::string lastFacetGroup;
  OpdsEntry currentEntry;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inFeedTitle = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;
  bool collectCurrentEntry = false;
  // Best acquisition rank committed for the current entry, and whether that
  // href points at a plain EPUB (see opdsAcquisitionRank()).
  int entryAcqRank = -1;
  bool entryHasPlainEpub = false;
  // Inside an entry's <link> element (OPDS 1.x price is a child element:
  // <link ...><opds:price currencycode="USD">4.99</opds:price></link>).
  bool inEntryLink = false;
  // The enclosing link is the entry's chosen purchase acquisition; its price
  // becomes the entry detail.
  bool chosenLinkIsPurchase = false;
  bool inPrice = false;
  std::string priceCurrency;

  bool errorOccured = false;
  bool feedTruncated = false;
};
