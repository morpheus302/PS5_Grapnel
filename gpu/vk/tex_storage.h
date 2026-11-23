/* (ps5) What the textures' storage holds of the game's memory: the rules of the storage report (vk_texture.cpp
   storage_report), apart from Vulkan and from the window so that tests/test_tex_storage.py compiles and runs them.

   A texture exists twice in this port: the game's copy in its own memory (the storage, often ASTC at full size) and the
   image the renderer made of it. The report says how much of that memory the storage takes, how much of it has memory
   behind it now, and for how much of that the image already holds everything and nothing has written since: what the port
   could in principle stop backing. It measures; it changes nothing. */
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mkwvk::tex_storage {

/// What a texture is to the report
enum Kind : uint8_t {
    ASTC,     // ASTC the renderer decodes (kept as RGBA8 or re-encoded as BC3): the image is not the storage's bytes
    BLOCK,    // block-compressed, kept as it is
    PLAIN,    // a plain format, uploaded from memory something wrote
    TARGET,   // a plain format the GPU wrote, or one never uploaded from memory anything wrote: a render target's storage
    KINDS
};
inline Kind kind_of(bool astc_decoded, bool block_format, bool gpu_written, bool read_from_memory) {
    if (astc_decoded) return ASTC;
    if (block_format) return BLOCK;
    return gpu_written || !read_from_memory ? TARGET : PLAIN;
}

/// What a texture's storage is to its image now, the least needed first: a range several textures share is the most
/// needed one's (pieces_of)
enum State : uint8_t {
    UNREAD,    // the image is the GPU's, or was never filled from memory anything wrote (never sampled over storage render
               // targets took, cleared in an upload's place): nothing of the storage is in use
    CLEAN,     // uploaded from it, and nothing written there since: the image has all of it
    PENDING,   // not uploaded yet: its first sampling reads all of it
    WRITTEN,   // written since its upload (or since the game finalized what the GPU drew): the next sampling reads that
    STATES
};
/// (gpu_written: the image is the GPU's; not one the GPU only cleared, whose storage texture_touch takes the CPU's writes
/// from as of any uploaded texture. written_since: asked from the generation texture_touch would ask from, and not at
/// all of an image the GPU wrote that the game still has, whose storage texture_touch does not look at)
inline State state_of(bool uploaded, bool gpu_written, bool read_from_memory, bool written_since) {
    if (gpu_written) return written_since ? WRITTEN : UNREAD;
    if (!uploaded) return PENDING;
    if (written_since) return WRITTEN;
    return read_from_memory ? CLEAN : UNREAD;
}

/// How long ago a texture was last used, in presents: in the last 8 (drawn now, the "used in the last 8 presents" of the
/// textures' line), since the last report (600), in the last 3600, longer
enum : uint8_t { AGES = 4 };
inline uint8_t age_of(uint64_t presents_unused) {
    return presents_unused < 8 ? 0 : presents_unused < 600 ? 1 : presents_unused < 3600 ? 2 : 3;
}

/// A run of guest memory that is a texture's storage (one a texture, or one for each mapped run of a virtual pool's)
struct Span {
    uint64_t lo = 0, hi = 0;
    Kind kind = TARGET;
    State state = UNREAD;
    uint64_t unused = 0;   // presents since its texture was last used
};
inline bool more_needed(const Span& a, const Span& b) {
    return a.state != b.state ? a.state > b.state : a.unused < b.unused;
}

/// The spans as disjoint pieces in ascending order, each range of memory once: where textures share storage (several
/// descriptions of one storage, a frame's transient targets made over one another) the piece is the most needed one's,
/// and of equals the one used last. Neighbours alike are one piece. (spans: sorted here)
inline void pieces_of(std::vector<Span>& spans, std::vector<Span>& out) {
    out.clear();
    std::erase_if(spans, [](const Span& s) { return s.hi <= s.lo; });
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.lo < b.lo; });
    std::vector<uint64_t> cuts;
    cuts.reserve(spans.size() * 2);
    for (const Span& s : spans) cuts.push_back(s.lo), cuts.push_back(s.hi);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::vector<const Span*> over;   // (the spans over the piece being made)
    size_t next = 0;
    for (size_t c = 0; c + 1 < cuts.size(); c++) {
        const uint64_t a = cuts[c], b = cuts[c + 1];
        std::erase_if(over, [a](const Span* s) { return s->hi <= a; });
        while (next < spans.size() && spans[next].lo <= a) over.push_back(&spans[next++]);
        if (over.empty()) continue;
        const Span* best = over[0];
        for (const Span* s : over)
            if (more_needed(*s, *best)) best = s;
        if (!out.empty() && out.back().hi == a && out.back().kind == best->kind && out.back().state == best->state &&
            out.back().unused == best->unused)
            out.back().hi = b;
        else
            out.push_back(Span{a, b, best->kind, best->state, best->unused});
    }
}

struct Totals {
    uint64_t once[KINDS]{};           // storage, each range of memory once
    uint64_t backed[KINDS]{};         // of it, in units of the window with memory of their own
    uint64_t pinned[KINDS]{};         // of that, in units an import pinned (never given back)
    uint64_t state_backed[STATES]{};  // the backed part by what the storage is to its image
    uint64_t clean_backed[KINDS]{};   // the CLEAN part of it by kind
    uint64_t clean_age[AGES]{};       // and by how long ago its texture was last used
    uint64_t free_unread = 0;         // what a decommit would give back of the UNREAD pieces alone
    uint64_t free_from[AGES]{};       // and of those with the CLEAN ones unused for at least that long ([0]: all of them)
};

/// The pieces counted. window(lo, hi, &pinned, &free): the bytes of guest [lo, hi) with memory of their own, of them those
/// pinned, and what a decommit of the range would give back (whole units, backed, not pinned; either pointer may be null):
/// host/posix/ps5_window.c hos_ps5_guest_backed_bytes on the console. What a decommit would give back is asked of runs of
/// pieces next to one another, since a unit two textures share goes only when both let it.
template <class Window> void count(const std::vector<Span>& pieces, Window&& window, Totals& t) {
    for (const Span& p : pieces) {
        uint64_t pinned = 0;
        const uint64_t backed = window(p.lo, p.hi, &pinned, nullptr);
        t.once[p.kind] += p.hi - p.lo;
        t.backed[p.kind] += backed;
        t.pinned[p.kind] += pinned;
        t.state_backed[p.state] += backed;
        if (p.state == CLEAN) t.clean_backed[p.kind] += backed, t.clean_age[age_of(p.unused)] += backed;
    }
    const auto given_back = [&](int from_age) {   // (-1: the UNREAD pieces alone)
        uint64_t sum = 0, lo = 0, hi = 0;
        const auto run_done = [&] {
            uint64_t free = 0;
            if (hi > lo) window(lo, hi, nullptr, &free);
            sum += free, lo = hi = 0;
        };
        for (const Span& p : pieces) {
            const bool in = p.state == UNREAD || (from_age >= 0 && p.state == CLEAN && age_of(p.unused) >= from_age);
            if (in && hi > lo && hi == p.lo) {
                hi = p.hi;
                continue;
            }
            run_done();
            if (in) lo = p.lo, hi = p.hi;
        }
        run_done();
        return sum;
    };
    t.free_unread = given_back(-1);
    for (int age = 0; age < AGES; age++) t.free_from[age] = given_back(age);
}

} // namespace mkwvk::tex_storage
