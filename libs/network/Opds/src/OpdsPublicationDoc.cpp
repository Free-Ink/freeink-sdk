#include "OpdsPublicationDoc.h"

#include <StreamingJsonParser.h>

#include <cstring>

#include "OpdsEntry.h"

namespace {

// Scans a standalone OPDS Publication document's top-level `links` array for
// the best acquisition link. Link objects live at depth 3 (root > "links" >
// object); a nested "properties"/"indirectAcquisition" object is skipped by
// only committing fields seen at the link-object depth.
struct PubDocCtx {
  std::string* outHref = nullptr;
  bool* outIsEpub = nullptr;

  uint8_t depth = 0;
  bool inLinksArray = false;
  uint8_t linkObjectDepth = 0;
  char pendingKey[16] = {0};

  struct {
    std::string href;
    int rank = -1;
    bool isEpub = false;
  } link;

  int bestRank = -1;
  bool bestIsEpub = false;
};

void onKey(void* ud, const char* key, size_t len) {
  auto& ctx = *static_cast<PubDocCtx*>(ud);
  const size_t max = sizeof(ctx.pendingKey) - 1;
  if (len > max) len = max;
  memcpy(ctx.pendingKey, key, len);
  ctx.pendingKey[len] = '\0';
}

void applyRel(PubDocCtx& ctx, const char* rel) {
  const int rank = opdsAcquisitionRank(rel);
  if (rank > ctx.link.rank) ctx.link.rank = rank;
}

void onString(void* ud, const char* value, size_t len) {
  auto& ctx = *static_cast<PubDocCtx*>(ud);
  if (ctx.linkObjectDepth == 0) return;
  if (ctx.depth == ctx.linkObjectDepth) {
    if (strcmp(ctx.pendingKey, "href") == 0) {
      ctx.link.href.assign(value, len < OpdsLimits::MAX_HREF_CHARS ? len : OpdsLimits::MAX_HREF_CHARS);
    } else if (strcmp(ctx.pendingKey, "rel") == 0) {
      applyRel(ctx, value);
    } else if (strcmp(ctx.pendingKey, "type") == 0) {
      if (strcmp(value, "application/epub+zip") == 0) ctx.link.isEpub = true;
    }
  } else if (ctx.depth == ctx.linkObjectDepth + 1 && strcmp(ctx.pendingKey, "rel") == 0) {
    applyRel(ctx, value);  // rel expressed as an array
  }
}

void onObjectStart(void* ud) {
  auto& ctx = *static_cast<PubDocCtx*>(ud);
  ++ctx.depth;
  // root object (1) > "links" array (2) > link object (3).
  if (ctx.inLinksArray && ctx.linkObjectDepth == 0 && ctx.depth == 3) {
    ctx.linkObjectDepth = ctx.depth;
    ctx.link = {};
  }
  ctx.pendingKey[0] = '\0';
}

void onObjectEnd(void* ud) {
  auto& ctx = *static_cast<PubDocCtx*>(ud);
  if (ctx.linkObjectDepth != 0 && ctx.depth == ctx.linkObjectDepth) {
    // Prefer a real EPUB, then higher acquisition rank; commit the winner.
    if (!ctx.link.href.empty() && ctx.link.rank >= 0) {
      const bool better = ctx.outHref->empty() || (ctx.link.isEpub && !ctx.bestIsEpub) ||
                          (ctx.link.isEpub == ctx.bestIsEpub && ctx.link.rank > ctx.bestRank);
      if (better) {
        *ctx.outHref = ctx.link.href;
        ctx.bestRank = ctx.link.rank;
        ctx.bestIsEpub = ctx.link.isEpub;
      }
    }
    ctx.linkObjectDepth = 0;
  }
  if (ctx.depth > 0) --ctx.depth;
  ctx.pendingKey[0] = '\0';
}

void onArrayStart(void* ud) {
  auto& ctx = *static_cast<PubDocCtx*>(ud);
  ++ctx.depth;
  if (ctx.depth == 2 && strcmp(ctx.pendingKey, "links") == 0) ctx.inLinksArray = true;
  // Keep pendingKey: a "rel" array's element strings need to see their key.
}

void onArrayEnd(void* ud) {
  auto& ctx = *static_cast<PubDocCtx*>(ud);
  if (ctx.inLinksArray && ctx.depth == 2) ctx.inLinksArray = false;
  if (ctx.depth > 0) --ctx.depth;
  ctx.pendingKey[0] = '\0';
}

}  // namespace

bool resolveOpdsIndirectAcquisition(const char* json, const size_t len, std::string& outHref, bool& outIsEpub) {
  outHref.clear();
  outIsEpub = false;
  PubDocCtx ctx;
  ctx.outHref = &outHref;
  ctx.outIsEpub = &outIsEpub;
  JsonCallbacks callbacks{&ctx,    &onKey,         &onString,    nullptr,       nullptr,
                          nullptr, &onObjectStart, &onObjectEnd, &onArrayStart, &onArrayEnd};
  StreamingJsonParser parser(callbacks);
  parser.feed(json, len);
  // Reject a truncated document: every container the parser opened must have
  // closed (depth back to 0, no link object or links array still open), or a
  // partial response could yield a half-read acquisition link.
  if (parser.hasError() || ctx.depth != 0 || ctx.linkObjectDepth != 0 || ctx.inLinksArray) return false;
  outIsEpub = ctx.bestIsEpub;
  return !outHref.empty();
}

// ---- full publication-document parser (metadata + availability) ----

namespace {

enum class PubScope : uint8_t {
  ROOT,
  META,        // "metadata" object
  META_TITLE,  // localized title object
  AUTHOR,      // author object ({name})
  AUTHOR_ARR,  // author array
  LINKS,       // "links" array
  LINK,        // one link object
  LINK_REL,    // link "rel" array
  IMAGES,      // top-level "images" array (cover art)
  IMAGE,       // one image object
  PROPS,       // link "properties" object
  AVAIL,       // properties.availability
  HOLDS,       // properties.holds
  COPIES,      // properties.copies
  PRICE,       // properties.price
  SKIP,
};

struct PubDoc2Ctx {
  OpdsPublication* out = nullptr;

  static constexpr uint8_t MAX = StreamingJsonParser::MAX_NESTING + 1;
  PubScope stack[MAX];
  uint8_t depth = 0;
  bool sawRoot = false;
  char key[24] = {0};

  // Accumulator for the current link; applied to `out` on link end if it is
  // the best-ranked acquisition seen so far.
  struct {
    std::string href;
    int rank = -1;
    bool isEpub = false;
    bool isPubDoc = false;
    std::string priceValue;
    std::string priceCurrency;
    OpdsAvailability avail;
  } link;
  int bestRank = -1;

  // Preferred title language (primary subtag, lowercase) and per-title lock, so
  // a localized title object resolves to the UI language, not the first entry.
  char preferredLang[12] = {0};
  bool titleLocked = false;

  PubScope cur() const { return depth > 0 ? stack[depth - 1] : PubScope::ROOT; }
  // True when `key` (a language tag in a localized title object) matches
  // preferredLang, exactly or as a "<lang>-REGION" variant.
  bool keyPreferred() const {
    if (!preferredLang[0]) return false;
    size_t i = 0;
    for (; preferredLang[i]; ++i) {
      const char k = key[i];
      const char kl = static_cast<char>((k >= 'A' && k <= 'Z') ? k + 32 : k);
      if (kl != preferredLang[i]) return false;
    }
    return key[i] == '\0' || key[i] == '-';
  }
};

int toInt(const char* v) { return static_cast<int>(strtol(v, nullptr, 10)); }

PubScope pubChildScope(const PubDoc2Ctx& c, PubScope parent, bool obj) {
  switch (parent) {
    case PubScope::ROOT:
      if (obj && strcmp(c.key, "metadata") == 0) return PubScope::META;
      if (!obj && strcmp(c.key, "links") == 0) return PubScope::LINKS;
      if (!obj && strcmp(c.key, "images") == 0) return PubScope::IMAGES;
      return PubScope::SKIP;
    case PubScope::IMAGES:
      return obj ? PubScope::IMAGE : PubScope::SKIP;
    case PubScope::IMAGE:
      return PubScope::SKIP;  // href/type read as strings under IMAGE
    case PubScope::META:
      if (strcmp(c.key, "author") == 0) return obj ? PubScope::AUTHOR : PubScope::AUTHOR_ARR;
      if (obj && strcmp(c.key, "title") == 0) return PubScope::META_TITLE;
      return PubScope::SKIP;  // belongsTo, subject, etc.
    case PubScope::AUTHOR_ARR:
      return obj ? PubScope::AUTHOR : PubScope::SKIP;
    case PubScope::LINKS:
      return obj ? PubScope::LINK : PubScope::SKIP;
    case PubScope::LINK:
      if (!obj && strcmp(c.key, "rel") == 0) return PubScope::LINK_REL;
      if (obj && strcmp(c.key, "properties") == 0) return PubScope::PROPS;
      return PubScope::SKIP;
    case PubScope::PROPS:
      if (obj && strcmp(c.key, "availability") == 0) return PubScope::AVAIL;
      if (obj && strcmp(c.key, "holds") == 0) return PubScope::HOLDS;
      if (obj && strcmp(c.key, "copies") == 0) return PubScope::COPIES;
      if (obj && strcmp(c.key, "price") == 0) return PubScope::PRICE;
      return PubScope::SKIP;  // indirectAcquisition
    default:
      return PubScope::SKIP;
  }
}

void pubOnKey(void* ud, const char* k, size_t len) {
  auto& c = *static_cast<PubDoc2Ctx*>(ud);
  const size_t m = sizeof(c.key) - 1;
  if (len > m) len = m;
  memcpy(c.key, k, len);
  c.key[len] = '\0';
}

void pubOnString(void* ud, const char* v, size_t len) {
  auto& c = *static_cast<PubDoc2Ctx*>(ud);
  auto set = [&](std::string& t, size_t max) { t.assign(v, len < max ? len : max); };
  switch (c.cur()) {
    case PubScope::META:
      if (strcmp(c.key, "title") == 0)
        set(c.out->title, OpdsLimits::MAX_TITLE_CHARS);
      else if (strcmp(c.key, "author") == 0 && c.out->author.empty())
        set(c.out->author, OpdsLimits::MAX_AUTHOR_CHARS);
      else if (strcmp(c.key, "description") == 0)
        set(c.out->description, 2048);
      else if (strcmp(c.key, "language") == 0)
        set(c.out->language, 32);
      else if (strcmp(c.key, "publisher") == 0)
        set(c.out->publisher, 120);
      else if ((strcmp(c.key, "published") == 0 || strcmp(c.key, "modified") == 0) && c.out->published.empty())
        set(c.out->published, 40);
      break;
    case PubScope::META_TITLE:
      // Localized title object: prefer the UI language, else keep the first.
      if (c.keyPreferred()) {
        set(c.out->title, OpdsLimits::MAX_TITLE_CHARS);
        c.titleLocked = true;
      } else if (!c.titleLocked && c.out->title.empty()) {
        set(c.out->title, OpdsLimits::MAX_TITLE_CHARS);
      }
      break;
    case PubScope::AUTHOR:
      if (strcmp(c.key, "name") == 0 && c.out->author.empty()) set(c.out->author, OpdsLimits::MAX_AUTHOR_CHARS);
      break;
    case PubScope::AUTHOR_ARR:
      if (c.out->author.empty()) set(c.out->author, OpdsLimits::MAX_AUTHOR_CHARS);
      break;
    case PubScope::IMAGE:
      // First image entry is the cover; keep its href.
      if (strcmp(c.key, "href") == 0 && c.out->coverHref.empty()) set(c.out->coverHref, OpdsLimits::MAX_HREF_CHARS);
      break;
    case PubScope::LINK:
      if (strcmp(c.key, "href") == 0)
        set(c.link.href, OpdsLimits::MAX_HREF_CHARS);
      else if (strcmp(c.key, "rel") == 0) {
        const int r = opdsAcquisitionRank(v);
        if (r > c.link.rank) c.link.rank = r;
      } else if (strcmp(c.key, "type") == 0) {
        if (strcmp(v, "application/epub+zip") == 0) c.link.isEpub = true;
        if (strcmp(v, "application/opds-publication+json") == 0) c.link.isPubDoc = true;
      }
      break;
    case PubScope::LINK_REL: {
      const int r = opdsAcquisitionRank(v);
      if (r > c.link.rank) c.link.rank = r;
      break;
    }
    case PubScope::AVAIL:
      if (strcmp(c.key, "state") == 0) set(c.link.avail.state, 24);
      break;
    case PubScope::PRICE:
      if (strcmp(c.key, "currency") == 0) set(c.link.priceCurrency, 8);
      break;
    default:
      break;
  }
}

void pubOnNumber(void* ud, const char* v, size_t) {
  auto& c = *static_cast<PubDoc2Ctx*>(ud);
  switch (c.cur()) {
    case PubScope::HOLDS:
      if (strcmp(c.key, "total") == 0)
        c.link.avail.holdsTotal = toInt(v);
      else if (strcmp(c.key, "position") == 0)
        c.link.avail.holdsPosition = toInt(v);
      break;
    case PubScope::COPIES:
      if (strcmp(c.key, "total") == 0)
        c.link.avail.copiesTotal = toInt(v);
      else if (strcmp(c.key, "available") == 0)
        c.link.avail.copiesAvailable = toInt(v);
      break;
    case PubScope::PRICE:
      if (strcmp(c.key, "value") == 0) c.link.priceValue.assign(v, strnlen(v, 16));
      break;
    default:
      break;
  }
}

void pubOnObjStart(void* ud) {
  auto& c = *static_cast<PubDoc2Ctx*>(ud);
  if (c.depth >= PubDoc2Ctx::MAX) return;
  PubScope next;
  if (!c.sawRoot) {
    c.sawRoot = true;
    next = PubScope::ROOT;
  } else {
    next = pubChildScope(c, c.cur(), true);
  }
  if (next == PubScope::LINK) c.link = {};
  if (next == PubScope::META_TITLE) c.titleLocked = false;
  c.stack[c.depth++] = next;
  c.key[0] = '\0';
}

void pubApplyLink(PubDoc2Ctx& c) {
  if (c.link.href.empty() || c.link.rank < 0) return;
  // A non-downloadable buy link (HTML checkout, format nested in
  // properties.indirectAcquisition): keep the purchase + price so the detail
  // page shows them, even without a resolvable acquisition href.
  if (!(c.link.isEpub || c.link.isPubDoc)) {
    if (c.link.rank == 0 && !c.out->purchase) {
      c.out->purchase = true;
      if (c.out->price.empty() && !c.link.priceValue.empty()) {
        c.out->price = c.link.priceValue;
        if (!c.link.priceCurrency.empty()) {
          c.out->price += ' ';
          c.out->price += c.link.priceCurrency;
        }
      }
    }
    return;
  }
  if (!c.out->acquisitionHref.empty() && c.link.rank <= c.bestRank) return;
  c.bestRank = c.link.rank;
  c.out->acquisitionHref = c.link.href;
  c.out->indirect = !c.link.isEpub;
  c.out->purchase = c.link.rank == 0;
  c.out->availability = c.link.avail;
  c.out->price.clear();
  if (c.out->purchase && !c.link.priceValue.empty()) {
    c.out->price = c.link.priceValue;
    if (!c.link.priceCurrency.empty()) {
      c.out->price += ' ';
      c.out->price += c.link.priceCurrency;
    }
  }
}

void pubOnObjEnd(void* ud) {
  auto& c = *static_cast<PubDoc2Ctx*>(ud);
  if (c.depth == 0) return;
  if (c.stack[c.depth - 1] == PubScope::LINK) pubApplyLink(c);
  --c.depth;
  c.key[0] = '\0';
}

void pubOnArrStart(void* ud) {
  auto& c = *static_cast<PubDoc2Ctx*>(ud);
  if (c.depth >= PubDoc2Ctx::MAX) return;
  const PubScope next = c.sawRoot ? pubChildScope(c, c.cur(), false) : PubScope::SKIP;
  c.stack[c.depth++] = next;
  // Keep key: a "rel" array's elements need it.
}

void pubOnArrEnd(void* ud) {
  auto& c = *static_cast<PubDoc2Ctx*>(ud);
  if (c.depth > 0) --c.depth;
  c.key[0] = '\0';
}

}  // namespace

bool parseOpdsPublicationDoc(const char* json, const size_t len, OpdsPublication& out, const char* preferredLang) {
  out = OpdsPublication{};
  PubDoc2Ctx ctx;
  ctx.out = &out;
  for (size_t i = 0; preferredLang && preferredLang[i] && i < sizeof(ctx.preferredLang) - 1; ++i) {
    ctx.preferredLang[i] = static_cast<char>((preferredLang[i] >= 'A' && preferredLang[i] <= 'Z') ? preferredLang[i] + 32
                                                                                                  : preferredLang[i]);
  }
  JsonCallbacks cb{&ctx,    &pubOnKey,      &pubOnString, &pubOnNumber,   nullptr,
                   nullptr, &pubOnObjStart, &pubOnObjEnd, &pubOnArrStart, &pubOnArrEnd};
  StreamingJsonParser parser(cb);
  parser.feed(json, len);
  if (parser.hasError() || ctx.depth != 0) return false;
  out.valid = !out.title.empty();
  return out.valid;
}
