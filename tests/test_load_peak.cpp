// load_peak (gpu/vk/load_peak.h): the rules that hold down the renderer's memory while a scene loads. tests/test_load_peak.py
#include "../gpu/vk/load_peak.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

static void reapable() {
    using load_peak::reapable;
    // (an image the eviction let go: its last submission is done, by the eviction's own rule)
    expect(reapable(100, 100, 5000, 5000, 0), "done at its last submission, no hold: now");
    expect(reapable(90, 100, 5000, 5000, 0), "done long ago, no hold: now");
    expect(!reapable(101, 100, 5000, 5000, 0), "its last submission not done: kept");
    // (RX_VK_DEAD_HOLD: so many presents more, whatever the GPU)
    expect(!reapable(90, 100, 5000, 5000, 3), "held 3 presents, died this present: kept");
    expect(!reapable(90, 100, 5002, 5000, 3), "held 3 presents, 2 went by: kept");
    expect(reapable(90, 100, 5003, 5000, 3), "held 3 presents, 3 went by: now");
    expect(!reapable(101, 100, 5003, 5000, 3), "held and not done: kept");
}

static void arena_free() {
    using load_peak::arena_free;
    const uint64_t MB = 1ull << 20;
    // (the present read the timeline at 100; the recording now is 107)
    expect(arena_free(99, 107, 100, 16 * MB, 4 * MB), "read by a submission the present saw done: free");
    expect(!arena_free(103, 107, 100, 16 * MB, 4 * MB), "read by a submission done since the present, not read again: not free");
    expect(arena_free(103, 107, 105, 16 * MB, 4 * MB), "the same, the timeline read again at 105: free");
    expect(!arena_free(106, 107, 105, 16 * MB, 4 * MB), "read by one still running: not free");
    expect(!arena_free(107, 107, 200, 16 * MB, 4 * MB), "the recording's own arena: never from its start");
    expect(!arena_free(99, 107, 100, 16 * MB, 17 * MB), "too small: not taken");
    expect(arena_free(99, 107, 100, 288 * MB, 17 * MB), "an oversized one (the shadow map's): taken when done");
    expect(arena_free(0, 107, 0, 16 * MB, MB), "a fresh arena of value 0 at start: free");
}

static void depth_skip() {
    using load_peak::DepthFirstUpload;
    using load_peak::depth_skip;
    using load_peak::depth_skip_possible;
    const uint64_t MB = 1ull << 20;
    DepthFirstUpload shadow{};   // (the game's shadow map: 12288 x 12288 D16, fresh storage, RX_VK_ZERO_DEPTH_MB=64)
    shadow.min_bytes = 64 * MB, shadow.total = 288 * MB, shadow.depth_only = true, shadow.first = true, shadow.direct = true;
    shadow.untouched = true;
    expect(depth_skip(shadow), "the shadow map's first upload, its storage never written: skipped");
    DepthFirstUpload u = shadow;
    u.min_bytes = 0;
    expect(!depth_skip(u) && !depth_skip_possible(u), "switch off: uploaded as before");
    u = shadow, u.total = 32 * MB;
    expect(!depth_skip(u), "under the size asked for: uploaded");
    u = shadow, u.total = 64 * MB;
    expect(depth_skip(u), "at the size asked for: skipped");
    u = shadow, u.untouched = false;
    expect(depth_skip_possible(u) && !depth_skip(u), "a unit of its storage was written: uploaded");
    u = shadow, u.shadowed = true;
    expect(!depth_skip(u), "a capture keeps a copy of its storage: uploaded");
    u = shadow, u.first = false;
    expect(!depth_skip_possible(u), "not its first upload (uploaded, drawn, copied or cleared since made): uploaded");
    u = shadow, u.drawn = true;
    expect(!depth_skip_possible(u), "the GPU drew it: uploaded as before");
    u = shadow, u.depth_only = false;
    expect(!depth_skip_possible(u), "colour, or depth with stencil: not this rule");
    u = shadow, u.sparse = true;
    expect(!depth_skip_possible(u), "sparse: not this rule");
    u = shadow, u.direct = false;
    expect(!depth_skip_possible(u), "a virtual pool's pages: not this rule");
}

int main() {
    reapable();
    arena_free();
    depth_skip();
    if (failures) return EXIT_FAILURE;
    std::printf("load peak: all checks passed\n");
    return EXIT_SUCCESS;
}
