// (ps5, lot 67) Three switches on the render thread's cost per draw, off unless set, each its own console test. On the crowded
// market the render thread is the frame's wall (~27 ms of work a ~30 ms frame, ~2,450 draws at ~10.5 us; notes-agents/
// image-de-base-place.md), and RX_VK_PROF_FINE=1 split a draw's time (logs/udp/essai-lot66-AP.txt, medians of the 19 lines at
// 2,000 draws and more): textures 3.88 ms a frame, bind descriptors 1.84, vertex+index binds 1.57, draw call 1.56, user uniform
// buffers 1.42, storage buffers 1.06, bind pipeline 0.97, dynamic state 0.70. None of them changes what the GPU is given:
//
//   RX_VK_TEX_LEAN=1      a stage's sampled textures (build_descriptors) resolved in one call: the thread's objects taken once a
//                         stage instead of once or twice a texture (on the console a thread_local is a call into emulated TLS and
//                         libkernel's pthread_getspecific), no per-texture call for the dump, watch, trace and image-log checks
//                         when none of them is on, and the lines each texture reads first (its slot's version, its handle_hits
//                         entry, its Texture) asked of the memory for the whole stage before the first one is resolved. The same
//                         handles, keys, validity tests, touches, views, samplers and dummies, in the same order; a miss goes
//                         through texture_view_of itself. RX_VK_TEX_LEAN=check (diagnostics, for the PC): the original path, then
//                         the lean one over the same textures, compared ("vk: lean textures (check): ...").
//   RX_VK_SMALL_COSTS=1   upstream Longshot #391's per-draw part (6112edb6, "six small costs of a draw ... removed"): a uniform
//                         buffer's offset tested with a mask, not a 64-bit division; dyn_same's words and bytes with two memcmp;
//                         RX_VK_LOG_PUSH's key formatted only with that switch set. (Not here: its memory parts, vk_memory.cpp being
//                         the memory lots'; dump_on inline, one load; the life log's counter read in draw(), whose block
//                         tests/test_draw_side_effects.py compiles alone, for one thread_local a draw.)
//   RX_VK_INDEX_FIRST=1   an indexed draw's index buffer bound at its buffer's start and the draw's firstIndex its offset in
//                         indices (vkCmdDrawIndexed reads index firstIndex at bound offset + firstIndex * size: the same bytes), so
//                         draws whose indices live in the same pool no longer bind it again each time (0.97 index binds a draw).
//
// What is pure here: the switches' reading, and the arithmetic the tests check against the live source
// (tests/test_draw_lean.py).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#ifndef LONGSHOT_ENV_H
#include "compat/longshot_env.h"
#endif

namespace draw_lean {

/// RX_VK_TEX_LEAN: 0 unset or off, 1 on, 2 "check" (the original path, the lean one beside it, compared). Read once, at the first use
/// (on the console the settings file is applied after the library loads)
inline int tex_lean_parse(const char* v) {
    if (!v || env_value_off(v)) return 0;
    return std::strcmp(v, "check") == 0 ? 2 : 1;
}
inline int tex_lean_mode() {
    static const int v = tex_lean_parse(std::getenv("RX_VK_TEX_LEAN"));
    return v;
}
inline bool small_costs_on() {
    static const bool v = env_on("RX_VK_SMALL_COSTS");
    return v;
}
inline bool index_first_on() {
    static const bool v = env_on("RX_VK_INDEX_FIRST");
    return v;
}

/// texture_view_of's key into handle_hits (the slot, the view type, the use, the format a storage view is made as)
inline uint64_t hit_key(uint32_t handle, uint32_t view_type, uint32_t use, uint32_t storage_as) {
    return (uint64_t)(handle & 0xFFFFF) | (uint64_t)view_type << 20 | (uint64_t)use << 28 | (uint64_t)storage_as << 32;
}
/// A remembered resolution stands while its slot was not registered anew and no texture went since (texture_view_of's test)
inline bool hit_stands(uint32_t kept_version, uint64_t kept_destroy_gen, uint32_t version, uint64_t destroy_gen) {
    return kept_version == version && kept_destroy_gen == destroy_gen;
}

/// The textures a lean stage resolves after one round of prefetches: at most this many (a stage has a few; more: no prefetch)
constexpr uint32_t kPrefetchMost = 64;

/// RX_VK_SMALL_COSTS: an offset's alignment tested with a mask. Vulkan's minUniformBufferOffsetAlignment is a power of two (the
/// specification's limits table), and uniform_range takes at least 16; the same answer as offset % align for those
inline bool aligned_pow2(uint64_t offset, uint64_t align) {
    return !(offset & (align - 1));
}
inline bool is_pow2(uint64_t v) {
    return v && !(v & (v - 1));
}

/// RX_VK_INDEX_FIRST: an indexed draw at offset `offset` in its buffer with `unit`-byte indices, bound at the buffer's start: the
/// draw's firstIndex. False (bind at the offset, as before) when the offset is not a whole number of indices or the index would not
/// fit vkCmdDrawIndexed's 32-bit firstIndex with the draw's count after it
inline bool first_index_for(uint64_t offset, uint32_t unit, uint32_t count, uint32_t* first) {
    if (unit != 1 && unit != 2 && unit != 4) return false;
    if (offset % unit) return false;
    const uint64_t at = offset / unit;
    if (at > 0xFFFFFFFFull - count) return false;
    *first = (uint32_t)at;
    return true;
}

}  // namespace draw_lean
