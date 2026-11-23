// tex_peaks::KeepRule, KeepPass and keep_due (gpu/vk/tex_peaks.h): sampled textures kept while the console's free direct memory
// is large, RX_VK_EVICT_KEEP_FREE_MB (lot 63). tests/test_tex_keep.py builds and runs this, and checks that vk_texture.cpp
// wires them as the pass below does.
#include "../gpu/vk/tex_peaks.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <vector>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

static const uint64_t MB = 1ull << 20;

struct Tex {
    bool gpu_written = false;
    bool dead = false;   // (a render target the pass may let go: drawn over, a dead transient one)
    uint64_t last_frame = 0, made_frame = 0, last_value = 0, bytes = 0, pad = 0;
    uint32_t id = 0;
};

static void room() {
    tex_peaks::KeepRule k;
    // (room(free_mb, let_go, threshold_mb, hyst_mb, cap))
    expect(k.room(5000, 900 * MB, 0, 256, 512 * MB) == 0 && !k.on, "threshold 0 (the setting unset): off, nothing held");
    expect(k.room(-1, 900 * MB, 1000, 256, 512 * MB) == 0 && !k.on, "free memory not known (-1, or not the console): off");
    expect(k.room(1100, 100 * MB, 1000, 256, 512 * MB) == 0 && !k.on, "1200 MiB once the pass let go, under 1000 + 256: still off");
    expect(k.room(1100, 156 * MB, 1000, 256, 512 * MB) == 256 * MB && k.on, "1256: on, the room is what stays over 1000");
    expect(k.room(1800, 400 * MB, 1000, 256, 512 * MB) == 512 * MB && k.on, "2200 MiB: the room 1200, held to the cap (512)");
    expect(k.room(1800, 400 * MB, 1000, 256, 2048 * MB) == 1200 * MB, "a bigger cap: the room itself");
    expect(k.room(700, 400 * MB, 1000, 256, 512 * MB) == 100 * MB && k.on, "on, 1100 once the pass let go: 100 MB held, not more");
    expect(k.room(700, 300 * MB, 1000, 256, 512 * MB) == 0 && k.on, "on, 1000 exactly: still on, nothing held");
    expect(k.room(600, 300 * MB, 1000, 256, 512 * MB) == 0 && !k.on, "on, 900 once the pass let go: off (all of it goes)");
    // (the way back: off until the free memory as the pass leaves it is 256 MiB over the threshold)
    expect(k.room(1000, 200 * MB, 1000, 256, 512 * MB) == 0 && !k.on, "off, 1200: still off (the hysteresis)");
    expect(k.room(1255, 0, 1000, 256, 512 * MB) == 0 && !k.on, "off, 1255: still off");
    expect(k.room(1256, 0, 1000, 256, 512 * MB) == 256 * MB && k.on, "off, 1256: on again");
    expect(k.room(5000, 0, 1000, 256, 0) == 0, "RX_VK_EVICT_KEEP_MB=0: nothing held");
    expect(k.room(5000, 0, 1000, 0, 512 * MB) == 512 * MB, "RX_VK_EVICT_KEEP_HYST_MB=0: on at the threshold");
    expect(k.room(-1, 0, 1000, 256, 512 * MB) == 0 && !k.on, "the measure lost: off at once");
}

static void chosen_and_pick() {
    std::vector<Tex> t(6);
    for (uint32_t i = 0; i < t.size(); i++) t[i].id = i + 1, t[i].bytes = 10 * MB;
    const uint64_t frame = 1000;
    // (all six chosen by the pass, by its own rules)
    t[0].last_frame = 900;                            // unused 100: a candidate
    t[1].last_frame = 800;                            // 200: a candidate
    t[2].last_frame = 700, t[2].gpu_written = true;   // a render target: never one, it goes as the pass chose
    t[3].last_frame = 100;                            // unused 900: a candidate
    t[4].last_frame = 0;                              // unused 1000, past keep_unused (950 here): goes
    t[5].last_frame = 960, t[5].pad = 1 * MB;         // unused 40 (a burst's age): a candidate, 11 MB with its pad
    tex_peaks::KeepPass<Tex> look;
    for (Tex& x : t) look.chosen(&x, frame, 950);
    expect(look.candidates.size() == 4, "the candidates: the sampled ones the pass chose, unused under keep_unused");
    expect(look.let_go == 61 * MB, "let_go: all the pass chose, render target and the one too long unused with them");
    std::vector<Tex*> held;
    expect(look.pick(0, held) == 0 && held.empty(), "no room: none held");
    expect(look.pick(30 * MB, held) == 21 * MB && held.size() == 2 && held[0] == &t[5] && held[1] == &t[0],
           "30 MB of room: the most recently used first (40, then 100 presents unused), the next does not fit");
    expect(look.pick(41 * MB, held) == 41 * MB && held.size() == 4 && held[3] == &t[3], "room for all: all four");
    expect(look.pick(35 * MB, held) == 31 * MB && held.size() == 3, "the first that does not fit ends it: the older ones go");
    // (the same unused count: the later made first, by id)
    Tex a, b;
    a.id = 5, b.id = 9, a.bytes = b.bytes = 10 * MB, a.last_frame = b.last_frame = 500;
    tex_peaks::KeepPass<Tex> two;
    two.chosen(&a, 1000, 1800);
    two.chosen(&b, 1000, 1800);
    expect(two.pick(10 * MB, held) == 10 * MB && held.size() == 1 && held[0] == &b, "a tie: the one made last stays");
}

static void due() {
    using tex_peaks::keep_due;
    expect(!keep_due(0, 100, 1000), "nothing held: no extra pass");
    expect(!keep_due(64 * MB, 2000, 1000), "over the threshold: no extra pass");
    expect(!keep_due(64 * MB, 936, 1000), "64 MiB short: the next 10th present will do");
    expect(keep_due(64 * MB, 935, 1000), "more than 64 MiB short while it holds textures: the pass now");
    expect(!keep_due(64 * MB, -1, 1000), "free memory not known: no extra pass");
    expect(!keep_due(64 * MB, 100, 0), "the setting off: no extra pass");
}

// ---- a pass as vk_texture.cpp's texture_frame runs it, reduced to what decides a texture's fate: the loop in the order of last
// use, its 7/8 line (over the budget), the age (tex_peaks::need_unused: 60, 20 in a burst for a sampled texture made 20
// presents ago or more; 120 in a sweep), recorded work, the render targets' own rules (here: only a dead one goes); then the
// keep rule's look at what it chose and its choice of what stays. keep == false: none of the rule's lines (the pass before lot 63)
struct Setting {
    bool keep = true;
    uint64_t threshold = 1000, hyst = 256, cap = 512 * MB, keep_unused = 1800;
};
struct Pass {
    std::set<uint32_t> gone;   // (ids)
    uint64_t held = 0, let_go = 0;
};
static Pass pass(std::vector<Tex>& cache, uint64_t frame, uint64_t completed, uint64_t budget, bool burst, int64_t free_mb,
                 tex_peaks::KeepRule& rule, const Setting& s) {
    Pass r;
    uint64_t total = 0;
    for (const Tex& t : cache) total += t.bytes + t.pad;
    const bool sweep = total <= budget;
    const uint64_t need_base = sweep ? 120 : 60;
    std::vector<Tex*> order;
    for (Tex& t : cache) order.push_back(&t);
    std::stable_sort(order.begin(), order.end(), [](const Tex* a, const Tex* b) { return a->last_frame < b->last_frame; });
    std::vector<Tex*> chosen;
    uint64_t pending = 0;
    for (Tex* t : order) {
        if ((!sweep && (pending >= total || total - pending <= budget / 8 * 7)) || frame - t->last_frame < 3) break;
        const uint64_t need = tex_peaks::need_unused(need_base, 20, burst, !sweep, t->gpu_written, frame - t->made_frame, 20);
        if (frame - t->last_frame < need && !t->dead) continue;
        if (t->last_value > completed) continue;
        if (t->gpu_written && !t->dead) continue;
        if (sweep && !t->dead) continue;
        chosen.push_back(t);
        pending += t->bytes + t->pad;
    }
    if (s.keep) {
        tex_peaks::KeepPass<Tex> look;
        for (Tex* t : chosen) look.chosen(t, frame, s.keep_unused);
        std::vector<Tex*> held;
        r.held = look.pick(rule.room(free_mb, look.let_go, s.threshold, s.hyst, s.cap), held);
        r.let_go = look.let_go;
        chosen.erase(std::remove_if(chosen.begin(), chosen.end(),
                                    [&](Tex* t) { return std::find(held.begin(), held.end(), t) != held.end(); }),
                     chosen.end());
    }
    for (const Tex* t : chosen) r.gone.insert(t->id);
    return r;
}

static std::vector<Tex> random_cache(std::mt19937& g, uint64_t frame, size_t n) {
    std::vector<Tex> c(n);
    for (size_t i = 0; i < n; i++) {
        Tex& t = c[i];
        t.id = (uint32_t)i + 1;
        t.gpu_written = g() % 4 == 0;
        t.dead = t.gpu_written && g() % 3 == 0;
        t.bytes = (1 + g() % 16) * MB / 4;
        t.last_frame = frame - g() % 2400;
        t.made_frame = t.last_frame - g() % 300;
        t.last_value = g() % 20 == 0 ? 200 : 10;   // (one in twenty still read by recorded work)
    }
    return c;
}

static bool same_targets(const Pass& a, const Pass& b, const std::vector<Tex>& c) {
    for (const Tex& t : c)
        if (t.gpu_written && a.gone.count(t.id) != b.gone.count(t.id)) return false;
    return true;
}

static void as_a_pass() {
    std::mt19937 g(63);
    const uint64_t frame = 100000, completed = 100;
    int over_ok = 0, under_ok = 0, off_ok = 0, targets_ok = 0, recent_ok = 0, room_ok = 0, cap_ok = 0, cases = 0;
    for (int i = 0; i < 300; i++) {
        std::vector<Tex> c = random_cache(g, frame, 300 + g() % 1200);
        const uint64_t budget = (64 + g() % 1024) * MB;   // (mostly under what the cache holds: the market's case; some sweeps)
        const bool burst = g() % 2;
        Setting off;
        off.keep = false;
        tex_peaks::KeepRule r0, r1, r2, r3;
        const Pass before = pass(c, frame, completed, budget, burst, 5000, r0, off);
        // (the setting unset: what goes is exactly what went before lot 63)
        Setting unset;
        unset.threshold = 0;
        if (pass(c, frame, completed, budget, burst, 5000, r1, unset).gone == before.gone) off_ok++;
        // (under the threshold even once the pass let go of all it chose: the same)
        Setting high;
        high.threshold = 1 << 20;
        if (pass(c, frame, completed, budget, burst, 5000, r2, high).gone == before.gone) under_ok++;
        // (over it, room for all: no sampled texture unused under keep_unused goes; the render targets as before)
        Setting big;
        big.cap = 1ull << 40;
        const Pass over = pass(c, frame, completed, budget, burst, 1 << 20, r3, big);
        bool none = true;
        for (const Tex& t : c)
            if (!t.gpu_written && frame - t.last_frame < big.keep_unused && over.gone.count(t.id)) none = false;
        over_ok += none;
        targets_ok += same_targets(over, before, c);
        // (a smaller room: what stays is the most recently used, and the free memory once the pass let go stays over it)
        tex_peaks::KeepRule r4;
        Setting s;
        const int64_t free_mb = 500 + (int64_t)(g() % 1000);
        const Pass mid = pass(c, frame, completed, budget, burst, free_mb, r4, s);
        uint64_t oldest_held = frame, youngest_gone = 0;
        for (const Tex& t : c) {
            if (t.gpu_written || frame - t.last_frame >= s.keep_unused) continue;
            const bool was_gone = before.gone.count(t.id), now_gone = mid.gone.count(t.id);
            if (was_gone && !now_gone) oldest_held = std::min(oldest_held, t.last_frame);
            if (now_gone) youngest_gone = std::max(youngest_gone, t.last_frame);
        }
        recent_ok += !mid.held || youngest_gone <= oldest_held;
        room_ok += !mid.held || (uint64_t)free_mb + ((mid.let_go - mid.held) >> 20) >= s.threshold;
        cap_ok += mid.held <= s.cap;
        targets_ok += same_targets(mid, before, c);
        // (and nothing that did not go before goes now)
        for (uint32_t id : mid.gone)
            if (!before.gone.count(id)) targets_ok -= 1000;
        cases++;
    }
    expect(off_ok == cases, "RX_VK_EVICT_KEEP_FREE_MB unset: the pass lets go of what it let go before, texture for texture");
    expect(under_ok == cases, "under the threshold: the same");
    expect(over_ok == cases, "far over it: no sampled texture unused under RX_VK_EVICT_KEEP_UNUSED goes");
    expect(targets_ok == 2 * cases, "the render targets: what goes is what went before; and nothing goes that did not");
    expect(recent_ok == cases, "what stays is the most recently used: none that goes was used later than one held");
    expect(room_ok == cases, "held, the free memory once the pass let go of the others stays at the threshold or over");
    expect(cap_ok == cases, "never more than RX_VK_EVICT_KEEP_MB held");
}

// (passes in a row: the market's long case. The camera goes round 12 views of 10 textures each, 3 MB a texture (360 MB in
// all, under the cap), one view every 25 presents (a turn in 300 presents); the cache over its budget (128 MB) all along.
// Today's pass lets a view go 20 presents after it left the screen (a burst) and makes it again a turn later; with the rule,
// while the free memory allows, it stays)
static void turning() {
    const uint64_t budget = 128 * MB;
    for (int mode = 0; mode < 3; mode++) {   // (0: today's rules; 1: the rule, room for it; 2: the rule, memory short)
        std::vector<Tex> c;
        tex_peaks::KeepRule rule;
        Setting s;
        s.keep = mode != 0;
        uint64_t made = 0;
        int64_t free_mb = mode == 2 ? 900 : 3000;
        for (uint64_t frame = 1; frame <= 6000; frame++) {
            // (the view on screen: each of its textures used, made if the cache lacks it)
            for (int k = 0; k < 10; k++) {
                const uint32_t want = (uint32_t)((frame / 25) % 12) * 100 + (uint32_t)k + 1;
                auto it = std::find_if(c.begin(), c.end(), [&](const Tex& t) { return t.id == want; });
                if (it == c.end()) {
                    Tex t;
                    t.id = want, t.bytes = 3 * MB, t.made_frame = frame;
                    c.push_back(t);
                    it = c.end() - 1;
                    made++;
                    if (mode != 2) free_mb -= 3;
                }
                it->last_frame = frame, it->last_value = frame;
            }
            if (frame % 10) continue;
            const Pass p = pass(c, frame, frame, budget, true, free_mb, rule, s);
            for (auto it = c.begin(); it != c.end();)
                if (p.gone.count(it->id)) {
                    if (mode != 2) free_mb += 3;
                    it = c.erase(it);
                } else ++it;
        }
        if (mode == 0) expect(made > 12 * 10 * 10, "today's rules: each view made again at every turn");
        if (mode == 1) expect(made == 12 * 10, "room for them: each texture made once");
        if (mode == 2) expect(made > 12 * 10 * 10, "memory short (900 MiB free, threshold 1000): as today");
    }
}

int main() {
    room();
    chosen_and_pick();
    due();
    as_a_pass();
    turning();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("tex_keep: all checks passed\n");
    return 0;
}
