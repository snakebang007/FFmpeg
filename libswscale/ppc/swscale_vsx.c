/*
 * AltiVec-enhanced yuv2yuvX
 *
 * Copyright (C) 2004 Romain Dolbeau <romain@dolbeau.org>
 * based on the equivalent C code in swscale.c
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <inttypes.h>

#include "config.h"
#include "libswscale/swscale.h"
#include "libswscale/swscale_internal.h"
#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "yuv2rgb_altivec.h"
#include "libavutil/ppc/util_altivec.h"

#if HAVE_VSX
#define vzero vec_splat_s32(0)

#if !HAVE_BIGENDIAN
#define  GET_LS(a,b,c,s) {\
        ls  = a;\
        a = vec_vsx_ld(((b) << 1)  + 16, s);\
    }

#define yuv2planeX_8(d1, d2, l1, src, x, perm, filter) do {\
        vector signed short ls;\
        vector signed int   vf1, vf2, i1, i2;\
        GET_LS(l1, x, perm, src);\
        i1  = vec_mule(filter, ls);\
        i2  = vec_mulo(filter, ls);\
        vf1 = vec_mergeh(i1, i2);\
        vf2 = vec_mergel(i1, i2);\
        d1 = vec_add(d1, vf1);\
        d2 = vec_add(d2, vf2);\
    } while (0)

#define LOAD_FILTER(vf,f) {\
        vf = vec_vsx_ld(joffset, f);\
}
#define LOAD_L1(ll1,s,p){\
        ll1  = vec_vsx_ld(xoffset, s);\
}

// The 3 above is 2 (filterSize == 4) + 1 (sizeof(short) == 2).

// The neat trick: We only care for half the elements,
// high or low depending on (i<<3)%16 (it's 0 or 8 here),
// and we're going to use vec_mule, so we choose
// carefully how to "unpack" the elements into the even slots.
#define GET_VF4(a, vf, f) {\
    vf = (vector signed short)vec_vsx_ld(a << 3, f);\
    vf = vec_mergeh(vf, (vector signed short)vzero);\
}
#define FIRST_LOAD(sv, pos, s, per) {}
#define UPDATE_PTR(s0, d0, s1, d1) {}
#define LOAD_SRCV(pos, a, s, per, v0, v1, vf) {\
    vf = vec_vsx_ld(pos + a, s);\
}
#define LOAD_SRCV8(pos, a, s, per, v0, v1, vf) LOAD_SRCV(pos, a, s, per, v0, v1, vf)
#define GET_VFD(a, b, f, vf0, vf1, per, vf, off) {\
    vf  = vec_vsx_ld((a * 2 * filterSize) + (b * 2) + off, f);\
}

#define FUNC(name) name ## _vsx
#include "swscale_ppc_template.c"
#undef FUNC

#undef vzero

#endif /* !HAVE_BIGENDIAN */

static void yuv2plane1_8_u(const int16_t *src, uint8_t *dest, int dstW,
                           const uint8_t *dither, int offset, int start)
{
    int i;
    for (i = start; i < dstW; i++) {
        int val = (src[i] + dither[(i + offset) & 7]) >> 7;
        dest[i] = av_clip_uint8(val);
    }
}

static void yuv2plane1_8_vsx(const int16_t *src, uint8_t *dest, int dstW,
                           const uint8_t *dither, int offset)
{
    const int dst_u = -(uintptr_t)dest & 15;
    int i, j;
    LOCAL_ALIGNED(16, int16_t, val, [16]);
    const vector uint16_t shifts = (vector uint16_t) {7, 7, 7, 7, 7, 7, 7, 7};
    vector int16_t vi, vileft, ditherleft, ditherright;
    vector uint8_t vd;

    for (j = 0; j < 16; j++) {
        val[j] = dither[(dst_u + offset + j) & 7];
    }

    ditherleft = vec_ld(0, val);
    ditherright = vec_ld(0, &val[8]);

    yuv2plane1_8_u(src, dest, dst_u, dither, offset, 0);

    for (i = dst_u; i < dstW - 15; i += 16) {

        vi = vec_vsx_ld(0, &src[i]);
        vi = vec_adds(ditherleft, vi);
        vileft = vec_sra(vi, shifts);

        vi = vec_vsx_ld(0, &src[i + 8]);
        vi = vec_adds(ditherright, vi);
        vi = vec_sra(vi, shifts);

        vd = vec_packsu(vileft, vi);
        vec_st(vd, 0, &dest[i]);
    }

    yuv2plane1_8_u(src, dest, dstW, dither, offset, i);
}

#if !HAVE_BIGENDIAN

#define output_pixel(pos, val) \
    if (big_endian) { \
        AV_WB16(pos, av_clip_uintp2(val >> shift, output_bits)); \
    } else { \
        AV_WL16(pos, av_clip_uintp2(val >> shift, output_bits)); \
    }

static void yuv2plane1_nbps_u(const int16_t *src, uint16_t *dest, int dstW,
                              int big_endian, int output_bits, int start)
{
    int i;
    int shift = 15 - output_bits;

    for (i = start; i < dstW; i++) {
        int val = src[i] + (1 << (shift - 1));
        output_pixel(&dest[i], val);
    }
}

static void yuv2plane1_nbps_vsx(const int16_t *src, uint16_t *dest, int dstW,
                           int big_endian, int output_bits)
{
    const int dst_u = -(uintptr_t)dest & 7;
    const int shift = 15 - output_bits;
    const int add = (1 << (shift - 1));
    const int clip = (1 << output_bits) - 1;
    const vector uint16_t vadd = (vector uint16_t) {add, add, add, add, add, add, add, add};
    const vector uint16_t vswap = (vector uint16_t) vec_splat_u16(big_endian ? 8 : 0);
    const vector uint16_t vshift = (vector uint16_t) vec_splat_u16(shift);
    const vector uint16_t vlargest = (vector uint16_t) {clip, clip, clip, clip, clip, clip, clip, clip};
    vector uint16_t v;
    int i;

    yuv2plane1_nbps_u(src, dest, dst_u, big_endian, output_bits, 0);

    for (i = dst_u; i < dstW - 7; i += 8) {
        v = vec_vsx_ld(0, (const uint16_t *) &src[i]);
        v = vec_add(v, vadd);
        v = vec_sr(v, vshift);
        v = vec_min(v, vlargest);
        v = vec_rl(v, vswap);
        vec_st(v, 0, &dest[i]);
    }

    yuv2plane1_nbps_u(src, dest, dstW, big_endian, output_bits, i);
}

static void yuv2planeX_nbps_u(const int16_t *filter, int filterSize,
                              const int16_t **src, uint16_t *dest, int dstW,
                              int big_endian, int output_bits, int start)
{
    int i;
    int shift = 11 + 16 - output_bits;

    for (i = start; i < dstW; i++) {
        int val = 1 << (shift - 1);
        int j;

        for (j = 0; j < filterSize; j++)
            val += src[j][i] * filter[j];

        output_pixel(&dest[i], val);
    }
}

static void yuv2planeX_nbps_vsx(const int16_t *filter, int filterSize,
                                const int16_t **src, uint16_t *dest, int dstW,
                                int big_endian, int output_bits)
{
    const int dst_u = -(uintptr_t)dest & 7;
    const int shift = 11 + 16 - output_bits;
    const int add = (1 << (shift - 1));
    const int clip = (1 << output_bits) - 1;
    const uint16_t swap = big_endian ? 8 : 0;
    const vector uint32_t vadd = (vector uint32_t) {add, add, add, add};
    const vector uint32_t vshift = (vector uint32_t) {shift, shift, shift, shift};
    const vector uint16_t vswap = (vector uint16_t) {swap, swap, swap, swap, swap, swap, swap, swap};
    const vector uint16_t vlargest = (vector uint16_t) {clip, clip, clip, clip, clip, clip, clip, clip};
    const vector int16_t vzero = vec_splat_s16(0);
    const vector uint8_t vperm = (vector uint8_t) {0, 1, 8, 9, 2, 3, 10, 11, 4, 5, 12, 13, 6, 7, 14, 15};
    vector int16_t vfilter[MAX_FILTER_SIZE], vin;
    vector uint16_t v;
    vector uint32_t vleft, vright, vtmp;
    int i, j;

    for (i = 0; i < filterSize; i++) {
        vfilter[i] = (vector int16_t) {filter[i], filter[i], filter[i], filter[i],
                                       filter[i], filter[i], filter[i], filter[i]};
    }

    yuv2planeX_nbps_u(filter, filterSize, src, dest, dst_u, big_endian, output_bits, 0);

    for (i = dst_u; i < dstW - 7; i += 8) {
        vleft = vright = vadd;

        for (j = 0; j < filterSize; j++) {
            vin = vec_vsx_ld(0, &src[j][i]);
            vtmp = (vector uint32_t) vec_mule(vin, vfilter[j]);
            vleft = vec_add(vleft, vtmp);
            vtmp = (vector uint32_t) vec_mulo(vin, vfilter[j]);
            vright = vec_add(vright, vtmp);
        }

        vleft = vec_sra(vleft, vshift);
        vright = vec_sra(vright, vshift);
        v = vec_packsu(vleft, vright);
        v = (vector uint16_t) vec_max((vector int16_t) v, vzero);
        v = vec_min(v, vlargest);
        v = vec_rl(v, vswap);
        v = vec_perm(v, v, vperm);
        vec_st(v, 0, &dest[i]);
    }

    yuv2planeX_nbps_u(filter, filterSize, src, dest, dstW, big_endian, output_bits, i);
}


#undef output_pixel

#define output_pixel(pos, val, bias, signedness) \
    if (big_endian) { \
        AV_WB16(pos, bias + av_clip_ ## signedness ## 16(val >> shift)); \
    } else { \
        AV_WL16(pos, bias + av_clip_ ## signedness ## 16(val >> shift)); \
    }

static void yuv2plane1_16_u(const int32_t *src, uint16_t *dest, int dstW,
                              int big_endian, int output_bits, int start)
{
    int i;
    const int shift = 3;

    for (i = start; i < dstW; i++) {
        int val = src[i] + (1 << (shift - 1));
        output_pixel(&dest[i], val, 0, uint);
    }
}

static void yuv2plane1_16_vsx(const int32_t *src, uint16_t *dest, int dstW,
                           int big_endian, int output_bits)
{
    const int dst_u = -(uintptr_t)dest & 7;
    const int shift = 3;
    const int add = (1 << (shift - 1));
    const vector uint32_t vadd = (vector uint32_t) {add, add, add, add};
    const vector uint16_t vswap = (vector uint16_t) vec_splat_u16(big_endian ? 8 : 0);
    const vector uint32_t vshift = (vector uint32_t) vec_splat_u32(shift);
    vector uint32_t v, v2;
    vector uint16_t vd;
    int i;

    yuv2plane1_16_u(src, dest, dst_u, big_endian, output_bits, 0);

    for (i = dst_u; i < dstW - 7; i += 8) {
        v = vec_vsx_ld(0, (const uint32_t *) &src[i]);
        v = vec_add(v, vadd);
        v = vec_sr(v, vshift);

        v2 = vec_vsx_ld(0, (const uint32_t *) &src[i + 4]);
        v2 = vec_add(v2, vadd);
        v2 = vec_sr(v2, vshift);

        vd = vec_packsu(v, v2);
        vd = vec_rl(vd, vswap);

        vec_st(vd, 0, &dest[i]);
    }

    yuv2plane1_16_u(src, dest, dstW, big_endian, output_bits, i);
}

#if HAVE_POWER8

static void yuv2planeX_16_u(const int16_t *filter, int filterSize,
                            const int32_t **src, uint16_t *dest, int dstW,
                            int big_endian, int output_bits, int start)
{
    int i;
    int shift = 15;

    for (i = start; i < dstW; i++) {
        int val = 1 << (shift - 1);
        int j;

        /* range of val is [0,0x7FFFFFFF], so 31 bits, but with lanczos/spline
         * filters (or anything with negative coeffs, the range can be slightly
         * wider in both directions. To account for this overflow, we subtract
         * a constant so it always fits in the signed range (assuming a
         * reasonable filterSize), and re-add that at the end. */
        val -= 0x40000000;
        for (j = 0; j < filterSize; j++)
            val += src[j][i] * (unsigned)filter[j];

        output_pixel(&dest[i], val, 0x8000, int);
    }
}

static void yuv2planeX_16_vsx(const int16_t *filter, int filterSize,
                              const int32_t **src, uint16_t *dest, int dstW,
                              int big_endian, int output_bits)
{
    const int dst_u = -(uintptr_t)dest & 7;
    const int shift = 15;
    const int bias = 0x8000;
    const int add = (1 << (shift - 1)) - 0x40000000;
    const uint16_t swap = big_endian ? 8 : 0;
    const vector uint32_t vadd = (vector uint32_t) {add, add, add, add};
    const vector uint32_t vshift = (vector uint32_t) {shift, shift, shift, shift};
    const vector uint16_t vswap = (vector uint16_t) {swap, swap, swap, swap, swap, swap, swap, swap};
    const vector uint16_t vbias = (vector uint16_t) {bias, bias, bias, bias, bias, bias, bias, bias};
    vector int32_t vfilter[MAX_FILTER_SIZE];
    vector uint16_t v;
    vector uint32_t vleft, vright, vtmp;
    vector int32_t vin32l, vin32r;
    int i, j;

    for (i = 0; i < filterSize; i++) {
        vfilter[i] = (vector int32_t) {filter[i], filter[i], filter[i], filter[i]};
    }

    yuv2planeX_16_u(filter, filterSize, src, dest, dst_u, big_endian, output_bits, 0);

    for (i = dst_u; i < dstW - 7; i += 8) {
        vleft = vright = vadd;

        for (j = 0; j < filterSize; j++) {
            vin32l = vec_vsx_ld(0, &src[j][i]);
            vin32r = vec_vsx_ld(0, &src[j][i + 4]);

            vtmp = (vector uint32_t) vec_mul(vin32l, vfilter[j]);
            vleft = vec_add(vleft, vtmp);
            vtmp = (vector uint32_t) vec_mul(vin32r, vfilter[j]);
            vright = vec_add(vright, vtmp);
        }

        vleft = vec_sra(vleft, vshift);
        vright = vec_sra(vright, vshift);
        v = (vector uint16_t) vec_packs((vector int32_t) vleft, (vector int32_t) vright);
        v = vec_add(v, vbias);
        v = vec_rl(v, vswap);
        vec_st(v, 0, &dest[i]);
    }

    yuv2planeX_16_u(filter, filterSize, src, dest, dstW, big_endian, output_bits, i);
}

#endif /* HAVE_POWER8 */

#define yuv2NBPS(bits, BE_LE, is_be, template_size, typeX_t) \
    yuv2NBPS1(bits, BE_LE, is_be, template_size, typeX_t) \
    yuv2NBPSX(bits, BE_LE, is_be, template_size, typeX_t)

#define yuv2NBPS1(bits, BE_LE, is_be, template_size, typeX_t) \
static void yuv2plane1_ ## bits ## BE_LE ## _vsx(const int16_t *src, \
                             uint8_t *dest, int dstW, \
                             const uint8_t *dither, int offset) \
{ \
    yuv2plane1_ ## template_size ## _vsx((const typeX_t *) src, \
                         (uint16_t *) dest, dstW, is_be, bits); \
}

#define yuv2NBPSX(bits, BE_LE, is_be, template_size, typeX_t) \
static void yuv2planeX_ ## bits ## BE_LE ## _vsx(const int16_t *filter, int filterSize, \
                              const int16_t **src, uint8_t *dest, int dstW, \
                              const uint8_t *dither, int offset)\
{ \
    yuv2planeX_## template_size ## _vsx(filter, \
                         filterSize, (const typeX_t **) src, \
                         (uint16_t *) dest, dstW, is_be, bits); \
}

yuv2NBPS( 9, BE, 1, nbps, int16_t)
yuv2NBPS( 9, LE, 0, nbps, int16_t)
yuv2NBPS(10, BE, 1, nbps, int16_t)
yuv2NBPS(10, LE, 0, nbps, int16_t)
yuv2NBPS(12, BE, 1, nbps, int16_t)
yuv2NBPS(12, LE, 0, nbps, int16_t)
yuv2NBPS(14, BE, 1, nbps, int16_t)
yuv2NBPS(14, LE, 0, nbps, int16_t)

yuv2NBPS1(16, BE, 1, 16, int32_t)
yuv2NBPS1(16, LE, 0, 16, int32_t)
#if HAVE_POWER8
yuv2NBPSX(16, BE, 1, 16, int32_t)
yuv2NBPSX(16, LE, 0, 16, int32_t)
#endif

static av_always_inline void
yuv2rgb_full_1_vsx_template(SwsContext *c, const int16_t *buf0,
                     const int16_t *ubuf[2], const int16_t *vbuf[2],
                     const int16_t *abuf0, uint8_t *dest, int dstW,
                     int uvalpha, int y, enum AVPixelFormat target,
                     int hasAlpha)
{
    const int16_t *ubuf0 = ubuf[0], *vbuf0 = vbuf[0];
    const int16_t *ubuf1 = ubuf[1], *vbuf1 = vbuf[1];
    vector int16_t vy, vu, vv, A = vec_splat_s16(0), tmp16;
    vector int32_t vy32_l, vy32_r, vu32_l, vu32_r, vv32_l, vv32_r, tmp32, tmp32_2;
    vector int32_t R_l, R_r, G_l, G_r, B_l, B_r;
    vector uint16_t rd16, gd16, bd16;
    vector uint8_t rd, bd, gd, ad, out0, out1, tmp8;
    const vector uint16_t zero16 = vec_splat_u16(0);
    const vector int32_t y_offset = vec_splats(c->yuv2rgb_y_offset);
    const vector int32_t y_coeff = vec_splats(c->yuv2rgb_y_coeff);
    const vector int32_t y_add = vec_splats(1 << 21);
    const vector int32_t v2r_coeff = vec_splats(c->yuv2rgb_v2r_coeff);
    const vector int32_t v2g_coeff = vec_splats(c->yuv2rgb_v2g_coeff);
    const vector int32_t u2g_coeff = vec_splats(c->yuv2rgb_u2g_coeff);
    const vector int32_t u2b_coeff = vec_splats(c->yuv2rgb_u2b_coeff);
    const vector int32_t rgbclip = vec_splats(1 << 30);
    const vector int32_t zero32 = vec_splat_s32(0);
    const vector uint32_t shift2 = vec_splat_u32(2);
    const vector uint32_t shift22 = vec_splats(22U);
    const vector uint16_t sub7 = vec_splats((uint16_t) (128 << 7));
    const vector uint16_t sub8 = vec_splats((uint16_t) (128 << 8));
    const vector int16_t mul4 = vec_splat_s16(4);
    const vector int16_t mul8 = vec_splat_s16(8);
    const vector int16_t add64 = vec_splat_s16(64);
    const vector uint16_t shift7 = vec_splat_u16(7);
    const vector int16_t max255 = vec_splat_s16(255);
    int i;

    // Various permutations
    const vector uint8_t perm3rg0 = (vector uint8_t) {0x0, 0x10, 0,
                                                      0x1, 0x11, 0,
                                                      0x2, 0x12, 0,
                                                      0x3, 0x13, 0,
                                                      0x4, 0x14, 0,
                                                      0x5 };
    const vector uint8_t perm3rg1 = (vector uint8_t) {     0x15, 0,
                                                      0x6, 0x16, 0,
                                                      0x7, 0x17, 0 };
    const vector uint8_t perm3tb0 = (vector uint8_t) {0x0, 0x1, 0x10,
                                                      0x3, 0x4, 0x11,
                                                      0x6, 0x7, 0x12,
                                                      0x9, 0xa, 0x13,
                                                      0xc, 0xd, 0x14,
                                                      0xf };
    const vector uint8_t perm3tb1 = (vector uint8_t) {     0x0, 0x15,
                                                      0x2, 0x3, 0x16,
                                                      0x5, 0x6, 0x17 };

    for (i = 0; i < dstW; i += 8) { // The x86 asm also overwrites padding bytes.
        vy = vec_ld(0, &buf0[i]);
        vy32_l = vec_unpackh(vy);
        vy32_r = vec_unpackl(vy);
        vy32_l = vec_sl(vy32_l, shift2);
        vy32_r = vec_sl(vy32_r, shift2);

        vu = vec_ld(0, &ubuf0[i]);
        vv = vec_ld(0, &vbuf0[i]);
        if (uvalpha < 2048) {
            vu = (vector int16_t) vec_sub((vector uint16_t) vu, sub7);
            vv = (vector int16_t) vec_sub((vector uint16_t) vv, sub7);

            tmp32 = vec_mule(vu, mul4);
            tmp32_2 = vec_mulo(vu, mul4);
            vu32_l = vec_mergeh(tmp32, tmp32_2);
            vu32_r = vec_mergel(tmp32, tmp32_2);
            tmp32 = vec_mule(vv, mul4);
            tmp32_2 = vec_mulo(vv, mul4);
            vv32_l = vec_mergeh(tmp32, tmp32_2);
            vv32_r = vec_mergel(tmp32, tmp32_2);
        } else {
            tmp16 = vec_ld(0, &ubuf1[i]);
            vu = vec_add(vu, tmp16);
            vu = (vector int16_t) vec_sub((vector uint16_t) vu, sub8);
            tmp16 = vec_ld(0, &vbuf1[i]);
            vv = vec_add(vv, tmp16);
            vv = (vector int16_t) vec_sub((vector uint16_t) vv, sub8);

            vu32_l = vec_mule(vu, mul8);
            vu32_r = vec_mulo(vu, mul8);
            vv32_l = vec_mule(vv, mul8);
            vv32_r = vec_mulo(vv, mul8);
        }

        if (hasAlpha) {
            A = vec_ld(0, &abuf0[i]);
            A = vec_add(A, add64);
            A = vec_sr(A, shift7);
            A = vec_max(A, max255);
            ad = vec_packsu(A, (vector int16_t) zero16);
        } else {
            ad = vec_splats((uint8_t) 255);
        }

        vy32_l = vec_sub(vy32_l, y_offset);
        vy32_r = vec_sub(vy32_r, y_offset);
        vy32_l = vec_mul(vy32_l, y_coeff);
        vy32_r = vec_mul(vy32_r, y_coeff);
        vy32_l = vec_add(vy32_l, y_add);
        vy32_r = vec_add(vy32_r, y_add);

        R_l = vec_mul(vv32_l, v2r_coeff);
        R_l = vec_add(R_l, vy32_l);
        R_r = vec_mul(vv32_r, v2r_coeff);
        R_r = vec_add(R_r, vy32_r);
        G_l = vec_mul(vv32_l, v2g_coeff);
        tmp32 = vec_mul(vu32_l, u2g_coeff);
        G_l = vec_add(G_l, vy32_l);
        G_l = vec_add(G_l, tmp32);
        G_r = vec_mul(vv32_r, v2g_coeff);
        tmp32 = vec_mul(vu32_r, u2g_coeff);
        G_r = vec_add(G_r, vy32_r);
        G_r = vec_add(G_r, tmp32);

        B_l = vec_mul(vu32_l, u2b_coeff);
        B_l = vec_add(B_l, vy32_l);
        B_r = vec_mul(vu32_r, u2b_coeff);
        B_r = vec_add(B_r, vy32_r);

        R_l = vec_max(R_l, zero32);
        R_r = vec_max(R_r, zero32);
        G_l = vec_max(G_l, zero32);
        G_r = vec_max(G_r, zero32);
        B_l = vec_max(B_l, zero32);
        B_r = vec_max(B_r, zero32);

        R_l = vec_min(R_l, rgbclip);
        R_r = vec_min(R_r, rgbclip);
        G_l = vec_min(G_l, rgbclip);
        G_r = vec_min(G_r, rgbclip);
        B_l = vec_min(B_l, rgbclip);
        B_r = vec_min(B_r, rgbclip);

        R_l = vec_sr(R_l, shift22);
        R_r = vec_sr(R_r, shift22);
        G_l = vec_sr(G_l, shift22);
        G_r = vec_sr(G_r, shift22);
        B_l = vec_sr(B_l, shift22);
        B_r = vec_sr(B_r, shift22);

        rd16 = vec_packsu(R_l, R_r);
        gd16 = vec_packsu(G_l, G_r);
        bd16 = vec_packsu(B_l, B_r);
        rd = vec_packsu(rd16, zero16);
        gd = vec_packsu(gd16, zero16);
        bd = vec_packsu(bd16, zero16);

        switch(target) {
        case AV_PIX_FMT_RGB24:
            out0 = vec_perm(rd, gd, perm3rg0);
            out0 = vec_perm(out0, bd, perm3tb0);
            out1 = vec_perm(rd, gd, perm3rg1);
            out1 = vec_perm(out1, bd, perm3tb1);

            vec_vsx_st(out0, 0, dest);
            vec_vsx_st(out1, 16, dest);

            dest += 24;
        break;
        case AV_PIX_FMT_BGR24:
            out0 = vec_perm(bd, gd, perm3rg0);
            out0 = vec_perm(out0, rd, perm3tb0);
            out1 = vec_perm(bd, gd, perm3rg1);
            out1 = vec_perm(out1, rd, perm3tb1);

            vec_vsx_st(out0, 0, dest);
            vec_vsx_st(out1, 16, dest);

            dest += 24;
        break;
        case AV_PIX_FMT_BGRA:
            out0 = vec_mergeh(bd, gd);
            out1 = vec_mergeh(rd, ad);

            tmp8 = (vector uint8_t) vec_mergeh((vector uint16_t) out0, (vector uint16_t) out1);
            vec_vsx_st(tmp8, 0, dest);
            tmp8 = (vector uint8_t) vec_mergel((vector uint16_t) out0, (vector uint16_t) out1);
            vec_vsx_st(tmp8, 16, dest);

            dest += 32;
        break;
        case AV_PIX_FMT_RGBA:
            out0 = vec_mergeh(rd, gd);
            out1 = vec_mergeh(bd, ad);

            tmp8 = (vector uint8_t) vec_mergeh((vector uint16_t) out0, (vector uint16_t) out1);
            vec_vsx_st(tmp8, 0, dest);
            tmp8 = (vector uint8_t) vec_mergel((vector uint16_t) out0, (vector uint16_t) out1);
            vec_vsx_st(tmp8, 16, dest);

            dest += 32;
        break;
        case AV_PIX_FMT_ARGB:
            out0 = vec_mergeh(ad, rd);
            out1 = vec_mergeh(gd, bd);

            tmp8 = (vector uint8_t) vec_mergeh((vector uint16_t) out0, (vector uint16_t) out1);
            vec_vsx_st(tmp8, 0, dest);
            tmp8 = (vector uint8_t) vec_mergel((vector uint16_t) out0, (vector uint16_t) out1);
            vec_vsx_st(tmp8, 16, dest);

            dest += 32;
        break;
        case AV_PIX_FMT_ABGR:
            out0 = vec_mergeh(ad, bd);
            out1 = vec_mergeh(gd, rd);

            tmp8 = (vector uint8_t) vec_mergeh((vector uint16_t) out0, (vector uint16_t) out1);
            vec_vsx_st(tmp8, 0, dest);
            tmp8 = (vector uint8_t) vec_mergel((vector uint16_t) out0, (vector uint16_t) out1);
            vec_vsx_st(tmp8, 16, dest);

            dest += 32;
        break;
        }
    }
}

#define YUV2RGBWRAPPER(name, base, ext, fmt, hasAlpha) \
static void name ## ext ## _1_vsx(SwsContext *c, const int16_t *buf0, \
                                const int16_t *ubuf[2], const int16_t *vbuf[2], \
                                const int16_t *abuf0, uint8_t *dest, int dstW, \
                                int uvalpha, int y) \
{ \
    name ## base ## _1_vsx_template(c, buf0, ubuf, vbuf, abuf0, dest, \
                                  dstW, uvalpha, y, fmt, hasAlpha); \
}

YUV2RGBWRAPPER(yuv2, rgb_full, bgrx32_full, AV_PIX_FMT_BGRA,  0)
YUV2RGBWRAPPER(yuv2, rgb_full, rgbx32_full, AV_PIX_FMT_RGBA,  0)
YUV2RGBWRAPPER(yuv2, rgb_full, xrgb32_full, AV_PIX_FMT_ARGB,  0)
YUV2RGBWRAPPER(yuv2, rgb_full, xbgr32_full, AV_PIX_FMT_ABGR,  0)

YUV2RGBWRAPPER(yuv2, rgb_full, rgb24_full,  AV_PIX_FMT_RGB24, 0)
YUV2RGBWRAPPER(yuv2, rgb_full, bgr24_full,  AV_PIX_FMT_BGR24, 0)

static av_always_inline void
write422(const vector int16_t vy1, const vector int16_t vy2,
         const vector int16_t vu, const vector int16_t vv,
         uint8_t *dest, const enum AVPixelFormat target)
{
    vector uint8_t vd1, vd2, tmp;
    const vector uint8_t yuyv1 = (vector uint8_t) {
                                 0x0, 0x10, 0x1, 0x18,
                                 0x2, 0x11, 0x3, 0x19,
                                 0x4, 0x12, 0x5, 0x1a,
                                 0x6, 0x13, 0x7, 0x1b };
    const vector uint8_t yuyv2 = (vector uint8_t) {
                                 0x8, 0x14, 0x9, 0x1c,
                                 0xa, 0x15, 0xb, 0x1d,
                                 0xc, 0x16, 0xd, 0x1e,
                                 0xe, 0x17, 0xf, 0x1f };
    const vector uint8_t yvyu1 = (vector uint8_t) {
                                 0x0, 0x18, 0x1, 0x10,
                                 0x2, 0x19, 0x3, 0x11,
                                 0x4, 0x1a, 0x5, 0x12,
                                 0x6, 0x1b, 0x7, 0x13 };
    const vector uint8_t yvyu2 = (vector uint8_t) {
                                 0x8, 0x1c, 0x9, 0x14,
                                 0xa, 0x1d, 0xb, 0x15,
                                 0xc, 0x1e, 0xd, 0x16,
                                 0xe, 0x1f, 0xf, 0x17 };
    const vector uint8_t uyvy1 = (vector uint8_t) {
                                 0x10, 0x0, 0x18, 0x1,
                                 0x11, 0x2, 0x19, 0x3,
                                 0x12, 0x4, 0x1a, 0x5,
                                 0x13, 0x6, 0x1b, 0x7 };
    const vector uint8_t uyvy2 = (vector uint8_t) {
                                 0x14, 0x8, 0x1c, 0x9,
                                 0x15, 0xa, 0x1d, 0xb,
                                 0x16, 0xc, 0x1e, 0xd,
                                 0x17, 0xe, 0x1f, 0xf };

    vd1 = vec_packsu(vy1, vy2);
    vd2 = vec_packsu(vu, vv);

    switch (target) {
    case AV_PIX_FMT_YUYV422:
        tmp = vec_perm(vd1, vd2, yuyv1);
        vec_st(tmp, 0, dest);
        tmp = vec_perm(vd1, vd2, yuyv2);
        vec_st(tmp, 16, dest);
    break;
    case AV_PIX_FMT_YVYU422:
        tmp = vec_perm(vd1, vd2, yvyu1);
        vec_st(tmp, 0, dest);
        tmp = vec_perm(vd1, vd2, yvyu2);
        vec_st(tmp, 16, dest);
    break;
    case AV_PIX_FMT_UYVY422:
        tmp = vec_perm(vd1, vd2, uyvy1);
        vec_st(tmp, 0, dest);
        tmp = vec_perm(vd1, vd2, uyvy2);
        vec_st(tmp, 16, dest);
    break;
    }
}

static av_always_inline void
yuv2422_X_vsx_template(SwsContext *c, const int16_t *lumFilter,
                     const int16_t **lumSrc, int lumFilterSize,
                     const int16_t *chrFilter, const int16_t **chrUSrc,
                     const int16_t **chrVSrc, int chrFilterSize,
                     const int16_t **alpSrc, uint8_t *dest, int dstW,
                     int y, enum AVPixelFormat target)
{
    int i, j;
    vector int16_t vy1, vy2, vu, vv;
    vector int32_t vy32[4], vu32[2], vv32[2], tmp, tmp2, tmp3, tmp4;
    vector int16_t vlumFilter[MAX_FILTER_SIZE], vchrFilter[MAX_FILTER_SIZE];
    const vector int32_t start = vec_splats(1 << 18);
    const vector uint32_t shift19 = vec_splats(19U);

    for (i = 0; i < lumFilterSize; i++)
        vlumFilter[i] = vec_splats(lumFilter[i]);
    for (i = 0; i < chrFilterSize; i++)
        vchrFilter[i] = vec_splats(chrFilter[i]);

    for (i = 0; i < ((dstW + 1) >> 1); i += 8) {
        vy32[0] =
        vy32[1] =
        vy32[2] =
        vy32[3] =
        vu32[0] =
        vu32[1] =
        vv32[0] =
        vv32[1] = start;

        for (j = 0; j < lumFilterSize; j++) {
            vv = vec_ld(0, &lumSrc[j][i * 2]);
            tmp = vec_mule(vv, vlumFilter[j]);
            tmp2 = vec_mulo(vv, vlumFilter[j]);
            tmp3 = vec_mergeh(tmp, tmp2);
            tmp4 = vec_mergel(tmp, tmp2);

            vy32[0] = vec_adds(vy32[0], tmp3);
            vy32[1] = vec_adds(vy32[1], tmp4);

            vv = vec_ld(0, &lumSrc[j][(i + 4) * 2]);
            tmp = vec_mule(vv, vlumFilter[j]);
            tmp2 = vec_mulo(vv, vlumFilter[j]);
            tmp3 = vec_mergeh(tmp, tmp2);
            tmp4 = vec_mergel(tmp, tmp2);

            vy32[2] = vec_adds(vy32[2], tmp3);
            vy32[3] = vec_adds(vy32[3], tmp4);
        }

        for (j = 0; j < chrFilterSize; j++) {
            vv = vec_ld(0, &chrUSrc[j][i]);
            tmp = vec_mule(vv, vchrFilter[j]);
            tmp2 = vec_mulo(vv, vchrFilter[j]);
            tmp3 = vec_mergeh(tmp, tmp2);
            tmp4 = vec_mergel(tmp, tmp2);

            vu32[0] = vec_adds(vu32[0], tmp3);
            vu32[1] = vec_adds(vu32[1], tmp4);

            vv = vec_ld(0, &chrVSrc[j][i]);
            tmp = vec_mule(vv, vchrFilter[j]);
            tmp2 = vec_mulo(vv, vchrFilter[j]);
            tmp3 = vec_mergeh(tmp, tmp2);
            tmp4 = vec_mergel(tmp, tmp2);

            vv32[0] = vec_adds(vv32[0], tmp3);
            vv32[1] = vec_adds(vv32[1], tmp4);
        }

        for (j = 0; j < 4; j++) {
            vy32[j] = vec_sra(vy32[j], shift19);
        }
        for (j = 0; j < 2; j++) {
            vu32[j] = vec_sra(vu32[j], shift19);
            vv32[j] = vec_sra(vv32[j], shift19);
        }

        vy1 = vec_packs(vy32[0], vy32[1]);
        vy2 = vec_packs(vy32[2], vy32[3]);
        vu = vec_packs(vu32[0], vu32[1]);
        vv = vec_packs(vv32[0], vv32[1]);

        write422(vy1, vy2, vu, vv, &dest[i * 4], target);
    }
}

#define SETUP(x, buf0, buf1, alpha) { \
    x = vec_ld(0, buf0); \
    tmp = vec_mule(x, alpha); \
    tmp2 = vec_mulo(x, alpha); \
    tmp3 = vec_mergeh(tmp, tmp2); \
    tmp4 = vec_mergel(tmp, tmp2); \
\
    x = vec_ld(0, buf1); \
    tmp = vec_mule(x, alpha); \
    tmp2 = vec_mulo(x, alpha); \
    tmp5 = vec_mergeh(tmp, tmp2); \
    tmp6 = vec_mergel(tmp, tmp2); \
\
    tmp3 = vec_add(tmp3, tmp5); \
    tmp4 = vec_add(tmp4, tmp6); \
\
    tmp3 = vec_sra(tmp3, shift19); \
    tmp4 = vec_sra(tmp4, shift19); \
    x = vec_packs(tmp3, tmp4); \
}

static av_always_inline void
yuv2422_2_vsx_template(SwsContext *c, const int16_t *buf[2],
                     const int16_t *ubuf[2], const int16_t *vbuf[2],
                     const int16_t *abuf[2], uint8_t *dest, int dstW,
                     int yalpha, int uvalpha, int y,
                     enum AVPixelFormat target)
{
    const int16_t *buf0  = buf[0],  *buf1  = buf[1],
                  *ubuf0 = ubuf[0], *ubuf1 = ubuf[1],
                  *vbuf0 = vbuf[0], *vbuf1 = vbuf[1];
    const int16_t  yalpha1 = 4096 - yalpha;
    const int16_t uvalpha1 = 4096 - uvalpha;
    vector int16_t vy1, vy2, vu, vv;
    vector int32_t tmp, tmp2, tmp3, tmp4, tmp5, tmp6;
    const vector int16_t vyalpha1 = vec_splats(yalpha1);
    const vector int16_t vuvalpha1 = vec_splats(uvalpha1);
    const vector uint32_t shift19 = vec_splats(19U);
    int i;
    av_assert2(yalpha  <= 4096U);
    av_assert2(uvalpha <= 4096U);

    for (i = 0; i < ((dstW + 1) >> 1); i += 8) {

        SETUP(vy1, &buf0[i * 2], &buf1[i * 2], vyalpha1)
        SETUP(vy2, &buf0[(i + 4) * 2], &buf1[(i + 4) * 2], vyalpha1)
        SETUP(vu, &ubuf0[i], &ubuf1[i], vuvalpha1)
        SETUP(vv, &vbuf0[i], &vbuf1[i], vuvalpha1)

        write422(vy1, vy2, vu, vv, &dest[i * 4], target);
    }
}

#undef SETUP

static av_always_inline void
yuv2422_1_vsx_template(SwsContext *c, const int16_t *buf0,
                     const int16_t *ubuf[2], const int16_t *vbuf[2],
                     const int16_t *abuf0, uint8_t *dest, int dstW,
                     int uvalpha, int y, enum AVPixelFormat target)
{
    const int16_t *ubuf0 = ubuf[0], *vbuf0 = vbuf[0];
    vector int16_t vy1, vy2, vu, vv, tmp;
    const vector int16_t add64 = vec_splats((int16_t) 64);
    const vector int16_t add128 = vec_splats((int16_t) 128);
    const vector uint16_t shift7 = vec_splat_u16(7);
    const vector uint16_t shift8 = vec_splat_u16(8);
    int i;

    if (uvalpha < 2048) {
        for (i = 0; i < ((dstW + 1) >> 1); i += 8) {
            vy1 = vec_ld(0, &buf0[i * 2]);
            vy2 = vec_ld(0, &buf0[(i + 4) * 2]);
            vu = vec_ld(0, &ubuf0[i]);
            vv = vec_ld(0, &vbuf0[i]);

            vy1 = vec_add(vy1, add64);
            vy2 = vec_add(vy2, add64);
            vu = vec_add(vu, add64);
            vv = vec_add(vv, add64);

            vy1 = vec_sra(vy1, shift7);
            vy2 = vec_sra(vy2, shift7);
            vu = vec_sra(vu, shift7);
            vv = vec_sra(vv, shift7);

            write422(vy1, vy2, vu, vv, &dest[i * 4], target);
        }
    } else {
        const int16_t *ubuf1 = ubuf[1], *vbuf1 = vbuf[1];
        for (i = 0; i < ((dstW + 1) >> 1); i += 8) {
            vy1 = vec_ld(0, &buf0[i * 2]);
            vy2 = vec_ld(0, &buf0[(i + 4) * 2]);
            vu = vec_ld(0, &ubuf0[i]);
            tmp = vec_ld(0, &ubuf1[i]);
            vu = vec_adds(vu, tmp);
            vv = vec_ld(0, &vbuf0[i]);
            tmp = vec_ld(0, &vbuf1[i]);
            vv = vec_adds(vv, tmp);

            vy1 = vec_add(vy1, add64);
            vy2 = vec_add(vy2, add64);
            vu = vec_adds(vu, add128);
            vv = vec_adds(vv, add128);

            vy1 = vec_sra(vy1, shift7);
            vy2 = vec_sra(vy2, shift7);
            vu = vec_sra(vu, shift8);
            vv = vec_sra(vv, shift8);

            write422(vy1, vy2, vu, vv, &dest[i * 4], target);
        }
    }
}

#define YUV2PACKEDWRAPPERX(name, base, ext, fmt) \
static void name ## ext ## _X_vsx(SwsContext *c, const int16_t *lumFilter, \
                                const int16_t **lumSrc, int lumFilterSize, \
                                const int16_t *chrFilter, const int16_t **chrUSrc, \
                                const int16_t **chrVSrc, int chrFilterSize, \
                                const int16_t **alpSrc, uint8_t *dest, int dstW, \
                                int y) \
{ \
    name ## base ## _X_vsx_template(c, lumFilter, lumSrc, lumFilterSize, \
                                  chrFilter, chrUSrc, chrVSrc, chrFilterSize, \
                                  alpSrc, dest, dstW, y, fmt); \
}

#define YUV2PACKEDWRAPPER2(name, base, ext, fmt) \
YUV2PACKEDWRAPPERX(name, base, ext, fmt) \
static void name ## ext ## _2_vsx(SwsContext *c, const int16_t *buf[2], \
                                const int16_t *ubuf[2], const int16_t *vbuf[2], \
                                const int16_t *abuf[2], uint8_t *dest, int dstW, \
                                int yalpha, int uvalpha, int y) \
{ \
    name ## base ## _2_vsx_template(c, buf, ubuf, vbuf, abuf, \
                                  dest, dstW, yalpha, uvalpha, y, fmt); \
}

#define YUV2PACKEDWRAPPER(name, base, ext, fmt) \
YUV2PACKEDWRAPPER2(name, base, ext, fmt) \
static void name ## ext ## _1_vsx(SwsContext *c, const int16_t *buf0, \
                                const int16_t *ubuf[2], const int16_t *vbuf[2], \
                                const int16_t *abuf0, uint8_t *dest, int dstW, \
                                int uvalpha, int y) \
{ \
    name ## base ## _1_vsx_template(c, buf0, ubuf, vbuf, \
                                  abuf0, dest, dstW, uvalpha, \
                                  y, fmt); \
}

YUV2PACKEDWRAPPER(yuv2, 422, yuyv422, AV_PIX_FMT_YUYV422)
YUV2PACKEDWRAPPER(yuv2, 422, yvyu422, AV_PIX_FMT_YVYU422)
YUV2PACKEDWRAPPER(yuv2, 422, uyvy422, AV_PIX_FMT_UYVY422)

#endif /* !HAVE_BIGENDIAN */

#endif /* HAVE_VSX */

av_cold void ff_sws_init_swscale_vsx(SwsContext *c)
{
#if HAVE_VSX
    enum AVPixelFormat dstFormat = c->dstFormat;
    const int cpu_flags = av_get_cpu_flags();

    if (!(cpu_flags & AV_CPU_FLAG_VSX))
        return;

#if !HAVE_BIGENDIAN
    if (c->srcBpc == 8 && c->dstBpc <= 14) {
        c->hyScale = c->hcScale = hScale_real_vsx;
    }
    if (!is16BPS(dstFormat) && !isNBPS(dstFormat) &&
        dstFormat != AV_PIX_FMT_NV12 && dstFormat != AV_PIX_FMT_NV21 &&
        dstFormat != AV_PIX_FMT_GRAYF32BE && dstFormat != AV_PIX_FMT_GRAYF32LE &&
        !c->needAlpha) {
        c->yuv2planeX = yuv2planeX_vsx;
    }
#endif

    if (!(c->flags & (SWS_BITEXACT | SWS_FULL_CHR_H_INT)) && !c->needAlpha) {
        switch (c->dstBpc) {
        case 8:
            c->yuv2plane1 = yuv2plane1_8_vsx;
            break;
#if !HAVE_BIGENDIAN
        case 9:
            c->yuv2plane1 = isBE(dstFormat) ? yuv2plane1_9BE_vsx  : yuv2plane1_9LE_vsx;
            c->yuv2planeX = isBE(dstFormat) ? yuv2planeX_9BE_vsx  : yuv2planeX_9LE_vsx;
            break;
        case 10:
            c->yuv2plane1 = isBE(dstFormat) ? yuv2plane1_10BE_vsx  : yuv2plane1_10LE_vsx;
            c->yuv2planeX = isBE(dstFormat) ? yuv2planeX_10BE_vsx  : yuv2planeX_10LE_vsx;
            break;
        case 12:
            c->yuv2plane1 = isBE(dstFormat) ? yuv2plane1_12BE_vsx  : yuv2plane1_12LE_vsx;
            c->yuv2planeX = isBE(dstFormat) ? yuv2planeX_12BE_vsx  : yuv2planeX_12LE_vsx;
            break;
        case 14:
            c->yuv2plane1 = isBE(dstFormat) ? yuv2plane1_14BE_vsx  : yuv2plane1_14LE_vsx;
            c->yuv2planeX = isBE(dstFormat) ? yuv2planeX_14BE_vsx  : yuv2planeX_14LE_vsx;
            break;
        case 16:
            c->yuv2plane1 = isBE(dstFormat) ? yuv2plane1_16BE_vsx  : yuv2plane1_16LE_vsx;
#if HAVE_POWER8
            if (cpu_flags & AV_CPU_FLAG_POWER8) {
                c->yuv2planeX = isBE(dstFormat) ? yuv2planeX_16BE_vsx  : yuv2planeX_16LE_vsx;
            }
#endif /* HAVE_POWER8 */
            break;
#endif /* !HAVE_BIGENDIAN */
        }
    }

    if (c->flags & SWS_BITEXACT)
        return;

#if !HAVE_BIGENDIAN
    if (c->flags & SWS_FULL_CHR_H_INT) {
        switch (dstFormat) {
            case AV_PIX_FMT_RGB24:
                if (HAVE_POWER8 && cpu_flags & AV_CPU_FLAG_POWER8) {
                    c->yuv2packed1 = yuv2rgb24_full_1_vsx;
                }
            break;
            case AV_PIX_FMT_BGR24:
                if (HAVE_POWER8 && cpu_flags & AV_CPU_FLAG_POWER8) {
                    c->yuv2packed1 = yuv2bgr24_full_1_vsx;
                }
            break;
            case AV_PIX_FMT_BGRA:
                if (HAVE_POWER8 && cpu_flags & AV_CPU_FLAG_POWER8) {
                    if (!c->needAlpha) {
                        c->yuv2packed1 = yuv2bgrx32_full_1_vsx;
                    }
                }
            break;
            case AV_PIX_FMT_RGBA:
                if (HAVE_POWER8 && cpu_flags & AV_CPU_FLAG_POWER8) {
                    if (!c->needAlpha) {
                        c->yuv2packed1 = yuv2rgbx32_full_1_vsx;
                    }
                }
            break;
            case AV_PIX_FMT_ARGB:
                if (HAVE_POWER8 && cpu_flags & AV_CPU_FLAG_POWER8) {
                    if (!c->needAlpha) {
                        c->yuv2packed1 = yuv2xrgb32_full_1_vsx;
                    }
                }
            break;
            case AV_PIX_FMT_ABGR:
                if (HAVE_POWER8 && cpu_flags & AV_CPU_FLAG_POWER8) {
                    if (!c->needAlpha) {
                        c->yuv2packed1 = yuv2xbgr32_full_1_vsx;
                    }
                }
            break;
        }
    } else { /* !SWS_FULL_CHR_H_INT */
        switch (dstFormat) {
            case AV_PIX_FMT_YUYV422:
                c->yuv2packed1 = yuv2yuyv422_1_vsx;
                c->yuv2packed2 = yuv2yuyv422_2_vsx;
                c->yuv2packedX = yuv2yuyv422_X_vsx;
            break;
            case AV_PIX_FMT_YVYU422:
                c->yuv2packed1 = yuv2yvyu422_1_vsx;
                c->yuv2packed2 = yuv2yvyu422_2_vsx;
                c->yuv2packedX = yuv2yvyu422_X_vsx;
            break;
            case AV_PIX_FMT_UYVY422:
                c->yuv2packed1 = yuv2uyvy422_1_vsx;
                c->yuv2packed2 = yuv2uyvy422_2_vsx;
                c->yuv2packedX = yuv2uyvy422_X_vsx;
            break;
        }
    }
#endif /* !HAVE_BIGENDIAN */

#endif /* HAVE_VSX */
}
