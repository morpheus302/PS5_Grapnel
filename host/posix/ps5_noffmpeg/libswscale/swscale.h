/* (ps5) No FFmpeg for the console yet: the part of libswscale's interface host/nn_video.c uses (libavcodec/avcodec.h
   beside this says why). No conversion is ever made: no decoder gives a picture. */
#ifndef HOOKSHOT_PS5_NOFFMPEG_SWSCALE_H
#define HOOKSHOT_PS5_NOFFMPEG_SWSCALE_H
#include "../libavcodec/avcodec.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SWS_BILINEAR 2
struct SwsContext;
struct SwsContext *sws_getContext(int sw, int sh, enum AVPixelFormat sf, int dw, int dh, enum AVPixelFormat df, int flags,
                                  void *src_filter, void *dst_filter, const double *param);
int sws_scale(struct SwsContext *c, const uint8_t *const src[], const int src_stride[], int y, int h, uint8_t *const dst[],
              const int dst_stride[]);
void sws_freeContext(struct SwsContext *c);
#ifdef __cplusplus
}
#endif
#endif
