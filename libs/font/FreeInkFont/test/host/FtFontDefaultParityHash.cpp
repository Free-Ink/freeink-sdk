// Prints a stable hash of HintingMode::Default's rasterized output across a
// range of codepoints/sizes. Built TWICE by run.sh — once with all three
// FREEINK_FONT_ENABLE_* flags on, once with none — and the two hashes are
// compared by the shell script. This is the actual regression check for "no
// caller who never touches RenderOptions sees different output just because
// an optional module got compiled in": a per-instance boolean assertion
// inside one binary can't catch a difference that only shows up BETWEEN
// two different build configurations of the same source.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "FtFont.h"

using freeink::font::FtFont;

namespace {

std::vector<uint8_t> readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (fread(data.data(), 1, data.size(), f) != data.size()) {
    fprintf(stderr, "short read on %s\n", path);
    exit(1);
  }
  fclose(f);
  return data;
}

void mix(uint64_t& hash, uint32_t value) {
  hash ^= value;
  hash *= 1099511628211ull;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s font.ttf\n", argv[0]);
    return 1;
  }
  const std::vector<uint8_t> bytes = readFile(argv[1]);

  uint64_t hash = 1469598103934665603ull;
  for (uint16_t sizePx : {8, 10, 12, 16, 20, 24, 32, 40}) {
    FtFont font;
    if (!font.init(bytes.data(), static_cast<uint32_t>(bytes.size()), sizePx, 400, false)) {
      fprintf(stderr, "init() failed at size %u\n", sizePx);
      return 1;
    }
    // Default is the constructor's default RenderOptions{} — never call
    // setRenderOptions() at all, matching a caller who doesn't know this API
    // exists.
    for (uint32_t cp = 0x20; cp < 0x7f; ++cp) {
      mix(hash, static_cast<uint32_t>(font.advance(cp, sizePx, 0)));
      const auto* bitmap = font.rasterize(cp, sizePx);
      if (!bitmap) continue;
      mix(hash, bitmap->width);
      mix(hash, bitmap->height);
      for (uint32_t i = 0; i < uint32_t(bitmap->width) * bitmap->height; ++i) mix(hash, bitmap->pixels[i]);
    }
  }
  printf("%016llx\n", static_cast<unsigned long long>(hash));
  return 0;
}
