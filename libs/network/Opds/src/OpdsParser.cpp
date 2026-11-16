#include "OpdsParser.h"

#include <Arduino.h>
#include <OpdsLog.h>
#include <XmlParserUtils.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace OpdsLimits;

namespace {
constexpr size_t STRING_HEADROOM_BYTES = 4096;  // left for the entries' own strings
constexpr size_t MIN_ENTRY_CAPACITY = 8;

size_t affordableEntries(const size_t cap) {
  const size_t block = ESP.getMaxAllocHeap();
  return std::min(cap, block > STRING_HEADROOM_BYTES ? (block - STRING_HEADROOM_BYTES) / sizeof(OpdsEntry) : 0);
}
}  // namespace

size_t OpdsLimits::reserveFeedEntries(std::vector<OpdsEntry>& entries) {
  const size_t capacity = affordableEntries(ENTRY_STORAGE_CAPACITY);
  if (capacity < MIN_ENTRY_CAPACITY) {
    LOG_ERR("OPDS", "OOM: room for %u feed entries", static_cast<unsigned>(capacity));
    return 0;
  }
  entries.reserve(capacity);
  return capacity - (ENTRY_STORAGE_CAPACITY - MAX_ENTRIES);
}

void OpdsLimits::reserveFacetEntries(std::vector<OpdsEntry>& facets) {
  facets.reserve(affordableEntries(MAX_FACET_ENTRIES));
}

OpdsParser::OpdsParser() {
  // Before expat: it allocates nothing large and fails without aborting.
  entryLimit = reserveFeedEntries(entries);
  if (entryLimit == 0) {
    errorOccured = true;
    return;
  }
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
    return;
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (errorOccured || !parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  searchDescriptionUrl.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  firstPageUrl.clear();
  lastPageUrl.clear();
  shelfUrl.clear();
  wishlistUrl.clear();
  historyUrl.clear();
  feedTitle.clear();
  inFeedTitle = false;
  entryAcqRank = -1;
  entryHasPlainEpub = false;
  facetEntries.clear();
  lastFacetGroup.clear();
  inEntryLink = false;
  chosenLinkIsPurchase = false;
  inPrice = false;
  priceCurrency.clear();
  osTarget = nullptr;
  osTotalResults = osItemsPerPage = osStartIndex = -1;
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = false;
  inSummary = summaryIsContent = descInMarkup = false;
  collectCurrentEntry = false;
  feedTruncated = false;
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

// Attribute lookup ignoring a namespace prefix ("opds:facetGroup" matches
// "facetGroup"); expat reports attribute names verbatim without namespace
// processing, and the prefix is whatever the feed declared.
static const char* findAttributeLocal(const XML_Char** atts, const char* localName) {
  for (int i = 0; atts[i]; i += 2) {
    if (xmlLocalNameEquals(atts[i], localName)) return atts[i + 1];
  }
  return nullptr;
}

void OpdsParser::assignBounded(std::string& target, const char* value, const size_t maxLen) {
  if (!value) {
    target.clear();
    return;
  }
  target.assign(value, strnlen(value, maxLen));
}

void OpdsParser::appendBounded(std::string& target, const char* value, const size_t len, const size_t maxLen) {
  if (target.size() >= maxLen) return;
  const size_t remaining = maxLen - target.size();
  target.append(value, len < remaining ? len : remaining);
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->collectCurrentEntry = self->entries.size() < self->entryLimit;
    self->feedTruncated = self->feedTruncated || !self->collectCurrentEntry;
    self->currentEntry = OpdsEntry{};
    self->currentText.clear();
    self->inTitle = self->inAuthor = self->inAuthorName = self->inId = false;
    self->inSummary = self->summaryIsContent = self->descInMarkup = false;
    self->entryAcqRank = -1;
    self->entryHasPlainEpub = false;
    self->inEntryLink = false;
    self->chosenLinkIsPurchase = false;
    self->inPrice = false;
    return;
  }

  if (self->inEntry && self->inSummary) {
    // Nested xhtml elements (<p>, <br>...) inside the description are word
    // boundaries; never let their names trip the entry-field handling below.
    if (!self->currentText.empty() && self->currentText.back() != ' ' &&
        self->currentText.size() < MAX_DESCRIPTION_CHARS) {
      self->currentText += ' ';
    }
    return;
  }

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        if (strstr(href, "{searchTerms}") != nullptr) {
          assignBounded(self->searchTemplate, href, MAX_SEARCH_TEMPLATE_CHARS);
        } else {
          // No inline template: href points at an OpenSearch description
          // document (calibre-web, COPS, Kavita).
          assignBounded(self->searchDescriptionUrl, href, MAX_SEARCH_TEMPLATE_CHARS);
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        assignBounded(self->nextPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && (strcmp(rel, "previous") == 0 || strcmp(rel, "prev") == 0) && !self->inEntry) {
        assignBounded(self->prevPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strcmp(rel, "first") == 0 && !self->inEntry) {
        assignBounded(self->firstPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strcmp(rel, "last") == 0 && !self->inEntry) {
        assignBounded(self->lastPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strstr(rel, "opds-spec.org/shelf") != nullptr && !self->inEntry) {
        assignBounded(self->shelfUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strstr(rel, "opds-spec.org/wishlist") != nullptr && !self->inEntry) {
        assignBounded(self->wishlistUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strstr(rel, "opds-spec.org/history") != nullptr && !self->inEntry) {
        assignBounded(self->historyUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strstr(rel, "opds-spec.org/facet") != nullptr && !self->inEntry) {
        const char* title = findAttribute(atts, "title");
        if (self->facetEntries.capacity() == 0) reserveFacetEntries(self->facetEntries);
        if (self->facetEntries.size() >= self->facetEntries.capacity()) self->feedTruncated = true;
        if (title && title[0] != '\0' && self->facetEntries.size() < self->facetEntries.capacity()) {
          OpdsEntry facet;
          facet.type = OpdsEntryType::NAVIGATION;
          assignBounded(facet.title, title, MAX_TITLE_CHARS);
          assignBounded(facet.href, href, MAX_HREF_CHARS);
          const char* count = findAttributeLocal(atts, "count");
          if (count) assignBounded(facet.detail, count, 12);
          const char* group = findAttributeLocal(atts, "facetGroup");
          if (group && self->lastFacetGroup != group) {
            assignBounded(facet.heading, group, MAX_TITLE_CHARS);
            self->lastFacetGroup = group;
          }
          self->facetEntries.push_back(std::move(facet));
        }
      }

      if (self->inEntry && self->collectCurrentEntry) {
        self->inEntryLink = true;
        self->chosenLinkIsPurchase = false;
        const int rank = rel ? opdsAcquisitionRank(rel) : -1;
        const bool isEpubType = type && strcmp(type, "application/epub+zip") == 0;
        const bool isLcpType = type && strcmp(type, "application/vnd.readium.lcp.license.v1.0+json") == 0;
        if (rank >= 0 && (isEpubType || isLcpType)) {
          // Prefer higher-ranked acquisitions (open-access over borrow,
          // never buy/sample); at equal rank prefer a direct EPUB over an
          // LCP license, and a plain EPUB path over derived formats.
          const bool isPlainEpub = isEpubType && (strstr(href, ".epub") != nullptr || strstr(href, "/epub/") != nullptr);
          const bool better =
              self->currentEntry.type != OpdsEntryType::BOOK || rank > self->entryAcqRank ||
              (rank == self->entryAcqRank &&
               ((isEpubType && self->currentEntry.lcpLicense) || (isPlainEpub && !self->entryHasPlainEpub)));
          if (better) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
            self->entryAcqRank = rank;
            self->entryHasPlainEpub = isPlainEpub;
            self->currentEntry.lcpLicense = isLcpType;
            self->currentEntry.purchase = rank == 0;
            self->currentEntry.detail.clear();
            self->chosenLinkIsPurchase = self->currentEntry.purchase;
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
          }
        } else if (rel && strstr(rel, "opds-spec.org/image") != nullptr) {
          // Cover art: prefer the full image; a thumbnail only fills a gap.
          if (strstr(rel, "/thumbnail") == nullptr || self->currentEntry.coverHref.empty()) {
            assignBounded(self->currentEntry.coverHref, href, MAX_HREF_CHARS);
          }
        }
      }
    }
  }

  if (self->inEntry && self->inEntryLink && self->collectCurrentEntry &&
      (strcmp(name, "price") == 0 || strstr(name, ":price") != nullptr)) {
    self->inPrice = true;
    self->currentText.clear();
    const char* currency = findAttribute(atts, "currencycode");
    if (!currency) currency = findAttribute(atts, "currency");
    assignBounded(self->priceCurrency, currency, 8);
    return;
  }

  if (!self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      self->inFeedTitle = true;
      self->currentText.clear();
    } else if (strstr(name, ":totalResults") != nullptr || strcmp(name, "totalResults") == 0) {
      self->osTarget = &self->osTotalResults;
      self->currentText.clear();
    } else if (strstr(name, ":itemsPerPage") != nullptr || strcmp(name, "itemsPerPage") == 0) {
      self->osTarget = &self->osItemsPerPage;
      self->currentText.clear();
    } else if (strstr(name, ":startIndex") != nullptr || strcmp(name, "startIndex") == 0) {
      self->osTarget = &self->osStartIndex;
      self->currentText.clear();
    }
    return;
  }
  if (!self->collectCurrentEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  } else if (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr || strcmp(name, "content") == 0 ||
             strstr(name, ":content") != nullptr) {
    self->inSummary = true;
    self->summaryIsContent = strcmp(name, "content") == 0 || strstr(name, ":content") != nullptr;
    self->descInMarkup = false;
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (self->collectCurrentEntry && !self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      self->entries.push_back(self->currentEntry);
    }
    self->inEntry = false;
    self->collectCurrentEntry = false;
  } else if (!self->inEntry && self->inFeedTitle && (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr)) {
    self->feedTitle = self->currentText;
    self->inFeedTitle = false;
  } else if (!self->inEntry && self->osTarget) {
    *self->osTarget = static_cast<int32_t>(strtol(self->currentText.c_str(), nullptr, 10));
    self->osTarget = nullptr;
  } else if (self->inEntry) {
    if (self->inSummary) {
      if (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr || strcmp(name, "content") == 0 ||
          strstr(name, ":content") != nullptr) {
        // <content> (the full text) overwrites; <summary> only fills a gap,
        // so whichever the feed provides wins without depending on order.
        if (self->collectCurrentEntry && (self->summaryIsContent || self->currentEntry.description.empty())) {
          while (!self->currentText.empty() && self->currentText.back() == ' ') self->currentText.pop_back();
          self->currentEntry.description = self->currentText;
        }
        self->inSummary = false;
      }
      return;
    }
    if (self->inPrice && (strcmp(name, "price") == 0 || strstr(name, ":price") != nullptr)) {
      if (self->chosenLinkIsPurchase && !self->currentText.empty()) {
        self->currentEntry.detail = self->currentText;
        if (!self->priceCurrency.empty()) {
          self->currentEntry.detail += ' ';
          self->currentEntry.detail += self->priceCurrency;
        }
      }
      self->inPrice = false;
      return;
    }
    if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
      self->inEntryLink = false;
      self->chosenLinkIsPurchase = false;
      return;
    }
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (!self->inEntry) {
    if (self->inFeedTitle) {
      appendBounded(self->currentText, s, len, MAX_TITLE_CHARS);
    } else if (self->osTarget) {
      appendBounded(self->currentText, s, len, 16);
    }
    return;
  }
  if (!self->collectCurrentEntry) return;
  if (self->inSummary) {
    for (int i = 0; i < len && self->currentText.size() < MAX_DESCRIPTION_CHARS; ++i) {
      const char c = s[i];
      if (c == '<') {
        // Literal tag (HTML-escaped content): strip it, keep a word boundary.
        if (!self->currentText.empty() && self->currentText.back() != ' ') self->currentText += ' ';
        self->descInMarkup = true;
        continue;
      }
      if (self->descInMarkup) {
        if (c == '>') self->descInMarkup = false;
        continue;
      }
      const char out = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
      if (out == ' ' && (self->currentText.empty() || self->currentText.back() == ' ')) continue;
      self->currentText += out;
    }
    return;
  }
  if (self->inPrice) {
    appendBounded(self->currentText, s, len, 16);
    return;
  }
  if (self->inTitle) {
    appendBounded(self->currentText, s, len, MAX_TITLE_CHARS);
  } else if (self->inAuthorName) {
    appendBounded(self->currentText, s, len, MAX_AUTHOR_CHARS);
  } else if (self->inId) {
    appendBounded(self->currentText, s, len, MAX_ID_CHARS);
  }
}
