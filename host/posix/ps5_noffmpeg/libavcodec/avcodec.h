/* (ps5) No FFmpeg for the console yet (docs/ps5-port.md, gap 9): the part of libavcodec's interface host/nn_video.c uses,
   so it builds unchanged. Every decoder is "not available in this FFmpeg" (avcodec_find_decoder answers NULL), and the
   game's movies are skipped as on a Linux build without dav1d. On the include path of RX_TARGET=ps5 builds only while
   HOOKSHOT_PS5_FFMPEG names no FFmpeg built for the console (nx2/build.py). The stand-ins: host/posix/ps5_stubs.c */
#ifndef HOOKSHOT_PS5_NOFFMPEG_AVCODEC_H
#define HOOKSHOT_PS5_NOFFMPEG_AVCODEC_H
#include <errno.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum AVCodecID { AV_CODEC_ID_NONE, AV_CODEC_ID_VP8 = 139, AV_CODEC_ID_VP9 = 167, AV_CODEC_ID_AV1 = 225 };
enum AVPixelFormat { AV_PIX_FMT_NONE = -1, AV_PIX_FMT_YUV420P = 0, AV_PIX_FMT_RGBA = 26, AV_PIX_FMT_NV12 = 23, AV_PIX_FMT_YUVA420P = 33 };
#define AVERROR(e) (-(e))
typedef struct AVCodec { const char *name; } AVCodec;
typedef struct AVCodecContext { int thread_count; } AVCodecContext;
typedef struct AVPacket { uint8_t *data; int size; int64_t pts; } AVPacket;
typedef struct AVFrame { uint8_t *data[8]; int linesize[8]; int width, height, format; int64_t pts; } AVFrame;
const AVCodec *avcodec_find_decoder(enum AVCodecID id);
const AVCodec *avcodec_find_decoder_by_name(const char *name);
AVCodecContext *avcodec_alloc_context3(const AVCodec *codec);
int avcodec_open2(AVCodecContext *ctx, const AVCodec *codec, void **options);
void avcodec_free_context(AVCodecContext **ctx);
void avcodec_flush_buffers(AVCodecContext *ctx);
int avcodec_send_packet(AVCodecContext *ctx, const AVPacket *pkt);
int avcodec_receive_frame(AVCodecContext *ctx, AVFrame *frame);
AVPacket *av_packet_alloc(void);
void av_packet_free(AVPacket **pkt);
int av_new_packet(AVPacket *pkt, int size);
AVFrame *av_frame_alloc(void);
void av_frame_free(AVFrame **frame);
void av_frame_unref(AVFrame *frame);
#ifdef __cplusplus
}
#endif
#endif
