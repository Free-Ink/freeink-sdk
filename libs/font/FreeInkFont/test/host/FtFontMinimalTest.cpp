// Host smoke test for a MINIMAL build (none of the three FREEINK_FONT_ENABLE_*
// flags set). Confirms Default rendering still works, and that requesting an
// unsupported mode is reported via setRenderOptions()'s return value rather
// than silently degrading with no signal at all.
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

int checks = 0;
int failures = 0;
void expect(bool cond, const char* what) {
  ++checks;
  if (!cond) {
    ++failures;
    fprintf(stderr, "FAIL: %s\n", what);
  } else {
    printf("ok: %s\n", what);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s font.ttf\n", argv[0]);
    return 1;
  }
  const std::vector<uint8_t> bytes = readFile(argv[1]);

  FtFont font;
  expect(font.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 16, 400, false), "init() loads the face");
  expect(font.rasterize('A', 16) != nullptr, "Default rasterize works with no optional modules compiled in");

  FtFont::RenderOptions autoHint;
  autoHint.hinting = FtFont::HintingMode::Auto;
  expect(font.setRenderOptions(autoHint) == false,
         "setRenderOptions(Auto) reports unsupported when AUTOHINT wasn't compiled in");

  FtFont::RenderOptions native;
  native.hinting = FtFont::HintingMode::Native;
  expect(font.setRenderOptions(native) == false,
         "setRenderOptions(Native) reports unsupported when NATIVE_HINTING wasn't compiled in");

  FtFont::RenderOptions mono;
  mono.monochrome = true;
  expect(font.setRenderOptions(mono) == false,
         "setRenderOptions(monochrome) reports unsupported when MONOCHROME wasn't compiled in");

  FtFont::RenderOptions none;
  none.hinting = FtFont::HintingMode::None;
  expect(font.setRenderOptions(none) == true, "setRenderOptions(None) is always supported");
  expect(font.rasterize('A', 16) != nullptr, "rasterize still works after a reported-unsupported request");

  printf("\n%d/%d checks passed\n", checks - failures, checks);
  return failures == 0 ? 0 : 1;
}
