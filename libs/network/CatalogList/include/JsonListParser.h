#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <string>
#include <vector>

// Streams rows out of a JSON list response without building a document.
//   itemsPath   dotted path to the item array ("" = the document root is the array)
//   fieldPaths  dotted paths relative to each item; a numeric segment is an
//               array index ("authors.0.name"). Empty leaves the field unset.
//   filesPath   relative path to an array of strings, collected into Row::files
// Strings and numbers are captured as text; the first match wins. Each row goes
// to the sink as soon as its item object closes.
class JsonListParser {
 public:
  enum Field { F_TITLE, F_AUTHOR, F_ID, F_URL, F_VERSION, F_BASE, F_COUNT };
  struct Row {
    std::string field[F_COUNT];
    std::vector<std::string> files;
  };

  using RowSink = void (*)(void* ctx, Row& row);
  // Fills buf with up to len bytes; returns the byte count, <= 0 at end of input.
  using ReadFn = int (*)(void* ctx, char* buf, size_t len);

  JsonListParser(const std::string& itemsPath, const std::string* const (&fieldPaths)[F_COUNT],
                 const std::string& filesPath, RowSink sink, void* sinkCtx);

  // Rows emitted before a syntax error are kept. Returns false on malformed
  // or truncated JSON. Large (the SAX token buffer plus a read buffer): allocate on the heap.
  bool parse(ReadFn read, void* readCtx);
  // Account for rows retained by the consumer, not just this parser's current row.
  void setCaptureBudget(size_t bytes) { captureBudget = bytes; }

 private:
  using Path = std::vector<std::string>;
  struct Frame {
    bool isArray;
    int index;        // array: index of the current element
    std::string key;  // object: key of the current member
  };

  static void split(const std::string& dotted, Path& out);
  static bool segEquals(const Frame& frame, const std::string& seg);
  // True when frames [from, from + path.size()) spell `path` and exactly
  // `extra` frames follow them.
  bool framesMatch(size_t from, const Path& path, size_t extra = 0) const;
  void beginValue();
  bool atItem() const;
  void onContainerStart(bool isArray);
  void onContainerEnd(bool isArray);
  void onScalar(const char* value, size_t len, bool isString);

  Path items;
  Path fields[F_COUNT];
  Path files;
  std::vector<Frame> stack;
  size_t itemDepth = 0;    // stack depth of the open item object; 0 = none
  bool malformed = false;  // mismatched closer (JsonSax does not track nesting)
  size_t captureBudget = 64 * 1024;
  size_t capturedBytes = 0;
  Row current;
  RowSink sink;
  void* sinkCtx;
  StreamingJsonParser json;
  char buf[1024] = {};
};
