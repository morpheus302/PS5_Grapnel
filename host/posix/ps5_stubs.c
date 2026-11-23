/* (ps5) What the Linux build has from its system and the console build goes without for now, each a stand-in that
   says so (the census in docs/ps5-port.md counts them):

   - FFmpeg (host/nn_video.c's VP8, VP9 and AV1 decoders): not built for the console yet (gap 9). With
     host/posix/ps5_noffmpeg on the include path, nn_video.c builds unchanged against these, every decoder is "not
     available in this FFmpeg", and the movies are skipped, as on a Linux build without dav1d. Phase 5: FFmpeg and dav1d
     cross-built (--target-os=freebsd --arch=x86_64 --cpu=znver2, as PS5_RetroArch's tools/build-ffmpeg.sh) and
     HOOKSHOT_PS5_FFMPEG=<prefix>, which takes this shim off the include path.
   - host/posix/prof.c (HOS_PROFILE, HOS_SAMPLE, HOS_WATCH): Linux's perf_event_open, tgkill and /proc. The console's
     equivalent is the shell's own profiler, not a title's: no profiler here. */
#ifdef __PROSPERO__
#include "../hos.h"
#include <stdlib.h>
#ifndef HOOKSHOT_PS5_FFMPEG
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#ifndef HOOKSHOT_PS5_DAV1D   /* (with dav1d: host/posix/ps5_dav1d.c answers these and sws_*) */
const AVCodec *avcodec_find_decoder(enum AVCodecID id) { (void)id; return NULL; }
const AVCodec *avcodec_find_decoder_by_name(const char *name) { (void)name; return NULL; }
AVCodecContext *avcodec_alloc_context3(const AVCodec *codec) { (void)codec; return NULL; }
int avcodec_open2(AVCodecContext *ctx, const AVCodec *codec, void **options) { (void)ctx, (void)codec, (void)options; return -1; }
void avcodec_free_context(AVCodecContext **ctx) { if (ctx) *ctx = NULL; }
void avcodec_flush_buffers(AVCodecContext *ctx) { (void)ctx; }
int avcodec_send_packet(AVCodecContext *ctx, const AVPacket *pkt) { (void)ctx, (void)pkt; return AVERROR(ENOSYS); }
int avcodec_receive_frame(AVCodecContext *ctx, AVFrame *frame) { (void)ctx, (void)frame; return AVERROR(EAGAIN); }
#endif
AVPacket *av_packet_alloc(void) { return (AVPacket *)calloc(1, sizeof(AVPacket)); }
void av_packet_free(AVPacket **pkt) {
    if (!pkt || !*pkt) return;
    free((*pkt)->data);
    free(*pkt);
    *pkt = NULL;
}
int av_new_packet(AVPacket *pkt, int size) {
    pkt->data = (uint8_t *)calloc(1, (size_t)(size > 0 ? size : 0) + 64);   /* (FFmpeg's padding) */
    pkt->size = size;
    return pkt->data ? 0 : AVERROR(ENOMEM);
}
AVFrame *av_frame_alloc(void) { return (AVFrame *)calloc(1, sizeof(AVFrame)); }
void av_frame_free(AVFrame **frame) {
    if (!frame) return;
    free(*frame);
    *frame = NULL;
}
void av_frame_unref(AVFrame *frame) { (void)frame; }
#ifndef HOOKSHOT_PS5_DAV1D
struct SwsContext *sws_getContext(int sw, int sh, enum AVPixelFormat sf, int dw, int dh, enum AVPixelFormat df, int flags,
                                  void *src_filter, void *dst_filter, const double *param) {
    (void)sw, (void)sh, (void)sf, (void)dw, (void)dh, (void)df, (void)flags, (void)src_filter, (void)dst_filter, (void)param;
    return NULL;
}
int sws_scale(struct SwsContext *c, const uint8_t *const src[], const int src_stride[], int y, int h, uint8_t *const dst[],
              const int dst_stride[]) {
    (void)c, (void)src, (void)src_stride, (void)y, (void)h, (void)dst, (void)dst_stride;
    return 0;
}
void sws_freeContext(struct SwsContext *c) { (void)c; }
#endif
#endif

void hos_prof_start(void) {
    if (getenv("HOS_PROFILE") || getenv("HOS_SAMPLE") || getenv("HOS_WATCH"))
        hos_log("prof: the Linux profilers (perf_event_open, /proc) are not in the PS5 build");
}

/* (ps5, merged with longshot dbad7d12) Upstream's crash records and crash window (host/hos_crash_record.c,
   hos_crash_dialog.c) and the report's run id (hos_report_run.c) are not built for the console (nx2/build.py
   PS5_HOST_LEFT_OUT): a record is a marker file per run and a pending/ folder in the game folder, written at every start,
   and the window is a helper process started beside the game, neither of which the console had; the console's own trail
   (host/posix/ps5_early.c, hookshot_stages.txt) and the UDP log say where a run stopped. These answer as a run that records
   nothing: no helper (-1), no window, no record */
#include "../hos_crash_record.h"
int hos_crash_helper_main(int argc, char **argv) { (void)argc, (void)argv; return -1; }
void hos_crash_watch_start(int argc, char **argv) { (void)argc, (void)argv; }
void hos_crash_test_arm(void) {}
void hos_crash_record_start(const char *dir, const char *version, const char *log_path) { (void)dir, (void)version, (void)log_path; }
void hos_crash_clean_end(void) {}
void hos_crash_quit_asked(void) {}
const char *hos_crash_record_run(void) { return NULL; }
const char *hos_crash_record_log(void) { return NULL; }
const char *hos_crash_record_dir(void) { return NULL; }
void hos_crash_record_log_notes(void) {}
int hos_crash_record_write(const HosCrashContext *context) { (void)context; return 0; }
const char *hos_report_run_id(void) { return "unknown"; }
#endif /* __PROSPERO__ */
