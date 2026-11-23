// (ps5) The GPU trail's calls: what the renderer's files call to feed gpu_trail.h's ring and to have it written out
// (vk_gputrail.cpp). RX_VK_GPU_TRAIL=0: off, =1: on; unset: on on the console, off elsewhere. The switch is read at the first
// call that asks (gt_on), not in a static initializer: the console's settings file is read in main.
//
// Off, a draw or a dispatch pays one load of gt_state and a branch, and nothing else changes: no memory, no thread, no line.
// On: gpu_trail.h says what a draw costs; about 1 MB is allocated once; nothing is recorded into the command buffers and no
// Vulkan call is added, moved or changed except the reads of the timelines' values when a dump is written.
// A draw or a dispatch finds its recording by the command buffer it is recorded into (no thread-local: on the console each
// one is a call, __emutls_get_address); the rare notes go by the thread's gt_acc.
#pragma once

#include "gpu_trail.h"

namespace mkwvk {
namespace gt = gpu_trail;

extern std::atomic<int> gt_state;   // 0: not asked yet, 1: on, -1: off (read with acquire: gt_trail is written before it says on)
extern gt::Trail* gt_trail;         // made when the switch is found on, never freed
bool gt_ask();                      // reads the switch (once), makes the trail
inline bool gt_on() {
    const int s = gt_state.load(std::memory_order_acquire);
    return s > 0 || (s == 0 && gt_ask());
}
/// The recording this thread records into (null: none, or the trail is off): where its notes go
inline constinit thread_local gt::Acc* gt_acc = nullptr;

/// A recording starts on this thread in that slot, into that command buffer (draw_begin_recording): the draws and dispatches
/// recorded into cmd, and this thread's notes, go to the slot's Acc until its submit (gt_submit) or gt_end_recording_thread
void gt_begin_recording(int slot, const void* cmd);
/// This thread records no more into its slot (a parallel replay's worker, whose segment the replay thread submits)
inline void gt_end_recording_thread() {
    if (gt_state.load(std::memory_order_acquire) > 0) gt_acc = nullptr;
}
inline gt::Acc* gt_slot(int slot) {
    return gt_state.load(std::memory_order_acquire) > 0 && (unsigned)slot < (unsigned)gt::kSlots ? &gt_trail->acc[slot] : nullptr;
}

/// A draw of program key recorded into cmd (gt::K_DRAW, K_MESH: mesh shaders run as such, K_FALLBACK: the fallback's vertex draw)
inline void gt_draw(const void* cmd, uint64_t key, uint8_t flags) {
    if (gt_state.load(std::memory_order_acquire) <= 0) return;
    gt_trail->draw_cmd(cmd, key, flags);
}
/// A dispatch of program key recorded into cmd (gt::K_DISPATCH, K_FB_DISPATCH: the mesh fallback's; with K_INDIRECT), x * y * z
/// workgroups
inline void gt_dispatch(const void* cmd, uint64_t key, uint8_t flags, uint32_t x, uint32_t y, uint32_t z) {
    if (gt_state.load(std::memory_order_acquire) <= 0) return;
    gt_trail->dispatch_cmd(cmd, key, flags, x, y, z);
}
/// Another command: an image's (dim 1 to 3, 0: none; its VkFormat, its NVN format or 0, its extent)
inline void gt_note(gt::Kind kind, unsigned dim, uint32_t format, uint32_t nvn, uint32_t w, uint32_t h, uint32_t d) {
    if (gt_on()) gt_trail->note(gt_acc, kind, dim, format, nvn, w, h, d, 0);
}
/// The same with a number: bytes, or the newest submission value (gpu_trail.h's S_BYTES and S_VALUE kinds)
inline void gt_note_num(gt::Kind kind, uint64_t num) {
    if (gt_on()) gt_trail->note(gt_acc, kind, 0, 0, 0, 0, 0, 0, num);
}
/// The same of a Texture (vk_texture.h): its image's type, format and size
template <class Tex>
inline void gt_note_tex(gt::Kind kind, const Tex& t) {
    if (gt_on()) gt_trail->note(gt_acc, kind, (unsigned)t.type + 1, (uint32_t)t.format, (uint32_t)t.nvn_format, t.width, t.height, t.depth, 0);
}

/// The same for the queue call written next (a read-back's submit, a sparse bind), whatever recording this thread has open
inline void gt_note_num_next(gt::Kind kind, uint64_t num) {
    if (gt_on()) gt_trail->note(nullptr, kind, 0, 0, 0, 0, 0, 0, num);
}
template <class Tex>
inline void gt_note_tex_next(gt::Kind kind, const Tex& t) {
    if (gt_on()) gt_trail->note(nullptr, kind, (unsigned)t.type + 1, (uint32_t)t.format, (uint32_t)t.nvn_format, t.width, t.height, t.depth, 0);
}

/// A submit, or another call on the queue, written into the ring with what a holds (null: no recording's) and the notes made
/// outside any recording. call: the driver is called for it next, and gt_back says when that call returned. Returns the
/// entry's number (0: the trail is off)
uint64_t gt_submit(gt::EntryKind kind, gt::Acc* a, int slot, uint64_t seq, uint64_t value, uint64_t signal, uint64_t batch, unsigned ncb,
                   int reason, bool call);
void gt_back(uint64_t entry, int result);

/// Until it ends, this thread's notes go into an Acc of its own, for the entry submit() writes (a present's command buffer)
struct GtScope {
    gt::Acc acc;   // (not touched when the trail is off)
    gt::Acc* before = nullptr;
    bool on;
    GtScope() : on(gt_on()) {
        if (on) acc.reset(), before = gt_acc, gt_acc = &acc;
    }
    ~GtScope() {
        if (on) gt_acc = before;
    }
    uint64_t submit(gt::EntryKind kind, unsigned ncb) { return on ? gt_submit(kind, &acc, 0, 0, 0, 0, 0, ncb, 0, true) : 0; }
    GtScope(const GtScope&) = delete;
    GtScope& operator=(const GtScope&) = delete;
};

/// A call on the queue that is no submit (the present itself): while it lasts, a dump says that it is not back
struct GtCall {
    bool on;
    explicit GtCall(const char* what) : on(gt_state.load(std::memory_order_acquire) > 0) {
        if (on) gt_trail->other_begin(what);
    }
    ~GtCall() {
        if (on) gt_trail->other_end();
    }
    GtCall(const GtCall&) = delete;
    GtCall& operator=(const GtCall&) = delete;
};

/// A wait for a submission (a fence, a timeline value): while it lasts it can be seen by the watcher (gt_poll), which has the
/// trail written out when one has lasted 5 s. The wait itself is not changed
int gt_wait_begin(const char* what, uint64_t value);
void gt_wait_end(int slot);
struct GtWait {
    int slot = -1;
    GtWait(const char* what, uint64_t value) {
        if (gt_state.load(std::memory_order_acquire) > 0) slot = gt_wait_begin(what, value);
    }
    ~GtWait() {
        if (slot >= 0) gt_wait_end(slot);
    }
    GtWait(const GtWait&) = delete;
    GtWait& operator=(const GtWait&) = delete;
};

/// The newest submission value the renderer itself read as done (for a dump's first line, written before the driver is asked)
extern std::atomic<uint64_t> gt_value_seen;
inline void gt_seen_value(uint64_t v) {
    if (gt_state.load(std::memory_order_acquire) > 0) gt_value_seen.store(v, std::memory_order_relaxed);
}

/// (vk_replay.cpp) The timelines' values as the driver gives them now, and the submission values handed out
void vk_trail_timelines(gt::Now& now);

/// The trail written out (the stage trail on the console, the log elsewhere), once for a stall: asked again with nothing
/// written and no timeline moved since, it says so in one line. From any thread; nothing when the trail is off.
/// (declared in vk_internal.h too, for the queue's lock)
void gpu_trail_dump(const char* why);
/// The watcher's tick, about once a second (the host's report thread on the console: rx_ps5_vk_gpu_trail_poll)
void gt_poll();

} // namespace mkwvk
