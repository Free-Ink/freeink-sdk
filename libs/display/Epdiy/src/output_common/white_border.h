#pragma once
#include <stdint.h>

// Four pixels per output byte. Runtime scans leave the white border electrically
// undriven (00). Only startup conditioning may use whitening (10), never dark.
static inline __attribute__((always_inline)) void epd_filter_white_border(
    uint8_t* line,int y,int line_bytes,int height,int border,int whitening
) {
    if (border<=0 || y>=height) return;
    if (y<border || y>=height-border) {
        for (int i=0;i<line_bytes;++i) line[i]=whitening ? line[i]&0xAA : 0;
    } else {
        const int edge_bytes=border/4;
        for (int i=0;i<edge_bytes;++i) {
            line[i]=whitening ? line[i]&0xAA : 0;
            line[line_bytes-1-i]=whitening ? line[line_bytes-1-i]&0xAA : 0;
        }
    }
}
