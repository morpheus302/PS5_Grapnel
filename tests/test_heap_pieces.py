"""(ps5) The host heap's blocks when the direct memory is in pieces (host/posix/ps5_heap_pieces.c).

The run of 11/11/2026 at 20:04 ended on an uncaught std::bad_alloc with 304 MiB of direct memory free in pieces of 15 MiB at
most: the title heap (the SDK's heap.c and dlmalloc, libps5platform.a) maps each of its segments from one allocation of direct
memory, 16 MiB at a time, and a request of 32 MiB and more as one segment of its size; ACO's allocations of 33 to 80 MiB had
been refused the same way, a compile faulted out of the driver and three background jobs failed. A segment's direct memory
refused in one piece is now made of several, mapped side by side, when HOOKSHOT_PS5_HEAP_PIECES_KB allows it. These checks
keep:
- the setting read at each refusal only (hookshot_env.txt is loaded in main), 0 or unset by default: as before;
- only the heap's map call's allocation made of pieces (the thread marked around the heap's map, the whole range, 64 KiB
  aligned), never the driver's, the window's or another reservation's; a start of the file's routed to its map and release
  by the wraps of host/posix/ps5_platform.c, every other one to the kernel as before; the link wraps the calls;
- built over a model of the console's direct memory (a memfd: each piece a range of its own) and of the heap's map and unmap
  as the archive's heap.o does them, and run: blocks of one piece where memory is abundant; refused with the setting off;
  made of pieces with it on, holding what is written, in distinct units, given back whole; a piece's mapping refused part
  way leaves nothing taken and the range reserved again; pieces under the least size, more than 64 pieces, every slot in
  use: refused cleanly; and, where the SDK's dlmalloc is found (ps5-stack/PS5_PayloadSDK above the repository), dlmalloc
  configured as the archive's mspace.o over all of it, on one thread and on four, with the trim of its arenas;
- the uncaught std::bad_alloc's stop line (gpu/vk/vk_vma.cpp's terminate handler) and the memory watch's trim
  (HOOKSHOT_PS5_HEAP_TRIM_S, off by default) and line.

python -I -B tests/test_heap_pieces.py [--cc cc]
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIECES = ROOT / "host" / "posix" / "ps5_heap_pieces.c"
PLATFORM = ROOT / "host" / "posix" / "ps5_platform.c"
HEADER = ROOT / "host" / "posix" / "ps5.h"
WATCH = ROOT / "host" / "hos_thread.c"
VMA = ROOT / "gpu" / "vk" / "vk_vma.cpp"
TOOLCHAIN = ROOT / "nx2" / "toolchain_ps5.py"
BUILD = ROOT / "nx2" / "build.py"
HARNESS = ROOT / "tests" / "heap_pieces_test.c"

# dlmalloc as libps5platform.a's mspace.o has it (its mparams, read from the archive on 11/11: 64 KiB pages, 16 MiB granularity,
# 32 MiB mmap threshold, 2 MiB trim threshold; the SDK's source has moved on to 2 MiB segments, the archive has not)
DLMALLOC_CONFIG = """#include <errno.h>
#include <stddef.h>
void *ps5p_heap_map(size_t bytes);
int ps5p_heap_unmap(void *address, size_t bytes);
#define ONLY_MSPACES 1
#define USE_LOCKS 1
#define USE_SPIN_LOCKS 0
#define FOOTERS 1
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define MMAP(s) ps5p_heap_map(s)
#define DIRECT_MMAP(s) ps5p_heap_map(s)
#define MUNMAP(a, s) ps5p_heap_unmap((a), (s))
#define MMAP_CLEARS 0
#define MALLOC_ALIGNMENT ((size_t)32)
#define DEFAULT_GRANULARITY ((size_t)16 << 20)
#define DEFAULT_MMAP_THRESHOLD ((size_t)32 << 20)
#define MALLOC_FAILURE_ACTION errno = ENOMEM
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define malloc_getpagesize ((size_t)0x10000)
#include "malloc.c"
"""


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def code(text):
    """text without its comments"""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def dlmalloc_dir():
    d = ROOT
    while True:
        p = d / "ps5-stack" / "PS5_PayloadSDK" / "platform" / "src" / "dlmalloc"
        if (p / "malloc.c").is_file():
            return p
        if d.parent == d:
            return None
        d = d.parent


def check_text():
    pieces, platform, header = code(PIECES.read_text()), code(PLATFORM.read_text()), HEADER.read_text()
    watch, vma = code(WATCH.read_text()), code(VMA.read_text())
    # the setting, read at a refusal only, off by default
    need(pieces.count('getenv("HOOKSHOT_PS5_HEAP_PIECES_KB")') == 1 and
         'getenv("HOOKSHOT_PS5_HEAP_PIECES_KB")' in block(pieces, "size_t hos_heap_pieces_min(void)"),
         "HOOKSHOT_PS5_HEAP_PIECES_KB is read elsewhere than hos_heap_pieces_min")
    need(not re.search(r"static[^;{]*=\s*hos_heap_pieces_min\(", pieces + platform + watch),
         "the setting is kept in a static initialised once")
    minimum = block(pieces, "size_t hos_heap_pieces_min(void)")
    need(": 0;" in minimum and "if (!kb" in minimum, "the setting is not off when unset or 0")
    after = block(pieces, "int32_t hos_heap_pieces_after_refusal(")
    need(after.index("if (!refused") < after.index("hp_is_mapper") < after.index("hos_heap_pieces_min()") < after.index("hp_take_lock"),
         "a refusal is not checked for the heap's map call and the setting before anything is taken")
    need("search_start != 0" in after and "alignment > HP_UNIT" in after,
         "a refusal of a part of the range, or of an alignment over 64 KiB, may be made of pieces")
    need("hos_ps5_piece_give" in after, "the pieces of a block that could not be made whole are not given back")
    need(re.search(r"#define HP_TAG \(\(uint64_t\)0x7a << 40\)", pieces), "the blocks' starts are not past the direct memory")
    mapping = block(pieces, "int32_t hos_heap_pieces_map(")
    need("hos_ps5_piece_unmap(at, off, 1)" in mapping, "a mapping that fails part way does not unmap and reserve what it mapped")
    need("HP_FIXED" in mapping, "a block of pieces is mapped at an address the call did not fix")
    for name in ("malloc", "calloc", "realloc", "free(", "operator new", "printf("):
        need(name not in pieces.replace("hos_ps5_trail(", "").replace("mspace_trim", ""), "ps5_heap_pieces.c calls %s" % name)
    # the console's glue
    allocate = block(platform, "int32_t __wrap_sceKernelAllocateDirectMemory(")
    need(re.search(r"direct_counted\(search_start, search_end, length, alignment, type, start_out\);\s*return r == 0 \? 0 : "
                   r"hos_heap_pieces_after_refusal\(r, search_start", allocate),
         "the allocation's wrap does not hand its refusal, and only its refusal, to ps5_heap_pieces.c")
    need(allocate.index("STOPPING THE TITLE") < allocate.index("direct_counted"), "the floor is not looked at before the allocation")
    need("hos_heap_pieces_is(start)) return hos_heap_pieces_map" in block(platform, "int32_t __wrap_sceKernelMapDirectMemory("),
         "the map's wrap does not route a block's start")
    need("__real_sceKernelMapDirectMemory(address, length, protection, flags, start, alignment)" in
         block(platform, "int32_t __wrap_sceKernelMapDirectMemory("), "the map's wrap does not pass every other start on")
    need("hos_heap_pieces_is(start)) return hos_heap_pieces_release" in block(platform, "int32_t __wrap_sceKernelReleaseDirectMemory("),
         "the release's wrap does not route a block's start")
    # (2026-11-15: the wrap's body is heap_map_counted, inlined into it; the wrap adds ps5_compile_heap.c's guards around it)
    heap_map = block(platform, "void *__wrap_ps5p_heap_map(size_t bytes)") + block(platform, "heap_map_counted(size_t bytes) {")
    need(re.search(r"hos_heap_mapper_enter\(\);\s*void \*const at = __real_ps5p_heap_map\(bytes\);\s*hos_heap_mapper_leave\(slot\);",
                   heap_map), "the heap's map call does not mark its thread around the heap's own")
    need("__real_create_mspace(capacity, locked)" in block(platform, "void *__wrap_create_mspace(") and
         "hos_heap_arena_note(m)" in block(platform, "void *__wrap_create_mspace("), "the arenas are not noted")
    taken = block(platform, "int32_t hos_ps5_piece_take(")
    need("direct_counted(0, search_end" in taken, "a piece is not placed and counted as any allocation")
    need("release_counted(start, length)" in block(platform, "int32_t hos_ps5_piece_give("), "a piece is not released as any is")
    need("always_inline" in platform[platform.index("int32_t direct_counted(") - 120:platform.index("int32_t direct_counted(")],
         "the counted allocation is not inlined (the site's words would change)")
    link = TOOLCHAIN.read_text()
    for w in ("--wrap=sceKernelAllocateDirectMemory", "--wrap=sceKernelReleaseDirectMemory", "--wrap=sceKernelMapDirectMemory",
              "--wrap=create_mspace", "--wrap=ps5p_heap_map"):
        need(link.count('"%s"' % w) == 1, "the eboot's link lacks %s" % w)
    need('"ps5_heap_pieces.c"' in BUILD.read_text(), "nx2/build.py does not build host/posix/ps5_heap_pieces.c for the console")
    for name in ("hos_heap_pieces_after_refusal", "hos_heap_pieces_map", "hos_heap_pieces_release", "hos_heap_mapper_enter",
                 "hos_ps5_piece_take", "hos_ps5_heap_stop_line", "hos_heap_trim_every", "hos_ps5_heap_trim_now"):
        need(name + "(" in header, "ps5.h does not declare %s" % name)
    # the stop line: a std::bad_alloc nobody caught, before the crash line and its abort
    term = block(vma, "[[noreturn]] void ps5_terminate()")
    need(term.index("catch (const std::bad_alloc& x)") < term.index("catch (const std::exception& x)"),
         "the terminate handler does not catch std::bad_alloc first")
    bad = term[term.index("catch (const std::bad_alloc& x)"):term.index("catch (const std::exception& x)")]
    need("hos_ps5_heap_stop_line(x.what())" in bad and bad.index("hos_ps5_heap_stop_line") < bad.index("crash: uncaught"),
         "an uncaught std::bad_alloc does not write the stop line first")
    stop = block(platform, "void hos_ps5_heap_stop_line(const char *what)")
    need('getenv("HOOKSHOT_PS5_HEAP_STOP")' in stop and "STOPPING THE TITLE" in stop and "largest" in stop and
         "heap_refusal.bytes" in stop, "the stop line lacks its setting, its words, the largest piece or the refused size")
    # the watch: the trim off unless asked, the line
    need("const unsigned trim_every = hos_heap_trim_every();" in watch and "if (trim_every &&" in watch,
         "the memory watch trims the heap without HOOKSHOT_PS5_HEAP_TRIM_S")
    need("hos_ps5_heap_pieces_line(hp, sizeof hp)" in watch, "the memory watch does not say what the blocks of pieces hold")
    trim_every = block(pieces, "unsigned hos_heap_trim_every(void)")
    need('getenv("HOOKSHOT_PS5_HEAP_TRIM_S")' in trim_every and ": 0;" in trim_every, "the trim is not off by default")


def run_harness(cc):
    if not shutil.which(cc):
        print("SKIP: no %s: the harness is not built" % cc)
        return
    dl = dlmalloc_dir()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        objs, defs = [], []
        if dl:
            (tmp / "dlm.c").write_text(DLMALLOC_CONFIG)
            subprocess.run([cc, "-O1", "-g", "-std=gnu11", "-w", "-I", str(dl), "-c", str(tmp / "dlm.c"), "-o", str(tmp / "dlm.o")],
                           check=True)
            objs, defs = [str(tmp / "dlm.o")], ["-DHAVE_DLMALLOC"]
        else:
            print("note: the SDK's dlmalloc (ps5-stack/PS5_PayloadSDK/platform/src/dlmalloc) is not above the repository: "
                  "its part of the harness is left out")
        exe = tmp / "heap_pieces_test"
        subprocess.run([cc, "-O1", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", "-DHOOKSHOT_PS5_HOST_MODEL"] + defs +
                       ["-I", str(ROOT / "host" / "posix"), str(HARNESS), str(PIECES)] + objs + ["-o", str(exe)], check=True)
        r = subprocess.run([str(exe), "-q"], capture_output=True, text=True, timeout=600)
        print(r.stderr.strip())
        need(r.returncode == 0, "the harness failed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.environ.get("CC") or "cc")
    a = ap.parse_args()
    check_text()
    run_harness(a.cc)
    print("ok")


if __name__ == "__main__":
    main()
