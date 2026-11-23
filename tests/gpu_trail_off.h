// (tests) The GPU trail's calls for a fixture that compiles a piece of the renderer alone: gpu/vk/vk_gputrail.h's names as
// calls that take anything and do nothing, with gpu/vk/gpu_trail.h's own kinds. The fixtures that cut create(), clear_tex,
// upscale_stub or replay_handle out of the source check something else (an image's first layout, a clear's mip, the stub's
// views, the segments' order); tests/test_gpu_trail.py checks that the record calls are where they belong.
#pragma once

#include "../gpu/vk/gpu_trail.h"

namespace gt = mkwvk::gpu_trail;
template <class... A> inline void gt_note(A&&...) {}
template <class... A> inline void gt_note_num(A&&...) {}
template <class... A> inline void gt_note_num_next(A&&...) {}
template <class... A> inline void gt_note_tex(A&&...) {}
template <class... A> inline void gt_note_tex_next(A&&...) {}
template <class... A> inline void trail_note(A&&...) {}   // (vk_xfer.cpp's own: a copy or a clear by its texture's side)
template <class... A> inline void gt_draw(A&&...) {}
template <class... A> inline void gt_dispatch(A&&...) {}
template <class... A> inline void gt_begin_recording(A&&...) {}
inline void gt_end_recording_thread() {}
template <class... A> inline gt::Acc* gt_slot(A&&...) { return nullptr; }
template <class... A> inline uint64_t gt_submit(A&&...) { return 0; }
template <class... A> inline void gt_back(A&&...) {}
template <class... A> inline void gt_seen_value(A&&...) {}
struct GtWait {
    template <class... A> explicit GtWait(A&&...) {}
    ~GtWait() {}
};
struct GtCall {
    template <class... A> explicit GtCall(A&&...) {}
    ~GtCall() {}
};
struct GtScope {
    GtScope() {}
    ~GtScope() {}
    template <class... A> uint64_t submit(A&&...) { return 0; }
};
