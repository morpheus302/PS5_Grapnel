// (ps5) The GPU trail's calls (gpu/vk/vk_gputrail.h, the console's) as nothing, for the tests that cut the renderer's code
// into a fixture of their own: the trail only writes down what the GPU is given, and is tested by tests/test_gpu_trail.py.
// Their arguments are not evaluated (a fixture's stand-in device has no swap_format, for one)
#pragma once
#define gt_note(...) ((void)0)
#define gt_note_num(...) ((void)0)
#define gt_note_tex(...) ((void)0)
#define gt_note_num_next(...) ((void)0)
#define gt_note_tex_next(...) ((void)0)
#define gt_draw(...) ((void)0)
#define gt_dispatch(...) ((void)0)
#define gt_submit(...) (0ull)
#define gt_back(...) ((void)0)
struct GtWait {
    template <class... A>
    explicit GtWait(A&&...) {}
};
