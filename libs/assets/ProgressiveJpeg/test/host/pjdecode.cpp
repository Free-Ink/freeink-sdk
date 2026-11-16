// pjdecode <in.jpg> <out.pgm>: decodes with ProgressiveJpeg and writes the luma
// as a binary PGM. Exit 2 = begin() status, 3 = decode() status (printed).
#include <cstdio>

#include "ProgressiveJpeg.h"

using freeink::ProgressiveJpeg;

static int32_t readFile(void* ctx, uint8_t* dst, uint32_t n) {
  return static_cast<int32_t>(fread(dst, 1, n, static_cast<FILE*>(ctx)));
}

static bool writeRow(void* user, uint16_t y, const uint8_t* row, uint16_t width) {
  static int expectY = 0;
  if (y != expectY++) return false;
  return fwrite(row, 1, width, static_cast<FILE*>(user)) == width;
}

int main(int argc, char** argv) {
  if (argc != 3) return 1;
  FILE* in = fopen(argv[1], "rb");
  if (!in) return 1;
  ProgressiveJpeg pj;
  ProgressiveJpeg::Status s = pj.begin(readFile, in);
  if (s != ProgressiveJpeg::Status::Ok) {
    printf("begin %d\n", static_cast<int>(s));
    return 2;
  }
  rewind(in);
  FILE* out = fopen(argv[2], "wb");
  fprintf(out, "P5\n%u %u\n255\n", pj.width(), pj.height());
  s = pj.decode(readFile, in, writeRow, out);
  fclose(out);
  fclose(in);
  if (s != ProgressiveJpeg::Status::Ok) {
    printf("decode %d\n", static_cast<int>(s));
    return 3;
  }
  return 0;
}
