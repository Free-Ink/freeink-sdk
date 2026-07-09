// FreeInkBook — expat-backed streaming XML parse of ZIP entries.

#include "epub/XmlSax.h"

#include <expat.h>

#include <new>

#include "text/EntityFilter.h"

namespace freeink {
namespace book {

namespace {

constexpr uint32_t kParseChunk = 2048;

void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  static_cast<XmlHandler*>(userData)->onStartElement(name, atts);
}

void XMLCALL endElement(void* userData, const XML_Char* name) {
  static_cast<XmlHandler*>(userData)->onEndElement(name);
}

void XMLCALL characterData(void* userData, const XML_Char* text, int len) {
  static_cast<XmlHandler*>(userData)->onText(text, len);
}

}  // namespace

BookStatus XmlSaxSession::open(BookSource& source, const ZipEntry& entry, Arena& scratch,
                               XmlHandler& handler, bool filterHtmlEntities) {
  close();
  handler_ = &handler;
  bytesConsumed_ = 0;
  finished_ = false;

  BookStatus status = reader_.open(source, entry, scratch);
  if (status != BookStatus::Ok) return status;

  buf_ = static_cast<char*>(scratch.alloc(kParseChunk, 1));
  filter_ = nullptr;
  if (filterHtmlEntities) {
    filter_ = static_cast<EntityFilter*>(scratch.alloc(sizeof(EntityFilter), alignof(EntityFilter)));
    if (filter_ != nullptr) {
      filter_ = new (filter_) EntityFilter();
      filter_->reset();
    }
  }
  if (buf_ == nullptr || (filterHtmlEntities && filter_ == nullptr)) {
    return BookStatus::OutOfMemory;
  }

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (parser == nullptr) return BookStatus::OutOfMemory;
  XML_SetUserData(parser, &handler);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  parser_ = parser;
  return BookStatus::Ok;
}

BookStatus XmlSaxSession::feedChunk(bool* atEnd) {
  if (atEnd != nullptr) *atEnd = finished_;
  if (parser_ == nullptr || finished_) return BookStatus::Unsupported;

  const int32_t n = filter_ != nullptr ? filter_->read(reader_, buf_, kParseChunk)
                                       : reader_.read(buf_, kParseChunk);
  if (n < 0) return BookStatus::IoError;
  if (XML_Parse(static_cast<XML_Parser>(parser_), buf_, n, n == 0) == XML_STATUS_ERROR) {
    return BookStatus::ParseError;
  }
  bytesConsumed_ += static_cast<uint64_t>(n);
  if (n == 0) {
    finished_ = true;
    if (atEnd != nullptr) *atEnd = true;
  }
  return BookStatus::Ok;
}

void XmlSaxSession::close() {
  if (parser_ != nullptr) {
    XML_ParserFree(static_cast<XML_Parser>(parser_));
    parser_ = nullptr;
  }
  // buf_/filter_ are arena allocations — reclaimed by the arena owner.
  buf_ = nullptr;
  filter_ = nullptr;
  handler_ = nullptr;
  finished_ = false;
}

BookStatus XmlSax::parseEntry(BookSource& source, const ZipEntry& entry, Arena& scratch,
                              XmlHandler& handler, bool filterHtmlEntities) {
  // One-shot = the session pumped to completion; a single code path keeps
  // stepped and blocking parses behaviorally identical.
  const size_t marked = scratch.mark();
  XmlSaxSession session;
  BookStatus status = session.open(source, entry, scratch, handler, filterHtmlEntities);
  if (status == BookStatus::Ok) {
    bool atEnd = false;
    while (!atEnd && !handler.stopParse) {
      status = session.feedChunk(&atEnd);
      if (status != BookStatus::Ok) break;
    }
  }
  session.close();
  scratch.release(marked);
  return status;
}

}  // namespace book
}  // namespace freeink
