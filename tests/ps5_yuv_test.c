/* host/posix/ps5_yuv.h against the conversion host/posix/ps5_dav1d.c had before it (the reference below is that loop as it
   was): every byte the same, on authored random pictures of several sizes (odd ones too), from planar 4:2:0 and from
   NV12. Built twice by tests/test_ps5_yuv.py: with AVX2 (the console's path) and without (the plain C alone).
   "bench" as the first argument also times a 1920x1080 picture, 200 times each way. No game data. */
#include "../host/posix/ps5_yuv.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void reference(int w, int h, const uint8_t *const src[], const int src_stride[], uint8_t *dst, int dst_stride) {
    for (int y = 0; y < h; y++) {
        const uint8_t *py = src[0] + (size_t)y * src_stride[0];
        const uint8_t *pu = src[1] + (size_t)(y / 2) * src_stride[1];
        const uint8_t *pv = src[2] + (size_t)(y / 2) * src_stride[2];
        uint8_t *out = dst + (size_t)y * dst_stride;
        for (int x = 0; x < w; x++) {
            const int Y = (py[x] - 16) * 76309 + 32768, U = pu[x >> 1] - 128, V = pv[x >> 1] - 128;
            out[4 * x] = clamp8((Y + 117489 * V) >> 16);
            out[4 * x + 1] = clamp8((Y - 13975 * U - 34925 * V) >> 16);
            out[4 * x + 2] = clamp8((Y + 138438 * U) >> 16);
            out[4 * x + 3] = 255;
        }
    }
}
static uint32_t seed = 0x12345678u;
static uint8_t rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return (uint8_t)(seed >> 24);
}
static double now_ms(void) { return clock() * 1e3 / CLOCKS_PER_SEC; }
/* mode 0: random; 1: the extremes (0, 16, 128, 235, 240, 255), where the clamps work */
static int one(int w, int h, int mode) {
    static const uint8_t edge[] = {0, 1, 15, 16, 17, 127, 128, 129, 234, 235, 239, 240, 254, 255};
    const int cw = (w + 1) / 2, ch = (h + 1) / 2, ys = w + 5, cs = cw + 3;   /* (strides wider than the rows) */
    uint8_t *y = malloc((size_t)ys * h), *u = malloc((size_t)cs * ch), *v = malloc((size_t)cs * ch);
    /* (NV12 as host/nn_video.c keeps it: rows of w bytes, h / 2 of them for the chroma, w / 2 pairs each) */
    uint8_t *nv = calloc(1, (size_t)w * h + (size_t)w * (h / 2) + 1);
    uint8_t *want = malloc((size_t)w * h * 4), *got = malloc((size_t)w * h * 4 + 1), *got_nv = malloc((size_t)w * h * 4 + 1);
    for (size_t i = 0; i < (size_t)ys * h; i++) y[i] = mode ? edge[rnd() % sizeof edge] : rnd();
    for (size_t i = 0; i < (size_t)cs * ch; i++) u[i] = mode ? edge[rnd() % sizeof edge] : rnd(), v[i] = mode ? edge[rnd() % sizeof edge] : rnd();
    for (int r = 0; r < h; r++) memcpy(nv + (size_t)r * w, y + (size_t)r * ys, (size_t)w);
    for (int r = 0; r < h / 2; r++)
        for (int x = 0; x < w / 2; x++)
            nv[(size_t)w * h + (size_t)r * w + 2 * x] = u[(size_t)r * cs + x], nv[(size_t)w * h + (size_t)r * w + 2 * x + 1] = v[(size_t)r * cs + x];
    const uint8_t *src[3] = {y, u, v};
    const int stride[3] = {ys, cs, cs};
    reference(w, h, src, stride, want, w * 4);
    got[(size_t)w * h * 4] = got_nv[(size_t)w * h * 4] = 0x5a;   /* (nothing written past the picture) */
    for (int r = 0; r < h; r++) {
        ps5_yuv_i420_row(got + (size_t)r * w * 4, y + (size_t)r * ys, u + (size_t)(r / 2) * cs, v + (size_t)(r / 2) * cs, w);
        if (h / 2) ps5_yuv_nv12_row(got_nv + (size_t)r * w * 4, nv + (size_t)r * w,
                                    nv + (size_t)w * h + (size_t)(r / 2 < h / 2 ? r / 2 : h / 2 - 1) * w, w);
    }
    int bad = memcmp(want, got, (size_t)w * h * 4) != 0 || got[(size_t)w * h * 4] != 0x5a || got_nv[(size_t)w * h * 4] != 0x5a;
    /* (NV12 has whole pairs and whole chroma rows only: the same as the planar picture where both have the pixel's chroma) */
    for (int r = 0; r < (h & ~1) && !bad; r++) bad = memcmp(want + (size_t)r * w * 4, got_nv + (size_t)r * w * 4, (size_t)(w & ~1) * 4) != 0;
    if (bad) fprintf(stderr, "FAIL: %dx%d mode %d differs from the reference\n", w, h, mode);
    free(y), free(u), free(v), free(nv), free(want), free(got), free(got_nv);
    return bad;
}
int main(int argc, char **argv) {
    static const int sizes[][2] = {{1920, 1080}, {1280, 720}, {64, 36}, {17, 9}, {16, 2}, {9, 3}, {8, 8}, {7, 5}, {3, 2}, {2, 2}, {1, 1}};
    int bad = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        for (int mode = 0; mode < 2; mode++) bad |= one(sizes[i][0], sizes[i][1], mode);
    if (bad) return 1;
#ifdef __AVX2__
    printf("ps5_yuv: AVX2 and the plain C give the reference's bytes\n");
#else
    printf("ps5_yuv: the plain C gives the reference's bytes\n");
#endif
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        const int w = 1920, h = 1080, n = 200;
        uint8_t *y = malloc((size_t)w * h), *u = malloc((size_t)w * h / 4), *v = malloc((size_t)w * h / 4), *out = malloc((size_t)w * h * 4);
        for (size_t i = 0; i < (size_t)w * h; i++) y[i] = rnd();
        for (size_t i = 0; i < (size_t)w * h / 4; i++) u[i] = rnd(), v[i] = rnd();
        const uint8_t *src[3] = {y, u, v};
        const int stride[3] = {w, w / 2, w / 2};
        unsigned sum = 0;   /* (the pictures are used: the loops stay) */
        double t0 = now_ms();
        for (int k = 0; k < n; k++) y[k] ^= 1, reference(w, h, src, stride, out, w * 4), sum += out[4 * k];
        double t1 = now_ms();
        for (int k = 0; k < n; k++) {
            y[k] ^= 1;
            for (int r = 0; r < h; r++)
                ps5_yuv_i420_row(out + (size_t)r * w * 4, y + (size_t)r * w, u + (size_t)(r / 2) * (w / 2), v + (size_t)(r / 2) * (w / 2), w);
            sum += out[4 * k];
        }
        double t2 = now_ms();
        printf("ps5_yuv: 1920x1080 to RGBA: the reference %.2f ms a picture, ps5_yuv.h %.2f ms (sum %u)\n", (t1 - t0) / n, (t2 - t1) / n, sum);
        free(y), free(u), free(v), free(out);
    }
    return 0;
}
