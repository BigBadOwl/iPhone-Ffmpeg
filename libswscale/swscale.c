/*
 * Copyright (C) 2001-2003 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * the C code (not assembly, mmx, ...) of this file can be used
 * under the LGPL license too
 */

/*
  supported Input formats: YV12, I420/IYUV, YUY2, UYVY, BGR32, BGR32_1, BGR24, BGR16, BGR15, RGB32, RGB32_1, RGB24, Y8/Y800, YVU9/IF09, PAL8
  supported output formats: YV12, I420/IYUV, YUY2, UYVY, {BGR,RGB}{1,4,8,15,16,24,32}, Y8/Y800, YVU9/IF09
  {BGR,RGB}{1,4,8,15,16} support dithering

  unscaled special converters (YV12=I420=IYUV, Y800=Y8)
  YV12 -> {BGR,RGB}{1,4,8,15,16,24,32}
  x -> x
  YUV9 -> YV12
  YUV9/YV12 -> Y800
  Y800 -> YUV9/YV12
  BGR24 -> BGR32 & RGB24 -> RGB32
  BGR32 -> BGR24 & RGB32 -> RGB24
  BGR15 -> BGR16
*/

/*
tested special converters (most are tested actually, but I did not write it down ...)
 YV12 -> BGR16
 YV12 -> YV12
 BGR15 -> BGR16
 BGR16 -> BGR16
 YVU9 -> YV12

untested special converters
  YV12/I420 -> BGR15/BGR24/BGR32 (it is the yuv2rgb stuff, so it should be OK)
  YV12/I420 -> YV12/I420
  YUY2/BGR15/BGR24/BGR32/RGB24/RGB32 -> same format
  BGR24 -> BGR32 & RGB24 -> RGB32
  BGR32 -> BGR24 & RGB32 -> RGB24
  BGR24 -> YV12
*/

#define _SVID_SOURCE //needed for MAP_ANONYMOUS
#include <inttypes.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "config.h"
#include <assert.h>
#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#if defined(MAP_ANON) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif
#if HAVE_VIRTUALALLOC
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "swscale.h"
#include "swscale_internal.h"
#include "rgb2rgb.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/x86_cpu.h"
#include "libavutil/avutil.h"
#include "libavutil/bswap.h"
#include "libavutil/pixdesc.h"

unsigned swscale_version(void)
{
    return LIBSWSCALE_VERSION_INT;
}

const char *swscale_configuration(void)
{
    return FFMPEG_CONFIGURATION;
}

const char *swscale_license(void)
{
#define LICENSE_PREFIX "libswscale license: "
    return LICENSE_PREFIX FFMPEG_LICENSE + sizeof(LICENSE_PREFIX) - 1;
}

#undef MOVNTQ
#undef PAVGB

//#undef HAVE_MMX2
//#define HAVE_AMD3DNOW
//#undef HAVE_MMX
//#undef ARCH_X86
#define DITHER1XBPP

#define FAST_BGR2YV12 // use 7 bit coefficients instead of 15 bit

#define RET 0xC3 //near return opcode for x86

#ifdef M_PI
#define PI M_PI
#else
#define PI 3.14159265358979323846
#endif

#define isSupportedIn(x)    (       \
           (x)==PIX_FMT_YUV420P     \
        || (x)==PIX_FMT_YUVA420P    \
        || (x)==PIX_FMT_YUYV422     \
        || (x)==PIX_FMT_UYVY422     \
        || (x)==PIX_FMT_RGB48BE     \
        || (x)==PIX_FMT_RGB48LE     \
        || (x)==PIX_FMT_RGB32       \
        || (x)==PIX_FMT_RGB32_1     \
        || (x)==PIX_FMT_BGR24       \
        || (x)==PIX_FMT_BGR565      \
        || (x)==PIX_FMT_BGR555      \
        || (x)==PIX_FMT_BGR32       \
        || (x)==PIX_FMT_BGR32_1     \
        || (x)==PIX_FMT_RGB24       \
        || (x)==PIX_FMT_RGB565      \
        || (x)==PIX_FMT_RGB555      \
        || (x)==PIX_FMT_GRAY8       \
        || (x)==PIX_FMT_YUV410P     \
        || (x)==PIX_FMT_YUV440P     \
        || (x)==PIX_FMT_NV12        \
        || (x)==PIX_FMT_NV21        \
        || (x)==PIX_FMT_GRAY16BE    \
        || (x)==PIX_FMT_GRAY16LE    \
        || (x)==PIX_FMT_YUV444P     \
        || (x)==PIX_FMT_YUV422P     \
        || (x)==PIX_FMT_YUV411P     \
        || (x)==PIX_FMT_PAL8        \
        || (x)==PIX_FMT_BGR8        \
        || (x)==PIX_FMT_RGB8        \
        || (x)==PIX_FMT_BGR4_BYTE   \
        || (x)==PIX_FMT_RGB4_BYTE   \
        || (x)==PIX_FMT_YUV440P     \
        || (x)==PIX_FMT_MONOWHITE   \
        || (x)==PIX_FMT_MONOBLACK   \
        || (x)==PIX_FMT_YUV420P16LE   \
        || (x)==PIX_FMT_YUV422P16LE   \
        || (x)==PIX_FMT_YUV444P16LE   \
        || (x)==PIX_FMT_YUV420P16BE   \
        || (x)==PIX_FMT_YUV422P16BE   \
        || (x)==PIX_FMT_YUV444P16BE   \
    )

int sws_isSupportedInput(enum PixelFormat pix_fmt)
{
    return isSupportedIn(pix_fmt);
}

#define isSupportedOut(x)   (       \
           (x)==PIX_FMT_YUV420P     \
        || (x)==PIX_FMT_YUVA420P    \
        || (x)==PIX_FMT_YUYV422     \
        || (x)==PIX_FMT_UYVY422     \
        || (x)==PIX_FMT_YUV444P     \
        || (x)==PIX_FMT_YUV422P     \
        || (x)==PIX_FMT_YUV411P     \
        || isRGB(x)                 \
        || isBGR(x)                 \
        || (x)==PIX_FMT_NV12        \
        || (x)==PIX_FMT_NV21        \
        || (x)==PIX_FMT_GRAY16BE    \
        || (x)==PIX_FMT_GRAY16LE    \
        || (x)==PIX_FMT_GRAY8       \
        || (x)==PIX_FMT_YUV410P     \
        || (x)==PIX_FMT_YUV440P     \
        || (x)==PIX_FMT_YUV420P16LE   \
        || (x)==PIX_FMT_YUV422P16LE   \
        || (x)==PIX_FMT_YUV444P16LE   \
        || (x)==PIX_FMT_YUV420P16BE   \
        || (x)==PIX_FMT_YUV422P16BE   \
        || (x)==PIX_FMT_YUV444P16BE   \
    )

int sws_isSupportedOutput(enum PixelFormat pix_fmt)
{
    return isSupportedOut(pix_fmt);
}

#define isPacked(x)         (       \
           (x)==PIX_FMT_PAL8        \
        || (x)==PIX_FMT_YUYV422     \
        || (x)==PIX_FMT_UYVY422     \
        || isRGB(x)                 \
        || isBGR(x)                 \
    )
#define usePal(x) (av_pix_fmt_descriptors[x].flags & PIX_FMT_PAL)

#define RGB2YUV_SHIFT 15
#define BY ( (int)(0.114*219/255*(1<<RGB2YUV_SHIFT)+0.5))
#define BV (-(int)(0.081*224/255*(1<<RGB2YUV_SHIFT)+0.5))
#define BU ( (int)(0.500*224/255*(1<<RGB2YUV_SHIFT)+0.5))
#define GY ( (int)(0.587*219/255*(1<<RGB2YUV_SHIFT)+0.5))
#define GV (-(int)(0.419*224/255*(1<<RGB2YUV_SHIFT)+0.5))
#define GU (-(int)(0.331*224/255*(1<<RGB2YUV_SHIFT)+0.5))
#define RY ( (int)(0.299*219/255*(1<<RGB2YUV_SHIFT)+0.5))
#define RV ( (int)(0.500*224/255*(1<<RGB2YUV_SHIFT)+0.5))
#define RU (-(int)(0.169*224/255*(1<<RGB2YUV_SHIFT)+0.5))

extern const int32_t ff_yuv2rgb_coeffs[8][4];

static const double rgb2yuv_table[8][9]={
    {0.7152, 0.0722, 0.2126, -0.386, 0.5, -0.115, -0.454, -0.046, 0.5},
    {0.7152, 0.0722, 0.2126, -0.386, 0.5, -0.115, -0.454, -0.046, 0.5},
    {0.587 , 0.114 , 0.299 , -0.331, 0.5, -0.169, -0.419, -0.081, 0.5},
    {0.587 , 0.114 , 0.299 , -0.331, 0.5, -0.169, -0.419, -0.081, 0.5},
    {0.59  , 0.11  , 0.30  , -0.331, 0.5, -0.169, -0.421, -0.079, 0.5}, //FCC
    {0.587 , 0.114 , 0.299 , -0.331, 0.5, -0.169, -0.419, -0.081, 0.5},
    {0.587 , 0.114 , 0.299 , -0.331, 0.5, -0.169, -0.419, -0.081, 0.5}, //SMPTE 170M
    {0.701 , 0.087 , 0.212 , -0.384, 0.5  -0.116, -0.445, -0.055, 0.5}, //SMPTE 240M
};

/*
NOTES
Special versions: fast Y 1:1 scaling (no interpolation in y direction)

TODO
more intelligent misalignment avoidance for the horizontal scaler
write special vertical cubic upscale version
optimize C code (YV12 / minmax)
add support for packed pixel YUV input & output
add support for Y8 output
optimize BGR24 & BGR32
add BGR4 output support
write special BGR->BGR scaler
*/

#if ARCH_X86 && CONFIG_GPL
DECLARE_ASM_CONST(8, uint64_t, bF8)=       0xF8F8F8F8F8F8F8F8LL;
DECLARE_ASM_CONST(8, uint64_t, bFC)=       0xFCFCFCFCFCFCFCFCLL;
DECLARE_ASM_CONST(8, uint64_t, w10)=       0x0010001000100010LL;
DECLARE_ASM_CONST(8, uint64_t, w02)=       0x0002000200020002LL;
DECLARE_ASM_CONST(8, uint64_t, bm00001111)=0x00000000FFFFFFFFLL;
DECLARE_ASM_CONST(8, uint64_t, bm00000111)=0x0000000000FFFFFFLL;
DECLARE_ASM_CONST(8, uint64_t, bm11111000)=0xFFFFFFFFFF000000LL;
DECLARE_ASM_CONST(8, uint64_t, bm01010101)=0x00FF00FF00FF00FFLL;

const DECLARE_ALIGNED(8, uint64_t, ff_dither4[2]) = {
        0x0103010301030103LL,
        0x0200020002000200LL,};

const DECLARE_ALIGNED(8, uint64_t, ff_dither8[2]) = {
        0x0602060206020602LL,
        0x0004000400040004LL,};

DECLARE_ASM_CONST(8, uint64_t, b16Mask)=   0x001F001F001F001FLL;
DECLARE_ASM_CONST(8, uint64_t, g16Mask)=   0x07E007E007E007E0LL;
DECLARE_ASM_CONST(8, uint64_t, r16Mask)=   0xF800F800F800F800LL;
DECLARE_ASM_CONST(8, uint64_t, b15Mask)=   0x001F001F001F001FLL;
DECLARE_ASM_CONST(8, uint64_t, g15Mask)=   0x03E003E003E003E0LL;
DECLARE_ASM_CONST(8, uint64_t, r15Mask)=   0x7C007C007C007C00LL;

DECLARE_ALIGNED(8, const uint64_t, ff_M24A)         = 0x00FF0000FF0000FFLL;
DECLARE_ALIGNED(8, const uint64_t, ff_M24B)         = 0xFF0000FF0000FF00LL;
DECLARE_ALIGNED(8, const uint64_t, ff_M24C)         = 0x0000FF0000FF0000LL;

#ifdef FAST_BGR2YV12
DECLARE_ALIGNED(8, const uint64_t, ff_bgr2YCoeff)   = 0x000000210041000DULL;
DECLARE_ALIGNED(8, const uint64_t, ff_bgr2UCoeff)   = 0x0000FFEEFFDC0038ULL;
DECLARE_ALIGNED(8, const uint64_t, ff_bgr2VCoeff)   = 0x00000038FFD2FFF8ULL;
#else
DECLARE_ALIGNED(8, const uint64_t, ff_bgr2YCoeff)   = 0x000020E540830C8BULL;
DECLARE_ALIGNED(8, const uint64_t, ff_bgr2UCoeff)   = 0x0000ED0FDAC23831ULL;
DECLARE_ALIGNED(8, const uint64_t, ff_bgr2VCoeff)   = 0x00003831D0E6F6EAULL;
#endif /* FAST_BGR2YV12 */
DECLARE_ALIGNED(8, const uint64_t, ff_bgr2YOffset)  = 0x1010101010101010ULL;
DECLARE_ALIGNED(8, const uint64_t, ff_bgr2UVOffset) = 0x8080808080808080ULL;
DECLARE_ALIGNED(8, const uint64_t, ff_w1111)        = 0x0001000100010001ULL;

DECLARE_ASM_CONST(8, uint64_t, ff_bgr24toY1Coeff) = 0x0C88000040870C88ULL;
DECLARE_ASM_CONST(8, uint64_t, ff_bgr24toY2Coeff) = 0x20DE4087000020DEULL;
DECLARE_ASM_CONST(8, uint64_t, ff_rgb24toY1Coeff) = 0x20DE0000408720DEULL;
DECLARE_ASM_CONST(8, uint64_t, ff_rgb24toY2Coeff) = 0x0C88408700000C88ULL;
DECLARE_ASM_CONST(8, uint64_t, ff_bgr24toYOffset) = 0x0008400000084000ULL;

DECLARE_ASM_CONST(8, uint64_t, ff_bgr24toUV[2][4]) = {
    {0x38380000DAC83838ULL, 0xECFFDAC80000ECFFULL, 0xF6E40000D0E3F6E4ULL, 0x3838D0E300003838ULL},
    {0xECFF0000DAC8ECFFULL, 0x3838DAC800003838ULL, 0x38380000D0E33838ULL, 0xF6E4D0E30000F6E4ULL},
};

DECLARE_ASM_CONST(8, uint64_t, ff_bgr24toUVOffset)= 0x0040400000404000ULL;

#endif /* ARCH_X86 && CONFIG_GPL */

// clipping helper table for C implementations:
static unsigned char clip_table[768];

static SwsVector *sws_getConvVec(SwsVector *a, SwsVector *b);

DECLARE_ALIGNED(8, static const uint8_t, dither_2x2_4[2][8])={
{  1,   3,   1,   3,   1,   3,   1,   3, },
{  2,   0,   2,   0,   2,   0,   2,   0, },
};

DECLARE_ALIGNED(8, static const uint8_t, dither_2x2_8[2][8])={
{  6,   2,   6,   2,   6,   2,   6,   2, },
{  0,   4,   0,   4,   0,   4,   0,   4, },
};

DECLARE_ALIGNED(8, const uint8_t, dither_8x8_32[8][8])={
{ 17,   9,  23,  15,  16,   8,  22,  14, },
{  5,  29,   3,  27,   4,  28,   2,  26, },
{ 21,  13,  19,  11,  20,  12,  18,  10, },
{  0,  24,   6,  30,   1,  25,   7,  31, },
{ 16,   8,  22,  14,  17,   9,  23,  15, },
{  4,  28,   2,  26,   5,  29,   3,  27, },
{ 20,  12,  18,  10,  21,  13,  19,  11, },
{  1,  25,   7,  31,   0,  24,   6,  30, },
};

DECLARE_ALIGNED(8, const uint8_t, dither_8x8_73[8][8])={
{  0,  55,  14,  68,   3,  58,  17,  72, },
{ 37,  18,  50,  32,  40,  22,  54,  35, },
{  9,  64,   5,  59,  13,  67,   8,  63, },
{ 46,  27,  41,  23,  49,  31,  44,  26, },
{  2,  57,  16,  71,   1,  56,  15,  70, },
{ 39,  21,  52,  34,  38,  19,  51,  33, },
{ 11,  66,   7,  62,  10,  65,   6,  60, },
{ 48,  30,  43,  25,  47,  29,  42,  24, },
};

#if 1
DECLARE_ALIGNED(8, const uint8_t, dither_8x8_220[8][8])={
{117,  62, 158, 103, 113,  58, 155, 100, },
{ 34, 199,  21, 186,  31, 196,  17, 182, },
{144,  89, 131,  76, 141,  86, 127,  72, },
{  0, 165,  41, 206,  10, 175,  52, 217, },
{110,  55, 151,  96, 120,  65, 162, 107, },
{ 28, 193,  14, 179,  38, 203,  24, 189, },
{138,  83, 124,  69, 148,  93, 134,  79, },
{  7, 172,  48, 213,   3, 168,  45, 210, },
};
#elif 1
// tries to correct a gamma of 1.5
DECLARE_ALIGNED(8, const uint8_t, dither_8x8_220[8][8])={
{  0, 143,  18, 200,   2, 156,  25, 215, },
{ 78,  28, 125,  64,  89,  36, 138,  74, },
{ 10, 180,   3, 161,  16, 195,   8, 175, },
{109,  51,  93,  38, 121,  60, 105,  47, },
{  1, 152,  23, 210,   0, 147,  20, 205, },
{ 85,  33, 134,  71,  81,  30, 130,  67, },
{ 14, 190,   6, 171,  12, 185,   5, 166, },
{117,  57, 101,  44, 113,  54,  97,  41, },
};
#elif 1
// tries to correct a gamma of 2.0
DECLARE_ALIGNED(8, const uint8_t, dither_8x8_220[8][8])={
{  0, 124,   8, 193,   0, 140,  12, 213, },
{ 55,  14, 104,  42,  66,  19, 119,  52, },
{  3, 168,   1, 145,   6, 187,   3, 162, },
{ 86,  31,  70,  21,  99,  39,  82,  28, },
{  0, 134,  11, 206,   0, 129,   9, 200, },
{ 62,  17, 114,  48,  58,  16, 109,  45, },
{  5, 181,   2, 157,   4, 175,   1, 151, },
{ 95,  36,  78,  26,  90,  34,  74,  24, },
};
#else
// tries to correct a gamma of 2.5
DECLARE_ALIGNED(8, const uint8_t, dither_8x8_220[8][8])={
{  0, 107,   3, 187,   0, 125,   6, 212, },
{ 39,   7,  86,  28,  49,  11, 102,  36, },
{  1, 158,   0, 131,   3, 180,   1, 151, },
{ 68,  19,  52,  12,  81,  25,  64,  17, },
{  0, 119,   5, 203,   0, 113,   4, 195, },
{ 45,   9,  96,  33,  42,   8,  91,  30, },
{  2, 172,   1, 144,   2, 165,   0, 137, },
{ 77,  23,  60,  15,  72,  21,  56,  14, },
};
#endif

const char *sws_format_name(enum PixelFormat format)
{
    if ((unsigned)format < PIX_FMT_NB && av_pix_fmt_descriptors[format].name)
        return av_pix_fmt_descriptors[format].name;
    else
        return "Unknown format";
}

static av_always_inline void yuv2yuvX16inC_template(const int16_t *lumFilter, const int16_t **lumSrc, int lumFilterSize,
                                                    const int16_t *chrFilter, const int16_t **chrSrc, int chrFilterSize,
                                                    const int16_t **alpSrc, uint16_t *dest, uint16_t *uDest, uint16_t *vDest, uint16_t *aDest,
                                                    int dstW, int chrDstW, int big_endian)
{
    //FIXME Optimize (just quickly written not optimized..)
    int i;

    for (i = 0; i < dstW; i++) {
        int val = 1 << 10;
        int j;

        for (j = 0; j < lumFilterSize; j++)
            val += lumSrc[j][i] * lumFilter[j];

        if (big_endian) {
            AV_WB16(&dest[i], av_clip_uint16(val >> 11));
        } else {
            AV_WL16(&dest[i], av_clip_uint16(val >> 11));
        }
    }

    if (uDest) {
        for (i = 0; i < chrDstW; i++) {
            int u = 1 << 10;
            int v = 1 << 10;
            int j;

            for (j = 0; j < chrFilterSize; j++) {
                u += chrSrc[j][i       ] * chrFilter[j];
                v += chrSrc[j][i + VOFW] * chrFilter[j];
            }

            if (big_endian) {
                AV_WB16(&uDest[i], av_clip_uint16(u >> 11));
                AV_WB16(&vDest[i], av_clip_uint16(v >> 11));
            } else {
                AV_WL16(&uDest[i], av_clip_uint16(u >> 11));
                AV_WL16(&vDest[i], av_clip_uint16(v >> 11));
            }
        }
    }

    if (CONFIG_SWSCALE_ALPHA && aDest) {
        for (i = 0; i < dstW; i++) {
            int val = 1 << 10;
            int j;

            for (j = 0; j < lumFilterSize; j++)
                val += alpSrc[j][i] * lumFilter[j];

            if (big_endian) {
                AV_WB16(&aDest[i], av_clip_uint16(val >> 11));
            } else {
                AV_WL16(&aDest[i], av_clip_uint16(val >> 11));
            }
        }
    }
}

static inline void yuv2yuvX16inC(const int16_t *lumFilter, const int16_t **lumSrc, int lumFilterSize,
                                 const int16_t *chrFilter, const int16_t **chrSrc, int chrFilterSize,
                                 const int16_t **alpSrc, uint16_t *dest, uint16_t *uDest, uint16_t *vDest, uint16_t *aDest, int dstW, int chrDstW,
                                 enum PixelFormat dstFormat)
{
    if (isBE(dstFormat)) {
        yuv2yuvX16inC_template(lumFilter, lumSrc, lumFilterSize,
                               chrFilter, chrSrc, chrFilterSize,
                               alpSrc,
                               dest, uDest, vDest, aDest,
                               dstW, chrDstW, 1);
    } else {
        yuv2yuvX16inC_template(lumFilter, lumSrc, lumFilterSize,
                               chrFilter, chrSrc, chrFilterSize,
                               alpSrc,
                               dest, uDest, vDest, aDest,
                               dstW, chrDstW, 0);
    }
}

static inline void yuv2yuvXinC(const int16_t *lumFilter, const int16_t **lumSrc, int lumFilterSize,
                               const int16_t *chrFilter, const int16_t **chrSrc, int chrFilterSize,
                               const int16_t **alpSrc, uint8_t *dest, uint8_t *uDest, uint8_t *vDest, uint8_t *aDest, int dstW, int chrDstW)
{
    //FIXME Optimize (just quickly written not optimized..)
    int i;
    for (i=0; i<dstW; i++) {
        int val=1<<18;
        int j;
        for (j=0; j<lumFilterSize; j++)
            val += lumSrc[j][i] * lumFilter[j];

        dest[i]= av_clip_uint8(val>>19);
    }

    if (uDest)
        for (i=0; i<chrDstW; i++) {
            int u=1<<18;
            int v=1<<18;
            int j;
            for (j=0; j<chrFilterSize; j++) {
                u += chrSrc[j][i] * chrFilter[j];
                v += chrSrc[j][i + VOFW] * chrFilter[j];
            }

            uDest[i]= av_clip_uint8(u>>19);
            vDest[i]= av_clip_uint8(v>>19);
        }

    if (CONFIG_SWSCALE_ALPHA && aDest)
        for (i=0; i<dstW; i++) {
            int val=1<<18;
            int j;
            for (j=0; j<lumFilterSize; j++)
                val += alpSrc[j][i] * lumFilter[j];

            aDest[i]= av_clip_uint8(val>>19);
        }

}

static inline void yuv2nv12XinC(const int16_t *lumFilter, const int16_t **lumSrc, int lumFilterSize,
                                const int16_t *chrFilter, const int16_t **chrSrc, int chrFilterSize,
                                uint8_t *dest, uint8_t *uDest, int dstW, int chrDstW, int dstFormat)
{
    //FIXME Optimize (just quickly written not optimized..)
    int i;
    for (i=0; i<dstW; i++) {
        int val=1<<18;
        int j;
        for (j=0; j<lumFilterSize; j++)
            val += lumSrc[j][i] * lumFilter[j];

        dest[i]= av_clip_uint8(val>>19);
    }

    if (!uDest)
        return;

    if (dstFormat == PIX_FMT_NV12)
        for (i=0; i<chrDstW; i++) {
            int u=1<<18;
            int v=1<<18;
            int j;
            for (j=0; j<chrFilterSize; j++) {
                u += chrSrc[j][i] * chrFilter[j];
                v += chrSrc[j][i + VOFW] * chrFilter[j];
            }

            uDest[2*i]= av_clip_uint8(u>>19);
            uDest[2*i+1]= av_clip_uint8(v>>19);
        }
    else
        for (i=0; i<chrDstW; i++) {
            int u=1<<18;
            int v=1<<18;
            int j;
            for (j=0; j<chrFilterSize; j++) {
                u += chrSrc[j][i] * chrFilter[j];
                v += chrSrc[j][i + VOFW] * chrFilter[j];
            }

            uDest[2*i]= av_clip_uint8(v>>19);
            uDest[2*i+1]= av_clip_uint8(u>>19);
        }
}

#define YSCALE_YUV_2_PACKEDX_NOCLIP_C(type,alpha) \
    for (i=0; i<(dstW>>1); i++) {\
        int j;\
        int Y1 = 1<<18;\
        int Y2 = 1<<18;\
        int U  = 1<<18;\
        int V  = 1<<18;\
        int av_unused A1, A2;\
        type av_unused *r, *b, *g;\
        const int i2= 2*i;\
        \
        for (j=0; j<lumFilterSize; j++) {\
            Y1 += lumSrc[j][i2] * lumFilter[j];\
            Y2 += lumSrc[j][i2+1] * lumFilter[j];\
        }\
        for (j=0; j<chrFilterSize; j++) {\
            U += chrSrc[j][i] * chrFilter[j];\
            V += chrSrc[j][i+VOFW] * chrFilter[j];\
        }\
        Y1>>=19;\
        Y2>>=19;\
        U >>=19;\
        V >>=19;\
        if (alpha) {\
            A1 = 1<<18;\
            A2 = 1<<18;\
            for (j=0; j<lumFilterSize; j++) {\
                A1 += alpSrc[j][i2  ] * lumFilter[j];\
                A2 += alpSrc[j][i2+1] * lumFilter[j];\
            }\
            A1>>=19;\
            A2>>=19;\
        }\

#define YSCALE_YUV_2_PACKEDX_C(type,alpha) \
        YSCALE_YUV_2_PACKEDX_NOCLIP_C(type,alpha)\
        if ((Y1|Y2|U|V)&256) {\
            if (Y1>255)   Y1=255; \
            else if (Y1<0)Y1=0;   \
            if (Y2>255)   Y2=255; \
            else if (Y2<0)Y2=0;   \
            if (U>255)    U=255;  \
            else if (U<0) U=0;    \
            if (V>255)    V=255;  \
            else if (V<0) V=0;    \
        }\
        if (alpha && ((A1|A2)&256)) {\
            A1=av_clip_uint8(A1);\
            A2=av_clip_uint8(A2);\
        }

#define YSCALE_YUV_2_PACKEDX_FULL_C(rnd,alpha) \
    for (i=0; i<dstW; i++) {\
        int j;\
        int Y = 0;\
        int U = -128<<19;\
        int V = -128<<19;\
        int av_unused A;\
        int R,G,B;\
        \
        for (j=0; j<lumFilterSize; j++) {\
            Y += lumSrc[j][i     ] * lumFilter[j];\
        }\
        for (j=0; j<chrFilterSize; j++) {\
            U += chrSrc[j][i     ] * chrFilter[j];\
            V += chrSrc[j][i+VOFW] * chrFilter[j];\
        }\
        Y >>=10;\
        U >>=10;\
        V >>=10;\
        if (alpha) {\
            A = rnd;\
            for (j=0; j<lumFilterSize; j++)\
                A += alpSrc[j][i     ] * lumFilter[j];\
            A >>=19;\
            if (A&256)\
                A = av_clip_uint8(A);\
        }\

#define YSCALE_YUV_2_RGBX_FULL_C(rnd,alpha) \
    YSCALE_YUV_2_PACKEDX_FULL_C(rnd>>3,alpha)\
        Y-= c->yuv2rgb_y_offset;\
        Y*= c->yuv2rgb_y_coeff;\
        Y+= rnd;\
        R= Y + V*c->yuv2rgb_v2r_coeff;\
        G= Y + V*c->yuv2rgb_v2g_coeff + U*c->yuv2rgb_u2g_coeff;\
        B= Y +                          U*c->yuv2rgb_u2b_coeff;\
        if ((R|G|B)&(0xC0000000)) {\
            if (R>=(256<<22))   R=(256<<22)-1; \
            else if (R<0)R=0;   \
            if (G>=(256<<22))   G=(256<<22)-1; \
            else if (G<0)G=0;   \
            if (B>=(256<<22))   B=(256<<22)-1; \
            else if (B<0)B=0;   \
        }\


#define YSCALE_YUV_2_GRAY16_C \
    for (i=0; i<(dstW>>1); i++) {\
        int j;\
        int Y1 = 1<<18;\
        int Y2 = 1<<18;\
        int U  = 1<<18;\
        int V  = 1<<18;\
        \
        const int i2= 2*i;\
        \
        for (j=0; j<lumFilterSize; j++) {\
            Y1 += lumSrc[j][i2] * lumFilter[j];\
            Y2 += lumSrc[j][i2+1] * lumFilter[j];\
        }\
        Y1>>=11;\
        Y2>>=11;\
        if ((Y1|Y2|U|V)&65536) {\
            if (Y1>65535)   Y1=65535; \
            else if (Y1<0)Y1=0;   \
            if (Y2>65535)   Y2=65535; \
            else if (Y2<0)Y2=0;   \
        }

#define YSCALE_YUV_2_RGBX_C(type,alpha) \
    YSCALE_YUV_2_PACKEDX_C(type,alpha)  /* FIXME fix tables so that clipping is not needed and then use _NOCLIP*/\
    r = (type *)c->table_rV[V];   \
    g = (type *)(c->table_gU[U] + c->table_gV[V]); \
    b = (type *)c->table_bU[U];   \

#define YSCALE_YUV_2_PACKED2_C(type,alpha)   \
    for (i=0; i<(dstW>>1); i++) { \
        const int i2= 2*i;       \
        int Y1= (buf0[i2  ]*yalpha1+buf1[i2  ]*yalpha)>>19;           \
        int Y2= (buf0[i2+1]*yalpha1+buf1[i2+1]*yalpha)>>19;           \
        int U= (uvbuf0[i     ]*uvalpha1+uvbuf1[i     ]*uvalpha)>>19;  \
        int V= (uvbuf0[i+VOFW]*uvalpha1+uvbuf1[i+VOFW]*uvalpha)>>19;  \
        type av_unused *r, *b, *g;                                    \
        int av_unused A1, A2;                                         \
        if (alpha) {\
            A1= (abuf0[i2  ]*yalpha1+abuf1[i2  ]*yalpha)>>19;         \
            A2= (abuf0[i2+1]*yalpha1+abuf1[i2+1]*yalpha)>>19;         \
        }\

#define YSCALE_YUV_2_GRAY16_2_C   \
    for (i=0; i<(dstW>>1); i++) { \
        const int i2= 2*i;       \
        int Y1= (buf0[i2  ]*yalpha1+buf1[i2  ]*yalpha)>>11;           \
        int Y2= (buf0[i2+1]*yalpha1+buf1[i2+1]*yalpha)>>11;           \

#define YSCALE_YUV_2_RGB2_C(type,alpha) \
    YSCALE_YUV_2_PACKED2_C(type,alpha)\
    r = (type *)c->table_rV[V];\
    g = (type *)(c->table_gU[U] + c->table_gV[V]);\
    b = (type *)c->table_bU[U];\

#define YSCALE_YUV_2_PACKED1_C(type,alpha) \
    for (i=0; i<(dstW>>1); i++) {\
        const int i2= 2*i;\
        int Y1= buf0[i2  ]>>7;\
        int Y2= buf0[i2+1]>>7;\
        int U= (uvbuf1[i     ])>>7;\
        int V= (uvbuf1[i+VOFW])>>7;\
        type av_unused *r, *b, *g;\
        int av_unused A1, A2;\
        if (alpha) {\
            A1= abuf0[i2  ]>>7;\
            A2= abuf0[i2+1]>>7;\
        }\

#define YSCALE_YUV_2_GRAY16_1_C \
    for (i=0; i<(dstW>>1); i++) {\
        const int i2= 2*i;\
        int Y1= buf0[i2  ]<<1;\
        int Y2= buf0[i2+1]<<1;\

#define YSCALE_YUV_2_RGB1_C(type,alpha) \
    YSCALE_YUV_2_PACKED1_C(type,alpha)\
    r = (type *)c->table_rV[V];\
    g = (type *)(c->table_gU[U] + c->table_gV[V]);\
    b = (type *)c->table_bU[U];\

#define YSCALE_YUV_2_PACKED1B_C(type,alpha) \
    for (i=0; i<(dstW>>1); i++) {\
        const int i2= 2*i;\
        int Y1= buf0[i2  ]>>7;\
        int Y2= buf0[i2+1]>>7;\
        int U= (uvbuf0[i     ] + uvbuf1[i     ])>>8;\
        int V= (uvbuf0[i+VOFW] + uvbuf1[i+VOFW])>>8;\
        type av_unused *r, *b, *g;\
        int av_unused A1, A2;\
        if (alpha) {\
            A1= abuf0[i2  ]>>7;\
            A2= abuf0[i2+1]>>7;\
        }\

#define YSCALE_YUV_2_RGB1B_C(type,alpha) \
    YSCALE_YUV_2_PACKED1B_C(type,alpha)\
    r = (type *)c->table_rV[V];\
    g = (type *)(c->table_gU[U] + c->table_gV[V]);\
    b = (type *)c->table_bU[U];\

#define YSCALE_YUV_2_MONO2_C \
    const uint8_t * const d128=dither_8x8_220[y&7];\
    uint8_t *g= c->table_gU[128] + c->table_gV[128];\
    for (i=0; i<dstW-7; i+=8) {\
        int acc;\
        acc =       g[((buf0[i  ]*yalpha1+buf1[i  ]*yalpha)>>19) + d128[0]];\
        acc+= acc + g[((buf0[i+1]*yalpha1+buf1[i+1]*yalpha)>>19) + d128[1]];\
        acc+= acc + g[((buf0[i+2]*yalpha1+buf1[i+2]*yalpha)>>19) + d128[2]];\
        acc+= acc + g[((buf0[i+3]*yalpha1+buf1[i+3]*yalpha)>>19) + d128[3]];\
        acc+= acc + g[((buf0[i+4]*yalpha1+buf1[i+4]*yalpha)>>19) + d128[4]];\
        acc+= acc + g[((buf0[i+5]*yalpha1+buf1[i+5]*yalpha)>>19) + d128[5]];\
        acc+= acc + g[((buf0[i+6]*yalpha1+buf1[i+6]*yalpha)>>19) + d128[6]];\
        acc+= acc + g[((buf0[i+7]*yalpha1+buf1[i+7]*yalpha)>>19) + d128[7]];\
        ((uint8_t*)dest)[0]= c->dstFormat == PIX_FMT_MONOBLACK ? acc : ~acc;\
        dest++;\
    }\


#define YSCALE_YUV_2_MONOX_C \
    const uint8_t * const d128=dither_8x8_220[y&7];\
    uint8_t *g= c->table_gU[128] + c->table_gV[128];\
    int acc=0;\
    for (i=0; i<dstW-1; i+=2) {\
        int j;\
        int Y1=1<<18;\
        int Y2=1<<18;\
\
        for (j=0; j<lumFilterSize; j++) {\
            Y1 += lumSrc[j][i] * lumFilter[j];\
            Y2 += lumSrc[j][i+1] * lumFilter[j];\
        }\
        Y1>>=19;\
        Y2>>=19;\
        if ((Y1|Y2)&256) {\
            if (Y1>255)   Y1=255;\
            else if (Y1<0)Y1=0;\
            if (Y2>255)   Y2=255;\
            else if (Y2<0)Y2=0;\
        }\
        acc+= acc + g[Y1+d128[(i+0)&7]];\
        acc+= acc + g[Y2+d128[(i+1)&7]];\
        if ((i&7)==6) {\
            ((uint8_t*)dest)[0]= c->dstFormat == PIX_FMT_MONOBLACK ? acc : ~acc;\
            dest++;\
        }\
    }


#define YSCALE_YUV_2_ANYRGB_C(func, func2, func_g16, func_monoblack)\
    switch(c->dstFormat) {\
    case PIX_FMT_RGB48BE:\
    case PIX_FMT_RGB48LE:\
        func(uint8_t,0)\
            ((uint8_t*)dest)[ 0]= r[Y1];\
            ((uint8_t*)dest)[ 1]= r[Y1];\
            ((uint8_t*)dest)[ 2]= g[Y1];\
            ((uint8_t*)dest)[ 3]= g[Y1];\
            ((uint8_t*)dest)[ 4]= b[Y1];\
            ((uint8_t*)dest)[ 5]= b[Y1];\
            ((uint8_t*)dest)[ 6]= r[Y2];\
            ((uint8_t*)dest)[ 7]= r[Y2];\
            ((uint8_t*)dest)[ 8]= g[Y2];\
            ((uint8_t*)dest)[ 9]= g[Y2];\
            ((uint8_t*)dest)[10]= b[Y2];\
            ((uint8_t*)dest)[11]= b[Y2];\
            dest+=12;\
        }\
        break;\
    case PIX_FMT_RGBA:\
    case PIX_FMT_BGRA:\
        if (CONFIG_SMALL) {\
            int needAlpha = CONFIG_SWSCALE_ALPHA && c->alpPixBuf;\
            func(uint32_t,needAlpha)\
                ((uint32_t*)dest)[i2+0]= r[Y1] + g[Y1] + b[Y1] + (needAlpha ? (A1<<24) : 0);\
                ((uint32_t*)dest)[i2+1]= r[Y2] + g[Y2] + b[Y2] + (needAlpha ? (A2<<24) : 0);\
            }\
        } else {\
            if (CONFIG_SWSCALE_ALPHA && c->alpPixBuf) {\
                func(uint32_t,1)\
                    ((uint32_t*)dest)[i2+0]= r[Y1] + g[Y1] + b[Y1] + (A1<<24);\
                    ((uint32_t*)dest)[i2+1]= r[Y2] + g[Y2] + b[Y2] + (A2<<24);\
                }\
            } else {\
                func(uint32_t,0)\
                    ((uint32_t*)dest)[i2+0]= r[Y1] + g[Y1] + b[Y1];\
                    ((uint32_t*)dest)[i2+1]= r[Y2] + g[Y2] + b[Y2];\
                }\
            }\
        }\
        break;\
    case PIX_FMT_ARGB:\
    case PIX_FMT_ABGR:\
        if (CONFIG_SMALL) {\
            int needAlpha = CONFIG_SWSCALE_ALPHA && c->alpPixBuf;\
            func(uint32_t,needAlpha)\
                ((uint32_t*)dest)[i2+0]= r[Y1] + g[Y1] + b[Y1] + (needAlpha ? A1 : 0);\
                ((uint32_t*)dest)[i2+1]= r[Y2] + g[Y2] + b[Y2] + (needAlpha ? A2 : 0);\
            }\
        } else {\
            if (CONFIG_SWSCALE_ALPHA && c->alpPixBuf) {\
                func(uint32_t,1)\
                    ((uint32_t*)dest)[i2+0]= r[Y1] + g[Y1] + b[Y1] + A1;\
                    ((uint32_t*)dest)[i2+1]= r[Y2] + g[Y2] + b[Y2] + A2;\
                }\
            } else {\
                func(uint32_t,0)\
                    ((uint32_t*)dest)[i2+0]= r[Y1] + g[Y1] + b[Y1];\
                    ((uint32_t*)dest)[i2+1]= r[Y2] + g[Y2] + b[Y2];\
                }\
            }\
        }                \
        break;\
    case PIX_FMT_RGB24:\
        func(uint8_t,0)\
            ((uint8_t*)dest)[0]= r[Y1];\
            ((uint8_t*)dest)[1]= g[Y1];\
            ((uint8_t*)dest)[2]= b[Y1];\
            ((uint8_t*)dest)[3]= r[Y2];\
            ((uint8_t*)dest)[4]= g[Y2];\
            ((uint8_t*)dest)[5]= b[Y2];\
            dest+=6;\
        }\
        break;\
    case PIX_FMT_BGR24:\
        func(uint8_t,0)\
            ((uint8_t*)dest)[0]= b[Y1];\
            ((uint8_t*)dest)[1]= g[Y1];\
            ((uint8_t*)dest)[2]= r[Y1];\
            ((uint8_t*)dest)[3]= b[Y2];\
            ((uint8_t*)dest)[4]= g[Y2];\
            ((uint8_t*)dest)[5]= r[Y2];\
            dest+=6;\
        }\
        break;\
    case PIX_FMT_RGB565:\
    case PIX_FMT_BGR565:\
        {\
            const int dr1= dither_2x2_8[y&1    ][0];\
            const int dg1= dither_2x2_4[y&1    ][0];\
            const int db1= dither_2x2_8[(y&1)^1][0];\
            const int dr2= dither_2x2_8[y&1    ][1];\
            const int dg2= dither_2x2_4[y&1    ][1];\
            const int db2= dither_2x2_8[(y&1)^1][1];\
            func(uint16_t,0)\
                ((uint16_t*)dest)[i2+0]= r[Y1+dr1] + g[Y1+dg1] + b[Y1+db1];\
                ((uint16_t*)dest)[i2+1]= r[Y2+dr2] + g[Y2+dg2] + b[Y2+db2];\
            }\
        }\
        break;\
    case PIX_FMT_RGB555:\
    case PIX_FMT_BGR555:\
        {\
            const int dr1= dither_2x2_8[y&1    ][0];\
            const int dg1= dither_2x2_8[y&1    ][1];\
            const int db1= dither_2x2_8[(y&1)^1][0];\
            const int dr2= dither_2x2_8[y&1    ][1];\
            const int dg2= dither_2x2_8[y&1    ][0];\
            const int db2= dither_2x2_8[(y&1)^1][1];\
            func(uint16_t,0)\
                ((uint16_t*)dest)[i2+0]= r[Y1+dr1] + g[Y1+dg1] + b[Y1+db1];\
                ((uint16_t*)dest)[i2+1]= r[Y2+dr2] + g[Y2+dg2] + b[Y2+db2];\
            }\
        }\
        break;\
    case PIX_FMT_RGB8:\
    case PIX_FMT_BGR8:\
        {\
            const uint8_t * const d64= dither_8x8_73[y&7];\
            const uint8_t * const d32= dither_8x8_32[y&7];\
            func(uint8_t,0)\
                ((uint8_t*)dest)[i2+0]= r[Y1+d32[(i2+0)&7]] + g[Y1+d32[(i2+0)&7]] + b[Y1+d64[(i2+0)&7]];\
                ((uint8_t*)dest)[i2+1]= r[Y2+d32[(i2+1)&7]] + g[Y2+d32[(i2+1)&7]] + b[Y2+d64[(i2+1)&7]];\
            }\
        }\
        break;\
    case PIX_FMT_RGB4:\
    case PIX_FMT_BGR4:\
        {\
            const uint8_t * const d64= dither_8x8_73 [y&7];\
            const uint8_t * const d128=dither_8x8_220[y&7];\
            func(uint8_t,0)\
                ((uint8_t*)dest)[i]= r[Y1+d128[(i2+0)&7]] + g[Y1+d64[(i2+0)&7]] + b[Y1+d128[(i2+0)&7]]\
                                 + ((r[Y2+d128[(i2+1)&7]] + g[Y2+d64[(i2+1)&7]] + b[Y2+d128[(i2+1)&7]])<<4);\
            }\
        }\
        break;\
    case PIX_FMT_RGB4_BYTE:\
    case PIX_FMT_BGR4_BYTE:\
        {\
            const uint8_t * const d64= dither_8x8_73 [y&7];\
            const uint8_t * const d128=dither_8x8_220[y&7];\
            func(uint8_t,0)\
                ((uint8_t*)dest)[i2+0]= r[Y1+d128[(i2+0)&7]] + g[Y1+d64[(i2+0)&7]] + b[Y1+d128[(i2+0)&7]];\
                ((uint8_t*)dest)[i2+1]= r[Y2+d128[(i2+1)&7]] + g[Y2+d64[(i2+1)&7]] + b[Y2+d128[(i2+1)&7]];\
            }\
        }\
        break;\
    case PIX_FMT_MONOBLACK:\
    case PIX_FMT_MONOWHITE:\
        {\
            func_monoblack\
        }\
        break;\
    case PIX_FMT_YUYV422:\
        func2\
            ((uint8_t*)dest)[2*i2+0]= Y1;\
            ((uint8_t*)dest)[2*i2+1]= U;\
            ((uint8_t*)dest)[2*i2+2]= Y2;\
            ((uint8_t*)dest)[2*i2+3]= V;\
        }                \
        break;\
    case PIX_FMT_UYVY422:\
        func2\
            ((uint8_t*)dest)[2*i2+0]= U;\
            ((uint8_t*)dest)[2*i2+1]= Y1;\
            ((uint8_t*)dest)[2*i2+2]= V;\
            ((uint8_t*)dest)[2*i2+3]= Y2;\
        }                \
        break;\
    case PIX_FMT_GRAY16BE:\
        func_g16\
            ((uint8_t*)dest)[2*i2+0]= Y1>>8;\
            ((uint8_t*)dest)[2*i2+1]= Y1;\
            ((uint8_t*)dest)[2*i2+2]= Y2>>8;\
            ((uint8_t*)dest)[2*i2+3]= Y2;\
        }                \
        break;\
    case PIX_FMT_GRAY16LE:\
        func_g16\
            ((uint8_t*)dest)[2*i2+0]= Y1;\
            ((uint8_t*)dest)[2*i2+1]= Y1>>8;\
            ((uint8_t*)dest)[2*i2+2]= Y2;\
            ((uint8_t*)dest)[2*i2+3]= Y2>>8;\
        }                \
        break;\
    }\


static inline void yuv2packedXinC(SwsContext *c, const int16_t *lumFilter, const int16_t **lumSrc, int lumFilterSize,
                                  const int16_t *chrFilter, const int16_t **chrSrc, int chrFilterSize,
                                  const int16_t **alpSrc, uint8_t *dest, int dstW, int y)
{
    int i;
    YSCALE_YUV_2_ANYRGB_C(YSCALE_YUV_2_RGBX_C, YSCALE_YUV_2_PACKEDX_C(void,0), YSCALE_YUV_2_GRAY16_C, YSCALE_YUV_2_MONOX_C)
}

static inline void yuv2rgbXinC_full(SwsContext *c, const int16_t *lumFilter, const int16_t **lumSrc, int lumFilterSize,
                                    const int16_t *chrFilter, const int16_t **chrSrc, int chrFilterSize,
                                    const int16_t **alpSrc, uint8_t *dest, int dstW, int y)
{
    int i;
    int step= fmt_depth(c->dstFormat)/8;
    int aidx= 3;

    switch(c->dstFormat) {
    case PIX_FMT_ARGB:
        dest++;
        aidx= 0;
    case PIX_FMT_RGB24:
        aidx--;
    case PIX_FMT_RGBA:
        if (CONFIG_SMALL) {
            int needAlpha = CONFIG_SWSCALE_ALPHA && c->alpPixBuf;
            YSCALE_YUV_2_RGBX_FULL_C(1<<21, needAlpha)
                dest[aidx]= needAlpha ? A : 255;
                dest[0]= R>>22;
                dest[1]= G>>22;
                dest[2]= B>>22;
                dest+= step;
            }
        } else {
            if (CONFIG_SWSCALE_ALPHA && c->alpPixBuf) {
                YSCALE_YUV_2_RGBX_FULL_C(1<<21, 1)
                    dest[aidx]= A;
                    dest[0]= R>>22;
                    dest[1]= G>>22;
                    dest[2]= B>>22;
                    dest+= step;
                }
            } else {
                YSCALE_YUV_2_RGBX_FULL_C(1<<21, 0)
                    dest[aidx]= 255;
                    dest[0]= R>>22;
                    dest[1]= G>>22;
                    dest[2]= B>>22;
                    dest+= step;
                }
            }
        }
        break;
    case PIX_FMT_ABGR:
        dest++;
        aidx= 0;
    case PIX_FMT_BGR24:
        aidx--;
    case PIX_FMT_BGRA:
        if (CONFIG_SMALL) {
            int needAlpha = CONFIG_SWSCALE_ALPHA && c->alpPixBuf;
            YSCALE_YUV_2_RGBX_FULL_C(1<<21, needAlpha)
                dest[aidx]= needAlpha ? A : 255;
                dest[0]= B>>22;
                dest[1]= G>>22;
                dest[2]= R>>22;
                dest+= step;
            }
        } else {
            if (CONFIG_SWSCALE_ALPHA && c->alpPixBuf) {
                YSCALE_YUV_2_RGBX_FULL_C(1<<21, 1)
                    dest[aidx]= A;
                    dest[0]= B>>22;
                    dest[1]= G>>22;
                    dest[2]= R>>22;
                    dest+= step;
                }
            } else {
                YSCALE_YUV_2_RGBX_FULL_C(1<<21, 0)
                    dest[aidx]= 255;
                    dest[0]= B>>22;
                    dest[1]= G>>22;
                    dest[2]= R>>22;
                    dest+= step;
                }
            }
        }
        break;
    default:
        assert(0);
    }
}

static void fillPlane(uint8_t* plane, int stride, int width, int height, int y, uint8_t val)
{
    int i;
    uint8_t *ptr = plane + stride*y;
    for (i=0; i<height; i++) {
        memset(ptr, val, width);
        ptr += stride;
    }
}

static inline void rgb48ToY(uint8_t *dst, const uint8_t *src, int width)
{
    int i;
    for (i = 0; i < width; i++) {
        int r = src[i*6+0];
        int g = src[i*6+2];
        int b = src[i*6+4];

        dst[i] = (RY*r + GY*g + BY*b + (33<<(RGB2YUV_SHIFT-1))) >> RGB2YUV_SHIFT;
    }
}

static inline void rgb48ToUV(uint8_t *dstU, uint8_t *dstV,
                             uint8_t *src1, uint8_t *src2, int width)
{
    int i;
    assert(src1==src2);
    for (i = 0; i < width; i++) {
        int r = src1[6*i + 0];
        int g = src1[6*i + 2];
        int b = src1[6*i + 4];

        dstU[i] = (RU*r + GU*g + BU*b + (257<<(RGB2YUV_SHIFT-1))) >> RGB2YUV_SHIFT;
        dstV[i] = (RV*r + GV*g + BV*b + (257<<(RGB2YUV_SHIFT-1))) >> RGB2YUV_SHIFT;
    }
}

static inline void rgb48ToUV_half(uint8_t *dstU, uint8_t *dstV,
                                  uint8_t *src1, uint8_t *src2, int width)
{
    int i;
    assert(src1==src2);
    for (i = 0; i < width; i++) {
        int r= src1[12*i + 0] + src1[12*i + 6];
        int g= src1[12*i + 2] + src1[12*i + 8];
        int b= src1[12*i + 4] + src1[12*i + 10];

        dstU[i]= (RU*r + GU*g + BU*b + (257<<RGB2YUV_SHIFT)) >> (RGB2YUV_SHIFT+1);
        dstV[i]= (RV*r + GV*g + BV*b + (257<<RGB2YUV_SHIFT)) >> (RGB2YUV_SHIFT+1);
    }
}

#define BGR2Y(type, name, shr, shg, shb, maskr, maskg, maskb, RY, GY, BY, S)\
static inline void name(uint8_t *dst, const uint8_t *src, long width, uint32_t *unused)\
{\
    int i;\
    for (i=0; i<width; i++) {\
        int b= (((const type*)src)[i]>>shb)&maskb;\
        int g= (((const type*)src)[i]>>shg)&maskg;\
        int r= (((const type*)src)[i]>>shr)&maskr;\
\
        dst[i]= (((RY)*r + (GY)*g + (BY)*b + (33<<((S)-1)))>>(S));\
    }\
}

BGR2Y(uint32_t, bgr32ToY,16, 0, 0, 0x00FF, 0xFF00, 0x00FF, RY<< 8, GY   , BY<< 8, RGB2YUV_SHIFT+8)
BGR2Y(uint32_t, rgb32ToY, 0, 0,16, 0x00FF, 0xFF00, 0x00FF, RY<< 8, GY   , BY<< 8, RGB2YUV_SHIFT+8)
BGR2Y(uint16_t, bgr16ToY, 0, 0, 0, 0x001F, 0x07E0, 0xF800, RY<<11, GY<<5, BY    , RGB2YUV_SHIFT+8)
BGR2Y(uint16_t, bgr15ToY, 0, 0, 0, 0x001F, 0x03E0, 0x7C00, RY<<10, GY<<5, BY    , RGB2YUV_SHIFT+7)
BGR2Y(uint16_t, rgb16ToY, 0, 0, 0, 0xF800, 0x07E0, 0x001F, RY    , GY<<5, BY<<11, RGB2YUV_SHIFT+8)
BGR2Y(uint16_t, rgb15ToY, 0, 0, 0, 0x7C00, 0x03E0, 0x001F, RY    , GY<<5, BY<<10, RGB2YUV_SHIFT+7)

static inline void abgrToA(uint8_t *dst, const uint8_t *src, long width, uint32_t *unused)
{
    int i;
    for (i=0; i<width; i++) {
        dst[i]= src[4*i];
    }
}

#define BGR2UV(type, name, shr, shg, shb, maska, maskr, maskg, maskb, RU, GU, BU, RV, GV, BV, S)\
static inline void name(uint8_t *dstU, uint8_t *dstV, const uint8_t *src, const uint8_t *dummy, long width, uint32_t *unused)\
{\
    int i;\
    for (i=0; i<width; i++) {\
        int b= (((const type*)src)[i]&maskb)>>shb;\
        int g= (((const type*)src)[i]&maskg)>>shg;\
        int r= (((const type*)src)[i]&maskr)>>shr;\
\
        dstU[i]= ((RU)*r + (GU)*g + (BU)*b + (257<<((S)-1)))>>(S);\
        dstV[i]= ((RV)*r + (GV)*g + (BV)*b + (257<<((S)-1)))>>(S);\
    }\
}\
static inline void name ## _half(uint8_t *dstU, uint8_t *dstV, const uint8_t *src, const uint8_t *dummy, long width, uint32_t *unused)\
{\
    int i;\
    for (i=0; i<width; i++) {\
        int pix0= ((const type*)src)[2*i+0];\
        int pix1= ((const type*)src)[2*i+1];\
        int g= (pix0&~(maskr|maskb))+(pix1&~(maskr|maskb));\
        int b= ((pix0+pix1-g)&(maskb|(2*maskb)))>>shb;\
        int r= ((pix0+pix1-g)&(maskr|(2*maskr)))>>shr;\
        g&= maskg|(2*maskg);\
\
        g>>=shg;\
\
        dstU[i]= ((RU)*r + (GU)*g + (BU)*b + (257<<(S)))>>((S)+1);\
        dstV[i]= ((RV)*r + (GV)*g + (BV)*b + (257<<(S)))>>((S)+1);\
    }\
}

BGR2UV(uint32_t, bgr32ToUV,16, 0, 0, 0xFF000000, 0xFF0000, 0xFF00,   0x00FF, RU<< 8, GU   , BU<< 8, RV<< 8, GV   , BV<< 8, RGB2YUV_SHIFT+8)
BGR2UV(uint32_t, rgb32ToUV, 0, 0,16, 0xFF000000,   0x00FF, 0xFF00, 0xFF0000, RU<< 8, GU   , BU<< 8, RV<< 8, GV   , BV<< 8, RGB2YUV_SHIFT+8)
BGR2UV(uint16_t, bgr16ToUV, 0, 0, 0,          0,   0x001F, 0x07E0,   0xF800, RU<<11, GU<<5, BU    , RV<<11, GV<<5, BV    , RGB2YUV_SHIFT+8)
BGR2UV(uint16_t, bgr15ToUV, 0, 0, 0,          0,   0x001F, 0x03E0,   0x7C00, RU<<10, GU<<5, BU    , RV<<10, GV<<5, BV    , RGB2YUV_SHIFT+7)
BGR2UV(uint16_t, rgb16ToUV, 0, 0, 0,          0,   0xF800, 0x07E0,   0x001F, RU    , GU<<5, BU<<11, RV    , GV<<5, BV<<11, RGB2YUV_SHIFT+8)
BGR2UV(uint16_t, rgb15ToUV, 0, 0, 0,          0,   0x7C00, 0x03E0,   0x001F, RU    , GU<<5, BU<<10, RV    , GV<<5, BV<<10, RGB2YUV_SHIFT+7)

static inline void palToY(uint8_t *dst, const uint8_t *src, long width, uint32_t *pal)
{
    int i;
    for (i=0; i<width; i++) {
        int d= src[i];

        dst[i]= pal[d] & 0xFF;
    }
}

static inline void palToUV(uint8_t *dstU, uint8_t *dstV,
                           const uint8_t *src1, const uint8_t *src2,
                           long width, uint32_t *pal)
{
    int i;
    assert(src1 == src2);
    for (i=0; i<width; i++) {
        int p= pal[src1[i]];

        dstU[i]= p>>8;
        dstV[i]= p>>16;
    }
}

static inline void monowhite2Y(uint8_t *dst, const uint8_t *src, long width, uint32_t *unused)
{
    int i, j;
    for (i=0; i<width/8; i++) {
        int d= ~src[i];
        for(j=0; j<8; j++)
            dst[8*i+j]= ((d>>(7-j))&1)*255;
    }
}

static inline void monoblack2Y(uint8_t *dst, const uint8_t *src, long width, uint32_t *unused)
{
    int i, j;
    for (i=0; i<width/8; i++) {
        int d= src[i];
        for(j=0; j<8; j++)
            dst[8*i+j]= ((d>>(7-j))&1)*255;
    }
}


//Note: we have C, MMX, MMX2, 3DNOW versions, there is no 3DNOW+MMX2 one
//Plain C versions
#if ((!HAVE_MMX || !CONFIG_GPL) && !HAVE_ALTIVEC) || CONFIG_RUNTIME_CPUDETECT
#define COMPILE_C
#endif

#if ARCH_PPC
#if HAVE_ALTIVEC || CONFIG_RUNTIME_CPUDETECT
#define COMPILE_ALTIVEC
#endif
#endif //ARCH_PPC

#if ARCH_X86

#if ((HAVE_MMX && !HAVE_AMD3DNOW && !HAVE_MMX2) || CONFIG_RUNTIME_CPUDETECT) && CONFIG_GPL
#define COMPILE_MMX
#endif

#if (HAVE_MMX2 || CONFIG_RUNTIME_CPUDETECT) && CONFIG_GPL
#define COMPILE_MMX2
#endif

#if ((HAVE_AMD3DNOW && !HAVE_MMX2) || CONFIG_RUNTIME_CPUDETECT) && CONFIG_GPL
#define COMPILE_3DNOW
#endif
#endif //ARCH_X86

#define COMPILE_TEMPLATE_MMX 0
#define COMPILE_TEMPLATE_MMX2 0
#define COMPILE_TEMPLATE_AMD3DNOW 0
#define COMPILE_TEMPLATE_ALTIVEC 0

#ifdef COMPILE_C
#define RENAME(a) a ## _C
#include "swscale_template.c"
#endif

#ifdef COMPILE_ALTIVEC
#undef RENAME
#undef COMPILE_TEMPLATE_ALTIVEC
#define COMPILE_TEMPLATE_ALTIVEC 1
#define RENAME(a) a ## _altivec
#include "swscale_template.c"
#endif

#if ARCH_X86

//MMX versions
#ifdef COMPILE_MMX
#undef RENAME
#undef COMPILE_TEMPLATE_MMX
#undef COMPILE_TEMPLATE_MMX2
#undef COMPILE_TEMPLATE_AMD3DNOW
#define COMPILE_TEMPLATE_MMX 1
#define COMPILE_TEMPLATE_MMX2 0
#define COMPILE_TEMPLATE_AMD3DNOW 0
#define RENAME(a) a ## _MMX
#include "swscale_template.c"
#endif

//MMX2 versions
#ifdef COMPILE_MMX2
#undef RENAME
#undef COMPILE_TEMPLATE_MMX
#undef COMPILE_TEMPLATE_MMX2
#undef COMPILE_TEMPLATE_AMD3DNOW
#define COMPILE_TEMPLATE_MMX 1
#define COMPILE_TEMPLATE_MMX2 1
#define COMPILE_TEMPLATE_AMD3DNOW 0
#define RENAME(a) a ## _MMX2
#include "swscale_template.c"
#endif

//3DNOW versions
#ifdef COMPILE_3DNOW
#undef RENAME
#undef COMPILE_TEMPLATE_MMX
#undef COMPILE_TEMPLATE_MMX2
#undef COMPILE_TEMPLATE_AMD3DNOW
#define COMPILE_TEMPLATE_MMX 1
#define COMPILE_TEMPLATE_MMX2 0
#define COMPILE_TEMPLATE_AMD3DNOW 1
#define RENAME(a) a ## _3DNow
#include "swscale_template.c"
#endif

#endif //ARCH_X86

static double getSplineCoeff(double a, double b, double c, double d, double dist)
{
//    printf("%f %f %f %f %f\n", a,b,c,d,dist);
    if (dist<=1.0) return ((d*dist + c)*dist + b)*dist +a;
    else           return getSplineCoeff(        0.0,
                                          b+ 2.0*c + 3.0*d,
                                                 c + 3.0*d,
                                         -b- 3.0*c - 6.0*d,
                                         dist-1.0);
}

static inline int initFilter(int16_t **outFilter, int16_t **filterPos, int *outFilterSize, int xInc,
                             int srcW, int dstW, int filterAlign, int one, int flags,
                             SwsVector *srcFilter, SwsVector *dstFilter, double param[2])
{
    int i;
    int filterSize;
    int filter2Size;
    int minFilterSize;
    int64_t *filter=NULL;
    int64_t *filter2=NULL;
    const int64_t fone= 1LL<<54;
    int ret= -1;
#if ARCH_X86
    if (flags & SWS_CPU_CAPS_MMX)
        __asm__ volatile("emms\n\t"::: "memory"); //FIXME this should not be required but it IS (even for non-MMX versions)
#endif

    // NOTE: the +1 is for the MMX scaler which reads over the end
    FF_ALLOC_OR_GOTO(NULL, *filterPos, (dstW+1)*sizeof(int16_t), fail);

    if (FFABS(xInc - 0x10000) <10) { // unscaled
        int i;
        filterSize= 1;
        FF_ALLOCZ_OR_GOTO(NULL, filter, dstW*sizeof(*filter)*filterSize, fail);

        for (i=0; i<dstW; i++) {
            filter[i*filterSize]= fone;
            (*filterPos)[i]=i;
        }

    } else if (flags&SWS_POINT) { // lame looking point sampling mode
        int i;
        int xDstInSrc;
        filterSize= 1;
        FF_ALLOC_OR_GOTO(NULL, filter, dstW*sizeof(*filter)*filterSize, fail);

        xDstInSrc= xInc/2 - 0x8000;
        for (i=0; i<dstW; i++) {
            int xx= (xDstInSrc - ((filterSize-1)<<15) + (1<<15))>>16;

            (*filterPos)[i]= xx;
            filter[i]= fone;
            xDstInSrc+= xInc;
        }
    } else if ((xInc <= (1<<16) && (flags&SWS_AREA)) || (flags&SWS_FAST_BILINEAR)) { // bilinear upscale
        int i;
        int xDstInSrc;
        filterSize= 2;
        FF_ALLOC_OR_GOTO(NULL, filter, dstW*sizeof(*filter)*filterSize, fail);

        xDstInSrc= xInc/2 - 0x8000;
        for (i=0; i<dstW; i++) {
            int xx= (xDstInSrc - ((filterSize-1)<<15) + (1<<15))>>16;
            int j;

            (*filterPos)[i]= xx;
            //bilinear upscale / linear interpolate / area averaging
            for (j=0; j<filterSize; j++) {
                int64_t coeff= fone - FFABS((xx<<16) - xDstInSrc)*(fone>>16);
                if (coeff<0) coeff=0;
                filter[i*filterSize + j]= coeff;
                xx++;
            }
            xDstInSrc+= xInc;
        }
    } else {
        int xDstInSrc;
        int sizeFactor;

        if      (flags&SWS_BICUBIC)      sizeFactor=  4;
        else if (flags&SWS_X)            sizeFactor=  8;
        else if (flags&SWS_AREA)         sizeFactor=  1; //downscale only, for upscale it is bilinear
        else if (flags&SWS_GAUSS)        sizeFactor=  8;   // infinite ;)
        else if (flags&SWS_LANCZOS)      sizeFactor= param[0] != SWS_PARAM_DEFAULT ? ceil(2*param[0]) : 6;
        else if (flags&SWS_SINC)         sizeFactor= 20; // infinite ;)
        else if (flags&SWS_SPLINE)       sizeFactor= 20;  // infinite ;)
        else if (flags&SWS_BILINEAR)     sizeFactor=  2;
        else {
            sizeFactor= 0; //GCC warning killer
            assert(0);
        }

        if (xInc <= 1<<16)      filterSize= 1 + sizeFactor; // upscale
        else                    filterSize= 1 + (sizeFactor*srcW + dstW - 1)/ dstW;

        if (filterSize > srcW-2) filterSize=srcW-2;

        FF_ALLOC_OR_GOTO(NULL, filter, dstW*sizeof(*filter)*filterSize, fail);

        xDstInSrc= xInc - 0x10000;
        for (i=0; i<dstW; i++) {
            int xx= (xDstInSrc - ((filterSize-2)<<16)) / (1<<17);
            int j;
            (*filterPos)[i]= xx;
            for (j=0; j<filterSize; j++) {
                int64_t d= ((int64_t)FFABS((xx<<17) - xDstInSrc))<<13;
                double floatd;
                int64_t coeff;

                if (xInc > 1<<16)
                    d= d*dstW/srcW;
                floatd= d * (1.0/(1<<30));

                if (flags & SWS_BICUBIC) {
                    int64_t B= (param[0] != SWS_PARAM_DEFAULT ? param[0] :   0) * (1<<24);
                    int64_t C= (param[1] != SWS_PARAM_DEFAULT ? param[1] : 0.6) * (1<<24);
                    int64_t dd = ( d*d)>>30;
                    int64_t ddd= (dd*d)>>30;

                    if      (d < 1LL<<30)
                        coeff = (12*(1<<24)-9*B-6*C)*ddd + (-18*(1<<24)+12*B+6*C)*dd + (6*(1<<24)-2*B)*(1<<30);
                    else if (d < 1LL<<31)
                        coeff = (-B-6*C)*ddd + (6*B+30*C)*dd + (-12*B-48*C)*d + (8*B+24*C)*(1<<30);
                    else
                        coeff=0.0;
                    coeff *= fone>>(30+24);
                }
/*                else if (flags & SWS_X) {
                    double p= param ? param*0.01 : 0.3;
                    coeff = d ? sin(d*PI)/(d*PI) : 1.0;
                    coeff*= pow(2.0, - p*d*d);
                }*/
                else if (flags & SWS_X) {
                    double A= param[0] != SWS_PARAM_DEFAULT ? param[0] : 1.0;
                    double c;

                    if (floatd<1.0)
                        c = cos(floatd*PI);
                    else
                        c=-1.0;
                    if (c<0.0)      c= -pow(-c, A);
                    else            c=  pow( c, A);
                    coeff= (c*0.5 + 0.5)*fone;
                } else if (flags & SWS_AREA) {
                    int64_t d2= d - (1<<29);
                    if      (d2*xInc < -(1LL<<(29+16))) coeff= 1.0 * (1LL<<(30+16));
                    else if (d2*xInc <  (1LL<<(29+16))) coeff= -d2*xInc + (1LL<<(29+16));
                    else coeff=0.0;
                    coeff *= fone>>(30+16);
                } else if (flags & SWS_GAUSS) {
                    double p= param[0] != SWS_PARAM_DEFAULT ? param[0] : 3.0;
                    coeff = (pow(2.0, - p*floatd*floatd))*fone;
                } else if (flags & SWS_SINC) {
                    coeff = (d ? sin(floatd*PI)/(floatd*PI) : 1.0)*fone;
                } else if (flags & SWS_LANCZOS) {
                    double p= param[0] != SWS_PARAM_DEFAULT ? param[0] : 3.0;
                    coeff = (d ? sin(floatd*PI)*sin(floatd*PI/p)/(floatd*floatd*PI*PI/p) : 1.0)*fone;
                    if (floatd>p) coeff=0;
                } else if (flags & SWS_BILINEAR) {
                    coeff= (1<<30) - d;
                    if (coeff<0) coeff=0;
                    coeff *= fone >> 30;
                } else if (flags & SWS_SPLINE) {
                    double p=-2.196152422706632;
                    coeff = getSplineCoeff(1.0, 0.0, p, -p-1.0, floatd) * fone;
                } else {
                    coeff= 0.0; //GCC warning killer
                    assert(0);
                }

                filter[i*filterSize + j]= coeff;
                xx++;
            }
            xDstInSrc+= 2*xInc;
        }
    }

    /* apply src & dst Filter to filter -> filter2
       av_free(filter);
    */
    assert(filterSize>0);
    filter2Size= filterSize;
    if (srcFilter) filter2Size+= srcFilter->length - 1;
    if (dstFilter) filter2Size+= dstFilter->length - 1;
    assert(filter2Size>0);
    FF_ALLOCZ_OR_GOTO(NULL, filter2, filter2Size*dstW*sizeof(*filter2), fail);

    for (i=0; i<dstW; i++) {
        int j, k;

        if(srcFilter) {
            for (k=0; k<srcFilter->length; k++) {
                for (j=0; j<filterSize; j++)
                    filter2[i*filter2Size + k + j] += srcFilter->coeff[k]*filter[i*filterSize + j];
            }
        } else {
            for (j=0; j<filterSize; j++)
                filter2[i*filter2Size + j]= filter[i*filterSize + j];
        }
        //FIXME dstFilter

        (*filterPos)[i]+= (filterSize-1)/2 - (filter2Size-1)/2;
    }
    av_freep(&filter);

    /* try to reduce the filter-size (step1 find size and shift left) */
    // Assume it is near normalized (*0.5 or *2.0 is OK but * 0.001 is not).
    minFilterSize= 0;
    for (i=dstW-1; i>=0; i--) {
        int min= filter2Size;
        int j;
        int64_t cutOff=0.0;

        /* get rid off near zero elements on the left by shifting left */
        for (j=0; j<filter2Size; j++) {
            int k;
            cutOff += FFABS(filter2[i*filter2Size]);

            if (cutOff > SWS_MAX_REDUCE_CUTOFF*fone) break;

            /* preserve monotonicity because the core can't handle the filter otherwise */
            if (i<dstW-1 && (*filterPos)[i] >= (*filterPos)[i+1]) break;

            // move filter coefficients left
            for (k=1; k<filter2Size; k++)
                filter2[i*filter2Size + k - 1]= filter2[i*filter2Size + k];
            filter2[i*filter2Size + k - 1]= 0;
            (*filterPos)[i]++;
        }

        cutOff=0;
        /* count near zeros on the right */
        for (j=filter2Size-1; j>0; j--) {
            cutOff += FFABS(filter2[i*filter2Size + j]);

            if (cutOff > SWS_MAX_REDUCE_CUTOFF*fone) break;
            min--;
        }

        if (min>minFilterSize) minFilterSize= min;
    }

    if (flags & SWS_CPU_CAPS_ALTIVEC) {
        // we can handle the special case 4,
        // so we don't want to go to the full 8
        if (minFilterSize < 5)
            filterAlign = 4;

        // We really don't want to waste our time
        // doing useless computation, so fall back on
        // the scalar C code for very small filters.
        // Vectorizing is worth it only if you have a
        // decent-sized vector.
        if (minFilterSize < 3)
            filterAlign = 1;
    }

    if (flags & SWS_CPU_CAPS_MMX) {
        // special case for unscaled vertical filtering
        if (minFilterSize == 1 && filterAlign == 2)
            filterAlign= 1;
    }

    assert(minFilterSize > 0);
    filterSize= (minFilterSize +(filterAlign-1)) & (~(filterAlign-1));
    assert(filterSize > 0);
    filter= av_malloc(filterSize*dstW*sizeof(*filter));
    if (filterSize >= MAX_FILTER_SIZE*16/((flags&SWS_ACCURATE_RND) ? APCK_SIZE : 16) || !filter)
        goto fail;
    *outFilterSize= filterSize;

    if (flags&SWS_PRINT_INFO)
        av_log(NULL, AV_LOG_VERBOSE, "SwScaler: reducing / aligning filtersize %d -> %d\n", filter2Size, filterSize);
    /* try to reduce the filter-size (step2 reduce it) */
    for (i=0; i<dstW; i++) {
        int j;

        for (j=0; j<filterSize; j++) {
            if (j>=filter2Size) filter[i*filterSize + j]= 0;
            else               filter[i*filterSize + j]= filter2[i*filter2Size + j];
            if((flags & SWS_BITEXACT) && j>=minFilterSize)
                filter[i*filterSize + j]= 0;
        }
    }


    //FIXME try to align filterPos if possible

    //fix borders
    for (i=0; i<dstW; i++) {
        int j;
        if ((*filterPos)[i] < 0) {
            // move filter coefficients left to compensate for filterPos
            for (j=1; j<filterSize; j++) {
                int left= FFMAX(j + (*filterPos)[i], 0);
                filter[i*filterSize + left] += filter[i*filterSize + j];
                filter[i*filterSize + j]=0;
            }
            (*filterPos)[i]= 0;
        }

        if ((*filterPos)[i] + filterSize > srcW) {
            int shift= (*filterPos)[i] + filterSize - srcW;
            // move filter coefficients right to compensate for filterPos
            for (j=filterSize-2; j>=0; j--) {
                int right= FFMIN(j + shift, filterSize-1);
                filter[i*filterSize +right] += filter[i*filterSize +j];
                filter[i*filterSize +j]=0;
            }
            (*filterPos)[i]= srcW - filterSize;
        }
    }

    // Note the +1 is for the MMX scaler which reads over the end
    /* align at 16 for AltiVec (needed by hScale_altivec_real) */
    FF_ALLOCZ_OR_GOTO(NULL, *outFilter, *outFilterSize*(dstW+1)*sizeof(int16_t), fail);

    /* normalize & store in outFilter */
    for (i=0; i<dstW; i++) {
        int j;
        int64_t error=0;
        int64_t sum=0;

        for (j=0; j<filterSize; j++) {
            sum+= filter[i*filterSize + j];
        }
        sum= (sum + one/2)/ one;
        for (j=0; j<*outFilterSize; j++) {
            int64_t v= filter[i*filterSize + j] + error;
            int intV= ROUNDED_DIV(v, sum);
            (*outFilter)[i*(*outFilterSize) + j]= intV;
            error= v - intV*sum;
        }
    }

    (*filterPos)[dstW]= (*filterPos)[dstW-1]; // the MMX scaler will read over the end
    for (i=0; i<*outFilterSize; i++) {
        int j= dstW*(*outFilterSize);
        (*outFilter)[j + i]= (*outFilter)[j + i - (*outFilterSize)];
    }

    ret=0;
fail:
    av_free(filter);
    av_free(filter2);
    return ret;
}

#ifdef COMPILE_MMX2
static int initMMX2HScaler(int dstW, int xInc, uint8_t *filterCode, int16_t *filter, int32_t *filterPos, int numSplits)
{
    uint8_t *fragmentA;
    x86_reg imm8OfPShufW1A;
    x86_reg imm8OfPShufW2A;
    x86_reg fragmentLengthA;
    uint8_t *fragmentB;
    x86_reg imm8OfPShufW1B;
    x86_reg imm8OfPShufW2B;
    x86_reg fragmentLengthB;
    int fragmentPos;

    int xpos, i;

    // create an optimized horizontal scaling routine
    /* This scaler is made of runtime-generated MMX2 code using specially
     * tuned pshufw instructions. For every four output pixels, if four
     * input pixels are enough for the fast bilinear scaling, then a chunk
     * of fragmentB is used. If five input pixels are needed, then a chunk
     * of fragmentA is used.
     */

    //code fragment

    __asm__ volatile(
        "jmp                         9f                 \n\t"
    // Begin
        "0:                                             \n\t"
        "movq    (%%"REG_d", %%"REG_a"), %%mm3          \n\t"
        "movd    (%%"REG_c", %%"REG_S"), %%mm0          \n\t"
        "movd   1(%%"REG_c", %%"REG_S"), %%mm1          \n\t"
        "punpcklbw                %%mm7, %%mm1          \n\t"
        "punpcklbw                %%mm7, %%mm0          \n\t"
        "pshufw                   $0xFF, %%mm1, %%mm1   \n\t"
        "1:                                             \n\t"
        "pshufw                   $0xFF, %%mm0, %%mm0   \n\t"
        "2:                                             \n\t"
        "psubw                    %%mm1, %%mm0          \n\t"
        "movl   8(%%"REG_b", %%"REG_a"), %%esi          \n\t"
        "pmullw                   %%mm3, %%mm0          \n\t"
        "psllw                       $7, %%mm1          \n\t"
        "paddw                    %%mm1, %%mm0          \n\t"

        "movq                     %%mm0, (%%"REG_D", %%"REG_a") \n\t"

        "add                         $8, %%"REG_a"      \n\t"
    // End
        "9:                                             \n\t"
//        "int $3                                         \n\t"
        "lea                 " LOCAL_MANGLE(0b) ", %0   \n\t"
        "lea                 " LOCAL_MANGLE(1b) ", %1   \n\t"
        "lea                 " LOCAL_MANGLE(2b) ", %2   \n\t"
        "dec                         %1                 \n\t"
        "dec                         %2                 \n\t"
        "sub                         %0, %1             \n\t"
        "sub                         %0, %2             \n\t"
        "lea                 " LOCAL_MANGLE(9b) ", %3   \n\t"
        "sub                         %0, %3             \n\t"


        :"=r" (fragmentA), "=r" (imm8OfPShufW1A), "=r" (imm8OfPShufW2A),
        "=r" (fragmentLengthA)
    );

    __asm__ volatile(
        "jmp                         9f                 \n\t"
    // Begin
        "0:                                             \n\t"
        "movq    (%%"REG_d", %%"REG_a"), %%mm3          \n\t"
        "movd    (%%"REG_c", %%"REG_S"), %%mm0          \n\t"
        "punpcklbw                %%mm7, %%mm0          \n\t"
        "pshufw                   $0xFF, %%mm0, %%mm1   \n\t"
        "1:                                             \n\t"
        "pshufw                   $0xFF, %%mm0, %%mm0   \n\t"
        "2:                                             \n\t"
        "psubw                    %%mm1, %%mm0          \n\t"
        "movl   8(%%"REG_b", %%"REG_a"), %%esi          \n\t"
        "pmullw                   %%mm3, %%mm0          \n\t"
        "psllw                       $7, %%mm1          \n\t"
        "paddw                    %%mm1, %%mm0          \n\t"

        "movq                     %%mm0, (%%"REG_D", %%"REG_a") \n\t"

        "add                         $8, %%"REG_a"      \n\t"
    // End
        "9:                                             \n\t"
//        "int                       $3                   \n\t"
        "lea                 " LOCAL_MANGLE(0b) ", %0   \n\t"
        "lea                 " LOCAL_MANGLE(1b) ", %1   \n\t"
        "lea                 " LOCAL_MANGLE(2b) ", %2   \n\t"
        "dec                         %1                 \n\t"
        "dec                         %2                 \n\t"
        "sub                         %0, %1             \n\t"
        "sub                         %0, %2             \n\t"
        "lea                 " LOCAL_MANGLE(9b) ", %3   \n\t"
        "sub                         %0, %3             \n\t"


        :"=r" (fragmentB), "=r" (imm8OfPShufW1B), "=r" (imm8OfPShufW2B),
        "=r" (fragmentLengthB)
    );

    xpos= 0; //lumXInc/2 - 0x8000; // difference between pixel centers
    fragmentPos=0;

    for (i=0; i<dstW/numSplits; i++) {
        int xx=xpos>>16;

        if ((i&3) == 0) {
            int a=0;
            int b=((xpos+xInc)>>16) - xx;
            int c=((xpos+xInc*2)>>16) - xx;
            int d=((xpos+xInc*3)>>16) - xx;
            int inc                = (d+1<4);
            uint8_t *fragment      = (d+1<4) ? fragmentB       : fragmentA;
            x86_reg imm8OfPShufW1  = (d+1<4) ? imm8OfPShufW1B  : imm8OfPShufW1A;
            x86_reg imm8OfPShufW2  = (d+1<4) ? imm8OfPShufW2B  : imm8OfPShufW2A;
            x86_reg fragmentLength = (d+1<4) ? fragmentLengthB : fragmentLengthA;
            int maxShift= 3-(d+inc);
            int shift=0;

            if (filterCode) {
                filter[i  ] = (( xpos         & 0xFFFF) ^ 0xFFFF)>>9;
                filter[i+1] = (((xpos+xInc  ) & 0xFFFF) ^ 0xFFFF)>>9;
                filter[i+2] = (((xpos+xInc*2) & 0xFFFF) ^ 0xFFFF)>>9;
                filter[i+3] = (((xpos+xInc*3) & 0xFFFF) ^ 0xFFFF)>>9;
                filterPos[i/2]= xx;

                memcpy(filterCode + fragmentPos, fragment, fragmentLength);

                filterCode[fragmentPos + imm8OfPShufW1]=
                    (a+inc) | ((b+inc)<<2) | ((c+inc)<<4) | ((d+inc)<<6);
                filterCode[fragmentPos + imm8OfPShufW2]=
                    a | (b<<2) | (c<<4) | (d<<6);

                if (i+4-inc>=dstW) shift=maxShift; //avoid overread
                else if ((filterPos[i/2]&3) <= maxShift) shift=filterPos[i/2]&3; //Align

                if (shift && i>=shift) {
                    filterCode[fragmentPos + imm8OfPShufW1]+= 0x55*shift;
                    filterCode[fragmentPos + imm8OfPShufW2]+= 0x55*shift;
                    filterPos[i/2]-=shift;
                }
            }

            fragmentPos+= fragmentLength;

            if (filterCode)
                filterCode[fragmentPos]= RET;
        }
        xpos+=xInc;
    }
    if (filterCode)
        filterPos[((i/2)+1)&(~1)]= xpos>>16; // needed to jump to the next part

    return fragmentPos + 1;
}
#endif /* COMPILE_MMX2 */

static void globalInit(void)
{
    // generating tables:
    int i;
    for (i=0; i<768; i++) {
        int c= av_clip_uint8(i-256);
        clip_table[i]=c;
    }
}

static SwsFunc getSwsFunc(SwsContext *c)
{
#if CONFIG_RUNTIME_CPUDETECT
    int flags = c->flags;

#if ARCH_X86 && CONFIG_GPL
    // ordered per speed fastest first
    if (flags & SWS_CPU_CAPS_MMX2) {
        sws_init_swScale_MMX2(c);
        return swScale_MMX2;
    } else if (flags & SWS_CPU_CAPS_3DNOW) {
        sws_init_swScale_3DNow(c);
        return swScale_3DNow;
    } else if (flags & SWS_CPU_CAPS_MMX) {
        sws_init_swScale_MMX(c);
        return swScale_MMX;
    } else {
        sws_init_swScale_C(c);
        return swScale_C;
    }

#else
#if ARCH_PPC
    if (flags & SWS_CPU_CAPS_ALTIVEC) {
        sws_init_swScale_altivec(c);
        return swScale_altivec;
    } else {
        sws_init_swScale_C(c);
        return swScale_C;
    }
#endif
    sws_init_swScale_C(c);
    return swScale_C;
#endif /* ARCH_X86 && CONFIG_GPL */
#else //CONFIG_RUNTIME_CPUDETECT
#if   COMPILE_TEMPLATE_MMX2
    sws_init_swScale_MMX2(c);
    return swScale_MMX2;
#elif COMPILE_TEMPLATE_AMD3DNOW
    sws_init_swScale_3DNow(c);
    return swScale_3DNow;
#elif COMPILE_TEMPLATE_MMX
    sws_init_swScale_MMX(c);
    return swScale_MMX;
#elif COMPILE_TEMPLATE_ALTIVEC
    sws_init_swScale_altivec(c);
    return swScale_altivec;
#else
    sws_init_swScale_C(c);
    return swScale_C;
#endif
#endif //!CONFIG_RUNTIME_CPUDETECT
}

static int PlanarToNV12Wrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                               int srcSliceH, uint8_t* dstParam[], int dstStride[])
{
    uint8_t *dst=dstParam[0] + dstStride[0]*srcSliceY;
    /* Copy Y plane */
    if (dstStride[0]==srcStride[0] && srcStride[0] > 0)
        memcpy(dst, src[0], srcSliceH*dstStride[0]);
    else {
        int i;
        const uint8_t *srcPtr= src[0];
        uint8_t *dstPtr= dst;
        for (i=0; i<srcSliceH; i++) {
            memcpy(dstPtr, srcPtr, c->srcW);
            srcPtr+= srcStride[0];
            dstPtr+= dstStride[0];
        }
    }
    dst = dstParam[1] + dstStride[1]*srcSliceY/2;
    if (c->dstFormat == PIX_FMT_NV12)
        interleaveBytes(src[1], src[2], dst, c->srcW/2, srcSliceH/2, srcStride[1], srcStride[2], dstStride[0]);
    else
        interleaveBytes(src[2], src[1], dst, c->srcW/2, srcSliceH/2, srcStride[2], srcStride[1], dstStride[0]);

    return srcSliceH;
}

static int PlanarToYuy2Wrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                               int srcSliceH, uint8_t* dstParam[], int dstStride[])
{
    uint8_t *dst=dstParam[0] + dstStride[0]*srcSliceY;

    yv12toyuy2(src[0], src[1], src[2], dst, c->srcW, srcSliceH, srcStride[0], srcStride[1], dstStride[0]);

    return srcSliceH;
}

static int PlanarToUyvyWrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                               int srcSliceH, uint8_t* dstParam[], int dstStride[])
{
    uint8_t *dst=dstParam[0] + dstStride[0]*srcSliceY;

    yv12touyvy(src[0], src[1], src[2], dst, c->srcW, srcSliceH, srcStride[0], srcStride[1], dstStride[0]);

    return srcSliceH;
}

static int YUV422PToYuy2Wrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                                int srcSliceH, uint8_t* dstParam[], int dstStride[])
{
    uint8_t *dst=dstParam[0] + dstStride[0]*srcSliceY;

    yuv422ptoyuy2(src[0],src[1],src[2],dst,c->srcW,srcSliceH,srcStride[0],srcStride[1],dstStride[0]);

    return srcSliceH;
}

static int YUV422PToUyvyWrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                                int srcSliceH, uint8_t* dstParam[], int dstStride[])
{
    uint8_t *dst=dstParam[0] + dstStride[0]*srcSliceY;

    yuv422ptouyvy(src[0],src[1],src[2],dst,c->srcW,srcSliceH,srcStride[0],srcStride[1],dstStride[0]);

    return srcSliceH;
}

static int YUYV2YUV420Wrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                               int srcSliceH, uint8_t* dstParam[], int dstStride[])
{
    uint8_t *ydst=dstParam[0] + dstStride[0]*srcSliceY;
    uint8_t *udst=dstParam[1] + dstStride[1]*srcSliceY/2;
    uint8_t *vdst=dstParam[2] + dstStride[2]*srcSliceY/2;

    yuyvtoyuv420(ydst, udst, vdst, src[0], c->srcW, srcSliceH, dstStride[0], dstStride[1], srcStride[0]);

    if (dstParam[3])
        fillPlane(dstParam[3], dstStride[3], c->srcW, srcSliceH, srcSliceY, 255);

    return srcSliceH;
}

static int YUYV2YUV422Wrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                               int srcSliceH, uint8_t* dstParam[], int dstStride[])
{
    uint8_t *ydst=dstParam[0] + dstStride[0]*srcSliceY;
    uint8_t *udst=dstParam[1] + dstStride[1]*srcSliceY;
    uint8_t *vdst=dstParam[2] + dstStride[2]*srcSliceY;

    yuyvtoyuv422(ydst, udst, vdst, src[0], c->srcW, srcSliceH, dstStride[0], dstStride[1], srcStride[0]);

    return srcSliceH;
}

static int UYVY2YUV420Wrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                               int srcSliceH, uint8_t* dstParam[], int dstStride[])
{
    uint8_t *ydst=dstParam[0] + dstStride[0]*srcSliceY;
    uint8_t *udst=dstParam[1] + dstStride[1]*srcSliceY/2;
    uint8_t *vdst=dstParam[2] + dstStride[2]*srcSliceY/2;

    uyvytoyuv420(ydst, udst, vdst, src[0], c->srcW, srcSliceH, dstStride[0], dstStride[1], srcStride[0]);

    if (dstParam[3])
        fillPlane(dstParam[3], dstStride[3], c->srcW, srcSliceH, srcSliceY, 255);

    return srcSliceH;
}

static int UYVY2YUV422Wrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                               int srcSliceH, uint8_t* dstParam[], int dstStride[])
{
    uint8_t *ydst=dstParam[0] + dstStride[0]*srcSliceY;
    uint8_t *udst=dstParam[1] + dstStride[1]*srcSliceY;
    uint8_t *vdst=dstParam[2] + dstStride[2]*srcSliceY;

    uyvytoyuv422(ydst, udst, vdst, src[0], c->srcW, srcSliceH, dstStride[0], dstStride[1], srcStride[0]);

    return srcSliceH;
}

static int pal2rgbWrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                          int srcSliceH, uint8_t* dst[], int dstStride[])
{
    const enum PixelFormat srcFormat= c->srcFormat;
    const enum PixelFormat dstFormat= c->dstFormat;
    void (*conv)(const uint8_t *src, uint8_t *dst, long num_pixels,
                 const uint8_t *palette)=NULL;
    int i;
    uint8_t *dstPtr= dst[0] + dstStride[0]*srcSliceY;
    uint8_t *srcPtr= src[0];

    if (!usePal(srcFormat))
        av_log(c, AV_LOG_ERROR, "internal error %s -> %s converter\n",
               sws_format_name(srcFormat), sws_format_name(dstFormat));

    switch(dstFormat) {
    case PIX_FMT_RGB32  : conv = palette8topacked32; break;
    case PIX_FMT_BGR32  : conv = palette8topacked32; break;
    case PIX_FMT_BGR32_1: conv = palette8topacked32; break;
    case PIX_FMT_RGB32_1: conv = palette8topacked32; break;
    case PIX_FMT_RGB24  : conv = palette8topacked24; break;
    case PIX_FMT_BGR24  : conv = palette8topacked24; break;
    default: av_log(c, AV_LOG_ERROR, "internal error %s -> %s converter\n",
                    sws_format_name(srcFormat), sws_format_name(dstFormat)); break;
    }


    for (i=0; i<srcSliceH; i++) {
        conv(srcPtr, dstPtr, c->srcW, (uint8_t *) c->pal_rgb);
        srcPtr+= srcStride[0];
        dstPtr+= dstStride[0];
    }

    return srcSliceH;
}

/* {RGB,BGR}{15,16,24,32,32_1} -> {RGB,BGR}{15,16,24,32} */
static int rgb2rgbWrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                          int srcSliceH, uint8_t* dst[], int dstStride[])
{
    const enum PixelFormat srcFormat= c->srcFormat;
    const enum PixelFormat dstFormat= c->dstFormat;
    const int srcBpp= (fmt_depth(srcFormat) + 7) >> 3;
    const int dstBpp= (fmt_depth(dstFormat) + 7) >> 3;
    const int srcId= fmt_depth(srcFormat) >> 2; /* 1:0, 4:1, 8:2, 15:3, 16:4, 24:6, 32:8 */
    const int dstId= fmt_depth(dstFormat) >> 2;
    void (*conv)(const uint8_t *src, uint8_t *dst, long src_size)=NULL;

    /* BGR -> BGR */
    if (  (isBGR(srcFormat) && isBGR(dstFormat))
       || (isRGB(srcFormat) && isRGB(dstFormat))) {
        switch(srcId | (dstId<<4)) {
        case 0x34: conv= rgb16to15; break;
        case 0x36: conv= rgb24to15; break;
        case 0x38: conv= rgb32to15; break;
        case 0x43: conv= rgb15to16; break;
        case 0x46: conv= rgb24to16; break;
        case 0x48: conv= rgb32to16; break;
        case 0x63: conv= rgb15to24; break;
        case 0x64: conv= rgb16to24; break;
        case 0x68: conv= rgb32to24; break;
        case 0x83: conv= rgb15to32; break;
        case 0x84: conv= rgb16to32; break;
        case 0x86: conv= rgb24to32; break;
        default: av_log(c, AV_LOG_ERROR, "internal error %s -> %s converter\n",
                        sws_format_name(srcFormat), sws_format_name(dstFormat)); break;
        }
    } else if (  (isBGR(srcFormat) && isRGB(dstFormat))
             || (isRGB(srcFormat) && isBGR(dstFormat))) {
        switch(srcId | (dstId<<4)) {
        case 0x33: conv= rgb15tobgr15; break;
        case 0x34: conv= rgb16tobgr15; break;
        case 0x36: conv= rgb24tobgr15; break;
        case 0x38: conv= rgb32tobgr15; break;
        case 0x43: conv= rgb15tobgr16; break;
        case 0x44: conv= rgb16tobgr16; break;
        case 0x46: conv= rgb24tobgr16; break;
        case 0x48: conv= rgb32tobgr16; break;
        case 0x63: conv= rgb15tobgr24; break;
        case 0x64: conv= rgb16tobgr24; break;
        case 0x66: conv= rgb24tobgr24; break;
        case 0x68: conv= rgb32tobgr24; break;
        case 0x83: conv= rgb15tobgr32; break;
        case 0x84: conv= rgb16tobgr32; break;
        case 0x86: conv= rgb24tobgr32; break;
        case 0x88: conv= rgb32tobgr32; break;
        default: av_log(c, AV_LOG_ERROR, "internal error %s -> %s converter\n",
                        sws_format_name(srcFormat), sws_format_name(dstFormat)); break;
        }
    } else {
        av_log(c, AV_LOG_ERROR, "internal error %s -> %s converter\n",
               sws_format_name(srcFormat), sws_format_name(dstFormat));
    }

    if(conv) {
        uint8_t *srcPtr= src[0];
        if(srcFormat == PIX_FMT_RGB32_1 || srcFormat == PIX_FMT_BGR32_1)
            srcPtr += ALT32_CORR;

        if (dstStride[0]*srcBpp == srcStride[0]*dstBpp && srcStride[0] > 0)
            conv(srcPtr, dst[0] + dstStride[0]*srcSliceY, srcSliceH*srcStride[0]);
        else {
            int i;
            uint8_t *dstPtr= dst[0] + dstStride[0]*srcSliceY;

            for (i=0; i<srcSliceH; i++) {
                conv(srcPtr, dstPtr, c->srcW*srcBpp);
                srcPtr+= srcStride[0];
                dstPtr+= dstStride[0];
            }
        }
    }
    return srcSliceH;
}

static int bgr24toyv12Wrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                              int srcSliceH, uint8_t* dst[], int dstStride[])
{

    rgb24toyv12(
        src[0],
        dst[0]+ srcSliceY    *dstStride[0],
        dst[1]+(srcSliceY>>1)*dstStride[1],
        dst[2]+(srcSliceY>>1)*dstStride[2],
        c->srcW, srcSliceH,
        dstStride[0], dstStride[1], srcStride[0]);
    if (dst[3])
        fillPlane(dst[3], dstStride[3], c->srcW, srcSliceH, srcSliceY, 255);
    return srcSliceH;
}

static int yvu9toyv12Wrapper(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                             int srcSliceH, uint8_t* dst[], int dstStride[])
{
    int i;

    /* copy Y */
    if (srcStride[0]==dstStride[0] && srcStride[0] > 0)
        memcpy(dst[0]+ srcSliceY*dstStride[0], src[0], srcStride[0]*srcSliceH);
    else {
        uint8_t *srcPtr= src[0];
        uint8_t *dstPtr= dst[0] + dstStride[0]*srcSliceY;

        for (i=0; i<srcSliceH; i++) {
            memcpy(dstPtr, srcPtr, c->srcW);
            srcPtr+= srcStride[0];
            dstPtr+= dstStride[0];
        }
    }

    if (c->dstFormat==PIX_FMT_YUV420P || c->dstFormat==PIX_FMT_YUVA420P) {
        planar2x(src[1], dst[1] + dstStride[1]*(srcSliceY >> 1), c->chrSrcW,
                 srcSliceH >> 2, srcStride[1], dstStride[1]);
        planar2x(src[2], dst[2] + dstStride[2]*(srcSliceY >> 1), c->chrSrcW,
                 srcSliceH >> 2, srcStride[2], dstStride[2]);
    } else {
        planar2x(src[1], dst[2] + dstStride[2]*(srcSliceY >> 1), c->chrSrcW,
                 srcSliceH >> 2, srcStride[1], dstStride[2]);
        planar2x(src[2], dst[1] + dstStride[1]*(srcSliceY >> 1), c->chrSrcW,
                 srcSliceH >> 2, srcStride[2], dstStride[1]);
    }
    if (dst[3])
        fillPlane(dst[3], dstStride[3], c->srcW, srcSliceH, srcSliceY, 255);
    return srcSliceH;
}

/* unscaled copy like stuff (assumes nearly identical formats) */
static int packedCopy(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                      int srcSliceH, uint8_t* dst[], int dstStride[])
{
    if (dstStride[0]==srcStride[0] && srcStride[0] > 0)
        memcpy(dst[0] + dstStride[0]*srcSliceY, src[0], srcSliceH*dstStride[0]);
    else {
        int i;
        uint8_t *srcPtr= src[0];
        uint8_t *dstPtr= dst[0] + dstStride[0]*srcSliceY;
        int length=0;

        /* universal length finder */
        while(length+c->srcW <= FFABS(dstStride[0])
           && length+c->srcW <= FFABS(srcStride[0])) length+= c->srcW;
        assert(length!=0);

        for (i=0; i<srcSliceH; i++) {
            memcpy(dstPtr, srcPtr, length);
            srcPtr+= srcStride[0];
            dstPtr+= dstStride[0];
        }
    }
    return srcSliceH;
}

static int planarCopy(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                      int srcSliceH, uint8_t* dst[], int dstStride[])
{
    int plane, i, j;
    for (plane=0; plane<4; plane++) {
        int length= (plane==0 || plane==3) ? c->srcW  : -((-c->srcW  )>>c->chrDstHSubSample);
        int y=      (plane==0 || plane==3) ? srcSliceY: -((-srcSliceY)>>c->chrDstVSubSample);
        int height= (plane==0 || plane==3) ? srcSliceH: -((-srcSliceH)>>c->chrDstVSubSample);
        uint8_t *srcPtr= src[plane];
        uint8_t *dstPtr= dst[plane] + dstStride[plane]*y;

        if (!dst[plane]) continue;
        // ignore palette for GRAY8
        if (plane == 1 && !dst[2]) continue;
        if (!src[plane] || (plane == 1 && !src[2])) {
            if(is16BPS(c->dstFormat))
                length*=2;
            fillPlane(dst[plane], dstStride[plane], length, height, y, (plane==3) ? 255 : 128);
        } else {
            if(is16BPS(c->srcFormat) && !is16BPS(c->dstFormat)) {
                if (!isBE(c->srcFormat)) srcPtr++;
                for (i=0; i<height; i++) {
                    for (j=0; j<length; j++) dstPtr[j] = srcPtr[j<<1];
                    srcPtr+= srcStride[plane];
                    dstPtr+= dstStride[plane];
                }
            } else if(!is16BPS(c->srcFormat) && is16BPS(c->dstFormat)) {
                for (i=0; i<height; i++) {
                    for (j=0; j<length; j++) {
                        dstPtr[ j<<1   ] = srcPtr[j];
                        dstPtr[(j<<1)+1] = srcPtr[j];
                    }
                    srcPtr+= srcStride[plane];
                    dstPtr+= dstStride[plane];
                }
            } else if(is16BPS(c->srcFormat) && is16BPS(c->dstFormat)
                  && isBE(c->srcFormat) != isBE(c->dstFormat)) {

                for (i=0; i<height; i++) {
                    for (j=0; j<length; j++)
                        ((uint16_t*)dstPtr)[j] = bswap_16(((uint16_t*)srcPtr)[j]);
                    srcPtr+= srcStride[plane];
                    dstPtr+= dstStride[plane];
                }
            } else if (dstStride[plane]==srcStride[plane] && srcStride[plane] > 0)
                memcpy(dst[plane] + dstStride[plane]*y, src[plane], height*dstStride[plane]);
            else {
                if(is16BPS(c->srcFormat) && is16BPS(c->dstFormat))
                    length*=2;
                for (i=0; i<height; i++) {
                    memcpy(dstPtr, srcPtr, length);
                    srcPtr+= srcStride[plane];
                    dstPtr+= dstStride[plane];
                }
            }
        }
    }
    return srcSliceH;
}


static void getSubSampleFactors(int *h, int *v, enum PixelFormat format)
{
    *h = av_pix_fmt_descriptors[format].log2_chroma_w;
    *v = av_pix_fmt_descriptors[format].log2_chroma_h;
}

static uint16_t roundToInt16(int64_t f)
{
    int r= (f + (1<<15))>>16;
         if (r<-0x7FFF) return 0x8000;
    else if (r> 0x7FFF) return 0x7FFF;
    else                return r;
}

int sws_setColorspaceDetails(SwsContext *c, const int inv_table[4], int srcRange, const int table[4], int dstRange, int brightness, int contrast, int saturation)
{
    int64_t crv =  inv_table[0];
    int64_t cbu =  inv_table[1];
    int64_t cgu = -inv_table[2];
    int64_t cgv = -inv_table[3];
    int64_t cy  = 1<<16;
    int64_t oy  = 0;

    memcpy(c->srcColorspaceTable, inv_table, sizeof(int)*4);
    memcpy(c->dstColorspaceTable,     table, sizeof(int)*4);

    c->brightness= brightness;
    c->contrast  = contrast;
    c->saturation= saturation;
    c->srcRange  = srcRange;
    c->dstRange  = dstRange;
    if (isYUV(c->dstFormat) || isGray(c->dstFormat)) return -1;

    c->uOffset=   0x0400040004000400LL;
    c->vOffset=   0x0400040004000400LL;

    if (!srcRange) {
        cy= (cy*255) / 219;
        oy= 16<<16;
    } else {
        crv= (crv*224) / 255;
        cbu= (cbu*224) / 255;
        cgu= (cgu*224) / 255;
        cgv= (cgv*224) / 255;
    }

    cy = (cy *contrast             )>>16;
    crv= (crv*contrast * saturation)>>32;
    cbu= (cbu*contrast * saturation)>>32;
    cgu= (cgu*contrast * saturation)>>32;
    cgv= (cgv*contrast * saturation)>>32;

    oy -= 256*brightness;

    c->yCoeff=    roundToInt16(cy *8192) * 0x0001000100010001ULL;
    c->vrCoeff=   roundToInt16(crv*8192) * 0x0001000100010001ULL;
    c->ubCoeff=   roundToInt16(cbu*8192) * 0x0001000100010001ULL;
    c->vgCoeff=   roundToInt16(cgv*8192) * 0x0001000100010001ULL;
    c->ugCoeff=   roundToInt16(cgu*8192) * 0x0001000100010001ULL;
    c->yOffset=   roundToInt16(oy *   8) * 0x0001000100010001ULL;

    c->yuv2rgb_y_coeff  = (int16_t)roundToInt16(cy <<13);
    c->yuv2rgb_y_offset = (int16_t)roundToInt16(oy << 9);
    c->yuv2rgb_v2r_coeff= (int16_t)roundToInt16(crv<<13);
    c->yuv2rgb_v2g_coeff= (int16_t)roundToInt16(cgv<<13);
    c->yuv2rgb_u2g_coeff= (int16_t)roundToInt16(cgu<<13);
    c->yuv2rgb_u2b_coeff= (int16_t)roundToInt16(cbu<<13);

    ff_yuv2rgb_c_init_tables(c, inv_table, srcRange, brightness, contrast, saturation);
    //FIXME factorize

#ifdef COMPILE_ALTIVEC
    if (c->flags & SWS_CPU_CAPS_ALTIVEC)
        ff_yuv2rgb_init_tables_altivec(c, inv_table, brightness, contrast, saturation);
#endif
    return 0;
}

int sws_getColorspaceDetails(SwsContext *c, int **inv_table, int *srcRange, int **table, int *dstRange, int *brightness, int *contrast, int *saturation)
{
    if (isYUV(c->dstFormat) || isGray(c->dstFormat)) return -1;

    *inv_table = c->srcColorspaceTable;
    *table     = c->dstColorspaceTable;
    *srcRange  = c->srcRange;
    *dstRange  = c->dstRange;
    *brightness= c->brightness;
    *contrast  = c->contrast;
    *saturation= c->saturation;

    return 0;
}

static int handle_jpeg(enum PixelFormat *format)
{
    switch (*format) {
    case PIX_FMT_YUVJ420P:
        *format = PIX_FMT_YUV420P;
        return 1;
    case PIX_FMT_YUVJ422P:
        *format = PIX_FMT_YUV422P;
        return 1;
    case PIX_FMT_YUVJ444P:
        *format = PIX_FMT_YUV444P;
        return 1;
    case PIX_FMT_YUVJ440P:
        *format = PIX_FMT_YUV440P;
        return 1;
    default:
        return 0;
    }
}

SwsContext *sws_getContext(int srcW, int srcH, enum PixelFormat srcFormat, int dstW, int dstH, enum PixelFormat dstFormat, int flags,
                           SwsFilter *srcFilter, SwsFilter *dstFilter, const double *param)
{

    SwsContext *c;
    int i;
    int usesVFilter, usesHFilter;
    int unscaled, needsDither;
    int srcRange, dstRange;
    SwsFilter dummyFilter= {NULL, NULL, NULL, NULL};
#if ARCH_X86
    if (flags & SWS_CPU_CAPS_MMX)
        __asm__ volatile("emms\n\t"::: "memory");
#endif

#if !CONFIG_RUNTIME_CPUDETECT //ensure that the flags match the compiled variant if cpudetect is off
    flags &= ~(SWS_CPU_CAPS_MMX|SWS_CPU_CAPS_MMX2|SWS_CPU_CAPS_3DNOW|SWS_CPU_CAPS_ALTIVEC|SWS_CPU_CAPS_BFIN);
#if   COMPILE_TEMPLATE_MMX2
    flags |= SWS_CPU_CAPS_MMX|SWS_CPU_CAPS_MMX2;
#elif COMPILE_TEMPLATE_AMD3DNOW
    flags |= SWS_CPU_CAPS_MMX|SWS_CPU_CAPS_3DNOW;
#elif COMPILE_TEMPLATE_MMX
    flags |= SWS_CPU_CAPS_MMX;
#elif COMPILE_TEMPLATE_ALTIVEC
    flags |= SWS_CPU_CAPS_ALTIVEC;
#elif ARCH_BFIN
    flags |= SWS_CPU_CAPS_BFIN;
#endif
#endif /* CONFIG_RUNTIME_CPUDETECT */
    if (clip_table[512] != 255) globalInit();
    if (!rgb15to16) sws_rgb2rgb_init(flags);

    unscaled = (srcW == dstW && srcH == dstH);
    needsDither= (isBGR(dstFormat) || isRGB(dstFormat))
        && (fmt_depth(dstFormat))<24
        && ((fmt_depth(dstFormat))<(fmt_depth(srcFormat)) || (!(isRGB(srcFormat) || isBGR(srcFormat))));

    srcRange = handle_jpeg(&srcFormat);
    dstRange = handle_jpeg(&dstFormat);

    if (!isSupportedIn(srcFormat)) {
        av_log(NULL, AV_LOG_ERROR, "swScaler: %s is not supported as input pixel format\n", sws_format_name(srcFormat));
        return NULL;
    }
    if (!isSupportedOut(dstFormat)) {
        av_log(NULL, AV_LOG_ERROR, "swScaler: %s is not supported as output pixel format\n", sws_format_name(dstFormat));
        return NULL;
    }

    i= flags & ( SWS_POINT
                |SWS_AREA
                |SWS_BILINEAR
                |SWS_FAST_BILINEAR
                |SWS_BICUBIC
                |SWS_X
                |SWS_GAUSS
                |SWS_LANCZOS
                |SWS_SINC
                |SWS_SPLINE
                |SWS_BICUBLIN);
    if(!i || (i & (i-1))) {
        av_log(NULL, AV_LOG_ERROR, "swScaler: Exactly one scaler algorithm must be chosen\n");
        return NULL;
    }

    /* sanity check */
    if (srcW<4 || srcH<1 || dstW<8 || dstH<1) { //FIXME check if these are enough and try to lowwer them after fixing the relevant parts of the code
        av_log(NULL, AV_LOG_ERROR, "swScaler: %dx%d -> %dx%d is invalid scaling dimension\n",
               srcW, srcH, dstW, dstH);
        return NULL;
    }
    if(srcW > VOFW || dstW > VOFW) {
        av_log(NULL, AV_LOG_ERROR, "swScaler: Compile-time maximum width is "AV_STRINGIFY(VOFW)" change VOF/VOFW and recompile\n");
        return NULL;
    }

    if (!dstFilter) dstFilter= &dummyFilter;
    if (!srcFilter) srcFilter= &dummyFilter;

    FF_ALLOCZ_OR_GOTO(NULL, c, sizeof(SwsContext), fail);

    c->av_class = &sws_context_class;
    c->srcW= srcW;
    c->srcH= srcH;
    c->dstW= dstW;
    c->dstH= dstH;
    c->lumXInc= ((srcW<<16) + (dstW>>1))/dstW;
    c->lumYInc= ((srcH<<16) + (dstH>>1))/dstH;
    c->flags= flags;
    c->dstFormat= dstFormat;
    c->srcFormat= srcFormat;
    c->vRounder= 4* 0x0001000100010001ULL;

    usesHFilter= usesVFilter= 0;
    if (dstFilter->lumV && dstFilter->lumV->length>1) usesVFilter=1;
    if (dstFilter->lumH && dstFilter->lumH->length>1) usesHFilter=1;
    if (dstFilter->chrV && dstFilter->chrV->length>1) usesVFilter=1;
    if (dstFilter->chrH && dstFilter->chrH->length>1) usesHFilter=1;
    if (srcFilter->lumV && srcFilter->lumV->length>1) usesVFilter=1;
    if (srcFilter->lumH && srcFilter->lumH->length>1) usesHFilter=1;
    if (srcFilter->chrV && srcFilter->chrV->length>1) usesVFilter=1;
    if (srcFilter->chrH && srcFilter->chrH->length>1) usesHFilter=1;

    getSubSampleFactors(&c->chrSrcHSubSample, &c->chrSrcVSubSample, srcFormat);
    getSubSampleFactors(&c->chrDstHSubSample, &c->chrDstVSubSample, dstFormat);

    // reuse chroma for 2 pixels RGB/BGR unless user wants full chroma interpolation
    if ((isBGR(dstFormat) || isRGB(dstFormat)) && !(flags&SWS_FULL_CHR_H_INT)) c->chrDstHSubSample=1;

    // drop some chroma lines if the user wants it
    c->vChrDrop= (flags&SWS_SRC_V_CHR_DROP_MASK)>>SWS_SRC_V_CHR_DROP_SHIFT;
    c->chrSrcVSubSample+= c->vChrDrop;

    // drop every other pixel for chroma calculation unless user wants full chroma
    if ((isBGR(srcFormat) || isRGB(srcFormat)) && !(flags&SWS_FULL_CHR_H_INP)
      && srcFormat!=PIX_FMT_RGB8      && srcFormat!=PIX_FMT_BGR8
      && srcFormat!=PIX_FMT_RGB4      && srcFormat!=PIX_FMT_BGR4
      && srcFormat!=PIX_FMT_RGB4_BYTE && srcFormat!=PIX_FMT_BGR4_BYTE
      && ((dstW>>c->chrDstHSubSample) <= (srcW>>1) || (flags&(SWS_FAST_BILINEAR|SWS_POINT))))
        c->chrSrcHSubSample=1;

    if (param) {
        c->param[0] = param[0];
        c->param[1] = param[1];
    } else {
        c->param[0] =
        c->param[1] = SWS_PARAM_DEFAULT;
    }

    // Note the -((-x)>>y) is so that we always round toward +inf.
    c->chrSrcW= -((-srcW) >> c->chrSrcHSubSample);
    c->chrSrcH= -((-srcH) >> c->chrSrcVSubSample);
    c->chrDstW= -((-dstW) >> c->chrDstHSubSample);
    c->chrDstH= -((-dstH) >> c->chrDstVSubSample);

    sws_setColorspaceDetails(c, ff_yuv2rgb_coeffs[SWS_CS_DEFAULT], srcRange, ff_yuv2rgb_coeffs[SWS_CS_DEFAULT] /* FIXME*/, dstRange, 0, 1<<16, 1<<16);

    /* unscaled special cases */
    if (unscaled && !usesHFilter && !usesVFilter && (srcRange == dstRange || isBGR(dstFormat) || isRGB(dstFormat))) {
        /* yv12_to_nv12 */
        if ((srcFormat == PIX_FMT_YUV420P || srcFormat == PIX_FMT_YUVA420P) && (dstFormat == PIX_FMT_NV12 || dstFormat == PIX_FMT_NV21)) {
            c->swScale= PlanarToNV12Wrapper;
        }
        /* yuv2bgr */
        if ((srcFormat==PIX_FMT_YUV420P || srcFormat==PIX_FMT_YUV422P || srcFormat==PIX_FMT_YUVA420P) && (isBGR(dstFormat) || isRGB(dstFormat))
            && !(flags & SWS_ACCURATE_RND) && !(dstH&1)) {
            c->swScale= ff_yuv2rgb_get_func_ptr(c);
        }

        if (srcFormat==PIX_FMT_YUV410P && (dstFormat==PIX_FMT_YUV420P || dstFormat==PIX_FMT_YUVA420P) && !(flags & SWS_BITEXACT)) {
            c->swScale= yvu9toyv12Wrapper;
        }

        /* bgr24toYV12 */
        if (srcFormat==PIX_FMT_BGR24 && (dstFormat==PIX_FMT_YUV420P || dstFormat==PIX_FMT_YUVA420P) && !(flags & SWS_ACCURATE_RND))
            c->swScale= bgr24toyv12Wrapper;

        /* RGB/BGR -> RGB/BGR (no dither needed forms) */
        if (  (isBGR(srcFormat) || isRGB(srcFormat))
           && (isBGR(dstFormat) || isRGB(dstFormat))
           && srcFormat != PIX_FMT_BGR8      && dstFormat != PIX_FMT_BGR8
           && srcFormat != PIX_FMT_RGB8      && dstFormat != PIX_FMT_RGB8
           && srcFormat != PIX_FMT_BGR4      && dstFormat != PIX_FMT_BGR4
           && srcFormat != PIX_FMT_RGB4      && dstFormat != PIX_FMT_RGB4
           && srcFormat != PIX_FMT_BGR4_BYTE && dstFormat != PIX_FMT_BGR4_BYTE
           && srcFormat != PIX_FMT_RGB4_BYTE && dstFormat != PIX_FMT_RGB4_BYTE
           && srcFormat != PIX_FMT_MONOBLACK && dstFormat != PIX_FMT_MONOBLACK
           && srcFormat != PIX_FMT_MONOWHITE && dstFormat != PIX_FMT_MONOWHITE
                                             && dstFormat != PIX_FMT_RGB32_1
                                             && dstFormat != PIX_FMT_BGR32_1
           && srcFormat != PIX_FMT_RGB48LE   && dstFormat != PIX_FMT_RGB48LE
           && srcFormat != PIX_FMT_RGB48BE   && dstFormat != PIX_FMT_RGB48BE
           && (!needsDither || (c->flags&(SWS_FAST_BILINEAR|SWS_POINT))))
             c->swScale= rgb2rgbWrapper;

        if ((usePal(srcFormat) && (
                 dstFormat == PIX_FMT_RGB32   ||
                 dstFormat == PIX_FMT_RGB32_1 ||
                 dstFormat == PIX_FMT_RGB24   ||
                 dstFormat == PIX_FMT_BGR32   ||
                 dstFormat == PIX_FMT_BGR32_1 ||
                 dstFormat == PIX_FMT_BGR24)))
             c->swScale= pal2rgbWrapper;

        if (srcFormat == PIX_FMT_YUV422P) {
            if (dstFormat == PIX_FMT_YUYV422)
                c->swScale= YUV422PToYuy2Wrapper;
            else if (dstFormat == PIX_FMT_UYVY422)
                c->swScale= YUV422PToUyvyWrapper;
        }

        /* LQ converters if -sws 0 or -sws 4*/
        if (c->flags&(SWS_FAST_BILINEAR|SWS_POINT)) {
            /* yv12_to_yuy2 */
            if (srcFormat == PIX_FMT_YUV420P || srcFormat == PIX_FMT_YUVA420P) {
                if (dstFormat == PIX_FMT_YUYV422)
                    c->swScale= PlanarToYuy2Wrapper;
                else if (dstFormat == PIX_FMT_UYVY422)
                    c->swScale= PlanarToUyvyWrapper;
            }
        }
        if(srcFormat == PIX_FMT_YUYV422 && (dstFormat == PIX_FMT_YUV420P || dstFormat == PIX_FMT_YUVA420P))
            c->swScale= YUYV2YUV420Wrapper;
        if(srcFormat == PIX_FMT_UYVY422 && (dstFormat == PIX_FMT_YUV420P || dstFormat == PIX_FMT_YUVA420P))
            c->swScale= UYVY2YUV420Wrapper;
        if(srcFormat == PIX_FMT_YUYV422 && dstFormat == PIX_FMT_YUV422P)
            c->swScale= YUYV2YUV422Wrapper;
        if(srcFormat == PIX_FMT_UYVY422 && dstFormat == PIX_FMT_YUV422P)
            c->swScale= UYVY2YUV422Wrapper;

#ifdef COMPILE_ALTIVEC
        if ((c->flags & SWS_CPU_CAPS_ALTIVEC) &&
            !(c->flags & SWS_BITEXACT) &&
            srcFormat == PIX_FMT_YUV420P) {
          // unscaled YV12 -> packed YUV, we want speed
          if (dstFormat == PIX_FMT_YUYV422)
              c->swScale= yv12toyuy2_unscaled_altivec;
          else if (dstFormat == PIX_FMT_UYVY422)
              c->swScale= yv12touyvy_unscaled_altivec;
        }
#endif

        /* simple copy */
        if (  srcFormat == dstFormat
            || (srcFormat == PIX_FMT_YUVA420P && dstFormat == PIX_FMT_YUV420P)
            || (srcFormat == PIX_FMT_YUV420P && dstFormat == PIX_FMT_YUVA420P)
            || (isPlanarYUV(srcFormat) && isGray(dstFormat))
            || (isPlanarYUV(dstFormat) && isGray(srcFormat))
            || (isGray(dstFormat) && isGray(srcFormat))
            || (isPlanarYUV(srcFormat) && isPlanarYUV(dstFormat)
                && c->chrDstHSubSample == c->chrSrcHSubSample
                && c->chrDstVSubSample == c->chrSrcVSubSample
                && dstFormat != PIX_FMT_NV12 && dstFormat != PIX_FMT_NV21
                && srcFormat != PIX_FMT_NV12 && srcFormat != PIX_FMT_NV21))
        {
            if (isPacked(c->srcFormat))
                c->swScale= packedCopy;
            else /* Planar YUV or gray */
                c->swScale= planarCopy;
        }
#if ARCH_BFIN
        if (flags & SWS_CPU_CAPS_BFIN)
            ff_bfin_get_unscaled_swscale (c);
#endif

        if (c->swScale) {
            if (flags&SWS_PRINT_INFO)
                av_log(c, AV_LOG_INFO, "using unscaled %s -> %s special converter\n",
                       sws_format_name(srcFormat), sws_format_name(dstFormat));
            return c;
        }
    }

    if (flags & SWS_CPU_CAPS_MMX2) {
        c->canMMX2BeUsed= (dstW >=srcW && (dstW&31)==0 && (srcW&15)==0) ? 1 : 0;
        if (!c->canMMX2BeUsed && dstW >=srcW && (srcW&15)==0 && (flags&SWS_FAST_BILINEAR)) {
            if (flags&SWS_PRINT_INFO)
                av_log(c, AV_LOG_INFO, "output width is not a multiple of 32 -> no MMX2 scaler\n");
        }
        if (usesHFilter) c->canMMX2BeUsed=0;
    }
    else
        c->canMMX2BeUsed=0;

    c->chrXInc= ((c->chrSrcW<<16) + (c->chrDstW>>1))/c->chrDstW;
    c->chrYInc= ((c->chrSrcH<<16) + (c->chrDstH>>1))/c->chrDstH;

    // match pixel 0 of the src to pixel 0 of dst and match pixel n-2 of src to pixel n-2 of dst
    // but only for the FAST_BILINEAR mode otherwise do correct scaling
    // n-2 is the last chrominance sample available
    // this is not perfect, but no one should notice the difference, the more correct variant
    // would be like the vertical one, but that would require some special code for the
    // first and last pixel
    if (flags&SWS_FAST_BILINEAR) {
        if (c->canMMX2BeUsed) {
            c->lumXInc+= 20;
            c->chrXInc+= 20;
        }
        //we don't use the x86 asm scaler if MMX is available
        else if (flags & SWS_CPU_CAPS_MMX) {
            c->lumXInc = ((srcW-2)<<16)/(dstW-2) - 20;
            c->chrXInc = ((c->chrSrcW-2)<<16)/(c->chrDstW-2) - 20;
        }
    }

    /* precalculate horizontal scaler filter coefficients */
    {
        const int filterAlign=
            (flags & SWS_CPU_CAPS_MMX) ? 4 :
            (flags & SWS_CPU_CAPS_ALTIVEC) ? 8 :
            1;

        if (initFilter(&c->hLumFilter, &c->hLumFilterPos, &c->hLumFilterSize, c->lumXInc,
                       srcW      ,       dstW, filterAlign, 1<<14,
                       (flags&SWS_BICUBLIN) ? (flags|SWS_BICUBIC)  : flags,
                       srcFilter->lumH, dstFilter->lumH, c->param) < 0)
            goto fail;
        if (initFilter(&c->hChrFilter, &c->hChrFilterPos, &c->hChrFilterSize, c->chrXInc,
                       c->chrSrcW, c->chrDstW, filterAlign, 1<<14,
                       (flags&SWS_BICUBLIN) ? (flags|SWS_BILINEAR) : flags,
                       srcFilter->chrH, dstFilter->chrH, c->param) < 0)
            goto fail;

#if defined(COMPILE_MMX2)
// can't downscale !!!
        if (c->canMMX2BeUsed && (flags & SWS_FAST_BILINEAR)) {
            c->lumMmx2FilterCodeSize = initMMX2HScaler(      dstW, c->lumXInc, NULL, NULL, NULL, 8);
            c->chrMmx2FilterCodeSize = initMMX2HScaler(c->chrDstW, c->chrXInc, NULL, NULL, NULL, 4);

#ifdef MAP_ANONYMOUS
            c->lumMmx2FilterCode = mmap(NULL, c->lumMmx2FilterCodeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            c->chrMmx2FilterCode = mmap(NULL, c->chrMmx2FilterCodeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
#elif HAVE_VIRTUALALLOC
            c->lumMmx2FilterCode = VirtualAlloc(NULL, c->lumMmx2FilterCodeSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            c->chrMmx2FilterCode = VirtualAlloc(NULL, c->chrMmx2FilterCodeSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
            c->lumMmx2FilterCode = av_malloc(c->lumMmx2FilterCodeSize);
            c->chrMmx2FilterCode = av_malloc(c->chrMmx2FilterCodeSize);
#endif

            FF_ALLOCZ_OR_GOTO(c, c->lumMmx2Filter   , (dstW        /8+8)*sizeof(int16_t), fail);
            FF_ALLOCZ_OR_GOTO(c, c->chrMmx2Filter   , (c->chrDstW  /4+8)*sizeof(int16_t), fail);
            FF_ALLOCZ_OR_GOTO(c, c->lumMmx2FilterPos, (dstW      /2/8+8)*sizeof(int32_t), fail);
            FF_ALLOCZ_OR_GOTO(c, c->chrMmx2FilterPos, (c->chrDstW/2/4+8)*sizeof(int32_t), fail);

            initMMX2HScaler(      dstW, c->lumXInc, c->lumMmx2FilterCode, c->lumMmx2Filter, c->lumMmx2FilterPos, 8);
            initMMX2HScaler(c->chrDstW, c->chrXInc, c->chrMmx2FilterCode, c->chrMmx2Filter, c->chrMmx2FilterPos, 4);

#ifdef MAP_ANONYMOUS
            mprotect(c->lumMmx2FilterCode, c->lumMmx2FilterCodeSize, PROT_EXEC | PROT_READ);
            mprotect(c->chrMmx2FilterCode, c->chrMmx2FilterCodeSize, PROT_EXEC | PROT_READ);
#endif
        }
#endif /* defined(COMPILE_MMX2) */
    } // initialize horizontal stuff



    /* precalculate vertical scaler filter coefficients */
    {
        const int filterAlign=
            (flags & SWS_CPU_CAPS_MMX) && (flags & SWS_ACCURATE_RND) ? 2 :
            (flags & SWS_CPU_CAPS_ALTIVEC) ? 8 :
            1;

        if (initFilter(&c->vLumFilter, &c->vLumFilterPos, &c->vLumFilterSize, c->lumYInc,
                       srcH      ,        dstH, filterAlign, (1<<12),
                       (flags&SWS_BICUBLIN) ? (flags|SWS_BICUBIC)  : flags,
                       srcFilter->lumV, dstFilter->lumV, c->param) < 0)
            goto fail;
        if (initFilter(&c->vChrFilter, &c->vChrFilterPos, &c->vChrFilterSize, c->chrYInc,
                       c->chrSrcH, c->chrDstH, filterAlign, (1<<12),
                       (flags&SWS_BICUBLIN) ? (flags|SWS_BILINEAR) : flags,
                       srcFilter->chrV, dstFilter->chrV, c->param) < 0)
            goto fail;

#ifdef COMPILE_ALTIVEC
        FF_ALLOC_OR_GOTO(c, c->vYCoeffsBank, sizeof (vector signed short)*c->vLumFilterSize*c->dstH, fail);
        FF_ALLOC_OR_GOTO(c, c->vCCoeffsBank, sizeof (vector signed short)*c->vChrFilterSize*c->chrDstH, fail);

        for (i=0;i<c->vLumFilterSize*c->dstH;i++) {
            int j;
            short *p = (short *)&c->vYCoeffsBank[i];
            for (j=0;j<8;j++)
                p[j] = c->vLumFilter[i];
        }

        for (i=0;i<c->vChrFilterSize*c->chrDstH;i++) {
            int j;
            short *p = (short *)&c->vCCoeffsBank[i];
            for (j=0;j<8;j++)
                p[j] = c->vChrFilter[i];
        }
#endif
    }

    // calculate buffer sizes so that they won't run out while handling these damn slices
    c->vLumBufSize= c->vLumFilterSize;
    c->vChrBufSize= c->vChrFilterSize;
    for (i=0; i<dstH; i++) {
        int chrI= i*c->chrDstH / dstH;
        int nextSlice= FFMAX(c->vLumFilterPos[i   ] + c->vLumFilterSize - 1,
                           ((c->vChrFilterPos[chrI] + c->vChrFilterSize - 1)<<c->chrSrcVSubSample));

        nextSlice>>= c->chrSrcVSubSample;
        nextSlice<<= c->chrSrcVSubSample;
        if (c->vLumFilterPos[i   ] + c->vLumBufSize < nextSlice)
            c->vLumBufSize= nextSlice - c->vLumFilterPos[i];
        if (c->vChrFilterPos[chrI] + c->vChrBufSize < (nextSlice>>c->chrSrcVSubSample))
            c->vChrBufSize= (nextSlice>>c->chrSrcVSubSample) - c->vChrFilterPos[chrI];
    }

    // allocate pixbufs (we use dynamic allocation because otherwise we would need to
    // allocate several megabytes to handle all possible cases)
    FF_ALLOC_OR_GOTO(c, c->lumPixBuf, c->vLumBufSize*2*sizeof(int16_t*), fail);
    FF_ALLOC_OR_GOTO(c, c->chrPixBuf, c->vChrBufSize*2*sizeof(int16_t*), fail);
    if (CONFIG_SWSCALE_ALPHA && isALPHA(c->srcFormat) && isALPHA(c->dstFormat))
        FF_ALLOCZ_OR_GOTO(c, c->alpPixBuf, c->vLumBufSize*2*sizeof(int16_t*), fail);
    //Note we need at least one pixel more at the end because of the MMX code (just in case someone wanna replace the 4000/8000)
    /* align at 16 bytes for AltiVec */
    for (i=0; i<c->vLumBufSize; i++) {
        FF_ALLOCZ_OR_GOTO(c, c->lumPixBuf[i+c->vLumBufSize], VOF+1, fail);
        c->lumPixBuf[i] = c->lumPixBuf[i+c->vLumBufSize];
    }
    for (i=0; i<c->vChrBufSize; i++) {
        FF_ALLOC_OR_GOTO(c, c->chrPixBuf[i+c->vChrBufSize], (VOF+1)*2, fail);
        c->chrPixBuf[i] = c->chrPixBuf[i+c->vChrBufSize];
    }
    if (CONFIG_SWSCALE_ALPHA && c->alpPixBuf)
        for (i=0; i<c->vLumBufSize; i++) {
            FF_ALLOCZ_OR_GOTO(c, c->alpPixBuf[i+c->vLumBufSize], VOF+1, fail);
            c->alpPixBuf[i] = c->alpPixBuf[i+c->vLumBufSize];
        }

    //try to avoid drawing green stuff between the right end and the stride end
    for (i=0; i<c->vChrBufSize; i++) memset(c->chrPixBuf[i], 64, (VOF+1)*2);

    assert(2*VOFW == VOF);

    assert(c->chrDstH <= dstH);

    if (flags&SWS_PRINT_INFO) {
#ifdef DITHER1XBPP
        const char *dither= " dithered";
#else
        const char *dither= "";
#endif
        if (flags&SWS_FAST_BILINEAR)
            av_log(c, AV_LOG_INFO, "FAST_BILINEAR scaler, ");
        else if (flags&SWS_BILINEAR)
            av_log(c, AV_LOG_INFO, "BILINEAR scaler, ");
        else if (flags&SWS_BICUBIC)
            av_log(c, AV_LOG_INFO, "BICUBIC scaler, ");
        else if (flags&SWS_X)
            av_log(c, AV_LOG_INFO, "Experimental scaler, ");
        else if (flags&SWS_POINT)
            av_log(c, AV_LOG_INFO, "Nearest Neighbor / POINT scaler, ");
        else if (flags&SWS_AREA)
            av_log(c, AV_LOG_INFO, "Area Averageing scaler, ");
        else if (flags&SWS_BICUBLIN)
            av_log(c, AV_LOG_INFO, "luma BICUBIC / chroma BILINEAR scaler, ");
        else if (flags&SWS_GAUSS)
            av_log(c, AV_LOG_INFO, "Gaussian scaler, ");
        else if (flags&SWS_SINC)
            av_log(c, AV_LOG_INFO, "Sinc scaler, ");
        else if (flags&SWS_LANCZOS)
            av_log(c, AV_LOG_INFO, "Lanczos scaler, ");
        else if (flags&SWS_SPLINE)
            av_log(c, AV_LOG_INFO, "Bicubic spline scaler, ");
        else
            av_log(c, AV_LOG_INFO, "ehh flags invalid?! ");

        if (dstFormat==PIX_FMT_BGR555 || dstFormat==PIX_FMT_BGR565)
            av_log(c, AV_LOG_INFO, "from %s to%s %s ",
                   sws_format_name(srcFormat), dither, sws_format_name(dstFormat));
        else
            av_log(c, AV_LOG_INFO, "from %s to %s ",
                   sws_format_name(srcFormat), sws_format_name(dstFormat));

        if (flags & SWS_CPU_CAPS_MMX2)
            av_log(c, AV_LOG_INFO, "using MMX2\n");
        else if (flags & SWS_CPU_CAPS_3DNOW)
            av_log(c, AV_LOG_INFO, "using 3DNOW\n");
        else if (flags & SWS_CPU_CAPS_MMX)
            av_log(c, AV_LOG_INFO, "using MMX\n");
        else if (flags & SWS_CPU_CAPS_ALTIVEC)
            av_log(c, AV_LOG_INFO, "using AltiVec\n");
        else
            av_log(c, AV_LOG_INFO, "using C\n");
    }

    if (flags & SWS_PRINT_INFO) {
        if (flags & SWS_CPU_CAPS_MMX) {
            if (c->canMMX2BeUsed && (flags&SWS_FAST_BILINEAR))
                av_log(c, AV_LOG_VERBOSE, "using FAST_BILINEAR MMX2 scaler for horizontal scaling\n");
            else {
                if (c->hLumFilterSize==4)
                    av_log(c, AV_LOG_VERBOSE, "using 4-tap MMX scaler for horizontal luminance scaling\n");
                else if (c->hLumFilterSize==8)
                    av_log(c, AV_LOG_VERBOSE, "using 8-tap MMX scaler for horizontal luminance scaling\n");
                else
                    av_log(c, AV_LOG_VERBOSE, "using n-tap MMX scaler for horizontal luminance scaling\n");

                if (c->hChrFilterSize==4)
                    av_log(c, AV_LOG_VERBOSE, "using 4-tap MMX scaler for horizontal chrominance scaling\n");
                else if (c->hChrFilterSize==8)
                    av_log(c, AV_LOG_VERBOSE, "using 8-tap MMX scaler for horizontal chrominance scaling\n");
                else
                    av_log(c, AV_LOG_VERBOSE, "using n-tap MMX scaler for horizontal chrominance scaling\n");
            }
        } else {
#if ARCH_X86
            av_log(c, AV_LOG_VERBOSE, "using x86 asm scaler for horizontal scaling\n");
#else
            if (flags & SWS_FAST_BILINEAR)
                av_log(c, AV_LOG_VERBOSE, "using FAST_BILINEAR C scaler for horizontal scaling\n");
            else
                av_log(c, AV_LOG_VERBOSE, "using C scaler for horizontal scaling\n");
#endif
        }
        if (isPlanarYUV(dstFormat)) {
            if (c->vLumFilterSize==1)
                av_log(c, AV_LOG_VERBOSE, "using 1-tap %s \"scaler\" for vertical scaling (YV12 like)\n", (flags & SWS_CPU_CAPS_MMX) ? "MMX" : "C");
            else
                av_log(c, AV_LOG_VERBOSE, "using n-tap %s scaler for vertical scaling (YV12 like)\n", (flags & SWS_CPU_CAPS_MMX) ? "MMX" : "C");
        } else {
            if (c->vLumFilterSize==1 && c->vChrFilterSize==2)
                av_log(c, AV_LOG_VERBOSE, "using 1-tap %s \"scaler\" for vertical luminance scaling (BGR)\n"
                       "      2-tap scaler for vertical chrominance scaling (BGR)\n", (flags & SWS_CPU_CAPS_MMX) ? "MMX" : "C");
            else if (c->vLumFilterSize==2 && c->vChrFilterSize==2)
                av_log(c, AV_LOG_VERBOSE, "using 2-tap linear %s scaler for vertical scaling (BGR)\n", (flags & SWS_CPU_CAPS_MMX) ? "MMX" : "C");
            else
                av_log(c, AV_LOG_VERBOSE, "using n-tap %s scaler for vertical scaling (BGR)\n", (flags & SWS_CPU_CAPS_MMX) ? "MMX" : "C");
        }

        if (dstFormat==PIX_FMT_BGR24)
            av_log(c, AV_LOG_VERBOSE, "using %s YV12->BGR24 converter\n",
                   (flags & SWS_CPU_CAPS_MMX2) ? "MMX2" : ((flags & SWS_CPU_CAPS_MMX) ? "MMX" : "C"));
        else if (dstFormat==PIX_FMT_RGB32)
            av_log(c, AV_LOG_VERBOSE, "using %s YV12->BGR32 converter\n", (flags & SWS_CPU_CAPS_MMX) ? "MMX" : "C");
        else if (dstFormat==PIX_FMT_BGR565)
            av_log(c, AV_LOG_VERBOSE, "using %s YV12->BGR16 converter\n", (flags & SWS_CPU_CAPS_MMX) ? "MMX" : "C");
        else if (dstFormat==PIX_FMT_BGR555)
            av_log(c, AV_LOG_VERBOSE, "using %s YV12->BGR15 converter\n", (flags & SWS_CPU_CAPS_MMX) ? "MMX" : "C");

        av_log(c, AV_LOG_VERBOSE, "%dx%d -> %dx%d\n", srcW, srcH, dstW, dstH);
    }
    if (flags & SWS_PRINT_INFO) {
        av_log(c, AV_LOG_DEBUG, "lum srcW=%d srcH=%d dstW=%d dstH=%d xInc=%d yInc=%d\n",
               c->srcW, c->srcH, c->dstW, c->dstH, c->lumXInc, c->lumYInc);
        av_log(c, AV_LOG_DEBUG, "chr srcW=%d srcH=%d dstW=%d dstH=%d xInc=%d yInc=%d\n",
               c->chrSrcW, c->chrSrcH, c->chrDstW, c->chrDstH, c->chrXInc, c->chrYInc);
    }

    c->swScale= getSwsFunc(c);
    return c;

fail:
    sws_freeContext(c);
    return NULL;
}

static void reset_ptr(uint8_t* src[], int format)
{
    if(!isALPHA(format))
        src[3]=NULL;
    if(!isPlanarYUV(format)) {
        src[3]=src[2]=NULL;
        if(   format != PIX_FMT_PAL8
           && format != PIX_FMT_RGB8
           && format != PIX_FMT_BGR8
           && format != PIX_FMT_RGB4_BYTE
           && format != PIX_FMT_BGR4_BYTE
          )
            src[1]= NULL;
    }
}

/**
 * swscale wrapper, so we don't need to export the SwsContext.
 * Assumes planar YUV to be in YUV order instead of YVU.
 */
int sws_scale(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
              int srcSliceH, uint8_t* dst[], int dstStride[])
{
    int i;
    uint8_t* src2[4]= {src[0], src[1], src[2], src[3]};
    uint8_t* dst2[4]= {dst[0], dst[1], dst[2], dst[3]};

    // do not mess up sliceDir if we have a "trailing" 0-size slice
    if (srcSliceH == 0)
        return 0;

    if (c->sliceDir == 0 && srcSliceY != 0 && srcSliceY + srcSliceH != c->srcH) {
        av_log(c, AV_LOG_ERROR, "Slices start in the middle!\n");
        return 0;
    }
    if (c->sliceDir == 0) {
        if (srcSliceY == 0) c->sliceDir = 1; else c->sliceDir = -1;
    }

    if (usePal(c->srcFormat)) {
        for (i=0; i<256; i++) {
            int p, r, g, b,y,u,v;
            if(c->srcFormat == PIX_FMT_PAL8) {
                p=((uint32_t*)(src[1]))[i];
                r= (p>>16)&0xFF;
                g= (p>> 8)&0xFF;
                b=  p     &0xFF;
            } else if(c->srcFormat == PIX_FMT_RGB8) {
                r= (i>>5    )*36;
                g= ((i>>2)&7)*36;
                b= (i&3     )*85;
            } else if(c->srcFormat == PIX_FMT_BGR8) {
                b= (i>>6    )*85;
                g= ((i>>3)&7)*36;
                r= (i&7     )*36;
            } else if(c->srcFormat == PIX_FMT_RGB4_BYTE) {
                r= (i>>3    )*255;
                g= ((i>>1)&3)*85;
                b= (i&1     )*255;
            } else {
                assert(c->srcFormat == PIX_FMT_BGR4_BYTE);
                b= (i>>3    )*255;
                g= ((i>>1)&3)*85;
                r= (i&1     )*255;
            }
            y= av_clip_uint8((RY*r + GY*g + BY*b + ( 33<<(RGB2YUV_SHIFT-1)))>>RGB2YUV_SHIFT);
            u= av_clip_uint8((RU*r + GU*g + BU*b + (257<<(RGB2YUV_SHIFT-1)))>>RGB2YUV_SHIFT);
            v= av_clip_uint8((RV*r + GV*g + BV*b + (257<<(RGB2YUV_SHIFT-1)))>>RGB2YUV_SHIFT);
            c->pal_yuv[i]= y + (u<<8) + (v<<16);


            switch(c->dstFormat) {
            case PIX_FMT_BGR32:
#if !HAVE_BIGENDIAN
            case PIX_FMT_RGB24:
#endif
                c->pal_rgb[i]=  r + (g<<8) + (b<<16);
                break;
            case PIX_FMT_BGR32_1:
#if HAVE_BIGENDIAN
            case PIX_FMT_BGR24:
#endif
                c->pal_rgb[i]= (r + (g<<8) + (b<<16)) << 8;
                break;
            case PIX_FMT_RGB32_1:
#if HAVE_BIGENDIAN
            case PIX_FMT_RGB24:
#endif
                c->pal_rgb[i]= (b + (g<<8) + (r<<16)) << 8;
                break;
            case PIX_FMT_RGB32:
#if !HAVE_BIGENDIAN
            case PIX_FMT_BGR24:
#endif
            default:
                c->pal_rgb[i]=  b + (g<<8) + (r<<16);
            }
        }
    }

    // copy strides, so they can safely be modified
    if (c->sliceDir == 1) {
        // slices go from top to bottom
        int srcStride2[4]= {srcStride[0], srcStride[1], srcStride[2], srcStride[3]};
        int dstStride2[4]= {dstStride[0], dstStride[1], dstStride[2], dstStride[3]};

        reset_ptr(src2, c->srcFormat);
        reset_ptr(dst2, c->dstFormat);

        /* reset slice direction at end of frame */
        if (srcSliceY + srcSliceH == c->srcH)
            c->sliceDir = 0;

        return c->swScale(c, src2, srcStride2, srcSliceY, srcSliceH, dst2, dstStride2);
    } else {
        // slices go from bottom to top => we flip the image internally
        int srcStride2[4]= {-srcStride[0], -srcStride[1], -srcStride[2], -srcStride[3]};
        int dstStride2[4]= {-dstStride[0], -dstStride[1], -dstStride[2], -dstStride[3]};

        src2[0] += (srcSliceH-1)*srcStride[0];
        if (!usePal(c->srcFormat))
            src2[1] += ((srcSliceH>>c->chrSrcVSubSample)-1)*srcStride[1];
        src2[2] += ((srcSliceH>>c->chrSrcVSubSample)-1)*srcStride[2];
        src2[3] += (srcSliceH-1)*srcStride[3];
        dst2[0] += ( c->dstH                      -1)*dstStride[0];
        dst2[1] += ((c->dstH>>c->chrDstVSubSample)-1)*dstStride[1];
        dst2[2] += ((c->dstH>>c->chrDstVSubSample)-1)*dstStride[2];
        dst2[3] += ( c->dstH                      -1)*dstStride[3];

        reset_ptr(src2, c->srcFormat);
        reset_ptr(dst2, c->dstFormat);

        /* reset slice direction at end of frame */
        if (!srcSliceY)
            c->sliceDir = 0;

        return c->swScale(c, src2, srcStride2, c->srcH-srcSliceY-srcSliceH, srcSliceH, dst2, dstStride2);
    }
}

#if LIBSWSCALE_VERSION_MAJOR < 1
int sws_scale_ordered(SwsContext *c, uint8_t* src[], int srcStride[], int srcSliceY,
                      int srcSliceH, uint8_t* dst[], int dstStride[])
{
    return sws_scale(c, src, srcStride, srcSliceY, srcSliceH, dst, dstStride);
}
#endif

SwsFilter *sws_getDefaultFilter(float lumaGBlur, float chromaGBlur,
                                float lumaSharpen, float chromaSharpen,
                                float chromaHShift, float chromaVShift,
                                int verbose)
{
    SwsFilter *filter= av_malloc(sizeof(SwsFilter));
    if (!filter)
        return NULL;

    if (lumaGBlur!=0.0) {
        filter->lumH= sws_getGaussianVec(lumaGBlur, 3.0);
        filter->lumV= sws_getGaussianVec(lumaGBlur, 3.0);
    } else {
        filter->lumH= sws_getIdentityVec();
        filter->lumV= sws_getIdentityVec();
    }

    if (chromaGBlur!=0.0) {
        filter->chrH= sws_getGaussianVec(chromaGBlur, 3.0);
        filter->chrV= sws_getGaussianVec(chromaGBlur, 3.0);
    } else {
        filter->chrH= sws_getIdentityVec();
        filter->chrV= sws_getIdentityVec();
    }

    if (chromaSharpen!=0.0) {
        SwsVector *id= sws_getIdentityVec();
        sws_scaleVec(filter->chrH, -chromaSharpen);
        sws_scaleVec(filter->chrV, -chromaSharpen);
        sws_addVec(filter->chrH, id);
        sws_addVec(filter->chrV, id);
        sws_freeVec(id);
    }

    if (lumaSharpen!=0.0) {
        SwsVector *id= sws_getIdentityVec();
        sws_scaleVec(filter->lumH, -lumaSharpen);
        sws_scaleVec(filter->lumV, -lumaSharpen);
        sws_addVec(filter->lumH, id);
        sws_addVec(filter->lumV, id);
        sws_freeVec(id);
    }

    if (chromaHShift != 0.0)
        sws_shiftVec(filter->chrH, (int)(chromaHShift+0.5));

    if (chromaVShift != 0.0)
        sws_shiftVec(filter->chrV, (int)(chromaVShift+0.5));

    sws_normalizeVec(filter->chrH, 1.0);
    sws_normalizeVec(filter->chrV, 1.0);
    sws_normalizeVec(filter->lumH, 1.0);
    sws_normalizeVec(filter->lumV, 1.0);

    if (verbose) sws_printVec2(filter->chrH, NULL, AV_LOG_DEBUG);
    if (verbose) sws_printVec2(filter->lumH, NULL, AV_LOG_DEBUG);

    return filter;
}

SwsVector *sws_allocVec(int length)
{
    SwsVector *vec = av_malloc(sizeof(SwsVector));
    if (!vec)
        return NULL;
    vec->length = length;
    vec->coeff  = av_malloc(sizeof(double) * length);
    if (!vec->coeff)
        av_freep(&vec);
    return vec;
}

SwsVector *sws_getGaussianVec(double variance, double quality)
{
    const int length= (int)(variance*quality + 0.5) | 1;
    int i;
    double middle= (length-1)*0.5;
    SwsVector *vec= sws_allocVec(length);

    if (!vec)
        return NULL;

    for (i=0; i<length; i++) {
        double dist= i-middle;
        vec->coeff[i]= exp(-dist*dist/(2*variance*variance)) / sqrt(2*variance*PI);
    }

    sws_normalizeVec(vec, 1.0);

    return vec;
}

SwsVector *sws_getConstVec(double c, int length)
{
    int i;
    SwsVector *vec= sws_allocVec(length);

    if (!vec)
        return NULL;

    for (i=0; i<length; i++)
        vec->coeff[i]= c;

    return vec;
}


SwsVector *sws_getIdentityVec(void)
{
    return sws_getConstVec(1.0, 1);
}

double sws_dcVec(SwsVector *a)
{
    int i;
    double sum=0;

    for (i=0; i<a->length; i++)
        sum+= a->coeff[i];

    return sum;
}

void sws_scaleVec(SwsVector *a, double scalar)
{
    int i;

    for (i=0; i<a->length; i++)
        a->coeff[i]*= scalar;
}

void sws_normalizeVec(SwsVector *a, double height)
{
    sws_scaleVec(a, height/sws_dcVec(a));
}

static SwsVector *sws_getConvVec(SwsVector *a, SwsVector *b)
{
    int length= a->length + b->length - 1;
    int i, j;
    SwsVector *vec= sws_getConstVec(0.0, length);

    if (!vec)
        return NULL;

    for (i=0; i<a->length; i++) {
        for (j=0; j<b->length; j++) {
            vec->coeff[i+j]+= a->coeff[i]*b->coeff[j];
        }
    }

    return vec;
}

static SwsVector *sws_sumVec(SwsVector *a, SwsVector *b)
{
    int length= FFMAX(a->length, b->length);
    int i;
    SwsVector *vec= sws_getConstVec(0.0, length);

    if (!vec)
        return NULL;

    for (i=0; i<a->length; i++) vec->coeff[i + (length-1)/2 - (a->length-1)/2]+= a->coeff[i];
    for (i=0; i<b->length; i++) vec->coeff[i + (length-1)/2 - (b->length-1)/2]+= b->coeff[i];

    return vec;
}

static SwsVector *sws_diffVec(SwsVector *a, SwsVector *b)
{
    int length= FFMAX(a->length, b->length);
    int i;
    SwsVector *vec= sws_getConstVec(0.0, length);

    if (!vec)
        return NULL;

    for (i=0; i<a->length; i++) vec->coeff[i + (length-1)/2 - (a->length-1)/2]+= a->coeff[i];
    for (i=0; i<b->length; i++) vec->coeff[i + (length-1)/2 - (b->length-1)/2]-= b->coeff[i];

    return vec;
}

/* shift left / or right if "shift" is negative */
static SwsVector *sws_getShiftedVec(SwsVector *a, int shift)
{
    int length= a->length + FFABS(shift)*2;
    int i;
    SwsVector *vec= sws_getConstVec(0.0, length);

    if (!vec)
        return NULL;

    for (i=0; i<a->length; i++) {
        vec->coeff[i + (length-1)/2 - (a->length-1)/2 - shift]= a->coeff[i];
    }

    return vec;
}

void sws_shiftVec(SwsVector *a, int shift)
{
    SwsVector *shifted= sws_getShiftedVec(a, shift);
    av_free(a->coeff);
    a->coeff= shifted->coeff;
    a->length= shifted->length;
    av_free(shifted);
}

void sws_addVec(SwsVector *a, SwsVector *b)
{
    SwsVector *sum= sws_sumVec(a, b);
    av_free(a->coeff);
    a->coeff= sum->coeff;
    a->length= sum->length;
    av_free(sum);
}

void sws_subVec(SwsVector *a, SwsVector *b)
{
    SwsVector *diff= sws_diffVec(a, b);
    av_free(a->coeff);
    a->coeff= diff->coeff;
    a->length= diff->length;
    av_free(diff);
}

void sws_convVec(SwsVector *a, SwsVector *b)
{
    SwsVector *conv= sws_getConvVec(a, b);
    av_free(a->coeff);
    a->coeff= conv->coeff;
    a->length= conv->length;
    av_free(conv);
}

SwsVector *sws_cloneVec(SwsVector *a)
{
    int i;
    SwsVector *vec= sws_allocVec(a->length);

    if (!vec)
        return NULL;

    for (i=0; i<a->length; i++) vec->coeff[i]= a->coeff[i];

    return vec;
}

void sws_printVec2(SwsVector *a, AVClass *log_ctx, int log_level)
{
    int i;
    double max=0;
    double min=0;
    double range;

    for (i=0; i<a->length; i++)
        if (a->coeff[i]>max) max= a->coeff[i];

    for (i=0; i<a->length; i++)
        if (a->coeff[i]<min) min= a->coeff[i];

    range= max - min;

    for (i=0; i<a->length; i++) {
        int x= (int)((a->coeff[i]-min)*60.0/range +0.5);
        av_log(log_ctx, log_level, "%1.3f ", a->coeff[i]);
        for (;x>0; x--) av_log(log_ctx, log_level, " ");
        av_log(log_ctx, log_level, "|\n");
    }
}

#if LIBSWSCALE_VERSION_MAJOR < 1
void sws_printVec(SwsVector *a)
{
    sws_printVec2(a, NULL, AV_LOG_DEBUG);
}
#endif

void sws_freeVec(SwsVector *a)
{
    if (!a) return;
    av_freep(&a->coeff);
    a->length=0;
    av_free(a);
}

void sws_freeFilter(SwsFilter *filter)
{
    if (!filter) return;

    if (filter->lumH) sws_freeVec(filter->lumH);
    if (filter->lumV) sws_freeVec(filter->lumV);
    if (filter->chrH) sws_freeVec(filter->chrH);
    if (filter->chrV) sws_freeVec(filter->chrV);
    av_free(filter);
}


void sws_freeContext(SwsContext *c)
{
    int i;
    if (!c) return;

    if (c->lumPixBuf) {
        for (i=0; i<c->vLumBufSize; i++)
            av_freep(&c->lumPixBuf[i]);
        av_freep(&c->lumPixBuf);
    }

    if (c->chrPixBuf) {
        for (i=0; i<c->vChrBufSize; i++)
            av_freep(&c->chrPixBuf[i]);
        av_freep(&c->chrPixBuf);
    }

    if (CONFIG_SWSCALE_ALPHA && c->alpPixBuf) {
        for (i=0; i<c->vLumBufSize; i++)
            av_freep(&c->alpPixBuf[i]);
        av_freep(&c->alpPixBuf);
    }

    av_freep(&c->vLumFilter);
    av_freep(&c->vChrFilter);
    av_freep(&c->hLumFilter);
    av_freep(&c->hChrFilter);
#ifdef COMPILE_ALTIVEC
    av_freep(&c->vYCoeffsBank);
    av_freep(&c->vCCoeffsBank);
#endif

    av_freep(&c->vLumFilterPos);
    av_freep(&c->vChrFilterPos);
    av_freep(&c->hLumFilterPos);
    av_freep(&c->hChrFilterPos);

#if ARCH_X86 && CONFIG_GPL
#ifdef MAP_ANONYMOUS
    if (c->lumMmx2FilterCode) munmap(c->lumMmx2FilterCode, c->lumMmx2FilterCodeSize);
    if (c->chrMmx2FilterCode) munmap(c->chrMmx2FilterCode, c->chrMmx2FilterCodeSize);
#elif HAVE_VIRTUALALLOC
    if (c->lumMmx2FilterCode) VirtualFree(c->lumMmx2FilterCode, c->lumMmx2FilterCodeSize, MEM_RELEASE);
    if (c->chrMmx2FilterCode) VirtualFree(c->chrMmx2FilterCode, c->chrMmx2FilterCodeSize, MEM_RELEASE);
#else
    av_free(c->lumMmx2FilterCode);
    av_free(c->chrMmx2FilterCode);
#endif
    c->lumMmx2FilterCode=NULL;
    c->chrMmx2FilterCode=NULL;
#endif /* ARCH_X86 && CONFIG_GPL */

    av_freep(&c->lumMmx2Filter);
    av_freep(&c->chrMmx2Filter);
    av_freep(&c->lumMmx2FilterPos);
    av_freep(&c->chrMmx2FilterPos);
    av_freep(&c->yuvTable);

    av_free(c);
}

struct SwsContext *sws_getCachedContext(struct SwsContext *context,
                                        int srcW, int srcH, enum PixelFormat srcFormat,
                                        int dstW, int dstH, enum PixelFormat dstFormat, int flags,
                                        SwsFilter *srcFilter, SwsFilter *dstFilter, const double *param)
{
    static const double default_param[2] = {SWS_PARAM_DEFAULT, SWS_PARAM_DEFAULT};

    if (!param)
        param = default_param;

    if (context) {
        if (context->srcW != srcW || context->srcH != srcH ||
            context->srcFormat != srcFormat ||
            context->dstW != dstW || context->dstH != dstH ||
            context->dstFormat != dstFormat || context->flags != flags ||
            context->param[0] != param[0] || context->param[1] != param[1])
        {
            sws_freeContext(context);
            context = NULL;
        }
    }
    if (!context) {
        return sws_getContext(srcW, srcH, srcFormat,
                              dstW, dstH, dstFormat, flags,
                              srcFilter, dstFilter, param);
    }
    return context;
}

