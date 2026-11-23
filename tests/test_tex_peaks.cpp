// tex_peaks (gpu/vk/tex_peaks.h): the renderer's texture peaks when the view changes. tests/test_tex_peaks.py
#include "../gpu/vk/tex_peaks.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

static const uint64_t MB = 1ull << 20;

static void made_ring() {
    tex_peaks::MadeRing r;
    expect(r.sum(1, 60, false) == 0 && r.sum(1, 60, true) == 0, "nothing made: 0");
    r.add(100, 10 * MB, false);
    r.add(100, 5 * MB, false);
    r.add(100, 7 * MB, true);
    expect(r.sum(100, 60, false) == 15 * MB, "two sampled at one present: summed");
    expect(r.sum(100, 60, true) == 7 * MB, "the targets apart");
    r.add(130, 20 * MB, false);
    expect(r.sum(130, 60, false) == 35 * MB, "30 presents later: both in the window");
    expect(r.sum(159, 60, false) == 35 * MB, "the first 59 presents back: still in");
    expect(r.sum(160, 60, false) == 20 * MB, "60 presents back: out");
    expect(r.sum(189, 60, false) == 20 * MB && r.sum(190, 60, false) == 0, "the second goes out 60 presents after it");
    // (a slot reused 128 presents later starts from zero)
    r.add(228, 3 * MB, false);
    expect(r.sum(228, 60, false) == 3 * MB, "a slot reused: the old present's bytes gone");
    expect(r.sum(100, 60, false) == 0, "a present asked before what the ring holds now: nothing of the future counted");
}

static void burst() {
    using tex_peaks::burst;
    expect(!burst(63 * MB, 64 * MB), "under the threshold: no burst");
    expect(burst(64 * MB, 64 * MB), "at it: a burst");
    expect(burst(0, 0), "threshold 0: always");
}

static void need_unused() {
    using tex_peaks::need_unused;
    // (need_unused(base, sampled, in_burst, over_budget, gpu_written, presents_since_made))
    expect(need_unused(60, 0, true, true, false, 1000, 60) == 60, "the setting off: 60 as ever");
    expect(need_unused(60, 20, true, true, false, 1000, 60) == 20, "in a burst, over the budget, sampled, old: 20");
    expect(need_unused(60, 20, false, true, false, 1000, 60) == 60, "no burst: 60");
    expect(need_unused(60, 20, true, false, false, 1000, 60) == 60, "under the budget (the sweep): its own");
    expect(need_unused(120, 20, true, false, false, 1000, 120) == 120, "the sweep's 120 untouched");
    expect(need_unused(60, 20, true, true, true, 1000, 60) == 60, "drawn by the GPU: never sooner (its texels are only the image's)");
    expect(need_unused(60, 20, true, true, false, 59, 60) == 60, "made in the last 60 presents (by the burst itself): 60");
    expect(need_unused(60, 20, true, true, false, 60, 60) == 20, "made 60 presents ago: 20");
    expect(need_unused(60, 3, true, true, false, 1000, 60) == 8, "never under 8: what the last 8 presents used stays");
    expect(need_unused(60, 80, true, true, false, 1000, 60) == 60, "never more than the base");
    expect(need_unused(30, 20, true, true, false, 1000, 30) == 20 && need_unused(30, 20, true, true, false, 29, 30) == 30,
           "RX_VK_EVICT_MIN_UNUSED=30: the made-lately line follows it");
    // (RX_VK_EVICT_BURST_MADE=20: a texture the burst made 20 presents ago or more may go at 20 too)
    expect(need_unused(60, 20, true, true, false, 20, 20) == 20, "made 20 presents ago, the line at 20: 20");
    expect(need_unused(60, 20, true, true, false, 19, 20) == 60, "made 19 presents ago, the line at 20: 60");
    expect(need_unused(60, 20, true, true, true, 1000, 20) == 60, "the line moved, drawn by the GPU: 60 all the same");
}

static void pass_now() {
    using tex_peaks::pass_now;
    expect(pass_now(120, false, false, false), "every 10th present");
    expect(!pass_now(121, false, false, false), "not the others");
    expect(pass_now(121, true, false, false), "under pressure: every present (RX_VK_EVICT_PRESSURE_MB)");
    expect(!pass_now(121, false, true, false), "a burst without RX_VK_EVICT_BURST_EVERY: every 10th");
    expect(pass_now(121, false, true, true), "a burst with it: every present");
    expect(!pass_now(121, false, false, true), "RX_VK_EVICT_BURST_EVERY and no burst: every 10th");
}

static void pool_drop() {
    using tex_peaks::pool_drop;
    const uint64_t cap = 128 * MB;
    expect(!pool_drop(100 * MB, cap, 1120, 1000, 120), "120 presents unused, under the cap: kept (as before)");
    expect(pool_drop(100 * MB, cap, 1121, 1000, 120), "121: destroyed (as before)");
    expect(pool_drop(129 * MB, cap, 1001, 1000, 120), "over the cap: destroyed whatever its age");
    expect(!pool_drop(100 * MB, cap, 1020, 1000, 20), "RX_VK_TARGET_POOL_HOLD=20: 20 presents, kept");
    expect(pool_drop(100 * MB, cap, 1021, 1000, 20), "RX_VK_TARGET_POOL_HOLD=20: 21, destroyed");
    expect(pool_drop(1, 0, 1000, 1000, 120), "a cap of 0 (VRAM short, given back): all of it");
}

static void trim_most() {
    using tex_peaks::trim_most;
    expect(trim_most(false, false, 16) == 16 && trim_most(false, true, 16) == 16, "the default: 16 a present (as before)");
    expect(trim_most(true, false, 256) == 64 && trim_most(true, true, 256) == 64, "VRAM short (the pool given back): 64");
    expect(trim_most(false, true, 256) == 256, "over the bound, RX_VK_TARGET_POOL_TRIM=256: 256");
    expect(trim_most(false, false, 256) == 16, "under the bound: the idle ones 16 at a time whatever it says");
    expect(trim_most(false, true, 4) == 16, "never fewer than before");
}

int main() {
    made_ring();
    burst();
    need_unused();
    pass_now();
    pool_drop();
    trim_most();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("tex_peaks: all checks passed\n");
    return 0;
}
