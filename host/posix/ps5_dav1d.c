/* (ps5) The game's AV1 movies on dav1d (2026-11-10: the sixth run showed the opening movie black, with its sound and its
   subtitles: "AV1 decoder not available in this FFmpeg"). No FFmpeg is built for the console: host/nn_video.c talks to the
   part of libavcodec and libswscale that host/posix/ps5_noffmpeg's headers declare, and this file answers it with dav1d
   alone, cross-built for the console (hookshot-ps5's build/dav1d-ps5: meson with PS5_Vulkan's cross file, its assembly
   on). Built only with HOOKSHOT_PS5_DAV1D (nx2/build.py, when toolchain_ps5.dav1d() finds the build); without it
   host/posix/ps5_stubs.c's stand-ins answer and the movies are black as before.

   - the decoder: "libdav1d" by name and AV_CODEC_ID_AV1 (VP8 and VP9 stay "not available"). A packet is one temporal
     unit of OBUs, as the Linux build gives FFmpeg's libdav1d; dav1d's EAGAIN is libavcodec's (nn_video.c drains, then sends
     the packet again).
   - a picture: copied out as 8-bit planar 4:2:0 (AV_PIX_FMT_YUV420P) into the context's own planes, whatever dav1d
     decoded (10-bit shifted down, 4:2:2 and 4:4:4 point-sampled, grey given neutral chroma), so nn_video.c's NV12 copy and
     sws_scale see one format.
   - sws_scale: planar 4:2:0 or NV12 to RGBA, BT.709 limited range (the movies are 1080p), nearest chroma
     (host/posix/ps5_yuv.h).
   HOOKSHOT_PS5_AV1_THREADS: dav1d's threads (default: what nn_video.c asks, 4). */
#if defined(__PROSPERO__) && defined(HOOKSHOT_PS5_DAV1D)
#include "../hos.h"
#include <dav1d/dav1d.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include "ps5_yuv.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    AVCodecContext pub;   /* (first: nn_video.c sets thread_count) */
    Dav1dContext *dc;
    uint8_t *plane[3];
    uint8_t *planes;      /* (their memory: one block of direct memory of its own, not the heap's; nn_video.c's VIDEO_BLOCKS) */
    size_t planes_size;
    int planes_own;
    int cap_w, cap_h;
    unsigned long pictures;
} Av1;
uint8_t *hos_video_block(size_t n, size_t *size, int *own);   /* (host/nn_video.c) */
void hos_video_block_free(uint8_t *p, size_t size, int own);
static const AVCodec av1_codec = {"libdav1d"};
extern __attribute__((visibility("hidden"))) unsigned dav1d_cpu_flags;   /* (dav1d's src/cpu.h: set by dav1d_open, once) */

const AVCodec *avcodec_find_decoder(enum AVCodecID id) { return id == AV_CODEC_ID_AV1 ? &av1_codec : NULL; }
const AVCodec *avcodec_find_decoder_by_name(const char *name) { return name && !strcmp(name, "libdav1d") ? &av1_codec : NULL; }
AVCodecContext *avcodec_alloc_context3(const AVCodec *codec) {
    if (codec != &av1_codec) return NULL;
    Av1 *a = (Av1 *)calloc(1, sizeof *a);
    return a ? &a->pub : NULL;
}
static void av1_log(void *cookie, const char *fmt, va_list ap) {
    (void)cookie;
    static int lines;
    if (lines >= 40) return;
    lines++;
    char buf[400];
    vsnprintf(buf, sizeof buf, fmt, ap);
    size_t n = strlen(buf);
    while (n && buf[n - 1] == '\n') buf[--n] = 0;
    hos_log("video: dav1d: %s", buf);
}
int avcodec_open2(AVCodecContext *ctx, const AVCodec *codec, void **options) {
    (void)options;
    Av1 *a = (Av1 *)ctx;
    if (!a || codec != &av1_codec) return -1;
    Dav1dSettings s;
    dav1d_default_settings(&s);
    const char *e = getenv("HOOKSHOT_PS5_AV1_THREADS");
    /* (2026-11-10, runs 10 and 11: at the title screen, while the game loads in the background (5 to 8.5 CPUs busy), a
       submission took 16 ms to decode on average and up to 175 ms with 4 threads, the picture fell 0.5 s behind and 200 to 300
       pictures were skipped every 5 s. The title has 13 CPUs: 8 threads here, whatever nn_video.c asks) */
    s.n_threads = e && atoi(e) > 0 ? atoi(e) : 8;
    s.logger.callback = av1_log;
    const int r = dav1d_open(&a->dc, &s);
    /* (dav1d's own word for the CPU it found, to tell its assembly runs: 0x1 SSE2, 0x2 SSSE3, 0x4 SSE4.1, 0x8 AVX2, 0x20 slow
       gathers; the console's Zen 2 should say 0x2f. The build's -march=x86-64-v3 turns the AVX2 paths on whatever this says) */
    hos_log("video: AV1 on dav1d %s, %d threads%s (CPU flags %#x, %d pictures decoded at once)", dav1d_version(), s.n_threads,
            r < 0 ? ": NOT OPENED" : "", dav1d_cpu_flags, r < 0 ? 0 : dav1d_get_frame_delay(&s));
    if (r < 0) a->dc = NULL;
    return r < 0 ? -1 : 0;
}
void avcodec_free_context(AVCodecContext **ctx) {
    if (!ctx || !*ctx) return;
    Av1 *a = (Av1 *)*ctx;
    if (a->dc) dav1d_close(&a->dc);
    {   /* (2026-11-10: said, as nn_video.c says what a decoder's end gives back) */
        static int told;
        if (a->planes && told++ < 24)
            hos_log("video: dav1d closed after %lu pictures; its planes' %.1f MiB given back%s", a->pictures,
                    a->planes_size / 1048576.0, a->planes_own ? " (a block of their own)" : "");
    }
    hos_video_block_free(a->planes, a->planes_size, a->planes_own);
    free(a);
    *ctx = NULL;
}
void avcodec_flush_buffers(AVCodecContext *ctx) {
    Av1 *a = (Av1 *)ctx;
    if (a && a->dc) dav1d_flush(a->dc);
}
int avcodec_send_packet(AVCodecContext *ctx, const AVPacket *pkt) {
    Av1 *a = (Av1 *)ctx;
    if (!a || !a->dc) return AVERROR(ENOSYS);
    if (!pkt || !pkt->data || pkt->size <= 0) return 0;   /* (the end: dav1d_get_picture gives what is left) */
    Dav1dData data;
    memset(&data, 0, sizeof data);
    uint8_t *p = dav1d_data_create(&data, (size_t)pkt->size);
    if (!p) return AVERROR(ENOMEM);
    memcpy(p, pkt->data, (size_t)pkt->size);
    data.m.timestamp = pkt->pts;
    const int r = dav1d_send_data(a->dc, &data);
    if (r < 0 || data.sz) dav1d_data_unref(&data);   /* (not taken: EAGAIN, the caller sends it again; or an error) */
    return r;
}
/* one plane into 8 bits, w by h, from a plane sx by sy times as dense (0 or 1 each way) */
static void av1_plane(uint8_t *dst, int w, int h, const void *src, ptrdiff_t stride, int bpc, int sx, int sy) {
    for (int y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)src + (ptrdiff_t)(y << sy) * stride;
        uint8_t *out = dst + (size_t)y * w;
        if (bpc <= 8) {
            if (!sx) memcpy(out, row, (size_t)w);
            else for (int x = 0; x < w; x++) out[x] = row[x << 1];
        } else {
            const uint16_t *r16 = (const uint16_t *)row;
            const int shift = bpc - 8;
            for (int x = 0; x < w; x++) out[x] = (uint8_t)(r16[x << sx] >> shift);
        }
    }
}
int avcodec_receive_frame(AVCodecContext *ctx, AVFrame *frame) {
    Av1 *a = (Av1 *)ctx;
    if (!a || !a->dc) return AVERROR(EAGAIN);
    Dav1dPicture pic;
    memset(&pic, 0, sizeof pic);
    const int r = dav1d_get_picture(a->dc, &pic);
    if (r < 0) return r;
    const int w = pic.p.w, h = pic.p.h, cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (w > a->cap_w || h > a->cap_h || !a->plane[0]) {
        hos_video_block_free(a->planes, a->planes_size, a->planes_own);
        a->planes = hos_video_block((size_t)w * h + 2 * (size_t)cw * ch, &a->planes_size, &a->planes_own);
        a->plane[0] = a->planes;
        a->plane[1] = a->planes ? a->planes + (size_t)w * h : NULL;
        a->plane[2] = a->planes ? a->plane[1] + (size_t)cw * ch : NULL;
        a->cap_w = w, a->cap_h = h;
        if (!a->planes) {
            a->cap_w = a->cap_h = 0;
            dav1d_picture_unref(&pic);
            return AVERROR(ENOMEM);
        }
    }
    av1_plane(a->plane[0], w, h, pic.data[0], pic.stride[0], pic.p.bpc, 0, 0);
    if (pic.p.layout == DAV1D_PIXEL_LAYOUT_I400) {
        memset(a->plane[1], 128, (size_t)cw * ch);
        memset(a->plane[2], 128, (size_t)cw * ch);
    } else {
        /* (4:2:0: the planes as they are; 4:2:2 and 4:4:4: every other sample; the last odd row or column is within the plane) */
        const int sx = pic.p.layout == DAV1D_PIXEL_LAYOUT_I420 ? 0 : 1, sy = pic.p.layout == DAV1D_PIXEL_LAYOUT_I444 ? 1 : 0;
        const int pw = sx ? w / 2 : cw, ph = sy ? h / 2 : ch;   /* (what can be read without leaving the source plane) */
        if (pw < cw || ph < ch) memset(a->plane[1], 128, (size_t)cw * ch), memset(a->plane[2], 128, (size_t)cw * ch);
        for (int i = 1; i < 3; i++) {
            uint8_t *dst = a->plane[i];
            for (int y = 0; y < ph; y++)   /* (row by row: the destination's rows are cw wide, the source gives pw of them) */
                av1_plane(dst + (size_t)y * cw, pw, 1, (const uint8_t *)pic.data[i] + (ptrdiff_t)(y << sy) * pic.stride[1], pic.stride[1],
                          pic.p.bpc, sx, 0);
        }
    }
    memset(frame, 0, sizeof *frame);
    frame->data[0] = a->plane[0], frame->data[1] = a->plane[1], frame->data[2] = a->plane[2];
    frame->linesize[0] = w, frame->linesize[1] = frame->linesize[2] = cw;
    frame->width = w, frame->height = h, frame->format = AV_PIX_FMT_YUV420P;
    frame->pts = pic.m.timestamp;
    if (!a->pictures++)
        hos_log("video: dav1d: the first picture: %dx%d, %d bits, layout %d (0 grey, 1 4:2:0, 2 4:2:2, 3 4:4:4)", w, h, pic.p.bpc,
                (int)pic.p.layout);
    dav1d_picture_unref(&pic);
    return 0;
}

/* ---- libswscale's part: planar 4:2:0 or NV12 to RGBA (host/posix/ps5_yuv.h: AVX2, the bytes the plain C gave) ----
   NV12 is what host/nn_video.c keeps a picture as: on the console it makes the RGBA only for a reader that asks for it
   (its pool_rgba), not for each picture decoded. */
struct SwsContext { int w, h, nv12; };
struct SwsContext *sws_getContext(int sw, int sh, enum AVPixelFormat sf, int dw, int dh, enum AVPixelFormat df, int flags,
                                  void *src_filter, void *dst_filter, const double *param) {
    (void)flags, (void)src_filter, (void)dst_filter, (void)param;
    if ((sf != AV_PIX_FMT_YUV420P && sf != AV_PIX_FMT_NV12) || df != AV_PIX_FMT_RGBA || sw != dw || sh != dh || sw <= 0 || sh <= 0 ||
        (sf == AV_PIX_FMT_NV12 && (sw < 2 || sh < 2)))   /* (NV12 needs a chroma pair and a chroma row) */
        return NULL;
    struct SwsContext *c = (struct SwsContext *)calloc(1, sizeof *c);
    if (c) c->w = sw, c->h = sh, c->nv12 = sf == AV_PIX_FMT_NV12;
    return c;
}
int sws_scale(struct SwsContext *c, const uint8_t *const src[], const int src_stride[], int y0, int h, uint8_t *const dst[],
              const int dst_stride[]) {
    if (!c || !src || !src[0] || !src[1] || (!c->nv12 && !src[2]) || !dst || !dst[0]) return 0;
    /* (NV12 as nn_video.c keeps it has h / 2 chroma rows: an odd picture's last row takes the row before) */
    const int crows = c->nv12 ? c->h / 2 : (c->h + 1) / 2;
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < c->h; y++) {
        const int cy = y / 2 < crows ? y / 2 : crows - 1;
        const uint8_t *py = src[0] + (ptrdiff_t)y * src_stride[0];
        uint8_t *out = dst[0] + (ptrdiff_t)y * dst_stride[0];
        if (c->nv12) ps5_yuv_nv12_row(out, py, src[1] + (ptrdiff_t)cy * src_stride[1], c->w);
        else ps5_yuv_i420_row(out, py, src[1] + (ptrdiff_t)cy * src_stride[1], src[2] + (ptrdiff_t)cy * src_stride[2], c->w);
    }
    return h;
}
void sws_freeContext(struct SwsContext *c) { free(c); }
#endif
