// (ps5, 2026-11-15) The rules for the renderer's texture peaks when the view changes (vk_texture.cpp), apart so that they are
// tested without a GPU: tests/test_tex_peaks.py. In Hyrule Castle Town the console's free direct memory fell to 239 MiB (the
// floor is 192) a few times an hour, each time while the next view's textures were made by the hundred (1100-1500 textures,
// 1.5-1.7 GB, to 2200-2500, 2.2-2.5 GB, in about 4 s) and the last view's waited the 60 presents of RX_VK_EVICT_MIN_UNUSED:
// 3.2 to 4 s at the 15-19 presents a second of those views (logs/udp/essai-lot34-part47-1.txt; notes-agents/
// pics-textures-citadelle.md). Each rule is off unless its setting asks for it; the burst line in the log is on.
#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace tex_peaks {

// The image bytes of the textures made at each present, the sampled ones (made for a draw to read, uploaded from the game's
// memory) apart from the render targets, over the last kSlots presents
struct MadeRing {
    static constexpr unsigned kSlots = 128;
    uint64_t at[kSlots] = {};        // (the present a slot holds)
    uint64_t sampled[kSlots] = {};
    uint64_t targets[kSlots] = {};
    void add(uint64_t frame, uint64_t bytes, bool target) {
        const unsigned i = (unsigned)(frame % kSlots);
        if (at[i] != frame) at[i] = frame, sampled[i] = targets[i] = 0;
        (target ? targets[i] : sampled[i]) += bytes;
    }
    // (over the presents (frame - window, frame]; window at most kSlots)
    uint64_t sum(uint64_t frame, uint64_t window, bool target) const {
        uint64_t s = 0;
        for (unsigned i = 0; i < kSlots; i++)
            if (at[i] && at[i] <= frame && frame - at[i] < window) s += target ? targets[i] : sampled[i];
        return s;
    }
};

// A burst: at least min_bytes of sampled textures made over the last 60 presents (RX_VK_EVICT_BURST_MB; 0: every present
// counts as one)
inline bool burst(uint64_t made_sampled, uint64_t min_bytes) {
    return !min_bytes || made_sampled >= min_bytes;
}

// The presents a texture must have gone unused before the pass may let it go. base: the pass's own (RX_VK_EVICT_MIN_UNUSED,
// 60, over the budget). sampled (RX_VK_EVICT_BURST_UNUSED, 0: off; at least 8): during a burst, over the budget, for a
// texture the GPU never drew (its texels are the game's memory, and one made again is uploaded from there before the draw
// that wants it) that has lived at least made_min presents itself (RX_VK_EVICT_BURST_MADE, the base by default: one the
// burst made stays; it is made at its first draw, and in a load the next may come many presents later. On the PC a view
// switched with the camera made hundreds of MB of textures in its few moving presents, which that line kept 60 presents)
inline uint64_t need_unused(uint64_t base, uint64_t sampled, bool in_burst, bool over_budget, bool gpu_written,
                            uint64_t presents_since_made, uint64_t made_min) {
    if (!sampled || !in_burst || !over_budget || gpu_written || presents_since_made < made_min) return base;
    const uint64_t s = sampled < 8 ? 8 : sampled;
    return s < base ? s : base;
}

// The pass runs at this present: every 10th, or every one under memory pressure (RX_VK_EVICT_PRESSURE_MB), or during a burst
// when RX_VK_EVICT_BURST_EVERY asks for it
inline bool pass_now(uint64_t frame, bool pressed, bool in_burst, bool every_in_burst) {
    return frame % 10 == 0 || pressed || (every_in_burst && in_burst);
}

// A pooled render target's image is destroyed once it has waited unused more than hold presents (RX_VK_TARGET_POOL_HOLD,
// 120 by default), or while the pool is over its bound
inline bool pool_drop(uint64_t pool_bytes, uint64_t cap, uint64_t frame, uint64_t since, uint64_t hold) {
    return pool_bytes > cap || frame - since > hold;
}

// How many pooled images the trim may destroy at this present: 64 while VRAM is short (the pool given back), 16 otherwise, and
// over the pool's bound RX_VK_TARGET_POOL_TRIM when it asks for more (16 by default: as before)
inline uint64_t trim_most(bool pressed, bool over_cap, uint64_t trim) {
    if (pressed) return 64;
    return over_cap && trim > 16 ? trim : 16;
}

// ---- (ps5, lot 63, 2026-11-22) Sampled textures kept while the console's free direct memory is large ----
// RX_VK_EVICT_KEEP_FREE_MB=<MiB> (0 or unset: off, the pass as it was). On Castle Town's crowded market the eviction pass let go,
// every 10 presents, of every sampled texture unused 20 presents (the burst's age) or 60: the cache is always over its budget
// there (1.1 to 2.0 GB of textures for 1,024 MB, and the 90 % rule on the driver's count, which counts its device memory twice),
// so it takes all that the age allows. 73 to 100 % of the textures uploaded in the slow frames there were such textures drawn
// again within 600 presents, each ~0.72 ms of GPU (decode and BC3 encode, ~20 passes the render thread waits for in its
// submit), while the median free direct memory was ~1.7 GiB (notes-agents/drops-gros-batiments.md, lot63-garder-textures.md).
// The rule: the pass chooses what goes by its own rules, as before; then, of the sampled textures it chose (the candidates),
// the most recently used stay, as many as fit in the room, which is the free memory as the pass would leave it less the
// threshold, at most RX_VK_EVICT_KEEP_MB (512); none unused RX_VK_EVICT_KEEP_UNUSED presents or more (1800). Render targets
// and everything else the pass chose go as before. When the free memory as the pass would leave it is under the threshold,
// the rule is off (the pass as before: all it holds goes), and it comes back once that is RX_VK_EVICT_KEEP_HYST_MB (256) over
// the threshold. Under the threshold by more than 64 MiB at a present while it holds textures, the pass runs at that present
// (keep_due) instead of the next 10th. tests/test_tex_keep.py
struct KeepRule {
    bool on = false;
    // The bytes of this pass's candidates that may stay. free_mb: the free direct memory now (-1: not known, and off);
    // let_go: the bytes of all the pass chose (KeepPass::let_go); threshold_mb: RX_VK_EVICT_KEEP_FREE_MB (0: off); hyst_mb:
    // back on only that far over the threshold; cap: RX_VK_EVICT_KEEP_MB
    uint64_t room(int64_t free_mb, uint64_t let_go, uint64_t threshold_mb, uint64_t hyst_mb, uint64_t cap) {
        if (!threshold_mb || free_mb < 0) {
            on = false;
            return 0;
        }
        const uint64_t after = (uint64_t)free_mb + (let_go >> 20);   // (MiB free once all the pass chose went, as before)
        if (on && after < threshold_mb) on = false;
        else if (!on && after >= threshold_mb + hyst_mb) on = true;
        if (!on) return 0;
        const uint64_t r = (after - threshold_mb) << 20;
        return r < cap ? r : cap;
    }
};

// What one pass chose, for the rule above. T: the renderer's Texture (gpu_written, last_frame, bytes, pad, id)
template <class T>
struct KeepPass {
    std::vector<std::pair<uint64_t, T*>> candidates;   // (presents unused, the texture)
    uint64_t let_go = 0;                                // (bytes: all the pass chose)
    // A texture the pass chose to let go, by its own rules. A sampled one unused fewer than keep_unused presents
    // (RX_VK_EVICT_KEEP_UNUSED) is a candidate; a render target never: its image may be its texels' only copy, and its rules
    // stay as they are
    void chosen(T* t, uint64_t frame, uint64_t keep_unused) {
        let_go += t->bytes + t->pad;
        if (t->gpu_written) return;
        const uint64_t unused = frame - t->last_frame;
        if (unused < keep_unused) candidates.push_back({unused, t});
    }
    // The candidates that stay: the most recently used first, while their bytes fit in room (the first that does not fit
    // ends the list: the older ones go). Returns their bytes
    uint64_t pick(uint64_t room, std::vector<T*>& held) {
        held.clear();
        if (!room || candidates.empty()) return 0;
        std::sort(candidates.begin(), candidates.end(), [](const std::pair<uint64_t, T*>& a, const std::pair<uint64_t, T*>& b) {
            return a.first != b.first ? a.first < b.first : a.second->id > b.second->id;
        });
        uint64_t sum = 0;
        for (const auto& [unused, t] : candidates) {
            const uint64_t b = t->bytes + t->pad;
            if (sum + b > room) break;
            sum += b;
            held.push_back(t);
        }
        return sum;
    }
};

// The pass runs at this present too: the rule holds textures and the free memory is under its threshold by more than 64 MiB
// (an area's first frame can make ~1 GB of textures before the next present: they are given back at the next one, not at
// the next 10th; a smaller shortfall waits for the 10th)
inline bool keep_due(uint64_t held, int64_t free_mb, uint64_t threshold_mb) {
    return held && threshold_mb && free_mb >= 0 && (uint64_t)free_mb + 64 < threshold_mb;
}

} // namespace tex_peaks
