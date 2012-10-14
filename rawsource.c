/*
  vsrawsource.c: Raw Format Reader for VapourSynth

  This file is a part of vsrawsource

  Copyright (C) 2012  Oka Motofumi

  Author: Oka Motofumi (chikuzen.mo at gmail dot com)

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with Libav; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/


#ifdef __MINGW32__
#define rs_fseek fseeko64
#define rs_ftell ftello64
#endif

#include <stdio.h>

#ifndef rs_fseek
#define _FILE_OFFSET_BITS 64
#define rs_fseek fseek
#define rs_ftell ftell
#endif

#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#ifdef __MINGW32__
#   if __MSVCRT_VERSION__ < 0x0700
#   undef __MSVCRT_VERSION__
#   define __MSVCRT_VERSION__ 0x0700
#   endif
#include <malloc.h>
#endif

#include "VapourSynth.h"


#define VS_RAWS_VERSION "0.1.1"
#define FORMAT_MAX_LEN 32


typedef struct rs_hndle {
    FILE *file;
    int64_t file_size;
    uint32_t frame_size;
    char src_format[FORMAT_MAX_LEN];
    int order[4];
    int off_header;
    int off_frame;
    int64_t *index;
    uint8_t *frame_buff;
    void (VS_CC *write_frame)(struct rs_hndle *, VSFrameRef *, const VSAPI *);
    VSVideoInfo vi;
} rs_hnd_t;

typedef void (VS_CC *write_frame)(rs_hnd_t *, VSFrameRef *, const VSAPI *);

typedef struct {
    const VSMap *in;
    VSMap *out;
    VSCore *core;
    const VSAPI *vsapi;
} vs_args_t;


static void *rs_malloc(size_t size)
{
#ifdef __MINGW32__
    return _aligned_malloc(size, 16);
#else
    return malloc(size);
#endif
}


static void rs_free(uint8_t *p)
{
#ifdef __MINGW32__
    _aligned_free(p);
#else
    free(p);
#endif
}


static void VS_CC
rs_bit_blt(uint8_t *srcp, int row_size, int height, VSFrameRef *dst, int plane,
           const VSAPI *vsapi)
{
    uint8_t *dstp = vsapi->getWritePtr(dst, plane);
    int dst_stride = vsapi->getStride(dst, plane);

    if (row_size == dst_stride) {
        memcpy(dstp, srcp, row_size * height);
        return;
    }

    for (int i = 0; i < height; i++) {
        memcpy(dstp, srcp, row_size);
        dstp += dst_stride;
        srcp += row_size;
    }
}


static inline uint32_t VS_CC
bitor8to32(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
           ((uint32_t)b2 << 8) | (uint32_t)b3;
}


static void VS_CC
write_planar_frame(rs_hnd_t *rh, VSFrameRef *dst, const VSAPI *vsapi)
{
    uint8_t *srcp = rh->frame_buff;

    for (int i = 0, num = rh->vi.format->numPlanes; i < num; i++) {
        int plane = rh->order[i];
        int row_size = vsapi->getFrameWidth(dst, plane) * rh->vi.format->bytesPerSample;
        int height = vsapi->getFrameHeight(dst, plane);
        rs_bit_blt(srcp, row_size, height, dst, plane, vsapi);
        srcp += row_size * height;
    }
}


static void VS_CC
write_nvxx_frame(rs_hnd_t *rh, VSFrameRef *dst, const VSAPI *vsapi)
{
    struct uv_t {
        uint8_t c[8];
    };

    uint8_t *srcp_orig = rh->frame_buff;

    int row_size = vsapi->getFrameWidth(dst, 0);
    int height = vsapi->getFrameHeight(dst, 0);
    rs_bit_blt(srcp_orig, row_size, height, dst, 0, vsapi);

    srcp_orig += row_size * height;
    int src_stride = row_size;
    row_size = (vsapi->getFrameWidth(dst, 1) + 3) >> 2;
    height = vsapi->getFrameHeight(dst, 1);

    int dst_stride = vsapi->getStride(dst, 1);
    uint8_t *dstp0_orig = vsapi->getWritePtr(dst, rh->order[1]);
    uint8_t *dstp1_orig = vsapi->getWritePtr(dst, rh->order[2]);

    for (int y = 0; y < height; y++) {
        struct uv_t *srcp = (struct uv_t *)(srcp_orig + y * src_stride);
        uint32_t *dstp0 = (uint32_t *)(dstp0_orig + y * dst_stride);
        uint32_t *dstp1 = (uint32_t *)(dstp1_orig + y * dst_stride);
        for (int x = 0; x < row_size; x++) {
            dstp0[x] = bitor8to32(srcp[x].c[6], srcp[x].c[4], srcp[x].c[2],
                                  srcp[x].c[0]);
            dstp1[x] = bitor8to32(srcp[x].c[7], srcp[x].c[5], srcp[x].c[3],
                                  srcp[x].c[1]);
        }
    }
}


static void VS_CC
write_px1x_frame(rs_hnd_t *rh, VSFrameRef *dst, const VSAPI *vsapi)
{
    struct uv16_t {
        uint16_t c[2];
    };

    uint8_t *srcp = rh->frame_buff;

    int row_size = vsapi->getFrameWidth(dst, 0) << 1;
    int height = vsapi->getFrameWidth(dst, 0);
    rs_bit_blt(srcp, row_size, height, dst, 0, vsapi);

    struct uv16_t *srcp_uv = (struct uv16_t *)(srcp + row_size * height);
    row_size = vsapi->getFrameWidth(dst, 1);
    height = vsapi->getFrameHeight(dst, 1);
    int stride = vsapi->getStride(dst, 1) >> 1;
    uint16_t *dstp0 = (uint16_t *)vsapi->getWritePtr(dst, rh->order[1]);
    uint16_t *dstp1 = (uint16_t *)vsapi->getWritePtr(dst, rh->order[2]);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < row_size; x++) {
            dstp0[x] = srcp_uv[x].c[0];
            dstp1[x] = srcp_uv[x].c[1];
        }
        srcp_uv += row_size;
        dstp0 += stride;
        dstp1 += stride;
    }
}


static void VS_CC
write_packed_rgb24(rs_hnd_t *rh, VSFrameRef *dst, const VSAPI *vsapi)
{
    struct rgb24_t {
        uint8_t c[12];
    };

    uint8_t *srcp_orig = rh->frame_buff;
    int row_size = (rh->vi.width + 3) >> 2;
    int height = rh->vi.height;
    int src_stride = rh->vi.width * 3;

    uint8_t *dstp0_orig = vsapi->getWritePtr(dst, rh->order[0]);
    uint8_t *dstp1_orig = vsapi->getWritePtr(dst, rh->order[1]);
    uint8_t *dstp2_orig = vsapi->getWritePtr(dst, rh->order[2]);
    int dst_stride = vsapi->getStride(dst, 0);

    for (int y = 0; y < height; y++) {
        struct rgb24_t *srcp = (struct rgb24_t *)(srcp_orig + y * src_stride);
        uint32_t *dstp0 = (uint32_t *)(dstp0_orig + y * dst_stride);
        uint32_t *dstp1 = (uint32_t *)(dstp1_orig + y * dst_stride);
        uint32_t *dstp2 = (uint32_t *)(dstp2_orig + y * dst_stride);
        for (int x = 0; x < row_size; x++) {
            dstp0[x] = bitor8to32(srcp[x].c[9], srcp[x].c[6],
                                  srcp[x].c[3], srcp[x].c[0]);
            dstp1[x] = bitor8to32(srcp[x].c[10], srcp[x].c[7],
                                  srcp[x].c[4], srcp[x].c[1]);
            dstp2[x] = bitor8to32(srcp[x].c[11], srcp[x].c[8],
                                  srcp[x].c[5], srcp[x].c[2]);
        }
    }
}


static void VS_CC
write_packed_rgb48(rs_hnd_t *rh, VSFrameRef *dst, const VSAPI *vsapi)
{
    struct rgb48_t {
        uint16_t c[3];
    };

    struct rgb48_t *srcp = (struct rgb48_t *)rh->frame_buff;
    int width = rh->vi.width;
    int height = rh->vi.height;

    uint16_t *dstp0 = (uint16_t *)vsapi->getWritePtr(dst, rh->order[0]);
    uint16_t *dstp1 = (uint16_t *)vsapi->getWritePtr(dst, rh->order[1]);
    uint16_t *dstp2 = (uint16_t *)vsapi->getWritePtr(dst, rh->order[2]);
    int stride = vsapi->getStride(dst, 0) >> 1;;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            dstp0[x] = srcp[x].c[0];
            dstp1[x] = srcp[x].c[1];
            dstp2[x] = srcp[x].c[2];
        }
        srcp += width;
        dstp0 += stride;
        dstp1 += stride;
        dstp2 += stride;
    }
}


static void VS_CC
write_packed_rgb32(rs_hnd_t *rh, VSFrameRef *dst, const VSAPI *vsapi)
{
    struct rgb32_t {
        uint8_t c[16];
    };

    uint8_t *srcp_orig = rh->frame_buff;
    int src_stride = rh->vi.width << 2;
    int row_size = (rh->vi.width + 3) >> 2;
    int height = rh->vi.height;

    int *order = rh->order;
    int offset = 0;
    if (order[0] == 9) {
        order++;
        offset++;
    }

    uint8_t *dstp0_orig = vsapi->getWritePtr(dst, order[0]);
    uint8_t *dstp1_orig = vsapi->getWritePtr(dst, order[1]);
    uint8_t *dstp2_orig = vsapi->getWritePtr(dst, order[2]);
    int dst_stride = vsapi->getStride(dst, 0);

    for (int y = 0; y < height; y++) {
        struct rgb32_t *srcp = (struct rgb32_t *)(srcp_orig + y * src_stride);
        uint32_t *dstp0 = (uint32_t *)(dstp0_orig + y * dst_stride);
        uint32_t *dstp1 = (uint32_t *)(dstp1_orig + y * dst_stride);
        uint32_t *dstp2 = (uint32_t *)(dstp2_orig + y * dst_stride);
        for (int x = 0; x < row_size; x++) {
            dstp0[x] = bitor8to32(srcp[x].c[offset + 12], srcp[x].c[offset + 8],
                                  srcp[x].c[offset + 4], srcp[x].c[offset]);
            dstp1[x] = bitor8to32(srcp[x].c[offset + 13], srcp[x].c[offset + 9],
                                  srcp[x].c[offset + 5], srcp[x].c[offset + 1]);
            dstp2[x] = bitor8to32(srcp[x].c[offset + 14], srcp[x].c[offset + 10],
                                  srcp[x].c[offset + 6], srcp[x].c[offset + 2]);
        }
    }
}


static void VS_CC
write_packed_yuv422(rs_hnd_t *rh, VSFrameRef *dst, const VSAPI *vsapi)
{
    struct packed422_t {
        uint8_t c[4];
    };

    struct packed422_t *srcp = (struct packed422_t *)rh->frame_buff;
    int width = rh->vi.width >> 1;
    int height = rh->vi.height;
    int o0 = rh->order[0];
    int o1 = rh->order[1];
    int o2 = rh->order[2];
    int o3 = rh->order[3];

    uint8_t *dstp[3];
    int padding[3];
    for (int i = 0; i < 3; i++) {
        dstp[i] = vsapi->getWritePtr(dst, i);
        padding[i] = vsapi->getStride(dst, i) - vsapi->getFrameWidth(dst, i);
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            *(dstp[o0]++) = srcp[x].c[0];
            *(dstp[o1]++) = srcp[x].c[1];
            *(dstp[o2]++) = srcp[x].c[2];
            *(dstp[o3]++) = srcp[x].c[3];
        }
        srcp += width;
        dstp[0] += padding[0];
        dstp[1] += padding[1];
        dstp[2] += padding[2];
    }
}


static int VS_CC create_index(rs_hnd_t *rh)
{
    int num_frames = rh->vi.numFrames;

    int64_t *index = (int64_t *)malloc(sizeof(int64_t) * num_frames);
    if (!index) {
        return -1;
    }

    int off_frame = rh->off_frame;
    uint32_t frame_size = rh->frame_size;
    int64_t pos = rh->off_header;
    for (int i = 0; i < num_frames; i++) {
        pos += off_frame;
        index[i] = pos;
        pos += frame_size;
    }

    rh->index = index;
    return 0;
}


static inline const char * VS_CC get_format(char *ctag)
{
    const struct {
        char *tag;
        char *format;
    } table[] = {
        { "420jpeg",  "YUV420P8"  },
        { "420mpeg2", "YUV420P8"  },
        { "420paldv", "YUV420P8"  },
        { "420p9",    "YUV420P9"  },
        { "420p10",   "YUV420P10" },
        { "420p16",   "YUV420P16" },
        { "411",      "YUV411P8"  },
        { "422",      "YUV422P8"  },
        { "422p9",    "YUV422P9"  },
        { "422p10",   "YUV422P10" },
        { "422p16",   "YUV422P16" },
        { "444",      "YUV444P8"  },
        { "444p9",    "YUV444P9"  },
        { "444p10",   "YUV444P10" },
        { "444p16",   "YUV444P16" },
        { "444alpha", "YUV444P8A" },
        { ctag,       "YUV420P8"  }
    };

    int i = 0;
    while (strcasecmp(ctag, table[i].tag) != 0) i++;
    return table[i].format;
}


#define PARSE_HEADER(cond, ...) {\
    i += 2;\
    sscanf(buff + i, __VA_ARGS__);\
    if (cond) {\
        return -1;\
    }\
}
static int VS_CC check_y4m(rs_hnd_t *rh)
{
    const char *stream_header = "YUV4MPEG2";
    const char *frame_header = "FRAME\n";
    size_t sh_length = strlen(stream_header);
    size_t fh_length = strlen(frame_header);
    char buff[256] = { 0 };
    char ctag[32] = { 0 };

    fread(buff, 1, sizeof buff, rh->file);
    if (strncmp(buff, stream_header, sh_length) != 0) {
        return 1;
    }

    int i;
    for (i = sh_length; buff[i] != '\n'; i++) {
        if (!strncmp(buff + i, " W", 2)) {
            PARSE_HEADER(rh->vi.width < 1, "%d", &rh->vi.width);
        }
        if (!strncmp(buff + i, " H", 2)) {
            PARSE_HEADER(rh->vi.height < 1, "%d", &rh->vi.height);
        }
        if (!strncmp(buff + i, " F", 2)) {
            i += 2;
            sscanf(buff + i, "%"SCNi64":%"SCNi64, &rh->vi.fpsNum, &rh->vi.fpsDen);
            if (rh->vi.fpsNum < 1 || rh->vi.fpsDen < 1) {
                return -1;
            }
        }
        if (!strncmp(buff + i, " I", 2)) {
            i += 2;
            if (buff[i] == 'm') {
                return -2;
            }
        }
        if (!strncmp(buff + i, " C", 2)) {
            i += 2;
            sscanf(buff + i, "%s", ctag);
            strcpy(rh->src_format, get_format(ctag));
        }
        if (i == sizeof buff - 1) {
            return -2;
        }
    }

    rh->off_header = ++i;

    if (strncmp(buff + i, frame_header, strlen(frame_header)) != 0) {
        return -2;
    }

    rh->off_frame = fh_length;

    if (strlen(rh->src_format) == 0) {
        strcpy(rh->src_format, "YUV420P8");
    }

    return 0;
}
#undef PARSE_HEADER


static const char * VS_CC check_args(rs_hnd_t *rh, vs_args_t *va)
{
    const struct {
        char *format_name;
        int subsample_h;
        int subsample_v;
        int bits_per_pix;
        int order[4];
        VSPresetFormat vsformat;
        write_frame func;
    } table[] = {
        { "i420",      2, 2, 12, { 0, 1, 2, 9 }, pfYUV420P8,  write_planar_frame  },
        { "IYUV",      2, 2, 12, { 0, 1, 2, 9 }, pfYUV420P8,  write_planar_frame  },
        { "YV12",      2, 2, 12, { 0, 2, 1, 9 }, pfYUV420P8,  write_planar_frame  },
        { "YUV420P8",  2, 2, 12, { 0, 1, 2, 9 }, pfYUV420P8,  write_planar_frame  },
        { "i422",      2, 1, 16, { 0, 1, 2, 9 }, pfYUV422P8,  write_planar_frame  },
        { "YV16",      2, 1, 16, { 0, 2, 1, 9 }, pfYUV422P8,  write_planar_frame  },
        { "YUV422P8",  2, 1, 16, { 0, 1, 2, 9 }, pfYUV422P8,  write_planar_frame  },
        { "i444",      1, 1, 24, { 0, 1, 2, 9 }, pfYUV444P8,  write_planar_frame  },
        { "YV24",      1, 1, 24, { 0, 2, 1, 9 }, pfYUV444P8,  write_planar_frame  },
        { "YUV444P8",  1, 1, 24, { 0, 1, 2, 9 }, pfYUV444P8,  write_planar_frame  },
        { "Y8",        1, 1,  8, { 0, 9, 9, 9 }, pfGray8,     write_planar_frame  },
        { "Y800",      1, 1,  8, { 0, 9, 9, 9 }, pfGray8,     write_planar_frame  },
        { "GRAY",      1, 1,  8, { 0, 9, 9, 9 }, pfGray8,     write_planar_frame  },
        { "GRAY16",    1, 1, 16, { 0, 9, 9, 9 }, pfGray16,    write_planar_frame  },
        { "YV411",     4, 1, 12, { 0, 2, 1, 9 }, pfYUV444P8,  write_planar_frame  },
        { "YUV411P8",  4, 1, 12, { 0, 1, 2, 9 }, pfYUV411P8,  write_planar_frame  },
        { "YUV9",      4, 4,  9, { 0, 1, 2, 9 }, pfYUV410P8,  write_planar_frame  },
        { "YVU9",      4, 4,  9, { 0, 2, 1, 9 }, pfYUV410P8,  write_planar_frame  },
        { "YUV410P8",  4, 4,  9, { 0, 1, 2, 9 }, pfYUV410P8,  write_planar_frame  },
        { "YUV440P8",  1, 2, 16, { 0, 1, 2, 9 }, pfYUV440P8,  write_planar_frame  },
        { "YUV420P9",  2, 2, 24, { 0, 1, 2, 9 }, pfYUV420P9,  write_planar_frame  },
        { "YUV420P10", 2, 2, 24, { 0, 1, 2, 9 }, pfYUV420P10, write_planar_frame  },
        { "YUV420P16", 2, 2, 24, { 0, 1, 2, 9 }, pfYUV420P16, write_planar_frame  },
        { "YUV422P9",  2, 2, 32, { 0, 1, 2, 9 }, pfYUV422P9,  write_planar_frame  },
        { "YUV422P10", 2, 2, 32, { 0, 1, 2, 9 }, pfYUV422P10, write_planar_frame  },
        { "YUV422P16", 2, 2, 32, { 0, 1, 2, 9 }, pfYUV422P16, write_planar_frame  },
        { "YUV444P9",  2, 2, 48, { 0, 1, 2, 9 }, pfYUV444P9,  write_planar_frame  },
        { "YUV444P10", 2, 2, 48, { 0, 1, 2, 9 }, pfYUV444P10, write_planar_frame  },
        { "YUV444P16", 2, 2, 48, { 0, 1, 2, 9 }, pfYUV444P16, write_planar_frame  },
        { "YUV444P8A", 1, 1, 32, { 0, 1, 2, 9 }, pfYUV444P8,  write_planar_frame  },
        { "YUY2",      2, 1, 16, { 0, 1, 0, 2 }, pfYUV422P8,  write_packed_yuv422 },
        { "YUYV",      2, 1, 16, { 0, 1, 0, 2 }, pfYUV422P8,  write_packed_yuv422 },
        { "UYVY",      2, 1, 16, { 1, 0, 2, 0 }, pfYUV422P8,  write_packed_yuv422 },
        { "YVYU",      2, 1, 16, { 0, 1, 0, 2 }, pfYUV422P8,  write_packed_yuv422 },
        { "VYUY",      2, 1, 16, { 2, 0, 1, 0 }, pfYUV422P8,  write_packed_yuv422 },
        { "BGR",       1, 1, 24, { 2, 1, 0, 9 }, pfRGB24,     write_packed_rgb24  },
        { "RGB",       1, 1, 24, { 0, 1, 2, 9 }, pfRGB24,     write_packed_rgb24  },
        { "BGRA",      1, 1, 32, { 2, 1, 0, 9 }, pfRGB24,     write_packed_rgb32  },
        { "ABGR",      1, 1, 32, { 9, 2, 1, 0 }, pfRGB24,     write_packed_rgb32  },
        { "RGBA",      1, 1, 32, { 0, 1, 2, 9 }, pfRGB24,     write_packed_rgb32  },
        { "ARGB",      1, 1, 32, { 9, 0, 1, 2 }, pfRGB24,     write_packed_rgb32  },
        { "AYUV",      1, 1, 32, { 9, 0, 1, 2 }, pfYUV444P8,  write_packed_rgb32  },
        { "GBRP8",     1, 1, 24, { 1, 2, 0, 9 }, pfRGB24,     write_planar_frame  },
        { "RGBP8",     1, 1, 24, { 0, 1, 2, 9 }, pfRGB24,     write_planar_frame  },
        { "GBRP9",     1, 1, 48, { 1, 2, 0, 9 }, pfRGB27,     write_planar_frame  },
        { "RGBP9",     1, 1, 48, { 0, 1, 2, 9 }, pfRGB27,     write_planar_frame  },
        { "GBRP10",    1, 1, 48, { 1, 2, 0, 9 }, pfRGB30,     write_planar_frame  },
        { "RGBP10",    1, 1, 48, { 0, 1, 2, 9 }, pfRGB30,     write_planar_frame  },
        { "GBRP16",    1, 1, 48, { 1, 2, 0, 9 }, pfRGB48,     write_planar_frame  },
        { "RGBP16",    1, 1, 48, { 0, 1, 2, 9 }, pfRGB48,     write_planar_frame  },
        { "BGR48",     1, 1, 48, { 2, 1, 0, 9 }, pfRGB48,     write_packed_rgb48  },
        { "RGB48",     1, 1, 48, { 0, 1, 2, 9 }, pfRGB48,     write_packed_rgb48  },
        { "NV12",      2, 2, 12, { 0, 1, 2, 9 }, pfYUV420P8,  write_nvxx_frame    },
        { "NV21",      2, 2, 12, { 0, 2, 1, 9 }, pfYUV420P8,  write_nvxx_frame    },
        { "P010",      2, 2, 24, { 0, 1, 2, 9 }, pfYUV420P16, write_px1x_frame    },
        { "P016",      2, 2, 24, { 0, 1, 2, 9 }, pfYUV420P16, write_px1x_frame    },
        { "P210",      2, 1, 32, { 0, 1, 2, 9 }, pfYUV422P16, write_px1x_frame    },
        { "P216",      2, 1, 32, { 0, 1, 2, 9 }, pfYUV422P16, write_px1x_frame    },
        { rh->src_format, 0 }
    };

    int i = 0;
    while (strcasecmp(rh->src_format, table[i].format_name) != 0) i++;
    if (table[i].vsformat == 0) {
        return "unsupported format";
    }
    if (rh->vi.width % table[i].subsample_h != 0) {
        return "invalid width was specified";
    }
    if (rh->vi.height % table[i].subsample_v != 0) {
        return "invalid height was specified";
    }

    rh->frame_size = (rh->vi.width * rh->vi.height * table[i].bits_per_pix) >> 3;
    rh->vi.format = va->vsapi->getFormatPreset(table[i].vsformat, va->core);
    memcpy(rh->order, table[i].order, sizeof(int) * 4);
    rh->write_frame = table[i].func;

    return NULL;
}


static void close_handler(rs_hnd_t *rh)
{
    if (!rh) {
        return;
    }
    if (rh->frame_buff) {
        rs_free(rh->frame_buff);
    }
    if (rh->index) {
        free(rh->index);
    }
    if (rh->file) {
        fclose(rh->file);
    }
    free(rh);
}


static void VS_CC
vs_close(void *instance_data, VSCore *core, const VSAPI *vsapi)
{
    rs_hnd_t *rh = (rs_hnd_t *)instance_data;
    close_handler(rh);
}


static void VS_CC
vs_init(VSMap *in, VSMap *out, void **instance_data, VSNode *node,
        VSCore *core, const VSAPI *vsapi)
{
    rs_hnd_t *rh = (rs_hnd_t *)*instance_data;
    vsapi->setVideoInfo(&rh->vi, node);
}


static const VSFrameRef * VS_CC
rs_get_frame(int n, int activation_reason, void **instance_data,
             void **frame_data, VSFrameContext *frame_ctx, VSCore *core,
             const VSAPI *vsapi)
{
    if (activation_reason != arInitial) {
        return NULL;
    }

    rs_hnd_t *rh = (rs_hnd_t *)*instance_data;

    int frame_number = n;
    if (n >= rh->vi.numFrames) {
        frame_number = rh->vi.numFrames - 1;
    }

    if (rs_fseek(rh->file, rh->index[frame_number], SEEK_SET) != 0 ||
        fread(rh->frame_buff, 1, rh->frame_size, rh->file) < rh->frame_size) {
        return NULL;
    }

    VSFrameRef *dst = vsapi->newVideoFrame(rh->vi.format, rh->vi.width,
                                           rh->vi.height, NULL, core);

    VSMap *props = vsapi->getFramePropsRW(dst);
    vsapi->propSetInt(props, "_DurationNum", rh->vi.fpsDen, 0);
    vsapi->propSetInt(props, "_DurationDen", rh->vi.fpsNum, 0);

    rh->write_frame(rh, dst, vsapi);

    return dst;
}


static void VS_CC
set_args_int(int *p, int default_value, const char *arg, vs_args_t *va)
{
    int err;
    *p = va->vsapi->propGetInt(va->in, arg, 0, &err);
    if (err) {
        *p = default_value;
    }
}


static void VS_CC
set_args_int64(int64_t *p, int default_value, const char *arg, vs_args_t *va)
{
    int err;
    *p = va->vsapi->propGetInt(va->in, arg, 0, &err);
    if (err) {
        *p = default_value;
    }
}


static void VS_CC
set_args_data(char *p, const char *default_value, const char *arg, size_t n,
              vs_args_t *va)
{
    int err;
    const char *data = va->vsapi->propGetData(va->in, arg, 0, &err);
    strncpy(p, err ? default_value : data, n);
}


#define RET_IF_ERROR(cond, ...) \
{\
    if (cond) {\
        close_handler(rh);\
        snprintf(msg, 240, __VA_ARGS__);\
        vsapi->setError(out, msg_buff);\
        return;\
    }\
}

static void VS_CC
create_source(const VSMap *in, VSMap *out, void *user_data, VSCore *core,
              const VSAPI *vsapi)
{
    char msg_buff[256] = "raws: ";
    char *msg = msg_buff + strlen(msg_buff);

    rs_hnd_t *rh = (rs_hnd_t *)calloc(sizeof(rs_hnd_t), 1);
    RET_IF_ERROR(!rh, "couldn't create handler");

    const char *src = vsapi->propGetData(in, "source", 0, 0);
    struct stat st;
    RET_IF_ERROR(stat(src, &st) != 0, "source does not exist.");

    rh->file_size = st.st_size;
    RET_IF_ERROR(rh->file_size == 0, "coudn't get source file size");

    rh->file = fopen(src, "rb");
    RET_IF_ERROR(!rh->file, "coudn't open %s", src);

    int y4m = check_y4m(rh);
    RET_IF_ERROR(y4m == -1, "invalid YUV4MPEG header was found");
    RET_IF_ERROR(y4m == -2, "unsupported YUV4MPEG header was found");

    vs_args_t va = { in, out, core, vsapi };

    if (y4m > 0) {
        set_args_int(&rh->vi.width, 720, "width", &va);
        set_args_int(&rh->vi.height, 480, "height", &va);
        set_args_int64(&rh->vi.fpsNum, 30000, "fpsnum", &va);
        set_args_int64(&rh->vi.fpsDen, 1001, "fpsden", &va);
        set_args_int(&rh->off_header, 0, "off_header", &va);
        set_args_int(&rh->off_frame, 0, "off_frame", &va);
        set_args_data(rh->src_format, "I420", "src_fmt", FORMAT_MAX_LEN, &va);
    }

    const char *ca = check_args(rh, &va);
    RET_IF_ERROR(ca, "%s", ca);

    rh->vi.numFrames =
        (rh->file_size - rh->off_header) / (rh->off_frame + rh->frame_size);
    RET_IF_ERROR(rh->vi.numFrames < 1, "too small file size");

    RET_IF_ERROR(create_index(rh), "failed to create index");

    rh->frame_buff = rs_malloc(rh->file_size + 32);
    RET_IF_ERROR(!rh->frame_buff, "failed to allocate buffer");

    const VSNodeRef *node =
        vsapi->createFilter(in, out, "Source", vs_init, rs_get_frame, vs_close,
                            fmSerial, 0, rh, core);

    vsapi->propSetNode(out, "clip", node, 0);
}
#undef RET_IF_ERROR


VS_EXTERNAL_API(void) VapourSynthPluginInit(
    VSConfigPlugin f_config, VSRegisterFunction f_register, VSPlugin *plugin)
{
    f_config("chikuzen.does.not.have.his.own.domain.raws", "raws",
             "Raw-format file Reader for VapourSynth " VS_RAWS_VERSION,
             VAPOURSYNTH_API_VERSION, 1, plugin);
    f_register("Source", "source:data;width:int:opt;height:int:opt;"
               "fpsnum:int:opt;fpsden:int:opt;src_fmt:data:opt;"
               "header_off:int:opt;frame_off:int:opt",
               create_source, NULL, plugin);
}
