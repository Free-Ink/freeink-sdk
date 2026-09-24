#pragma once
#include <cstddef>
#include <cstring>
#include <string>

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 * Shared between the OPDS 1.x (Atom) and OPDS 2.0 (JSON) parsers.
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL or epub download URL
  std::string id;
  // Section heading drawn above this row (group or facet-group title).
  std::string heading;
  // Right-column annotation (a facet's publication count, or a price).
  std::string detail;
  // The chosen acquisition is a purchase (rel buy): the UI labels the action
  // accordingly, and the download is verified to actually be a book.
  bool purchase = false;
  // The acquisition is indirect (type application/opds-publication+json): href
  // points at a publication document that must be fetched to find the real
  // download link, rather than at the EPUB itself. See OPDS 2.0 5.3.
  bool indirect = false;
  // Publication "self" link (application/opds-publication+json): fetched to
  // build the detail page. Empty when the feed offers none.
  std::string selfHref;
};

// Entry id marking a group's "see all" link; the UI supplies the label.
inline constexpr const char* OPDS_SEE_ALL_ID = "opds:group-self";

/**
 * Preference rank of an acquisition link relation, covering both the OPDS 1.x
 * URI forms and the OPDS 2.0 short names. Higher ranks are preferred when a
 * publication offers several acquisition links. `buy` (rank 0) is a last
 * resort: attempted with the user's credentials and verified after download.
 * -1 means unusable: not an acquisition rel, or `sample`/`preview` (not the
 * full book).
 */
inline int opdsAcquisitionRank(const char* rel) {
  if (strstr(rel, "opds-spec.org/acquisition") != nullptr) {
    if (strstr(rel, "/open-access") != nullptr) return 3;
    if (strstr(rel, "/sample") != nullptr) return -1;
    if (strstr(rel, "/buy") != nullptr) return 0;
    if (strstr(rel, "/borrow") != nullptr || strstr(rel, "/subscribe") != nullptr) return 1;
    return 2;  // bare http://opds-spec.org/acquisition
  }
  if (strcmp(rel, "download") == 0 || strcmp(rel, "open-access") == 0) return 3;
  if (strcmp(rel, "acquisition") == 0) return 2;
  if (strcmp(rel, "borrow") == 0 || strcmp(rel, "subscribe") == 0) return 1;
  if (strcmp(rel, "buy") == 0) return 0;
  return -1;  // preview/sample, or not an acquisition rel
}

// Shared memory bounds for both feed parsers.
namespace OpdsLimits {
constexpr size_t ENTRY_STORAGE_CAPACITY = 64;
constexpr size_t MAX_ENTRIES = ENTRY_STORAGE_CAPACITY - 2;
constexpr size_t MAX_TITLE_CHARS = 160;
constexpr size_t MAX_AUTHOR_CHARS = 120;
constexpr size_t MAX_ID_CHARS = 128;
constexpr size_t MAX_HREF_CHARS = 768;
constexpr size_t MAX_SEARCH_TEMPLATE_CHARS = 768;
constexpr size_t MAX_PAGE_URL_CHARS = 768;
constexpr size_t MAX_FACET_ENTRIES = 24;
}  // namespace OpdsLimits
