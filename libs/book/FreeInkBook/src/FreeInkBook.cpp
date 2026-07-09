// FreeInkBook — book opening: ZIP catalog → container.xml → OPF → TOC.

#include "FreeInkBook.h"

#include <string.h>

#include "epub/MinizConfig.h"
#include "epub/PackageParsers.h"
#include "epub/XmlSax.h"
#include "epub/XmlUtil.h"

#include <stdio.h>

#include <expat.h>

namespace freeink {
namespace book {

using xmlutil::EncryptionScan;

const char* vendorVersions() {
  static char buf[96];
  snprintf(buf, sizeof(buf), "miniz %s expat %d.%d.%d", MZ_VERSION, XML_MAJOR_VERSION,
           XML_MINOR_VERSION, XML_MICRO_VERSION);
  return buf;
}

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
