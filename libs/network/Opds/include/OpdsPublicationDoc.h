#pragma once
#include <cstddef>
#include <string>

/**
 * Resolve an indirect OPDS acquisition. Given a standalone OPDS Publication
 * document (application/opds-publication+json, fetched from an indirect
 * acquisition link), find the best direct download link in its top-level
 * `links`: a real EPUB acquisition preferred, otherwise any acquisition link
 * (which may still be DRM-wrapped — the caller verifies the downloaded bytes).
 *
 * Returns true and sets outHref when an acquisition link is found. outIsEpub
 * reports whether the chosen link is typed application/epub+zip.
 */
bool resolveOpdsIndirectAcquisition(const char* json, size_t len, std::string& outHref, bool& outIsEpub);

/**
 * Library loan/availability info from an acquisition link's `properties`
 * (Readium `availability`/`holds`/`copies`). Fields are -1/empty when absent.
 */
struct OpdsAvailability {
  std::string state;  // "available", "unavailable", "reserved", "ready"
  int32_t copiesTotal = -1;
  int32_t copiesAvailable = -1;
  int32_t holdsTotal = -1;
  int32_t holdsPosition = -1;

  bool present() const {
    return !state.empty() || copiesTotal >= 0 || copiesAvailable >= 0 || holdsTotal >= 0 || holdsPosition >= 0;
  }
};

/**
 * Parsed detail of a standalone OPDS Publication document, for the detail page.
 */
struct OpdsPublication {
  std::string title;
  std::string author;
  std::string description;
  std::string language;
  std::string publisher;
  std::string published;  // metadata.published or .modified
  std::string coverHref;  // first top-level `images` entry (cover art), if any

  // Best acquisition link (same ranking as the feed parser).
  std::string acquisitionHref;
  bool indirect = false;  // href is another publication doc (resolve on download)
  bool purchase = false;  // rel buy
  std::string price;      // "4.99 USD" when a buy link carries one
  OpdsAvailability availability;

  bool valid = false;  // parsed cleanly and has at least a title
};

/**
 * Parse a standalone OPDS Publication document into `out`. Returns false on
 * malformed/truncated JSON or when no title was found. `preferredLang` (a
 * primary subtag such as "en") selects a translation from a localized title
 * object; nullptr keeps the first-translation behavior.
 */
bool parseOpdsPublicationDoc(const char* json, size_t len, OpdsPublication& out, const char* preferredLang = nullptr);
