"""(ps5) The uploads' big scratch buffers do not come from the host heap on the console.

Where the texture shadow holds a page of a texture's storage, an upload reads the storage into a scratch vector and lays the
kept copies over it (gpu/vk/vk_memory.cpp, memory_as_submitted): a whole 16 MB level for the big textures the CPU rewrites in
Hyrule Field. From the console's heap each was a 32 MB block mapped for one upload (nine in the field's first minute,
"heap: a block of 32 MiB asked" in logs/udp/essai-lot33-1.txt), and the small allocations that landed in its other half
kept it. gpu/vk/vk_internal.h's ScratchAlloc now takes buffers of 1 MB and more from blocks of direct memory of their own on
the console (hos_ps5_block), given back when the vector lets them go; smaller ones, a block refused and every other
platform keep the heap. These checks:
- the allocator and vk_memory.cpp's scratch_take / scratch_give, compiled from their own text over a model of the console's
  two calls, and as a PC builds them: sizes in 64 KiB units, given back with the size they were made with; the heap below
  1 MB, where a block is refused, past the table's 32 entries and with RX_VK_SCRATCH_BLOCKS=0; nothing of the console's
  named on a PC;
- the bytes a vector holds the same whichever way it was made (assign, resize, growth);
- the uploads' scratch (guest_bytes, upload, upload_astc_gpu, memory_as_submitted) and the re-upload log's buffer are that
  vector.

python -I -B tests/test_upload_scratch_blocks.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]   # (tests/run_all.py reads it)
RUN_ALL_SKIP = {"windows": "reads the PC fixture's symbols with nm"}

ROOT = Path(__file__).resolve().parents[1]
MEMORY = ROOT / "gpu" / "vk" / "vk_memory.cpp"
INTERNAL = ROOT / "gpu" / "vk" / "vk_internal.h"
TEXTURE = ROOT / "gpu" / "vk" / "vk_texture.cpp"


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


FIXTURE = r'''
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
static long heap_big, heap_big_freed;
static void* big_ptrs[256];   // (the heap's live allocations of 1 MB and more)
void* operator new(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    if (n >= (1u << 20)) {
        heap_big++;
        for (void*& b : big_ptrs) if (!b) { b = p; break; }
    }
    return p;
}
void operator delete(void* p) noexcept {
    for (void*& b : big_ptrs) if (p && b == p) { b = nullptr, heap_big_freed++; break; }
    std::free(p);
}
void operator delete(void* p, size_t) noexcept { operator delete(p); }
#ifdef CONSOLE
#define __PROSPERO__ 1
static long own_made, own_freed, own_refuse;
static size_t own_bytes, last_size;
static struct { void* p; size_t size; } owned[64];
extern "C" void* hos_ps5_block(size_t size) {
    CHECK(size && !(size & 0xFFFF));
    if (own_refuse) return nullptr;
    for (auto& o : owned)
        if (!o.p) {
            o.p = std::malloc(size), o.size = size;
            std::memset(o.p, 0xA5, size);   // (not zeros: whoever wants zeros makes them)
            own_made++, own_bytes += size, last_size = size;
            return o.p;
        }
    return nullptr;
}
extern "C" void hos_ps5_block_free(void* p, size_t size) {
    for (auto& o : owned)
        if (o.p == p) {
            CHECK(o.size == size);
            std::free(p);
            o.p = nullptr, own_freed++, own_bytes -= size;
            return;
        }
    CHECK(!"a block given back that was never made");
}
#endif
namespace mkwvk {
static void logf(const char*, ...) {}
@DECL@
@IMPL@
}  // namespace mkwvk
using mkwvk::ScratchBytes;
static void pattern(uint8_t* p, size_t n, uint8_t k) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 31 + k); }
static bool is_pattern(const uint8_t* p, size_t n, uint8_t k) { for (size_t i = 0; i < n; i++) if (p[i] != (uint8_t)(i * 31 + k)) return false; return true; }
int main(int argc, char** argv) {
    const bool off = argc > 1 && !std::strcmp(argv[1], "off");
    const size_t level = 16u << 20;   // (a 4096x4096 level of one byte a texel: Hyrule Field's big CPU-written textures)
    std::vector<uint8_t> live(level + 4096);
    pattern(live.data(), live.size(), 3);
    heap_big = 0;   // (live itself does not count)
#ifdef CONSOLE
    const bool blocks = !off;
#else
    const bool blocks = false;
#endif
    {   // memory_as_submitted's assign of a whole level
        ScratchBytes s;
        s.assign(live.data(), live.data() + level);
        CHECK(s.size() == level && is_pattern(s.data(), level, 3));
#ifdef CONSOLE
        CHECK(blocks ? own_made == 1 && last_size == level && own_bytes == level && heap_big == 0 : own_made == 0 && heap_big == 1);
#else
        CHECK(heap_big == 1);
#endif
    }
#ifdef CONSOLE
    CHECK(own_bytes == 0 && own_freed == own_made);
#endif
    {   // below 1 MB: the heap, whatever the switch
        const long before = heap_big;
        ScratchBytes s(512u << 10, 0);
        CHECK(heap_big == before);
#ifdef CONSOLE
        CHECK(own_bytes == 0);
#endif
    }
    {   // a size past a 64 KiB unit, then growth: each buffer of its own, the bytes carried over
        ScratchBytes s;
        s.assign(live.data(), live.data() + (1u << 20) + 1);
#ifdef CONSOLE
        if (blocks) CHECK(last_size == (1u << 20) + 0x10000 && own_bytes == last_size);
#endif
        s.resize(3u << 20);
        CHECK(is_pattern(s.data(), (1u << 20) + 1, 3));
        for (size_t i = (1u << 20) + 1; i < s.size(); i++) CHECK(s[i] == 0);   // (resize's new bytes are zeros, a block's 0xA5 or not)
#ifdef CONSOLE
        if (blocks) CHECK(own_bytes == ((s.capacity() + 0xFFFF) & ~(size_t)0xFFFF));   // (the first one already given back)
#endif
        ScratchBytes z;
        z.assign(2u << 20, 0);
        for (uint8_t b : z) CHECK(b == 0);
    }
#ifdef CONSOLE
    CHECK(own_bytes == 0 && own_freed == own_made);
    if (blocks) {   // a block refused: the heap, and it goes back there
        own_refuse = 1;
        const long before = heap_big, freed = heap_big_freed, made = own_made;
        {
            ScratchBytes s;
            s.assign(live.data(), live.data() + level);
            CHECK(heap_big == before + 1 && own_made == made && is_pattern(s.data(), level, 3));
        }
        CHECK(heap_big_freed == freed + 1);
        own_refuse = 0;
        // (33 at once: the table's 32 blocks, then the heap)
        std::vector<ScratchBytes> many(33);
        const long h = heap_big;
        for (auto& s : many) s.assign(1u << 20, 1);
        CHECK(own_bytes == 32u << 20 && heap_big == h + 1);
        many.clear();
        CHECK(own_bytes == 0 && own_freed == own_made);
    }
#endif
    std::printf("%s\n", blocks ? "blocks" : "heap");
}
'''


def run(cmd, env=None):
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        raise SystemExit("FAIL: %s\n%s%s" % (" ".join(map(str, cmd)), r.stdout[-3000:], r.stderr[-3000:]))
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    mem = MEMORY.read_text(encoding="utf-8")
    internal = INTERNAL.read_text(encoding="utf-8")
    tex = TEXTURE.read_text(encoding="utf-8")
    # (the text: what the uploads read into is that vector, end to end)
    need("const uint8_t* memory_as_submitted(uint64_t guest, const uint8_t* live, uint64_t n, ScratchBytes& scratch);" in internal and
         "const uint8_t* memory_as_submitted(uint64_t guest, const uint8_t* live, uint64_t n, ScratchBytes& scratch) {" in mem,
         "memory_as_submitted does not take the scratch vector")
    need("const uint8_t* guest_bytes(uint64_t gpu, uint64_t n, ScratchBytes& scratch) {" in tex, "guest_bytes does not take it")
    need("ScratchBytes scratch;" in block(tex, "bool upload_inner(Texture& tex, const Ranges* dirty) {") and
         "ScratchBytes scratch;" in block(tex, "bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty)"),
         "an upload's own scratch is not that vector")
    need("ScratchBytes b;" in block(tex, "Texture* texture_touch(Texture* tex, bool for_sampling)"),
         "the re-upload log's read of what the CPU wrote is still a heap vector")
    need(not re.search(r"std::vector<uint8_t>\s*&\s*scratch", tex + mem + internal), "a scratch is still a std::vector<uint8_t>&")
    on = block(mem, "bool scratch_blocks_on()")
    need(re.search(r"#if defined\(__PROSPERO__\).*getenv\(\"RX_VK_SCRATCH_BLOCKS\"\).*const bool v = e \? atoi\(e\) != 0 : true;.*"
                   r"#else\s+return false;\s+#endif", on, re.S), "RX_VK_SCRATCH_BLOCKS is not on by default on the console and off elsewhere")
    decl = internal[internal.index("void* scratch_take(size_t bytes);"):internal.index("using ScratchBytes")]
    decl += internal[internal.index("using ScratchBytes"):].split("\n", 1)[0]
    lo = mem.index("// ---- (hookshot, ps5, 2026-11-15) the uploads' scratch bytes")
    impl = mem[lo:mem.index("const uint8_t* memory_as_submitted(", lo)]
    with tempfile.TemporaryDirectory(prefix="scratch-blocks-") as tmp:
        tmp = Path(tmp)
        src = FIXTURE.replace("@DECL@", decl).replace("@IMPL@", impl)
        (tmp / "f.cpp").write_text(src, encoding="utf-8")
        flags = ["-std=c++20", "-O1", "-Wall", "-Wno-unused-function", "-Wno-sized-deallocation"]
        run([a.cxx, *flags, "-DCONSOLE", str(tmp / "f.cpp"), "-o", str(tmp / "console")])
        run([a.cxx, *flags, str(tmp / "f.cpp"), "-o", str(tmp / "pc")])
        nm = subprocess.run(["nm", str(tmp / "pc")], capture_output=True, text=True).stdout
        need("hos_ps5_block" not in nm, "a PC build names the console's block calls")
        env = dict(os.environ)
        env.pop("RX_VK_SCRATCH_BLOCKS", None)
        need(run([str(tmp / "console")], env).stdout.strip() == "blocks", "the console's default is not the blocks")
        need(run([str(tmp / "console"), "off"], dict(env, RX_VK_SCRATCH_BLOCKS="0")).stdout.strip() == "heap",
             "RX_VK_SCRATCH_BLOCKS=0 does not give the heap back its buffers")
        need(run([str(tmp / "pc")], env).stdout.strip() == "heap", "a PC build does not keep the heap")
    print("PASS: the uploads' scratch of 1 MB and more is a block of its own on the console, given back with its vector; the heap "
          "below that, when refused, past 32 at once, with RX_VK_SCRATCH_BLOCKS=0 and on a PC")


if __name__ == "__main__":
    main()
