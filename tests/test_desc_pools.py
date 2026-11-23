"""(ps5) A recording's descriptor pools: small ones a slot keeps, where the full size is memory the console cannot spare.

Each of the 48 recording slots has a pool for the sets of the programs with more descriptors than the push limit (32):
8192 sets of up to 32768 descriptors a type. RADV gives a pool one buffer for all of that, 4.06 MiB, and a megabyte of
host memory for its sets' headers, and on the console both are real from the moment the pool is made: 195 MiB of direct
memory in 48 allocations ten minutes into play, none given back, while a recording of the PC build took at most 269 sets
(about half a MiB) in the same scenes, and most took none.

RX_VK_DESC_POOL_DIV=<n> (1..32) makes a pool a n-th of that. These checks keep what makes a small pool safe:
- the setting is read at the first pool, not before main (the console's settings file is read there);
- a pool that runs out is followed by the next one the slot has, then by a new one, as before; an allocation an empty
  pool refuses makes no other pool; a pool that could not be made is no pool, and its draws are left out as with a full one;
- with the full size the pools added are destroyed at the slot's next recording, as before; smaller ones are kept, reset
  only when a recording went into them, and given back when none of the slot's last 120 recordings did;
- "vk: the recording pools" says how many there are and the most a recording took, with the streams' line.
The rules are compiled and run over a model of the driver's pools (a pool holds so many sets and so many descriptors).

python -I -B tests/test_desc_pools.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "g++"]   # (tests/run_all.py reads it)
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"


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


def check_source(draw):
    caps = block(draw, "const PoolCaps& pool_caps()")
    need(draw.count('getenv("RX_VK_DESC_POOL_DIV")') == 2 and caps.count('getenv("RX_VK_DESC_POOL_DIV")') == 2,
         "RX_VK_DESC_POOL_DIV is read outside pool_caps (a static initializer runs before the console's settings are read)")
    need(re.search(r"static const PoolCaps caps = pool_caps_of\(getenv\(\"RX_VK_DESC_POOL_DIV\"\) \? atoi\(getenv\(\"RX_VK_DESC_POOL_DIV\"\)\) : 1\);",
                   caps), "the pools' size is not read at first use, or its default is no longer the full size")
    of = block(draw, "PoolCaps pool_caps_of(int asked)")
    need("std::clamp(asked, 1, 32)" in of, "the divisor is not held to 1..32")
    need("{div, 8192 / div, {32768 / div, 16384 / div, 32768 / div, 4096 / div, 2048 / div, 2048 / div}}" in of,
         "the full size is no longer 8192 sets of 32768 uniform, 16384 storage, 32768 texture, 4096 image and 2048+2048 texel descriptors")

    make = block(draw, "VkDescriptorPool make_pool()")
    need("ci.maxSets = caps.sets;" in make and "sizes[k] = {kPoolTypes[k], caps.kinds[k]}" in make, "make_pool does not make the size pool_caps says")
    need(re.search(r"if \(made != VK_SUCCESS \|\| !p\) \{.*return VK_NULL_HANDLE;", make, re.S), "a pool that was not made is not answered as none")
    need("was not made" in make and "pool_stats.failed++ < 8" in make, "a pool not made is not said, or not a limited number of times")

    take = block(draw, "VkDescriptorSet pool_set(RecordingRes& r, VkDescriptorSetLayout layout)")
    need("ai.descriptorPool = r.extra_used ? r.extra[r.extra_used - 1] : r.pool;" in take, "a set is not taken from the pool the recording is in")
    need(take.index("if (!r.in_pool) return pool_misfit();") < take.index("make_pool()"),
         "an allocation an empty pool refused goes on to make another pool")
    need(take.index("if (r.extra_used == r.extra.size())") < take.index("ai.descriptorPool = r.extra[r.extra_used++];"),
         "a pool the slot kept is not taken before a new one is made")
    need("if (!p) return VK_NULL_HANDLE;" in take, "a pool that could not be made is given to the driver")
    need(take.count("vkAllocateDescriptorSets(") == 2, "a set is tried more than once in the pool the recording goes on into")
    need("a recording needed more than its descriptor pool" in take and "pool_stats.added++ < 8" in take,
         "a pool added is not said, or not a limited number of times")

    bind = block(draw, "bool bind_descriptors(VkCommandBuffer cmd, VkPipelineBindPoint bp, const Program& p)")
    need("const VkDescriptorSet set = pool_set(*cur, p.set_layout);" in bind and "if (!set) return false;" in bind,
         "a draw whose set no pool gave is not left out, as with a full pool before")
    need("vkAllocateDescriptorSets" not in bind and "make_pool" not in bind, "bind_descriptors allocates from a pool by itself")
    need("cur->sets++;" in bind and "cur->descs[pool_kind(w.descriptorType)] += w.descriptorCount" in bind,
         "the sets and descriptors a recording takes are not counted")

    begin = block(draw, "void pools_begin(RecordingRes& r, bool first)")
    need("if (!r.pool) r.pool = make_pool();" in begin and "else vkResetDescriptorPool(dev.device, r.pool, 0);" in begin,
         "the slot's first pool is not made once and reset at each recording, as before")
    need("pools_kept(pool_caps().div, r.extra.size(), r.extra_used, r.extra_peak, r.pool_age)" in begin, "pools_kept does not say what the slot keeps")
    need("if (i >= keep) vkDestroyDescriptorPool(dev.device, r.extra[i], nullptr), s.retired++;" in begin, "the pools not kept are not destroyed")
    need("else if (i < r.extra_used) vkResetDescriptorPool(dev.device, r.extra[i], 0);" in begin, "a kept pool a recording went into is not reset")
    need("r.extra_used = 0, r.in_pool = 0, r.sets = 0;" in begin, "a recording does not begin in its first pool")

    rec = block(draw, "void draw_begin_recording(int slot, VkCommandBuffer cmd, uint64_t value)")
    need("pools_begin(r, first);" in rec and "const bool first = !r.pool && r.streams.empty();" in rec, "a recording does not begin with its slot's pools")
    need("vkResetDescriptorPool" not in rec and "vkDestroyDescriptorPool" not in rec, "draw_begin_recording resets or destroys pools by itself")
    need("if (begun % 3000 == 0) pools_log();" in rec, "the pools' line is not written with the streams'")
    log = re.sub(r'"\s*\n\s*"', "", block(draw, "void pools_log()"))   # (the line's literals as one)
    for part in ("vk: the recording pools: %zu slots' first and %llu more kept now", "(RX_VK_DESC_POOL_DIV=%u)",
                 "the most a recording took: %u sets, of %u pools", "went past their first pool"):
        need(part in log, "the pools' line no longer says: " + part)
    print("PASS: gpu/vk/vk_draw.cpp: the pools' size is read at first use, a full pool is followed as before, the slot keeps small ones", flush=True)


FIXTURE = r'''#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
// ---- a model of the driver: a pool holds so many sets and so many descriptors; a set layout is its descriptor count ----
enum VkResult { VK_SUCCESS = 0, VK_ERROR_OUT_OF_POOL_MEMORY = -1000069000, VK_ERROR_OUT_OF_DEVICE_MEMORY = -2 };
enum VkDescriptorType { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE = 3,
                        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER = 4, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER = 5,
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7 };
enum { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34 };
struct FakePool { uint32_t max_sets, max_descs, sets = 0, descs = 0, resets = 0; bool live = true; };
typedef FakePool* VkDescriptorPool;
typedef uintptr_t VkDescriptorSetLayout;
typedef uintptr_t VkDescriptorSet;
typedef int VkDevice;
#define VK_NULL_HANDLE 0
struct VkDescriptorPoolSize { VkDescriptorType type; uint32_t descriptorCount; };
struct VkDescriptorPoolCreateInfo { int sType; uint32_t maxSets = 0, poolSizeCount = 0; const VkDescriptorPoolSize* pPoolSizes = nullptr; };
struct VkDescriptorSetAllocateInfo { int sType; VkDescriptorPool descriptorPool = nullptr; uint32_t descriptorSetCount = 0;
                                     const VkDescriptorSetLayout* pSetLayouts = nullptr; };
struct { VkDevice device = 1; } dev;
int made, destroyed, resets, alloc_calls, fail_creates, said;
std::vector<FakePool*> all;
VkResult vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo* ci, void*, VkDescriptorPool* out) {
    if (fail_creates) { fail_creates--; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    CHECK(ci->poolSizeCount == 6);
    uint32_t d = 0;
    for (uint32_t i = 0; i < ci->poolSizeCount; i++) d += ci->pPoolSizes[i].descriptorCount;
    *out = new FakePool{ci->maxSets, d};
    all.push_back(*out);
    made++;
    return VK_SUCCESS;
}
void vkDestroyDescriptorPool(VkDevice, VkDescriptorPool p, void*) { CHECK(p && p->live); p->live = false; destroyed++; }
VkResult vkResetDescriptorPool(VkDevice, VkDescriptorPool p, int) { CHECK(p && p->live); p->sets = p->descs = 0; p->resets++; resets++; return VK_SUCCESS; }
VkResult vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* out) {
    FakePool* p = ai->descriptorPool;
    CHECK(p && p->live);   // (a pool that was not made, or one given back, must never reach the driver)
    alloc_calls++;
    const uint32_t n = (uint32_t)ai->pSetLayouts[0];
    if (p->sets + 1 > p->max_sets || p->descs + n > p->max_descs) return VK_ERROR_OUT_OF_POOL_MEMORY;
    p->sets++, p->descs += n;
    *out = (VkDescriptorSet)(p->sets);
    return VK_SUCCESS;
}
void logf(const char*, ...) { said++; }
const char* test_div = nullptr;
#define getenv(name) test_div
// ---- gpu/vk/vk_draw.cpp's own ----
@KINDS@
struct RecordingRes {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> extra;
    uint32_t extra_used = 0;
    uint32_t extra_peak = 0, pool_age = 0;
    uint32_t in_pool = 0;
    uint32_t sets = 0, descs[kPoolKinds] = {};
};
@CAPS_STRUCT@
@CAPS_OF@
@CAPS@
@KEPT@
@TYPES@
@KIND@
@STATS@
PoolStats pool_stats;
std::atomic<uint64_t> pools_extra{0};   // (longshot's count of the pools added, #387: pool_set counts them too)
@MAKE@
@MISFIT@
@SET@
@BEGIN@

int live_pools() { int n = 0; for (FakePool* p : all) n += p->live; return n; }
/// A recording of the slot that takes `sets` sets of `each` descriptors; the sets it got
int record(RecordingRes& r, bool first, int sets, uint32_t each) {
    pools_begin(r, first);
    int got = 0;
    for (int i = 0; i < sets; i++)
        if (pool_set(r, (VkDescriptorSetLayout)each)) got++, r.sets++;
    return got;
}

int main(int argc, char**) {
    const bool small = argc > 1;   // (one process a size: the size is read once)
    if (!small) {
        // the sizes, and what a slot keeps
        PoolCaps c = pool_caps_of(1);
        CHECK(c.div == 1 && c.sets == 8192 && c.kinds[0] == 32768 && c.kinds[1] == 16384 && c.kinds[2] == 32768 && c.kinds[3] == 4096 &&
              c.kinds[4] == 2048 && c.kinds[5] == 2048);
        c = pool_caps_of(32);
        CHECK(c.div == 32 && c.sets == 256 && c.kinds[0] == 1024 && c.kinds[1] == 512 && c.kinds[2] == 1024 && c.kinds[3] == 128 &&
              c.kinds[4] == 64 && c.kinds[5] == 64);
        CHECK(pool_caps_of(0).div == 1 && pool_caps_of(-5).div == 1 && pool_caps_of(1000).div == 32 && pool_caps_of(8).sets == 1024);
        CHECK(pool_kind(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) == 0 && pool_kind(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) == 1 &&
              pool_kind(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) == 2 && pool_kind(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) == 3 &&
              pool_kind(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) == 4 && pool_kind(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) == 5);
        uint32_t peak = 0, age = 0;
        for (int i = 0; i < 500; i++) CHECK(pools_kept(1, 5, 5, peak, age) == 0);   // (the full size: none, as before)
        CHECK(peak == 0 && age == 0);
        for (int i = 0; i < 119; i++) CHECK(pools_kept(32, 4, i == 7 ? 3 : 1, peak, age) == 4);   // (all of them for 119 recordings)
        CHECK(pools_kept(32, 4, 0, peak, age) == 3 && peak == 0 && age == 0);                      // (then as many as one went into)
        for (int i = 0; i < 119; i++) CHECK(pools_kept(32, 3, 0, peak, age) == 3);
        CHECK(pools_kept(32, 3, 0, peak, age) == 0);                                               // (120 recordings in the first pool: all given back)
        for (int i = 0; i < 119; i++) pools_kept(32, 2, 9, peak, age);
        CHECK(pools_kept(32, 2, 9, peak, age) == 2);                                               // (never more than it has)

        // the full size (the default, RX_VK_DESC_POOL_DIV unset): as before
        CHECK(pool_caps().div == 1 && pool_caps().sets == 8192);
        RecordingRes r;
        CHECK(record(r, true, 100, 40) == 100 && made == 1 && r.extra.empty() && r.in_pool == 100);
        CHECK(record(r, false, 9000, 4) == 9000 && made == 2 && r.extra.size() == 1 && r.extra_used == 1);   // (8192 sets a pool)
        CHECK(pool_stats.added == 1 && r.in_pool == 9000 - 8192);
        const FakePool* added = r.extra[0];
        CHECK(record(r, false, 10, 4) == 10 && destroyed == 1 && !added->live && r.extra.empty() && made == 2);   // (destroyed at the next recording)
        CHECK(pool_stats.retired == 1 && r.pool->resets == 2 && live_pools() == 1);
        // an allocation an empty pool refuses: no other pool, the draw left out, the next ones served
        const int before = made;
        CHECK(record(r, false, 1, 200000) == 0 && made == before && pool_stats.misfits == 1);
        CHECK(pool_set(r, 200000) == 0 && made == before && pool_stats.misfits == 2);
        CHECK(pool_set(r, 8) != 0 && r.in_pool == 1);
        // ... and one a pool with sets in it refuses goes on to the next pool once, no further
        CHECK(pool_set(r, 200000) == 0 && made == before + 1 && r.extra_used == 1 && r.in_pool == 0);
        CHECK(pool_set(r, 200000) == 0 && made == before + 1);
        // a pool that cannot be made: no set, nothing given to the driver, and the slot's first is tried again at its next recording
        RecordingRes q;
        fail_creates = 4;   // (the slot's first, then one asked for by each of the three sets)
        const int calls = alloc_calls;
        CHECK(record(q, true, 3, 8) == 0 && !q.pool && q.extra.empty() && alloc_calls == calls && pool_stats.failed == 4);
        CHECK(record(q, false, 3, 8) == 3 && q.pool && q.extra.empty());
        std::printf("PASS: the full size: a pool added when one runs out is destroyed at the slot's next recording, as before\n");
        return 0;
    }
    test_div = "32";
    CHECK(pool_caps().div == 32 && pool_caps().sets == 256);
    RecordingRes r;
    CHECK(record(r, true, 256, 4) == 256 && made == 1 && r.extra.empty());
    CHECK(record(r, false, 700, 4) == 700 && made == 3 && r.extra.size() == 2 && r.extra_used == 2);   // (256 sets a pool: three pools)
    const FakePool *e0 = r.extra[0], *e1 = r.extra[1];
    // the next recordings of the slot find them: none made, none destroyed, and only the ones gone into are reset
    for (int i = 0; i < 50; i++) CHECK(record(r, false, 700, 4) == 700);
    CHECK(made == 3 && destroyed == 0 && e0->live && e1->live && e0->resets == 50 && e1->resets == 50);
    CHECK(record(r, false, 300, 4) == 300 && r.extra_used == 1 && e1->resets == 51);
    CHECK(record(r, false, 10, 4) == 10 && r.extra_used == 0 && e0->resets == 52 && e1->resets == 51);
    CHECK(record(r, false, 10, 4) == 10 && e0->resets == 52 && e1->resets == 51);   // (not gone into: not reset again)
    // 120 recordings of the slot since the first: it keeps as many as one of them went into (two), then none
    while (r.pool_age) CHECK(record(r, false, 10, 4) == 10);
    CHECK(r.extra.size() == 2 && destroyed == 0);
    for (int i = 0; i < 119; i++) CHECK(record(r, false, 300, 4) == 300 && r.extra.size() == 2);
    CHECK(record(r, false, 10, 4) == 10 && r.extra.size() == 1 && destroyed == 1 && e0->live && !e1->live);
    for (int i = 0; i < 119; i++) CHECK(record(r, false, 10, 4) == 10 && r.extra.size() == 1);
    CHECK(record(r, false, 10, 4) == 10 && r.extra.empty() && destroyed == 2 && live_pools() == 1 && pool_stats.retired == 2);
    // a scene that needs them again makes them again
    CHECK(record(r, false, 700, 4) == 700 && r.extra.size() == 2 && made == 5);
    // descriptors run out before sets do: the same rule (the model's pool holds 2816 descriptors here: 70 sets of 40)
    RecordingRes d;
    CHECK(record(d, true, 200, 40) == 200 && d.extra_used == 2 && d.in_pool == 200 - 2 * 70);
    // what the log says of a slot's last recording
    pools_begin(d, false);
    CHECK(pool_stats.most_sets == 700 && pool_stats.most_pools == 3 && pool_stats.past_first > 0 && pool_stats.with_sets > 0);
    std::printf("PASS: a 32nd of the size: a slot keeps the pools its recordings go on into, and gives back the ones 120 of them left alone\n");
    return 0;
}
'''


def check_rules(draw, cxx):
    stats = block(draw, "struct PoolStats {") + ";"
    kept = draw[draw.index("size_t pools_kept("):]
    parts = {
        "@KINDS@": re.search(r"constexpr int kPoolKinds = 6;", draw).group(0),
        "@CAPS_STRUCT@": re.search(r"struct PoolCaps \{[^}]*\};", draw).group(0),
        "@CAPS_OF@": block(draw, "PoolCaps pool_caps_of(int asked)"),
        "@CAPS@": block(draw, "const PoolCaps& pool_caps()"),
        "@KEPT@": block(kept, "size_t pools_kept("),
        "@TYPES@": re.search(r"constexpr VkDescriptorType kPoolTypes\[kPoolKinds\] = \{[^}]*\};", draw).group(0),
        "@KIND@": block(draw, "constexpr int pool_kind(VkDescriptorType t)"),
        "@STATS@": stats,
        "@MAKE@": block(draw, "VkDescriptorPool make_pool()"),
        "@MISFIT@": block(draw, "VkDescriptorSet pool_misfit()"),
        "@SET@": block(draw, "VkDescriptorSet pool_set(RecordingRes& r, VkDescriptorSetLayout layout)"),
        "@BEGIN@": block(draw, "void pools_begin(RecordingRes& r, bool first)"),
    }
    # (the fixture's RecordingRes is the pools' part of the real one: the same fields, in the same words)
    real = block(draw, "struct RecordingRes {")
    for field in ("VkDescriptorPool pool = VK_NULL_HANDLE;", "std::vector<VkDescriptorPool> extra;", "uint32_t extra_used = 0;",
                  "uint32_t extra_peak = 0, pool_age = 0;", "uint32_t in_pool = 0;", "uint32_t sets = 0, descs[kPoolKinds] = {};"):
        need(field in real and field in FIXTURE, "RecordingRes no longer has: " + field)
    source = FIXTURE
    for key, text in parts.items():
        source = source.replace(key, text)
    with tempfile.TemporaryDirectory(prefix="desc-pools-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(source, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        subprocess.run([str(exe), "small"], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    draw = DRAW.read_text(encoding="utf-8")
    check_source(draw)
    check_rules(draw, a.cxx)


if __name__ == "__main__":
    main()
