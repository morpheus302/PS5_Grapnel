/* (ps5) 8-bit 4:2:0 to RGBA for host/posix/ps5_dav1d.c's sws_scale: BT.709, limited range, nearest chroma, 16.16 fixed
   point: 1.1644 (Y-16); R + 1.7927 V; G - 0.2132 U - 0.5329 V; B + 2.1124 U. One row at a time, the chroma planar (I420) or
   interleaved (NV12). With AVX2 (the console's build is -march=x86-64-v3) eight pixels a step, bit for bit what the plain
   C gives (tests/ps5_yuv_test.c compares them on the PC); the plain C is the rest of a row, and every row without AVX2.
   No dependency: the test builds this file alone. */
#ifndef HOOKSHOT_PS5_YUV_H
#define HOOKSHOT_PS5_YUV_H
#include <stdint.h>
#include <string.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif

static inline uint8_t ps5_yuv_clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline void ps5_yuv_pixel(uint8_t *out, int y, int u, int v) {
    const int Y = (y - 16) * 76309 + 32768, U = u - 128, V = v - 128;
    out[0] = ps5_yuv_clamp8((Y + 117489 * V) >> 16);
    out[1] = ps5_yuv_clamp8((Y - 13975 * U - 34925 * V) >> 16);
    out[2] = ps5_yuv_clamp8((Y + 138438 * U) >> 16);
    out[3] = 255;
}
#ifdef __AVX2__
/* eight pixels: their luma, and their chroma already one value a pixel (32 bits each) */
static inline void ps5_yuv_rgba8(uint8_t *out, __m128i y8, __m256i u, __m256i v) {
    const __m256i Y = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_sub_epi32(_mm256_cvtepu8_epi32(y8), _mm256_set1_epi32(16)),
                                                          _mm256_set1_epi32(76309)), _mm256_set1_epi32(32768));
    u = _mm256_sub_epi32(u, _mm256_set1_epi32(128));
    v = _mm256_sub_epi32(v, _mm256_set1_epi32(128));
    const __m256i r = _mm256_srai_epi32(_mm256_add_epi32(Y, _mm256_mullo_epi32(v, _mm256_set1_epi32(117489))), 16);
    const __m256i g = _mm256_srai_epi32(_mm256_sub_epi32(_mm256_sub_epi32(Y, _mm256_mullo_epi32(u, _mm256_set1_epi32(13975))),
                                                         _mm256_mullo_epi32(v, _mm256_set1_epi32(34925))), 16);
    const __m256i b = _mm256_srai_epi32(_mm256_add_epi32(Y, _mm256_mullo_epi32(u, _mm256_set1_epi32(138438))), 16);
    /* (each 128-bit half: R0-3 G0-3 as 16 bits, B0-3 A0-3, then the 16 bytes R0-3 G0-3 B0-3 A0-3, saturated to 0..255 as
       the clamp does; the shuffle puts each pixel's four together) */
    const __m256i rg = _mm256_packs_epi32(r, g), ba = _mm256_packs_epi32(b, _mm256_set1_epi32(255));
    const __m256i px = _mm256_shuffle_epi8(_mm256_packus_epi16(rg, ba),
                                           _mm256_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15,
                                                            0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15));
    _mm256_storeu_si256((__m256i *)out, px);
}
#endif
/* a row of w pixels: y has w values, u and v (w + 1) / 2 each */
static inline void ps5_yuv_i420_row(uint8_t *out, const uint8_t *y, const uint8_t *u, const uint8_t *v, int w) {
    int x = 0;
#ifdef __AVX2__
    for (; x + 8 <= w; x += 8) {
        int32_t u4, v4;
        memcpy(&u4, u + (x >> 1), 4), memcpy(&v4, v + (x >> 1), 4);
        const __m128i ub = _mm_cvtsi32_si128(u4), vb = _mm_cvtsi32_si128(v4);
        ps5_yuv_rgba8(out + 4 * x, _mm_loadl_epi64((const __m128i *)(y + x)), _mm256_cvtepu8_epi32(_mm_unpacklo_epi8(ub, ub)),
                      _mm256_cvtepu8_epi32(_mm_unpacklo_epi8(vb, vb)));
    }
#endif
    for (; x < w; x++) ps5_yuv_pixel(out + 4 * x, y[x], u[x >> 1], v[x >> 1]);
}
/* a row of w pixels: y has w values, uv the U V pairs of the row (w / 2 whole pairs: an odd last pixel takes the pair before) */
static inline void ps5_yuv_nv12_row(uint8_t *out, const uint8_t *y, const uint8_t *uv, int w) {
    int x = 0;
#ifdef __AVX2__
    for (; x + 8 <= w; x += 8) {
        const __m128i c = _mm_loadl_epi64((const __m128i *)(uv + x));   /* (U0 V0 U1 V1 U2 V2 U3 V3) */
        ps5_yuv_rgba8(out + 4 * x, _mm_loadl_epi64((const __m128i *)(y + x)),
                      _mm256_cvtepu8_epi32(_mm_shuffle_epi8(c, _mm_setr_epi8(0, 0, 2, 2, 4, 4, 6, 6, -1, -1, -1, -1, -1, -1, -1, -1))),
                      _mm256_cvtepu8_epi32(_mm_shuffle_epi8(c, _mm_setr_epi8(1, 1, 3, 3, 5, 5, 7, 7, -1, -1, -1, -1, -1, -1, -1, -1))));
    }
#endif
    for (; x < w; x++) {
        const int k = (x | 1) < w ? x & ~1 : w >= 2 ? (w - 2) & ~1 : -1;
        if (k < 0) ps5_yuv_pixel(out + 4 * x, y[x], 128, 128);
        else ps5_yuv_pixel(out + 4 * x, y[x], uv[k], uv[k + 1]);
    }
}
#endif
