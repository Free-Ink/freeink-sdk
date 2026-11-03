#pragma once

#include <expat.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Streams rows out of an XML list response, matching elements by local name
// (namespace prefixes ignored). Field selectors:
//   "elem"       leading text of the first descendant <elem>
//   "elem@attr"  attribute of the first descendant <elem>
//   "@attr"      attribute on the item element itself
// An empty selector leaves that field empty. Each row goes to the sink as soon
// as its item element closes, so the document is never held in memory.
//
// Rows are URL listings (WebDAV PROPFIND and similar): a row without a url is
// dropped, and an empty title falls back to the url's last path segment.
class XmlListParser {
 public:
  static constexpr size_t MAX_ITEMS = 200;
  static constexpr size_t MAX_FIELD_CHARS = 768;
  enum Field { F_URL, F_TITLE, F_AUTHOR, F_ID, F_COUNT };
  struct RawItem {
    std::string field[F_COUNT];
    bool isDir = false;  // the item contains a `containerName` element
  };

  using ItemSink = void (*)(void* ctx, RawItem& item);
  // Fills buf with up to len bytes; returns the byte count, <= 0 at end of input.
  using ReadFn = int (*)(void* ctx, char* buf, size_t len);

  struct UrlOptions {
    std::string requestUrl;               // the listing's own URL
    bool skipSelf = false;                // drop the entry for requestUrl itself
    bool resolveUrls = false;             // resolve "/path" and relative hrefs against requestUrl
    std::vector<std::string> extensions;  // allowed file extensions, case-insensitive; empty = all
  };

  XmlListParser(const std::string& itemName, const std::string& containerName,
                const std::string* const (&selectors)[F_COUNT], ItemSink sink, void* sinkCtx);

  // Rows emitted before a parse error are kept. Returns false on parser OOM or
  // malformed XML. Large (2 KB read buffer): allocate the parser on the heap.
  bool parse(ReadFn read, void* readCtx);
  void setUrlOptions(UrlOptions options);
  void setCaptureBudget(size_t bytes) { captureBudget = bytes; }

 private:
  struct Selector {
    std::string elem, attr;
    bool onItemTag = false;
    bool isSet() const { return !elem.empty() || !attr.empty(); }
  };

  void onStart(const XML_Char* name, const XML_Char** atts);
  void onEnd(const XML_Char* name);
  void onText(const XML_Char* s, int len);
  // Applies UrlOptions to a finished row; false drops it.
  bool acceptRow(RawItem& row) const;

  std::string item;
  std::string container;
  UrlOptions urls;
  std::string origin;       // scheme://host[:port] of requestUrl
  std::string decodedSelf;  // requestUrl's decoded path, trailing slashes trimmed
  std::string requestDir;   // requestUrl's decoded folder, with trailing '/', for relative hrefs
  Selector sel[F_COUNT];
  char buf[2048] = {};
  ItemSink sink;
  void* sinkCtx;
  size_t parsedItems = 0;
  size_t captureBudget = 64 * 1024;
  size_t capturedBytes = 0;
  bool limitExceeded = false;
  RawItem current;
  bool done[F_COUNT] = {};
  uint8_t capturingMask = 0;
  int depth = 0;
  int itemDepth = -1;
};
