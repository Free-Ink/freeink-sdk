// Host test for JsonListParser against the item/field path shapes the
// shipping catalog manifests use.
#include <JsonListParser.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using Row = JsonListParser::Row;
std::vector<Row> rows;

struct Src {
  const char* p;
  size_t left;
};
// 5-byte reads split tokens across feeds.
int read5(void* c, char* b, size_t n) {
  auto* s = static_cast<Src*>(c);
  size_t k = s->left < 5 ? s->left : 5;
  if (k > n) k = n;
  memcpy(b, s->p, k);
  s->p += k;
  s->left -= k;
  return static_cast<int>(k);
}

bool run(const char* doc, const std::string& items, std::initializer_list<const char*> paths, const char* filesPath,
         size_t budget = 64 * 1024) {
  std::vector<std::string> p(paths.begin(), paths.end());
  p.resize(JsonListParser::F_COUNT);
  const std::string* const fields[] = {&p[0], &p[1], &p[2], &p[3], &p[4], &p[5]};
  rows.clear();
  auto* parser = new JsonListParser(items, fields, filesPath, [](void*, Row& r) { rows.push_back(r); }, nullptr);
  parser->setCaptureBudget(budget);
  Src s{doc, strlen(doc)};
  const bool ok = parser->parse(read5, &s);
  delete parser;
  return ok;
}
}  // namespace

int main() {
  // bookfusion: root array, indexed author path, numeric id, unrelated nesting.
  assert(run(R"([{"id":17,"title":"A","authors":[{"name":"Ann"},{"name":"Bob"}],"meta":{"title":"no"}},)"
             R"({"title":"B \"q\"","authors":[]}])",
             "", {"title", "authors.0.name", "id"}, ""));
  assert(rows.size() == 2);
  assert(rows[0].field[JsonListParser::F_TITLE] == "A" && rows[0].field[JsonListParser::F_AUTHOR] == "Ann" &&
         rows[0].field[JsonListParser::F_ID] == "17");
  assert(rows[1].field[JsonListParser::F_TITLE] == "B \"q\"" && rows[1].field[JsonListParser::F_AUTHOR].empty());

  // wallabag: nested items path; siblings of the items array are ignored.
  assert(run(R"({"page":1,"_embedded":{"items":[{"id":"x","title":"T"}],"other":[{"title":"skip"}]}})",
             "_embedded.items", {"title", "", "id"}, ""));
  assert(rows.size() == 1 && rows[0].field[JsonListParser::F_ID] == "x");

  // plugin-store / dictionaries: version, bundle base and files array.
  assert(
      run(R"({"plugins":[{"name":"p","title":"P","version":"1.2","base":"https://h/p","files":["a.js","b/c.bin"]}]})",
          "plugins", {"title", "", "name", "", "version", "base"}, "files"));
  assert(rows.size() == 1 && rows[0].field[JsonListParser::F_VERSION] == "1.2" &&
         rows[0].field[JsonListParser::F_BASE] == "https://h/p" && rows[0].files.size() == 2 &&
         rows[0].files[1] == "b/c.bin");

  // Mismatched closers and a cut-off body both report failure.
  assert(!run(R"([{"title":"A"}}])", "", {"title"}, ""));
  assert(!run(R"([{"title":"A"},{"title":"B")", "", {"title"}, "") && rows.size() == 1);

  std::string deep(33, '[');
  deep += std::string(33, ']');
  assert(!run(deep.c_str(), "", {}, ""));
  assert(!run(R"([{"title":"abc"},{"title":"def"}])", "", {"title"}, "", 5));
  assert(rows.size() == 1);
  std::string bundle = R"([{"title":"bundle","files":[)";
  for (int i = 0; i < 65; ++i) {
    if (i) bundle += ',';
    bundle += "\"a\"";
  }
  bundle += "]}]";
  assert(!run(bundle.c_str(), "", {"title"}, "files"));
  puts("json list ok");
}
