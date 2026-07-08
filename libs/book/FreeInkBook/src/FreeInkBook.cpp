// FreeInkBook — book opening: ZIP catalog → container.xml → OPF → TOC.

#include "FreeInkBook.h"

#include <string.h>

#include "epub/PackageParsers.h"
#include "epub/XmlSax.h"

namespace freeink {
namespace book {

namespace {

// META-INF/encryption.xml does NOT always mean DRM: retail EPUBs routinely
// declare only obfuscated embedded fonts (IDPF or Adobe mangling), which the
// engine never reads anyway. Only actual content encryption (ADEPT, LCP —
// anything beyond the two font-obfuscation algorithms) makes a book
// unreadable.
class EncryptionScan : public XmlHandler {
 public:
  void onStartElement(const char* name, const char** atts) override {
    const char* colon = strrchr(name, ':');
    const char* local = colon != nullptr ? colon + 1 : name;
    if (strcmp(local, "EncryptionMethod") != 0) return;
    for (int i = 0; atts != nullptr && atts[i] != nullptr; i += 2) {
      const char* acolon = strrchr(atts[i], ':');
      const char* alocal = acolon != nullptr ? acolon + 1 : atts[i];
      if (strcmp(alocal, "Algorithm") != 0) continue;
      const char* alg = atts[i + 1];
      const bool fontObfuscation = strcmp(alg, "http://www.idpf.org/2008/embedding") == 0 ||
                                   strcmp(alg, "http://ns.adobe.com/pdf/enc#RC") == 0;
      if (!fontObfuscation) contentEncrypted = true;
    }
  }
  bool contentEncrypted = false;
};

}  // namespace

BookStatus Book::open(BookSource& source, Arena& bookArena, Arena& scratch) {
  source_ = &source;
  manifest_ = nullptr;
  manifestCount_ = 0;
  spine_ = nullptr;
  spineCount_ = 0;
  toc_ = nullptr;
  tocCount_ = 0;
  metadata_ = BookMetadata{};

  BookStatus status = zip_.open(source, bookArena);
  if (status != BookStatus::Ok) return status;

  const ZipEntry* encryption = zip_.find("META-INF/encryption.xml");
  if (encryption != nullptr) {
    EncryptionScan scan;
    const size_t encMark = scratch.mark();
    const BookStatus encStatus = XmlSax::parseEntry(source, *encryption, scratch, scan);
    scratch.release(encMark);
    // Unparseable encryption manifest: assume the worst rather than render
    // garbage.
    if (encStatus != BookStatus::Ok || scan.contentEncrypted) return BookStatus::Encrypted;
  }

  const ZipEntry* container = zip_.find("META-INF/container.xml");
  if (container == nullptr) return BookStatus::NotEpub;

  const char* opfPath = nullptr;
  status = parseContainer(source, *container, bookArena, scratch, &opfPath);
  if (status != BookStatus::Ok) return status;

  const ZipEntry* opfEntry = zip_.find(opfPath);
  if (opfEntry == nullptr) return BookStatus::NotEpub;

  char opfDir[512];
  if (!dirName(opfPath, opfDir, sizeof(opfDir))) return BookStatus::Unsupported;

  PackageResult package;
  status = parsePackage(source, *opfEntry, opfDir, bookArena, scratch, &package);
  if (status != BookStatus::Ok) return status;

  metadata_ = package.metadata;
  manifest_ = package.manifest;
  manifestCount_ = package.manifestCount;
  spine_ = package.spine;
  spineCount_ = package.spineCount;

  // Prefer the EPUB 3 nav document; fall back to the EPUB 2 NCX. A book
  // without any TOC still opens — the spine is the authoritative reading
  // order and paging must not depend on navigation data.
  const int tocIndex = package.navIndex >= 0 ? package.navIndex : package.ncxIndex;
  if (tocIndex >= 0) {
    const ManifestItem& tocItem = manifest_[tocIndex];
    const ZipEntry* tocZipEntry = zip_.find(tocItem.href);
    if (tocZipEntry != nullptr) {
      char tocDir[512];
      if (!dirName(tocItem.href, tocDir, sizeof(tocDir))) return BookStatus::Unsupported;
      status = package.navIndex >= 0
                   ? parseNavToc(source, *tocZipEntry, tocDir, bookArena, scratch, &toc_,
                                 &tocCount_)
                   : parseNcxToc(source, *tocZipEntry, tocDir, bookArena, scratch, &toc_,
                                 &tocCount_);
      if (status != BookStatus::Ok) return status;
    }
  }
  return BookStatus::Ok;
}

BookStatus Book::openItem(const ManifestItem& item, ZipEntryReader* reader,
                          Arena& scratch) const {
  if (source_ == nullptr) return BookStatus::NotFound;
  const ZipEntry* entry = zip_.find(item.href);
  if (entry == nullptr) return BookStatus::NotFound;
  return reader->open(*source_, *entry, scratch);
}

}  // namespace book
}  // namespace freeink
