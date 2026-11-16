#include <XmlListParser.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
static const char* kDoc =
    "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">"
    "<d:response><d:href>/books/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></"
    "d:prop></d:propstat></d:response>"
    "<d:response><d:href> /books/a%20b.epub </d:href><d:propstat><d:prop><d:displayname>A "
    "B</d:displayname><d:resourcetype/></d:prop></d:propstat></d:response>"
    "</d:multistatus>";
struct Src {
  const char* p;
  size_t left;
};
int main() {
  std::string url = "href", title = "displayname", none;
  const std::string* const sel[] = {&url, &title, &none, &none};
  std::vector<XmlListParser::RawItem> rows;
  auto* p = new XmlListParser(
      "response", "collection", sel,
      [](void* c, XmlListParser::RawItem& r) { static_cast<std::vector<XmlListParser::RawItem>*>(c)->push_back(r); },
      &rows);
  Src s{kDoc, strlen(kDoc)};
  // 7-byte reads exercise element/text boundaries split across chunks.
  bool ok = p->parse(
      [](void* c, char* b, size_t n) {
        auto* s = static_cast<Src*>(c);
        size_t k = s->left < 7 ? s->left : 7;
        if (k > n) k = n;
        memcpy(b, s->p, k);
        s->p += k;
        s->left -= k;
        return (int)k;
      },
      &s);
  assert(ok && rows.size() == 2);
  assert(rows[0].isDir && rows[0].field[XmlListParser::F_URL] == "/books/");
  assert(!rows[1].isDir && rows[1].field[XmlListParser::F_URL] == "/books/a%20b.epub" &&
         rows[1].field[XmlListParser::F_TITLE] == "A B");
  Src bad{"<a><b></a>", 10};
  XmlListParser q("b", "", sel, [](void*, XmlListParser::RawItem&) {}, nullptr);
  assert(!q.parse(
      [](void* c, char* b, size_t n) {
        auto* s = static_cast<Src*>(c);
        size_t k = s->left < n ? s->left : n;
        memcpy(b, s->p, k);
        s->p += k;
        s->left -= k;
        return (int)k;
      },
      &bad));
  delete p;

  // WebDAV options: skip the listing itself, filter files by extension (not
  // folders), resolve relative hrefs, fall back to the file name for a title.
  static const char* kDav =
      "<d:multistatus xmlns:d=\"DAV:\">"
      "<d:response><d:href>/My%20Books/</d:href><d:resourcetype><d:collection/></d:resourcetype></d:response>"
      "<d:response><d:href>/My%20Books/sub/</d:href><d:resourcetype><d:collection/></d:resourcetype></d:response>"
      "<d:response><d:href>/My%20Books/Caf%C3%A9.EPUB</d:href></d:response>"
      "<d:response><d:href>/My%20Books/notes.txt</d:href></d:response>"
      "<d:response><d:displayname></d:displayname></d:response>"
      "</d:multistatus>";
  rows.clear();
  XmlListParser dav(
      "response", "collection", sel,
      [](void* c, XmlListParser::RawItem& r) { static_cast<std::vector<XmlListParser::RawItem>*>(c)->push_back(r); },
      &rows);
  XmlListParser::UrlOptions urls;
  urls.requestUrl = "https://dav.example:8443/My%20Books/";
  urls.skipSelf = true;
  urls.resolveUrls = true;
  urls.extensions = {".epub"};
  dav.setUrlOptions(urls);
  Src d{kDav, strlen(kDav)};
  assert(dav.parse(
      [](void* c, char* b, size_t n) {
        auto* s = static_cast<Src*>(c);
        size_t k = s->left < n ? s->left : n;
        memcpy(b, s->p, k);
        s->p += k;
        s->left -= k;
        return (int)k;
      },
      &d));
  assert(rows.size() == 2);
  assert(rows[0].isDir && rows[0].field[XmlListParser::F_TITLE] == "sub" &&
         rows[0].field[XmlListParser::F_URL] == "https://dav.example:8443/My%20Books/sub/");
  assert(!rows[1].isDir && rows[1].field[XmlListParser::F_TITLE] == "Caf\xC3\xA9.EPUB" &&
         rows[1].field[XmlListParser::F_URL] == "https://dav.example:8443/My%20Books/Caf%C3%A9.EPUB");
  // Relative hrefs join the listing's folder; '+' stays literal in paths; a
  // full-URL self entry is still recognized and skipped.
  static const char* kRel =
      "<d:multistatus xmlns:d=\"DAV:\">"
      "<d:response><d:href>https://h/a/books/</d:href><d:resourcetype><d:collection/></d:resourcetype></d:response>"
      "<d:response><d:href>book.epub</d:href></d:response>"
      "<d:response><d:href>/a/books/A+B.epub</d:href></d:response>"
      "<d:response><d:href>https://cdn/x/C.epub</d:href></d:response>"
      "</d:multistatus>";
  rows.clear();
  XmlListParser rel(
      "response", "collection", sel,
      [](void* c, XmlListParser::RawItem& r) { static_cast<std::vector<XmlListParser::RawItem>*>(c)->push_back(r); },
      &rows);
  XmlListParser::UrlOptions relUrls;
  relUrls.requestUrl = "https://h/a/books/";
  relUrls.skipSelf = true;
  relUrls.resolveUrls = true;
  rel.setUrlOptions(relUrls);
  Src rs{kRel, strlen(kRel)};
  assert(rel.parse(
      [](void* c, char* b, size_t n) {
        auto* s = static_cast<Src*>(c);
        size_t k = s->left < n ? s->left : n;
        memcpy(b, s->p, k);
        s->p += k;
        s->left -= k;
        return (int)k;
      },
      &rs));
  assert(rows.size() == 3);
  assert(rows[0].field[XmlListParser::F_URL] == "https://h/a/books/book.epub" &&
         rows[0].field[XmlListParser::F_TITLE] == "book.epub");
  assert(rows[1].field[XmlListParser::F_URL] == "https://h/a/books/A%2BB.epub" &&
         rows[1].field[XmlListParser::F_TITLE] == "A+B.epub");
  assert(rows[2].field[XmlListParser::F_URL] == "https://cdn/x/C.epub" &&
         rows[2].field[XmlListParser::F_TITLE] == "C.epub");
  // Captured rows and attribute values must stay inside explicit budgets.
  auto checkLimited = [&](const std::string& xml, size_t budget) {
    XmlListParser parser("response", "collection", sel, [](void*, XmlListParser::RawItem&) {}, nullptr);
    parser.setCaptureBudget(budget);
    Src source{xml.data(), xml.size()};
    return parser.parse(
        [](void* c, char* b, size_t n) {
          auto* s = static_cast<Src*>(c);
          const size_t k = std::min(s->left, n);
          memcpy(b, s->p, k);
          s->p += k;
          s->left -= k;
          return static_cast<int>(k);
        },
        &source);
  };
  assert(!checkLimited("<root><response><href>/a</href></response></root>", 1));
  std::string deep;
  for (int i = 0; i < 33; ++i) deep += "<a>";
  for (int i = 0; i < 33; ++i) deep += "</a>";
  assert(!checkLimited(deep, 1024));
  puts("xmllist ok");
}
