#pragma once
#include "epd_internals.h"
#include <stdint.h>

// Actual target/old gray pairs remain intact. Each band starts one phase later.
static inline void epd_spatial_build(uint8_t* luts,const EpdWaveformPhases* phases,int frame) {
    for(int band=0;band<10;++band) for(int target=0;target<16;++target) for(int old=0;old<16;++old) {
        const int local=frame-band;
        int action=0;
        if(local>=0 && local<phases->phases && !(target==15 && old==15))
            action=(phases->luts[(local*16+target)*4+old/4]>>(6-2*(old&3)))&3;
        luts[band*256+target*16+old]=action;
    }
}
static inline void epd_spatial_bands(uint8_t* bands,int size,int reverse) {
    for(int i=0;i<size;++i) bands[i]=(reverse ? size-1-i : i)*10/size;
}
// Expand four packed target/old pairs with one aligned PSRAM load. S31 consumes
// the first pixel in the high bits. Force inline into the IRAM/O3 feeder rather
// than outlining this hot loop into a size-optimized flash function.
static inline __attribute__((always_inline)) uint8_t epd_spatial_lookup4(uint32_t in,const uint8_t* lut) {
    return (lut[in&255]<<6)|(lut[(in>>8)&255]<<4)|(lut[(in>>16)&255]<<2)|lut[in>>24];
}
static inline __attribute__((always_inline)) void epd_spatial_span(const uint8_t* input,uint8_t* output,
                                                               int words,const uint8_t* lut) {
    const uint32_t* src=(const uint32_t*)input;
    int i=0;
    for(;i+4<=words;i+=4) {
        output[i]=epd_spatial_lookup4(src[i],lut);
        output[i+1]=epd_spatial_lookup4(src[i+1],lut);
        output[i+2]=epd_spatial_lookup4(src[i+2],lut);
        output[i+3]=epd_spatial_lookup4(src[i+3],lut);
    }
    for(;i<words;++i) output[i]=epd_spatial_lookup4(src[i],lut);
}
static inline __attribute__((always_inline)) void epd_spatial_line(const uint8_t* input,uint8_t* output,int width,
                                                               const uint8_t* luts,const uint8_t* bands,int by_y,int row) {
    if(by_y) {
        epd_spatial_span(input,output,width/4,luts+bands[row]*256);
        return;
    }
    // Landscape: bulk-convert aligned spans; only the nine band boundaries
    // need individual per-pixel LUT selection. No extra internal buffer.
    int x=0;
    while(x<width) {
        const int band=bands[x];
        int end=x;
        while(end+4<=width && bands[end]==band && bands[end+3]==band) end+=4;
        if(end>x) {
            epd_spatial_span(input+x,output+x/4,(end-x)/4,luts+band*256);
            x=end;
        } else {
            output[x/4]=(luts[bands[x]*256+input[x]]<<6)|
                (luts[bands[x+1]*256+input[x+1]]<<4)|
                (luts[bands[x+2]*256+input[x+2]]<<2)|luts[bands[x+3]*256+input[x+3]];
            x+=4;
        }
    }
}
