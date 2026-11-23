// (ps5, 2026-11-12) The rules that hold down the renderer's memory while a scene loads (vk_texture.cpp), apart so that they
// are tested without a GPU: tests/test_load_peak.py. Leaving Kokiri Forest the title ended at its memory floor three times;
// in the last of them the driver's memory took 1208 MiB in 4.5 s, the device heap's blocks 1204 of it, nearly all of it the
// next scene's images (notes-agents/pic-memoire-chargement.md).
#pragma once
#include <cstdint>

namespace load_peak {

// A dead image (its texture evicted or gone) may be destroyed: the GPU is past the last submission that used it, and the
// presents RX_VK_DEAD_HOLD asks for went by since it died. The rule texture_frame applied at each present, and that the
// eviction pass now also applies to what it has just let go (RX_VK_EVICT_FREE_NOW)
inline bool reapable(uint64_t value, uint64_t completed, uint64_t frame, uint64_t died_at, uint64_t hold) {
    return value <= completed && frame >= died_at + hold;
}

// A staging arena may take a submission's uploads from its start: it is not the one the recording fills now, the GPU is
// past the submission that read it last (done: the timeline as last read, at the present or read again for it,
// RX_VK_STAGING_RECHECK), and it is big enough
inline bool arena_free(uint64_t value, uint64_t recording, uint64_t done, uint64_t size, uint64_t n) {
    return value != recording && value <= done && size >= n;
}

// A depth texture's first upload that need not be made (RX_VK_ZERO_DEPTH_MB): the image create() made is cleared to depth 0.0,
// and nothing has been uploaded, drawn, copied or cleared into it since (any of those marks it uploaded); its storage reads
// zero in every byte (no unit of it ever had memory of its own: the console's window) and no capture keeps another copy of it.
struct DepthFirstUpload {
    uint64_t min_bytes = 0;   // (RX_VK_ZERO_DEPTH_MB in bytes; 0: off)
    uint64_t total = 0;       // (the storage the upload would read)
    bool depth_only = false;  // (the image's aspect is depth alone: D16, D32; not stencil, not colour)
    bool first = false;       // (nothing uploaded into it yet)
    bool drawn = false;       // (the GPU wrote it)
    bool sparse = false;
    bool direct = false;      // (one run of guest memory, not a virtual pool's pages)
    bool untouched = false;   // (no unit of the storage has memory of its own)
    bool shadowed = false;    // (a capture's copy of the storage lies over it)
};
// (what is known without looking at the memory: the cheap test every upload passes through)
inline bool depth_skip_possible(const DepthFirstUpload& u) {
    return u.min_bytes && u.total >= u.min_bytes && u.depth_only && u.first && !u.drawn && !u.sparse && u.direct;
}
inline bool depth_skip(const DepthFirstUpload& u) {
    return depth_skip_possible(u) && u.untouched && !u.shadowed;
}

} // namespace load_peak
