#include "../gpu/vk/tex_storage.h"

#include <cstdio>
#include <set>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

using namespace mkwvk::tex_storage;

namespace {
// A window of 64 KiB units, as host/posix/ps5_window.c answers for it (hos_ps5_guest_backed_bytes): the bytes of the
// range in backed units, of them those in pinned units, and the units whole inside the range, backed and not pinned
constexpr uint64_t U = 65536;
std::set<uint64_t> backed_units, pinned_units;
int asked;
uint64_t window(uint64_t lo, uint64_t hi, uint64_t* pinned, uint64_t* free) {
    asked++;
    uint64_t bytes = 0, held = 0, whole = 0;
    for (uint64_t u = lo / U; u < (hi + U - 1) / U; u++) {
        if (!backed_units.count(u)) continue;
        const uint64_t part = std::min(hi, (u + 1) * U) - std::max(lo, u * U);
        bytes += part;
        if (pinned_units.count(u)) held += part;
        else if (part == U) whole += U;
    }
    if (pinned) *pinned = held;
    if (free) *free = whole;
    return bytes;
}
void back(uint64_t lo, uint64_t hi) {
    for (uint64_t u = lo / U; u < (hi + U - 1) / U; u++) backed_units.insert(u);
}
} // namespace

int main() {
    // ---- kinds ----
    CHECK("ASTC the renderer decodes, whatever happened to it", kind_of(true, false, false, true) == ASTC && kind_of(true, true, true, false) == ASTC);
    CHECK("a block format kept as it is", kind_of(false, true, false, true) == BLOCK && kind_of(false, true, true, false) == BLOCK);
    CHECK("a plain format uploaded from memory", kind_of(false, false, false, true) == PLAIN);
    CHECK("a plain format the GPU wrote is a target", kind_of(false, false, true, true) == TARGET && kind_of(false, false, true, false) == TARGET);
    CHECK("a plain format never read from memory is a target (never uploaded, or cleared in an upload's place)",
          kind_of(false, false, false, false) == TARGET);

    // ---- states (uploaded, gpu_written, read_from_memory, written_since) ----
    CHECK("uploaded from memory, nothing written since: the image has all of it", state_of(true, false, true, false) == CLEAN);
    CHECK("written since its upload", state_of(true, false, true, true) == WRITTEN);
    CHECK("not uploaded yet: its first sampling reads it, written or not", state_of(false, false, false, false) == PENDING &&
                                                                          state_of(false, false, false, true) == PENDING);
    CHECK("cleared in an upload's place, or over storage render targets took: nothing was read",
          state_of(true, false, false, false) == UNREAD);
    CHECK("and written after that: read at the next sampling", state_of(true, false, false, true) == WRITTEN);
    CHECK("the GPU's image: its storage is not in use", state_of(true, true, false, false) == UNREAD && state_of(true, true, true, false) == UNREAD);
    CHECK("the GPU's image finalized by the game and written since", state_of(true, true, false, true) == WRITTEN);
    CHECK("the most needed last", UNREAD < CLEAN && CLEAN < PENDING && PENDING < WRITTEN);

    // ---- ages ----
    CHECK("used in the last 8 presents", age_of(0) == 0 && age_of(7) == 0 && age_of(8) == 1);
    CHECK("since the last report", age_of(599) == 1 && age_of(600) == 2);
    CHECK("in the last 3600, and before", age_of(3599) == 2 && age_of(3600) == 3 && age_of(~0ull) == 3);

    // ---- pieces: each range of memory once ----
    std::vector<Span> spans, pieces;
    pieces_of(spans, pieces);
    CHECK("no texture, no piece", pieces.empty());
    const uint64_t A = 0x1000000000ull;
    // two textures apart, in the wrong order, and an empty one
    spans = {{A + 10 * U, A + 12 * U, PLAIN, CLEAN, 5}, {A, A + 2 * U, ASTC, CLEAN, 700}, {A + 5 * U, A + 5 * U, BLOCK, WRITTEN, 0}};
    pieces_of(spans, pieces);
    CHECK("two pieces in ascending order, the empty span left out",
          pieces.size() == 2 && pieces[0].lo == A && pieces[0].hi == A + 2 * U && pieces[0].kind == ASTC && pieces[0].unused == 700 &&
              pieces[1].lo == A + 10 * U && pieces[1].hi == A + 12 * U && pieces[1].kind == PLAIN);
    // one storage under two descriptions: the same range twice
    spans = {{A, A + 4 * U, ASTC, CLEAN, 900}, {A, A + 4 * U, ASTC, CLEAN, 3}};
    pieces_of(spans, pieces);
    CHECK("the same storage twice is one piece, the one used last", pieces.size() == 1 && pieces[0].hi - pieces[0].lo == 4 * U && pieces[0].unused == 3);
    // a clean texture, a render target drawn over half of it, and one written inside the other half
    spans = {{A, A + 8 * U, PLAIN, CLEAN, 100},
             {A + 4 * U, A + 12 * U, TARGET, UNREAD, 1},
             {A + U, A + 2 * U, BLOCK, WRITTEN, 50},
             {A + 2 * U, A + 3 * U, ASTC, PENDING, 2000}};
    pieces_of(spans, pieces);
    CHECK("five pieces", pieces.size() == 5);
    CHECK("the clean one's start", pieces[0].lo == A && pieces[0].hi == A + U && pieces[0].state == CLEAN && pieces[0].kind == PLAIN);
    CHECK("a written texture inside it takes its part", pieces[1].lo == A + U && pieces[1].hi == A + 2 * U && pieces[1].state == WRITTEN && pieces[1].kind == BLOCK);
    CHECK("and one waiting for its upload", pieces[2].lo == A + 2 * U && pieces[2].hi == A + 3 * U && pieces[2].state == PENDING && pieces[2].kind == ASTC);
    CHECK("the clean one over the render target where they share: neighbours alike are one piece",
          pieces[3].lo == A + 3 * U && pieces[3].hi == A + 8 * U && pieces[3].state == CLEAN && pieces[3].unused == 100);
    CHECK("the render target's own end", pieces[4].lo == A + 8 * U && pieces[4].hi == A + 12 * U && pieces[4].state == UNREAD && pieces[4].kind == TARGET);
    uint64_t total = 0;
    for (const Span& p : pieces) total += p.hi - p.lo;
    CHECK("12 units of memory, each once (the spans' sizes add up to 18)", total == 12 * U);
    for (size_t i = 1; i < pieces.size(); i++) CHECK("disjoint and ascending", pieces[i].lo >= pieces[i - 1].hi);
    // many spans over one another: nothing counted twice, nothing lost
    spans.clear();
    for (uint64_t i = 0; i < 200; i++) spans.push_back({A + i * 4096, A + i * 4096 + 40 * 4096, i % 3 ? PLAIN : TARGET, i % 3 ? CLEAN : UNREAD, i});
    pieces_of(spans, pieces);
    total = 0;
    for (const Span& p : pieces) total += p.hi - p.lo;
    CHECK("200 shifted spans: their union", total == 239 * 4096 && pieces.front().lo == A && pieces.back().hi == A + 239 * 4096);
    CHECK("the union is the clean ones' wherever one of them reaches", pieces.front().state == UNREAD && pieces[1].state == CLEAN && pieces[1].unused == 1);

    // ---- the count ----
    // a clean ASTC texture of 3.5 units from the middle of a unit; next to it, sharing a unit, a clean plain one used now; then a
    // render target whose storage something backed; apart from them a texture written since and one not uploaded yet
    const uint64_t B = 0x1100000000ull;
    spans = {{B + U / 2, B + 4 * U, ASTC, CLEAN, 1000},
             {B + 4 * U, B + 6 * U + 100, PLAIN, CLEAN, 2},
             {B + 6 * U + 100, B + 10 * U, TARGET, UNREAD, 0},
             {B + 20 * U, B + 22 * U, BLOCK, WRITTEN, 10},
             {B + 30 * U, B + 31 * U, ASTC, PENDING, 0}};
    back(B, B + 9 * U);              // (units 0..8: the game wrote the first textures, and something the target's first units)
    back(B + 20 * U, B + 21 * U);    // (half of the written one)
    pinned_units.insert((B + 2 * U) / U);   // (an import took one unit of the ASTC texture)
    pieces_of(spans, pieces);
    Totals t;
    asked = 0;
    count(pieces, window, t);
    CHECK("storage once, by kind", t.once[ASTC] == 3 * U + U / 2 + U && t.once[PLAIN] == 2 * U + 100 && t.once[TARGET] == 4 * U - 100 && t.once[BLOCK] == 2 * U);
    CHECK("backed, by kind", t.backed[ASTC] == 3 * U + U / 2 && t.backed[PLAIN] == 2 * U + 100 && t.backed[TARGET] == 3 * U - 100 && t.backed[BLOCK] == U);
    CHECK("pinned, by kind", t.pinned[ASTC] == U && !t.pinned[PLAIN] && !t.pinned[TARGET] && !t.pinned[BLOCK]);
    CHECK("the backed part by state", t.state_backed[CLEAN] == 5 * U + U / 2 + 100 && t.state_backed[UNREAD] == 3 * U - 100 &&
                                          t.state_backed[WRITTEN] == U && t.state_backed[PENDING] == 0);
    CHECK("the clean part by kind", t.clean_backed[ASTC] == 3 * U + U / 2 && t.clean_backed[PLAIN] == 2 * U + 100 && !t.clean_backed[BLOCK] && !t.clean_backed[TARGET]);
    CHECK("and by last use", t.clean_age[0] == 2 * U + 100 && t.clean_age[1] == 0 && t.clean_age[2] == 3 * U + U / 2 && t.clean_age[3] == 0);
    // what a decommit would give back: whole units, not pinned, of runs of pieces next to one another
    CHECK("of the unread alone: the target's two whole backed units (its first one is shared with the plain texture)", t.free_unread == 2 * U);
    CHECK("with every clean one: units 1 to 8 less the pinned one (unit 0 is half someone else's)", t.free_from[0] == 7 * U);
    CHECK("without those used in the last 8 presents: units 1 and 3, and the target's two", t.free_from[1] == 4 * U && t.free_from[2] == 4 * U);
    CHECK("without those used in the last 3600: the target's alone", t.free_from[3] == 2 * U);
    CHECK("the window asked once a piece and once a run", asked == (int)pieces.size() + 1 + 1 + 2 + 2 + 1);
    // the window answering nothing (off the console): sizes stand, nothing is backed
    Totals off;
    count(pieces, [](uint64_t, uint64_t, uint64_t* pinned, uint64_t* free) {
        if (pinned) *pinned = 0;
        if (free) *free = 0;
        return uint64_t(0);
    }, off);
    CHECK("off the console the sizes stand and nothing is backed",
          off.once[ASTC] == t.once[ASTC] && off.once[TARGET] == t.once[TARGET] && !off.backed[ASTC] && !off.state_backed[CLEAN] && !off.free_from[0] && !off.free_unread);
    std::puts("texture storage: all checks passed");
}
