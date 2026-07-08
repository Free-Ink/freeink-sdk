// FreeInkBook — container.xml, OPF package, and TOC (nav/NCX) parsers.

#include "epub/PackageParsers.h"

#include <string.h>

#include "epub/XmlSax.h"

namespace freeink {
namespace book {

namespace {

constexpr size_t kMaxPath = 512;
constexpr size_t kMaxTextCapture = 255;
constexpr int kMaxTocDepth = 255;

// EPUB documents bind namespace prefixes inconsistently ("dc:title",
// "opf:item", or no prefix at all), so elements and attributes are matched on
// the local part of the qualified name.
const char* localName(const char* qname) {
  const char* colon = strrchr(qname, ':');
  return colon != nullptr ? colon + 1 : qname;
}

bool localIs(const char* qname, const char* local) {
  return strcmp(localName(qname), local) == 0;
}

const char* attrLocal(const char** atts, const char* local) {
  for (int i = 0; atts != nullptr && atts[i] != nullptr; i += 2) {
    if (localIs(atts[i], local)) return atts[i + 1];
  }
  return nullptr;
}

const char* attrExact(const char** atts, const char* qname) {
  for (int i = 0; atts != nullptr && atts[i] != nullptr; i += 2) {
    if (strcmp(atts[i], qname) == 0) return atts[i + 1];
  }
  return nullptr;
}

// True when `value` contains `token` as a whitespace-separated word (for
// attributes like properties="nav scripted").
bool hasToken(const char* value, const char* token) {
  if (value == nullptr) return false;
  const size_t tokenLen = strlen(token);
  const char* p = value;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    const char* start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
    if (static_cast<size_t>(p - start) == tokenLen && strncmp(start, token, tokenLen) == 0) {
      return true;
    }
  }
  return false;
}

// Fixed-capacity accumulator for element text (titles, labels). Content
// beyond the cap is truncated, never overflowed.
struct TextCapture {
  char buf[kMaxTextCapture + 1];
  size_t len = 0;
  bool active = false;

  void begin() {
    active = true;
    len = 0;
    buf[0] = '\0';
  }
  void end() { active = false; }
  void add(const char* text, int textLen) {
    if (!active || textLen <= 0) return;
    size_t n = static_cast<size_t>(textLen);
    if (len + n > kMaxTextCapture) n = kMaxTextCapture - len;
    memcpy(buf + len, text, n);
    len += n;
    buf[len] = '\0';
  }
  // Trims leading/trailing ASCII whitespace in place and returns the string.
  const char* trimmed() {
    size_t start = 0;
    while (start < len && (buf[start] == ' ' || buf[start] == '\t' || buf[start] == '\n' ||
                           buf[start] == '\r')) {
      ++start;
    }
    size_t stop = len;
    while (stop > start && (buf[stop - 1] == ' ' || buf[stop - 1] == '\t' ||
                            buf[stop - 1] == '\n' || buf[stop - 1] == '\r')) {
      --stop;
    }
    buf[stop] = '\0';
    return buf + start;
  }
};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool dirName(const char* path, char* out, size_t outCap) {
  const char* slash = strrchr(path, '/');
  const size_t len = slash != nullptr ? static_cast<size_t>(slash - path) : 0;
  if (len >= outCap) return false;
  memcpy(out, path, len);
  out[len] = '\0';
  return true;
}

const char* resolveHref(Arena& arena, const char* baseDir, const char* href,
                        const char** fragmentOut) {
  if (fragmentOut != nullptr) *fragmentOut = nullptr;
  if (href == nullptr) return nullptr;

  // Percent-decode and split off the fragment.
  char decoded[kMaxPath];
  size_t decodedLen = 0;
  const char* fragment = nullptr;
  for (const char* p = href; *p != '\0'; ++p) {
    if (*p == '#') {
      fragment = p + 1;
      break;
    }
    char c = *p;
    if (c == '%' && hexDigit(p[1]) >= 0 && hexDigit(p[2]) >= 0) {
      c = static_cast<char>(hexDigit(p[1]) * 16 + hexDigit(p[2]));
      p += 2;
    }
    if (decodedLen + 1 >= sizeof(decoded)) return nullptr;
    decoded[decodedLen++] = c;
  }
  decoded[decodedLen] = '\0';

  // Join with the base directory, folding "." and "..".
  char joined[kMaxPath];
  size_t joinedLen = 0;
  const char* start = decoded;
  if (decoded[0] == '/') {
    start = decoded + 1;  // container paths have no leading slash
  } else if (baseDir != nullptr && baseDir[0] != '\0') {
    joinedLen = strlen(baseDir);
    if (joinedLen >= sizeof(joined)) return nullptr;
    memcpy(joined, baseDir, joinedLen);
  }
  const char* p = start;
  while (*p != '\0') {
    const char* end = strchr(p, '/');
    const size_t segLen = end != nullptr ? static_cast<size_t>(end - p) : strlen(p);
    if (segLen == 1 && p[0] == '.') {
      // current directory — skip
    } else if (segLen == 2 && p[0] == '.' && p[1] == '.') {
      while (joinedLen > 0 && joined[joinedLen - 1] != '/') --joinedLen;
      if (joinedLen > 0) --joinedLen;  // drop the slash too
    } else if (segLen > 0) {
      if (joinedLen > 0) {
        if (joinedLen + 1 >= sizeof(joined)) return nullptr;
        joined[joinedLen++] = '/';
      }
      if (joinedLen + segLen >= sizeof(joined)) return nullptr;
      memcpy(joined + joinedLen, p, segLen);
      joinedLen += segLen;
    }
    if (end == nullptr) break;
    p = end + 1;
  }

  if (fragment != nullptr && fragmentOut != nullptr) {
    *fragmentOut = arena.strdup(fragment);
    if (*fragmentOut == nullptr) return nullptr;
  }
  return arena.strdup(joined, joinedLen);
}

// ---------------------------------------------------------------------------
// container.xml

namespace {

class ContainerHandler : public XmlHandler {
 public:
  explicit ContainerHandler(Arena& arena) : arena_(arena) {}

  void onStartElement(const char* name, const char** atts) override {
    if (opfPath != nullptr || !localIs(name, "rootfile")) return;
    const char* mediaType = attrLocal(atts, "media-type");
    if (mediaType != nullptr && strcmp(mediaType, "application/oebps-package+xml") != 0) return;
    const char* fullPath = attrLocal(atts, "full-path");
    if (fullPath != nullptr) opfPath = arena_.strdup(fullPath);
  }

  const char* opfPath = nullptr;

 private:
  Arena& arena_;
};

}  // namespace

BookStatus parseContainer(BookSource& source, const ZipEntry& entry, Arena& bookArena,
                          Arena& scratch, const char** opfPathOut) {
  ContainerHandler handler(bookArena);
  const BookStatus status = XmlSax::parseEntry(source, entry, scratch, handler);
  if (status != BookStatus::Ok) return status;
  if (handler.opfPath == nullptr) return BookStatus::NotEpub;
  *opfPathOut = handler.opfPath;
  return BookStatus::Ok;
}

// ---------------------------------------------------------------------------
// OPF package

namespace {

class PackageHandler : public XmlHandler {
 public:
  enum class Mode { Count, Fill };

  PackageHandler(Mode mode, Arena& bookArena, const char* opfDir)
      : mode_(mode), arena_(bookArena), opfDir_(opfDir) {}

  void onStartElement(const char* name, const char** atts) override {
    if (localIs(name, "metadata")) {
      ++inMetadata_;
    } else if (localIs(name, "manifest")) {
      ++inManifest_;
    } else if (localIs(name, "spine")) {
      ++inSpine_;
      if (mode_ == Mode::Count && tocId == nullptr) {
        const char* toc = attrLocal(atts, "toc");
        if (toc != nullptr) tocId = arena_.strdup(toc);
      }
    } else if (inMetadata_ > 0 && localIs(name, "meta")) {
      // EPUB 2 cover declaration: <meta name="cover" content="manifest-id"/>.
      const char* metaName = attrLocal(atts, "name");
      const char* content = attrLocal(atts, "content");
      if (metaName != nullptr && content != nullptr && strcmp(metaName, "cover") == 0) {
        coverIdHash = ZipCatalog::hashPath(content);
      }
    } else if (inMetadata_ > 0 && mode_ == Mode::Count) {
      captureKind_ = MetaField::None;
      if (localIs(name, "title")) captureKind_ = MetaField::Title;
      else if (localIs(name, "creator")) captureKind_ = MetaField::Author;
      else if (localIs(name, "language")) captureKind_ = MetaField::Language;
      else if (localIs(name, "identifier")) captureKind_ = MetaField::Identifier;
      if (captureKind_ != MetaField::None) capture_.begin();
    } else if (inManifest_ > 0 && localIs(name, "item")) {
      if (mode_ == Mode::Count) {
        ++manifestCount;
      } else if (manifestUsed < manifestCap) {
        ManifestItem& item = manifest[manifestUsed];
        const char* id = attrLocal(atts, "id");
        const char* href = attrLocal(atts, "href");
        const char* mediaType = attrLocal(atts, "media-type");
        if (id != nullptr && href != nullptr) {
          item.id = arena_.strdup(id);
          item.idHash = ZipCatalog::hashPath(id);
          item.href = resolveHref(arena_, opfDir_, href, nullptr);
          item.mediaType = mediaType != nullptr ? arena_.strdup(mediaType) : "";
          const char* properties = attrLocal(atts, "properties");
          item.isNav = hasToken(properties, "nav");
          item.isCoverImage = hasToken(properties, "cover-image");
          if (item.id == nullptr || item.href == nullptr || item.mediaType == nullptr) {
            outOfMemory = true;
          } else {
            ++manifestUsed;
          }
        }
      }
    } else if (inSpine_ > 0 && localIs(name, "itemref")) {
      if (mode_ == Mode::Count) {
        ++spineCount;
      } else if (spineUsed < spineCap) {
        const char* idref = attrLocal(atts, "idref");
        if (idref != nullptr) spineIdHashes[spineUsed++] = ZipCatalog::hashPath(idref);
      }
    }
  }

  void onEndElement(const char* name) override {
    if (localIs(name, "metadata")) {
      if (inMetadata_ > 0) --inMetadata_;
    } else if (localIs(name, "manifest")) {
      if (inManifest_ > 0) --inManifest_;
    } else if (localIs(name, "spine")) {
      if (inSpine_ > 0) --inSpine_;
    } else if (capture_.active && mode_ == Mode::Count) {
      const char* text = capture_.trimmed();
      if (text[0] != '\0') {
        const char** slot = nullptr;
        switch (captureKind_) {
          case MetaField::Title: slot = &metadata.title; break;
          case MetaField::Author: slot = &metadata.author; break;
          case MetaField::Language: slot = &metadata.language; break;
          case MetaField::Identifier: slot = &metadata.identifier; break;
          case MetaField::None: break;
        }
        // Keep the first non-empty occurrence (books often repeat dc: fields).
        if (slot != nullptr && (*slot)[0] == '\0') {
          const char* copy = arena_.strdup(text);
          if (copy == nullptr) outOfMemory = true;
          else *slot = copy;
        }
      }
      capture_.end();
      captureKind_ = MetaField::None;
    }
  }

  void onText(const char* text, int len) override { capture_.add(text, len); }

  // Count-pass results.
  size_t manifestCount = 0;
  size_t spineCount = 0;
  BookMetadata metadata;
  const char* tocId = nullptr;

  // Fill-pass targets.
  ManifestItem* manifest = nullptr;
  size_t manifestCap = 0;
  size_t manifestUsed = 0;
  uint32_t* spineIdHashes = nullptr;
  size_t spineCap = 0;
  size_t spineUsed = 0;

  bool outOfMemory = false;
  uint32_t coverIdHash = 0;  // from <meta name="cover">, 0 = none

 private:
  enum class MetaField { None, Title, Author, Language, Identifier };

  Mode mode_;
  Arena& arena_;
  const char* opfDir_;
  int inMetadata_ = 0;
  int inManifest_ = 0;
  int inSpine_ = 0;
  TextCapture capture_;
  MetaField captureKind_ = MetaField::None;
};

}  // namespace

BookStatus parsePackage(BookSource& source, const ZipEntry& entry, const char* opfDir,
                        Arena& bookArena, Arena& scratch, PackageResult* out) {
  PackageHandler counter(PackageHandler::Mode::Count, bookArena, opfDir);
  BookStatus status = XmlSax::parseEntry(source, entry, scratch, counter);
  if (status != BookStatus::Ok) return status;
  if (counter.outOfMemory) return BookStatus::OutOfMemory;
  if (counter.manifestCount == 0 || counter.spineCount == 0) return BookStatus::NotEpub;

  const size_t scratchMark = scratch.mark();
  ManifestItem* manifest = bookArena.allocArray<ManifestItem>(counter.manifestCount);
  uint32_t* spineIdHashes = scratch.allocArray<uint32_t>(counter.spineCount);
  if (manifest == nullptr || spineIdHashes == nullptr) {
    scratch.release(scratchMark);
    return BookStatus::OutOfMemory;
  }

  PackageHandler filler(PackageHandler::Mode::Fill, bookArena, opfDir);
  filler.manifest = manifest;
  filler.manifestCap = counter.manifestCount;
  filler.spineIdHashes = spineIdHashes;
  filler.spineCap = counter.spineCount;
  status = XmlSax::parseEntry(source, entry, scratch, filler);
  if (status != BookStatus::Ok || filler.outOfMemory) {
    scratch.release(scratchMark);
    return status != BookStatus::Ok ? status : BookStatus::OutOfMemory;
  }

  // EPUB 2 books declare the cover via metadata rather than a manifest
  // property; promote the referenced item so consumers see one flag.
  if (filler.coverIdHash != 0) {
    for (size_t i = 0; i < counter.manifestCount; ++i) {
      if (manifest[i].idHash == filler.coverIdHash) {
        manifest[i].isCoverImage = true;
        break;
      }
    }
  }

  // Resolve spine idrefs to manifest indices; unresolvable refs are dropped.
  uint16_t* spine = bookArena.allocArray<uint16_t>(filler.spineUsed);
  if (spine == nullptr && filler.spineUsed != 0) {
    scratch.release(scratchMark);
    return BookStatus::OutOfMemory;
  }
  size_t spineCount = 0;
  for (size_t s = 0; s < filler.spineUsed; ++s) {
    for (size_t m = 0; m < filler.manifestUsed; ++m) {
      if (manifest[m].idHash == spineIdHashes[s]) {
        spine[spineCount++] = static_cast<uint16_t>(m);
        break;
      }
    }
  }
  scratch.release(scratchMark);
  if (spineCount == 0) return BookStatus::NotEpub;

  out->metadata = counter.metadata;
  out->manifest = manifest;
  out->manifestCount = filler.manifestUsed;
  out->spine = spine;
  out->spineCount = spineCount;
  out->navIndex = -1;
  out->ncxIndex = -1;
  for (size_t m = 0; m < filler.manifestUsed; ++m) {
    if (out->navIndex < 0 && manifest[m].isNav) out->navIndex = static_cast<int>(m);
    const bool idMatchesToc = counter.tocId != nullptr && manifest[m].id != nullptr &&
                              strcmp(manifest[m].id, counter.tocId) == 0;
    const bool isNcxType = strcmp(manifest[m].mediaType, "application/x-dtbncx+xml") == 0;
    if (out->ncxIndex < 0 && (idMatchesToc || isNcxType)) out->ncxIndex = static_cast<int>(m);
  }
  return BookStatus::Ok;
}

// ---------------------------------------------------------------------------
// EPUB 3 nav document

namespace {

class NavHandler : public XmlHandler {
 public:
  enum class Mode { Count, Fill };

  NavHandler(Mode mode, Arena& bookArena, const char* docDir)
      : mode_(mode), arena_(bookArena), docDir_(docDir) {}

  void onStartElement(const char* name, const char** atts) override {
    if (localIs(name, "nav")) {
      ++navDepth_;
      if (!inToc_ && navDepth_ == 1) {
        const char* type = attrExact(atts, "epub:type");
        if (hasToken(type, "toc")) inToc_ = true;
      }
      return;
    }
    if (!inToc_) return;
    if (localIs(name, "ol")) {
      if (olDepth_ < kMaxTocDepth) ++olDepth_;
    } else if (localIs(name, "a")) {
      const char* href = attrLocal(atts, "href");
      if (href != nullptr && pendingHref_ == nullptr) {
        pendingHref_ = href;  // borrowed only within this callback...
        if (mode_ == Mode::Count) {
          ++entryCount;
          pendingHref_ = nullptr;
        } else {
          // ...so resolve into the arena immediately.
          resolvedHref_ = resolveHref(arena_, docDir_, href, &resolvedFragment_);
          if (resolvedHref_ == nullptr) outOfMemory = true;
          pendingHref_ = nullptr;
          capture_.begin();
          inAnchor_ = true;
        }
      }
    }
  }

  void onEndElement(const char* name) override {
    if (localIs(name, "nav")) {
      if (navDepth_ > 0) --navDepth_;
      if (navDepth_ == 0) inToc_ = false;
      return;
    }
    if (!inToc_) return;
    if (localIs(name, "ol")) {
      if (olDepth_ > 0) --olDepth_;
    } else if (localIs(name, "a") && inAnchor_) {
      inAnchor_ = false;
      if (mode_ == Mode::Fill && entryUsed < entryCap && resolvedHref_ != nullptr) {
        TocEntry& e = entries[entryUsed];
        const char* title = arena_.strdup(capture_.trimmed());
        if (title == nullptr) {
          outOfMemory = true;
          return;
        }
        e.title = title;
        e.href = resolvedHref_;
        e.fragment = resolvedFragment_;
        e.depth = static_cast<uint8_t>(olDepth_ > 0 ? olDepth_ - 1 : 0);
        ++entryUsed;
      }
      capture_.end();
      resolvedHref_ = nullptr;
      resolvedFragment_ = nullptr;
    }
  }

  void onText(const char* text, int len) override {
    if (inAnchor_) capture_.add(text, len);
  }

  size_t entryCount = 0;
  TocEntry* entries = nullptr;
  size_t entryCap = 0;
  size_t entryUsed = 0;
  bool outOfMemory = false;

 private:
  Mode mode_;
  Arena& arena_;
  const char* docDir_;
  int navDepth_ = 0;
  int olDepth_ = 0;
  bool inToc_ = false;
  bool inAnchor_ = false;
  const char* pendingHref_ = nullptr;
  const char* resolvedHref_ = nullptr;
  const char* resolvedFragment_ = nullptr;
  TextCapture capture_;
};

}  // namespace

BookStatus parseNavToc(BookSource& source, const ZipEntry& entry, const char* docDir,
                       Arena& bookArena, Arena& scratch, TocEntry** entriesOut,
                       size_t* countOut) {
  NavHandler counter(NavHandler::Mode::Count, bookArena, docDir);
  BookStatus status = XmlSax::parseEntry(source, entry, scratch, counter);
  if (status != BookStatus::Ok) return status;

  TocEntry* entries = bookArena.allocArray<TocEntry>(counter.entryCount);
  if (entries == nullptr && counter.entryCount != 0) return BookStatus::OutOfMemory;

  NavHandler filler(NavHandler::Mode::Fill, bookArena, docDir);
  filler.entries = entries;
  filler.entryCap = counter.entryCount;
  status = XmlSax::parseEntry(source, entry, scratch, filler);
  if (status != BookStatus::Ok) return status;
  if (filler.outOfMemory) return BookStatus::OutOfMemory;

  *entriesOut = entries;
  *countOut = filler.entryUsed;
  return BookStatus::Ok;
}

// ---------------------------------------------------------------------------
// EPUB 2 NCX

namespace {

class NcxHandler : public XmlHandler {
 public:
  enum class Mode { Count, Fill };

  NcxHandler(Mode mode, Arena& bookArena, const char* docDir)
      : mode_(mode), arena_(bookArena), docDir_(docDir) {}

  void onStartElement(const char* name, const char** atts) override {
    if (localIs(name, "navPoint")) {
      if (pointDepth_ < kMaxTocDepth) ++pointDepth_;
      pendingTitle_[0] = '\0';
    } else if (localIs(name, "navLabel")) {
      ++inLabel_;
    } else if (inLabel_ > 0 && localIs(name, "text")) {
      capture_.begin();
    } else if (pointDepth_ > 0 && localIs(name, "content")) {
      if (mode_ == Mode::Count) {
        ++entryCount;
      } else if (entryUsed < entryCap) {
        const char* src = attrLocal(atts, "src");
        if (src != nullptr) {
          TocEntry& e = entries[entryUsed];
          const char* fragment = nullptr;
          const char* href = resolveHref(arena_, docDir_, src, &fragment);
          const char* title = arena_.strdup(pendingTitle_);
          if (href == nullptr || title == nullptr) {
            outOfMemory = true;
            return;
          }
          e.href = href;
          e.fragment = fragment;
          e.title = title;
          e.depth = static_cast<uint8_t>(pointDepth_ - 1);
          ++entryUsed;
        }
      }
    }
  }

  void onEndElement(const char* name) override {
    if (localIs(name, "navPoint")) {
      if (pointDepth_ > 0) --pointDepth_;
    } else if (localIs(name, "navLabel")) {
      if (inLabel_ > 0) --inLabel_;
    } else if (capture_.active && localIs(name, "text")) {
      const char* text = capture_.trimmed();
      const size_t n = strlen(text);
      memcpy(pendingTitle_, text, n + 1);
      capture_.end();
    }
  }

  void onText(const char* text, int len) override { capture_.add(text, len); }

  size_t entryCount = 0;
  TocEntry* entries = nullptr;
  size_t entryCap = 0;
  size_t entryUsed = 0;
  bool outOfMemory = false;

 private:
  Mode mode_;
  Arena& arena_;
  const char* docDir_;
  int pointDepth_ = 0;
  int inLabel_ = 0;
  char pendingTitle_[kMaxTextCapture + 1] = "";
  TextCapture capture_;
};

}  // namespace

BookStatus parseNcxToc(BookSource& source, const ZipEntry& entry, const char* docDir,
                       Arena& bookArena, Arena& scratch, TocEntry** entriesOut,
                       size_t* countOut) {
  NcxHandler counter(NcxHandler::Mode::Count, bookArena, docDir);
  BookStatus status = XmlSax::parseEntry(source, entry, scratch, counter);
  if (status != BookStatus::Ok) return status;

  TocEntry* entries = bookArena.allocArray<TocEntry>(counter.entryCount);
  if (entries == nullptr && counter.entryCount != 0) return BookStatus::OutOfMemory;

  NcxHandler filler(NcxHandler::Mode::Fill, bookArena, docDir);
  filler.entries = entries;
  filler.entryCap = counter.entryCount;
  status = XmlSax::parseEntry(source, entry, scratch, filler);
  if (status != BookStatus::Ok) return status;
  if (filler.outOfMemory) return BookStatus::OutOfMemory;

  *entriesOut = entries;
  *countOut = filler.entryUsed;
  return BookStatus::Ok;
}

}  // namespace book
}  // namespace freeink
