"""(ps5) The texture shadow's copies lie in blocks of their own, given back when empty, and hold the same bytes as before.

On the console each of the texture shadow's 4 KB copies (gpu/vk/vk_memory.cpp, "texture storage in the capture") was an
allocation of the host heap: 47,000 of them (185 MB) on the arrival in Hyrule Field, between the hash table's nodes and the
replay thread's other allocations, in the 16 MB blocks of the console's heap, which gives a block back only when all of it
is free. The heap stayed 288 MB bigger (logs/udp/essai-lot33-1.txt; notes-agents/captures-tas-hote.md). The copies are now
slots of 4 MB blocks of their own (gpu/vk/shadow_store.h): direct memory on the console, mappings elsewhere.

These checks:
- the store (shadow_store.h, compiled as it is) against a model of what each page's copy must hold, through random takes,
  frees and moves; a scene's peak then its fall (47,000 copies, then 7,000 scattered survivors): the blocks follow the
  copies down to their own size plus a block and a half and the spare, and to nothing once the spare has waited; a block
  refused leaves the page out, nothing else;
- vk_memory.cpp's shadow, compiled from its own text (the entries, the sweeps, the cap, the take's update and the moves), run
  over the same random captures with RX_VK_SHADOW_BLOCKS=1 and =0: the same pages with the same bytes and the same capture
  after every take, the blocks' bookkeeping right (each entry's slot owned by its page, its pointer that slot's);
- the switch read once, on by default; every way an entry goes gives its slot back (its destructor); a refused block makes
  the take stop as a full shadow does; the moves under the shadow's lock, after the take's copies; the blocks' calls of the
  console named only for it; RX_VK_SHADOW_VERIFY's comparison where the uploads read the copies.

python -I -B tests/test_shadow_blocks.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]   # (tests/run_all.py reads it)
RUN_ALL_SKIP = {"windows": "the texture shadow's blocks are mmap's off Windows and the console: the fixture maps them"}

ROOT = Path(__file__).resolve().parents[1]
MEMORY = ROOT / "gpu" / "vk" / "vk_memory.cpp"
STORE = ROOT / "gpu" / "vk" / "shadow_store.h"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def console_only(text, name):
    """Every use of name in the code (not its comments) lies between an #if defined(__PROSPERO__) and its #else/#elif/#endif"""
    inside, depth, uses = [], 0, 0
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#if"):
            depth += 1
            if "__PROSPERO__" in s and "!" not in s and "||" not in s:
                inside.append(depth)
        elif s.startswith("#else") or s.startswith("#elif"):
            if inside and inside[-1] == depth:
                inside.pop()
        elif s.startswith("#endif"):
            if inside and inside[-1] == depth:
                inside.pop()
            depth -= 1
        elif name in line and not s.startswith("//") and not s.startswith("void* " + name) and not s.startswith("void " + name):
            uses += 1
            if not inside:
                return False
    return uses > 0


STORE_TEST = r'''
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <vector>
#include "shadow_store.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using mkwvk::ShadowSlabs;
static long made, given, refuse;
static std::map<void*, size_t> live_blocks;
static void* make(size_t n) {
    CHECK(n == ShadowSlabs::kBytes);
    if (refuse) return nullptr;
    void* p = std::malloc(n);
    std::memset(p, 0xA5, n);
    live_blocks[p] = n, made++;
    return p;
}
static void give(void* p, size_t n) {
    CHECK(live_blocks.count(p) && live_blocks[p] == n);
    live_blocks.erase(p), given++;
    std::free(p);
}
struct Model { uint32_t slot; uint32_t fill; };
static void fill(uint8_t* p, uint32_t page, uint32_t v) {
    for (int i = 0; i < 1024; i++) { uint32_t w = page * 2654435761u ^ v ^ (uint32_t)i * 40503u; std::memcpy(p + i * 4, &w, 4); }
}
static bool same(const uint8_t* p, uint32_t page, uint32_t v) {
    uint8_t q[4096];
    fill(q, page, v);
    return std::memcmp(p, q, 4096) == 0;
}
static void check_all(ShadowSlabs& s, std::map<uint32_t, Model>& m) {
    CHECK(s.pages() == m.size());
    std::map<uint32_t, uint32_t> seen;
    for (auto& [page, e] : m) {
        CHECK(s.owner(e.slot) == page);
        CHECK(same(s.at(e.slot), page, e.fill));
        CHECK(seen.emplace(e.slot, page).second);   // (no slot held twice)
    }
    CHECK((long)live_blocks.size() == (long)s.blocks());
}
static uint32_t blocks_for(uint64_t pages) { return (uint32_t)((pages + ShadowSlabs::kPages - 1) / ShadowSlabs::kPages); }
int main() {
    std::mt19937 rng(7);
    {   // random takes, frees and moves against the model
        ShadowSlabs s(make, give, 50, 20);
        std::map<uint32_t, Model> m;
        std::vector<uint32_t> keys;   // (the pages held, for picking one at random)
        uint32_t v = 1;
        auto on_move = [&](uint32_t page, uint32_t from, uint32_t to) {
            CHECK(m.count(page) && m[page].slot == from && s.owner(to) == page);
            m[page].slot = to;
        };
        auto drop = [&](size_t i) {
            const uint32_t page = keys[i];
            s.give(m[page].slot);
            m.erase(page);
            keys[i] = keys.back();
            keys.pop_back();
        };
        for (int round = 0; round < 1200; round++) {
            const int phase = round / 200;   // (rising, steady, falling, empty, rising again, falling)
            const int adds = phase == 0 || phase == 4 ? 300 : phase == 1 ? 100 : phase == 3 ? 0 : 20;
            const int drops = phase == 0 || phase == 4 ? 50 : phase == 1 ? 100 : phase == 3 ? 400 : 250;
            for (int i = 0; i < drops && !keys.empty(); i++) drop(rng() % keys.size());
            for (int i = 0; i < adds; i++) {
                const uint32_t page = rng() % 60000;
                if (m.count(page)) drop((size_t)(std::find(keys.begin(), keys.end(), page) - keys.begin()));   // (taken again: the old copy goes first, as take_capture does)
                const uint32_t slot = s.take(page);
                CHECK(slot != ShadowSlabs::kNone);
                fill(s.at(slot), page, ++v);
                m[page] = {slot, v};
                keys.push_back(page);
            }
            s.compact(128, on_move);
            if (round % 100 == 99) check_all(s, m);
        }
        check_all(s, m);
        CHECK(s.moves() > 0 && s.given_back() > 0);
    }
    {   // a scene's peak, then its fall to scattered survivors: the blocks come back
        made = given = 0;
        ShadowSlabs s(make, give, 3600);
        std::map<uint32_t, Model> m;
        for (uint32_t page = 0; page < 47000; page++) {
            const uint32_t slot = s.take(page);
            fill(s.at(slot), page, 9);
            m[page] = {slot, 9};
        }
        CHECK(s.blocks() == blocks_for(47000) && s.peak_bytes() == (uint64_t)blocks_for(47000) * ShadowSlabs::kBytes);
        std::vector<uint32_t> pages;
        for (auto& [p, e] : m) pages.push_back(p);
        std::shuffle(pages.begin(), pages.end(), rng);
        for (size_t i = 7000; i < pages.size(); i++) { s.give(m[pages[i]].slot); m.erase(pages[i]); }
        CHECK(s.blocks() == blocks_for(47000));   // (the survivors are in every block: none is empty yet)
        int calls = 0;
        auto on_move = [&](uint32_t page, uint32_t from, uint32_t to) { CHECK(m[page].slot == from); m[page].slot = to; };
        while (s.compact(128, on_move)) calls++;
        check_all(s, m);
        CHECK(s.blocks() > blocks_for(7000) + 2);   // (the emptied blocks wait 600 calls before they go)
        for (int i = 0; i < 600; i++) s.compact(128, on_move);
        check_all(s, m);
        // (what is left: the copies' own blocks, at most a block and a half of room among them, and the spare)
        CHECK(s.blocks() <= blocks_for(7000) + 2);
        std::printf("peak then fall: %u blocks (%llu MB) for 47000 copies, %u blocks (%llu MB) for 7000 after %d calls, %llu moved\n",
                    blocks_for(47000), (unsigned long long)(blocks_for(47000) * 4), s.blocks(), (unsigned long long)(s.bytes() >> 20),
                    calls, (unsigned long long)s.moves());
        for (int i = 0; i < 3600; i++) s.compact(128, on_move);
        CHECK(!s.has_spare() && s.blocks() <= blocks_for(7000) + 1);   // (the spare given back once it waited)
        const uint32_t before = s.blocks();
        for (auto& [p, e] : m) s.give(e.slot);
        m.clear();
        CHECK(s.pages() == 0 && s.blocks() == before);   // (emptied blocks wait: none given back in a take's middle)
        for (int i = 0; i < 599; i++) s.compact(128, on_move);
        CHECK(s.blocks() == before);
        s.compact(128, on_move);
        CHECK(s.blocks() == 1 && s.has_spare());   // (then all but the spare, kept a while longer)
        for (int i = 0; i < 3600; i++) s.compact(128, on_move);
        CHECK(s.blocks() == 0 && live_blocks.empty() && made == given);
    }
    {   // a take that frees a whole block's pages and copies them again: no block given back or made in its middle
        made = given = 0;
        ShadowSlabs s(make, give);
        std::vector<uint32_t> slots;
        for (uint32_t page = 0; page < 3 * 1024; page++) slots.push_back(s.take(page));
        auto none = [](uint32_t, uint32_t, uint32_t) {};
        s.compact(128, none);
        for (int take = 0; take < 50; take++) {
            for (uint32_t i = 1024; i < 3 * 1024; i++) s.give(slots[i]);   // (two blocks' pages rewritten)
            for (uint32_t i = 1024; i < 3 * 1024; i++) slots[i] = s.take(i);
            s.compact(128, none);
        }
        CHECK(made == 3 && given == 0 && s.blocks() == 3);
    }
    {   // a block refused: the page is left out, the blocks held go on
        ShadowSlabs s(make, give);
        std::map<uint32_t, Model> m;
        for (uint32_t page = 0; page < 1024; page++) m[page] = {s.take(page), 0};
        refuse = 1;
        CHECK(s.take(5000) == ShadowSlabs::kNone && s.refused() == 1 && s.pages() == 1024);
        s.give(m[3].slot);
        m.erase(3);
        const uint32_t slot = s.take(5000);   // (room again in the block held)
        CHECK(slot != ShadowSlabs::kNone && s.owner(slot) == 5000);
        refuse = 0;
    }
    std::printf("PASS: the store keeps every copy's bytes at its slot through takes, frees and moves, and gives its blocks back\n");
}
'''

SHADOW_TEST = r'''
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <sys/mman.h>
#include "shadow_store.h"
#include "compat/longshot_env.h"   // (env_on, since the merge)
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace mkwvk {
static void logf(const char*, ...) {}
namespace {
@RANGEWALK@
@SHADOW@
struct Capture {
    std::vector<uint32_t> pages, copied, slots;
    void* map = nullptr;
    std::vector<int32_t> sidx;            // (since the merge: the staging, none here: every page from map)
    std::unique_ptr<uint8_t[]> stage;
    bool buf_cached = false;
};
bool tex_capture_on() { return true; }
bool stage_on() { return false; }
struct CopyPair { uint8_t* dst; const uint8_t* src; };   // (vk_memory.cpp's, and its copies, one by one)
void copy_pairs(const CopyPair* pairs, int64_t n) { for (int64_t i = 0; i < n; i++) std::memcpy(pairs[i].dst, pairs[i].src, 4096); }
void take(Capture* c) {
    const bool fixed = false;   // (upstream's late pages fixed on the GPU: none here)
    static std::vector<uint8_t> late_index;
    static std::vector<uint32_t> deferred;   // (#977's late pages left to their reads, RX_VK_LATE_DEFER: none here)
    @TAKE@
}
}  // namespace
}  // namespace mkwvk
using namespace mkwvk;
static uint64_t digest() {   // (every entry: its page, its capture and its bytes; in no order)
    uint64_t d = 0;
    for (auto& [page, e] : shadow) {
        uint64_t h = 1469598103934665603ull ^ page ^ (e.seq << 32);
        for (int i = 0; i < 4096; i++) h = (h ^ e.b[i]) * 1099511628211ull;
        d += h * 0x9E3779B97F4A7C15ull;
    }
    return d ^ (uint64_t)shadow.size() << 48;
}
int main() {
    const bool blocks = shadow_blocks_on();
    std::mt19937 rng(11);
    tex_merged = {{0, 6000ull << 12}, {9000ull << 12, 14000ull << 12}};   // (two spans; pages 6000-9000 and above 14000 are not)
    std::vector<uint8_t> buf(4096ull * 2000);
    uint64_t moved_seen = 0;
    for (int round = 0; round < 1400; round++) {
        // (a load's rise, play, a calm stretch where only a few pages are written, then another rise)
        const int phase = round < 300 ? 0 : round < 700 ? 1 : round < 1100 ? 2 : 3;
        const int n = phase == 0 || phase == 3 ? 900 + rng() % 600 : phase == 1 ? 200 + rng() % 200 : rng() % 30;
        std::vector<uint32_t> pages;
        for (int i = 0; i < n; i++) {
            const uint32_t base = phase == 2 ? 100 : rng() % 2 ? 0 : 9000;   // (the calm writes the same few pages)
            pages.push_back(base + rng() % (phase == 2 ? 40 : 6000));
        }
        pages.push_back(7000 + rng() % 100);   // (always one outside the spans)
        std::sort(pages.begin(), pages.end());
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        Capture c;
        c.pages = pages;
        for (size_t i = 0; i < pages.size(); i++) {
            if (rng() % 50 == 0) continue;   // (a copy that failed: left out of copied)
            c.copied.push_back(pages[i]), c.slots.push_back((uint32_t)i);
            for (int k = 0; k < 4096; k += 4) {
                const uint32_t w = pages[i] * 2246822519u ^ (uint32_t)round * 3266489917u ^ (uint32_t)k;
                std::memcpy(&buf[i * 4096ull + k], &w, 4);
            }
        }
        c.map = buf.data();
        take(&c);
        if (round >= 700 && round < 760) {   // (the calm's start: single pages written outside the captures, holes everywhere)
            std::lock_guard lk(shadow_mu);
            for (int i = 0; i < 160; i++) {
                const uint64_t at = (uint64_t)(rng() % 14000) << 12;
                shadow_drop_range(at, at + 4096);
            }
        }
        if (round % 97 == 13) {   // (a write the capture did not take, and a span let go: their copies dropped)
            std::lock_guard lk(shadow_mu);
            const uint64_t lo = (uint64_t)(rng() % 14000) << 12;
            shadow_drop_range(lo, lo + ((1 + rng() % 300) << 12));
        }
        std::lock_guard lk(shadow_mu);
        CHECK(shadow.size() == shadow_n.load() && shadow.size() <= kShadowMax);
        if (blocks) {
            const ShadowSlabs& s = shadow_slabs();
            CHECK(s.pages() == shadow.size());
            for (auto& [page, e] : shadow) CHECK(e.slot != ShadowSlabs::kNone && s.owner(e.slot) == page && e.b == s.at(e.slot));
            moved_seen = s.moves();
        }
        std::printf("%d %zu %016llx\n", round, shadow.size(), (unsigned long long)digest());
    }
    if (blocks) {
        const ShadowSlabs& s = shadow_slabs();
        std::fprintf(stderr, "blocks: %u held (peak %llu MB) for %llu copies; %llu made, %llu given back, %llu moved\n", s.blocks(),
                     (unsigned long long)(s.peak_bytes() >> 20), (unsigned long long)s.pages(), (unsigned long long)s.made(),
                     (unsigned long long)s.given_back(), (unsigned long long)moved_seen);
        CHECK(s.given_back() > 0 && moved_seen > 0);
        CHECK(s.blocks() <= (s.pages() + 1023) / 1024 + 2);
    }
}
'''


def run(cmd, env=None):
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        raise SystemExit("FAIL: %s\n%s%s" % (" ".join(map(str, cmd)), r.stdout[-3000:], r.stderr[-3000:]))
    return r


def check_text(mem):
    on = block(mem, "bool shadow_blocks_on()")
    need('getenv("RX_VK_SHADOW_BLOCKS")' in on and "const bool v = e ? atoi(e) != 0 : true;" in on and "static const bool on" in on,
         "RX_VK_SHADOW_BLOCKS is not read once, on by default")
    need(mem.count('getenv("RX_VK_SHADOW_BLOCKS")') == 1, "RX_VK_SHADOW_BLOCKS is read elsewhere")
    make = block(mem, "bool shadow_page_make(ShadowPage& s, uint32_t page)")
    need(mem.count("new uint8_t[4096]") == 2 and make.count("new uint8_t[4096]") == 2,
         "a copy's 4 KB is allocated from the heap outside shadow_page_make (its RX_VK_SHADOW_BLOCKS=0 way and the check's)")
    need(re.search(r"if \(shadow_blocks_on\(\)\) \{\s+const uint32_t slot = shadow_slabs\(\)\.take\(page\);\s+"
                   r"if \(slot == ShadowSlabs::kNone\) return false;", make), "shadow_page_make does not take a slot first")
    page = block(mem, "struct ShadowPage {")
    dtor = block(page, "~ShadowPage()")
    need("if (slot != ShadowSlabs::kNone) shadow_slabs().give(slot);" in dtor and "else delete[] b;" in dtor,
         "an entry that goes does not give its slot (or its heap copy) back")
    need("ShadowPage(const ShadowPage&) = delete;" in page, "an entry could be copied (its slot given back twice)")
    slabs = block(mem, "ShadowSlabs& shadow_slabs()")
    need("static ShadowSlabs* const s = new ShadowSlabs(shadow_block_make, shadow_block_give);" in slabs,
         "the store is destroyed at exit before the entries that give their slots back to it")
    take = block(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed)")
    shadow_part = block(take, "if (!c->pages.empty() && tex_capture_on()) {")
    need(shadow_part.index("std::lock_guard sk(shadow_mu);") < shadow_part.index("shadow_compact();"),
         "the moves are not made under the shadow's lock")
    # (since the Longshot merge the take's copies go together through copy_pairs, slot.b and the check copy among them)
    need("pairs.push_back({slot.b, from});" in shadow_part and "if (slot.check) pairs.push_back({slot.check.get(), from});" in shadow_part,
         "the take's copies are not into the blocks' slots (and the check copies)")
    need(shadow_part.rindex("copy_pairs(pairs.data(), (int64_t)pairs.size());") < shadow_part.index("shadow_compact();"),
         "the moves come before the take's own copies")
    need(re.search(r"if \(!shadow_page_make\(slot, p\)\) \{[^}]*shadow\.erase\(p\);\s+break;", shadow_part),
         "a refused block does not end the take's copies as a full shadow does")
    need("slot.b.reset(" not in mem and ".b.get()" not in block(mem, "const uint8_t* memory_as_submitted("),
         "a copy's bytes are still an owning pointer somewhere")
    compact = block(mem, "void shadow_compact()")
    need("it->second.slot = to;" in compact and "it->second.b = shadow_slabs().at(to);" in compact,
         "a moved copy's entry does not take its new slot and bytes")
    need(console_only(mem, "hos_ps5_block("), "hos_ps5_block is named outside #if defined(__PROSPERO__)")
    give = block(mem, "void shadow_block_give(void* p, size_t n)")
    need("hos_ps5_block_free(p, n);" in give and "munmap(p, n);" in give and "VirtualFree(p, 0, MEM_RELEASE);" in give,
         "a block is not given back the way it was made")
    read = block(mem, "const uint8_t* memory_as_submitted(")
    need(read.index("std::lock_guard lk(shadow_mu);") < read.index("it->second.check"), "the check reads the copies without the lock")
    need("if (std::memcmp(it->second.b, it->second.check.get(), 4096)) st_check_bad++;" in read, "RX_VK_SHADOW_VERIFY compares nothing")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    mem = MEMORY.read_text(encoding="utf-8")
    check_text(mem)
    # (vk_memory.cpp's shadow, as it is: from its lock to the spans' publishing, and the take's part; a smaller cap, so the
    # full shadow's sweep and its refusals are reached with little memory: 4,096 copies, swept above 1,024)
    lo = mem.index("std::mutex shadow_mu;")
    hi = mem.index("/// (the spans unused for 1800 presents")
    # (lot 55: the cap is shadow_max(), RX_VK_TEX_SHADOW_MB, from heydemoura PR #49; the fixture gives it 4,096 copies)
    fn_lo = mem.index("size_t shadow_max() {", lo)
    fn_hi = mem.index("\n}\n", fn_lo) + 3
    need(lo < fn_lo < hi, "shadow_max is not where the test expects it")
    shadow = (mem[lo:fn_lo] + "constexpr size_t kShadowMax = 4096;\nsize_t shadow_max() { return kShadowMax; }\n" + mem[fn_hi:hi])
    need("constexpr size_t kShadowMax = 4096;" in shadow, "kShadowMax is not where the test expects it")
    # (and the blocks' waits, for a run of 1,400 takes: emptied blocks back after 50 takes, the spare after 200)
    made = "new ShadowSlabs(shadow_block_make, shadow_block_give)"
    need(shadow.count(made) == 1, "the store is not made where the test expects it")
    shadow = shadow.replace(made, "new ShadowSlabs(shadow_block_make, shadow_block_give, 200, 50)")
    take = block(block(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed)"),
                 "if (!c->pages.empty() && tex_capture_on()) {")
    need(take.count("shadow_n.load(std::memory_order_relaxed) > 8192") == 1, "the sweep's threshold is not where the test expects it")
    take = take.replace("shadow_n.load(std::memory_order_relaxed) > 8192", "shadow_n.load(std::memory_order_relaxed) > 1024")
    with tempfile.TemporaryDirectory(prefix="shadow-blocks-") as tmp:
        tmp = Path(tmp)
        (tmp / "store.cpp").write_text(STORE_TEST, encoding="utf-8")
        run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(STORE.parent), str(tmp / "store.cpp"), "-o", str(tmp / "store")])
        print(run([str(tmp / "store")]).stdout.strip())
        (tmp / "shadow.cpp").write_text(SHADOW_TEST.replace("@SHADOW@", shadow).replace("@TAKE@", take).replace("@RANGEWALK@", block(mem, "struct RangeWalk {") + ";"), encoding="utf-8")
        run([a.cxx, "-std=c++20", "-O1", "-Wno-unused-function", "-Wno-unused-variable", "-I", str(STORE.parent),
             str(tmp / "shadow.cpp"), "-o", str(tmp / "shadow")])
        outs = {}
        for mode in ("1", "0"):
            env = dict(os.environ, RX_VK_SHADOW_BLOCKS=mode)
            env.pop("RX_VK_SHADOW_VERIFY", None)
            r = run([str(tmp / "shadow")], env)
            outs[mode] = r.stdout
            if mode == "1":
                print("vk_memory.cpp's shadow in blocks: " + r.stderr.strip())
        need(outs["1"] == outs["0"], "the shadow's pages, captures or bytes differ with the blocks (RX_VK_SHADOW_BLOCKS=1 against 0)")
        lines = outs["1"].strip().split("\n")
        sizes = [int(l.split()[1]) for l in lines]
        need(len(lines) == 1400 and max(sizes) == 4096 and min(sizes[1100:]) < 1024,
             "the run did not reach the full shadow and fall back (the cap, the sweeps, the drops)")
        print("vk_memory.cpp's shadow: the same pages, captures and bytes after each of 1,400 takes with the blocks as without "
              "(largest %d, last %d)" % (max(sizes), sizes[-1]))
    print("PASS: the texture shadow's copies in blocks of their own: same bytes, blocks given back, switch and locks in place")


if __name__ == "__main__":
    main()
