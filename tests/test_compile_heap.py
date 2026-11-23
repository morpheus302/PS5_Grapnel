"""(ps5) The driver's compiles in arenas of their own (host/posix/ps5_compile_heap.c, HOOKSHOT_PS5_COMPILE_HEAP).

The run of 15/11/2026 (75 min, logs/udp/essai-lot33-1.txt) stopped at the memory floor after the driver compiled the graphics
pipelines met in Castle Town: the host heap went from 1,544 to 1,816 MiB (peak 1,928) and kept it. A compile frees nearly all
it asks for, but in the title heap (the SDK's heap.c over dlmalloc, eight arenas shared by the title's threads, segments of 16
MiB joined when side by side) what other threads keep meanwhile pins the compile's segments. These checks keep:
- the setting read once, at the first compile, 0 or unset by default: as before;
- the heap's allocations (ps5_heap_malloc, ps5_heap_calloc, ps5_heap_memalign: not dlmalloc's mspace_malloc, which its own
  memalign and realloc call, and lld wraps a call inside the defining object too), its segments' return, RADV's two entry points
  that compile and the three calls that make what a compile keeps, wrapped by the link; the source in the console's build;
- ps5_platform.c's wraps: a segment mapped with the guard on each side, dlmalloc given the middle, its return of exactly that
  handed to the heap as the whole block, the record forgotten only when the heap took it;
- the wrapped functions' signatures as the console's RADV has them (mesa-work/game-align above the repository, when found);
- no allocation in the file (its callers allocate through it);
- built and run over a model of the console's heap with the SDK's dlmalloc configured as the archive's (tests/compile_heap_test.c;
  skipped when ps5-stack/PS5_PayloadSDK is not above the repository): off, a compile mixed with another thread's kept blocks
  leaves its high-water mark in the heap, dlmalloc's trim or not (the case of 15/11 in small); on, the same compile gives it
  back; a paused call allocates in the thread's arena; guarded segments never touch and go back whole; a compile left by a jump
  is found stale; nested scopes trim once; four threads compiling, pausing and freeing each other's blocks at once.

python -I -B tests/test_compile_heap.py [--cc cc]
"""
import argparse
import functools
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "host" / "posix" / "ps5_compile_heap.c"
PLATFORM = ROOT / "host" / "posix" / "ps5_platform.c"
HEADER = ROOT / "host" / "posix" / "ps5.h"
TOOLCHAIN = ROOT / "nx2" / "toolchain_ps5.py"
BUILD = ROOT / "nx2" / "build.py"
HARNESS = ROOT / "tests" / "compile_heap_test.c"
WRAPS = ("ps5_heap_malloc", "ps5_heap_calloc", "ps5_heap_memalign", "ps5p_heap_unmap", "radv_CreateGraphicsPipelines",
         "radv_CreateComputePipelines", "radv_shader_create", "radv_pipeline_cache_insert", "vk_pipeline_cache_lookup_object")

# dlmalloc as libps5platform.a's mspace.o has it (tests/test_heap_pieces.py)
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


def above(*parts):
    d = ROOT
    while True:
        p = d.joinpath(*parts)
        if p.exists():
            return p
        if d.parent == d:
            return None
        d = d.parent


def check_text():
    src, platform, header = code(SRC.read_text()), code(PLATFORM.read_text()), HEADER.read_text()
    # the setting: once, at the first compile, off by default
    need(src.count('getenv("HOOKSHOT_PS5_COMPILE_HEAP")') == 1 and 'getenv("HOOKSHOT_PS5_COMPILE_HEAP")' in block(src, "static int ch_ready(void)"),
         "HOOKSHOT_PS5_COMPILE_HEAP is read elsewhere than ch_ready")
    need("atoi(e) > 0" in block(src, "static int ch_ready(void)"), "the setting is not off when unset or 0")
    need(block(src, "int hos_compile_heap_enter(void)").index("ch_ready()") < block(src, "int hos_compile_heap_enter(void)").index("depth++"),
         "a scope is entered before the setting is read")
    need(not re.search(r"static[^;{(]*=\s*(getenv|ch_ready)\(", src), "the setting is kept in a static initialised before main")
    need('getenv("HOOKSHOT_PS5_COMPILE_KEEP_MB")' in block(src, "static int ch_ready(void)") and ": 64) << 20" in src,
         "HOOKSHOT_PS5_COMPILE_KEEP_MB is not read with the setting, 64 MiB by default")
    need("if (ch_keep && before <= ch_keep) return;" in block(src, "void hos_compile_heap_leave(int token)"),
         "an arena under the threshold is trimmed after a compile")
    # no allocation here (its callers allocate through it), but in the allocator the driver is given for the pipeline objects
    parts = [block(src, m) for m in ("static void *ch_object_allocation(", "static void *ch_object_reallocation(", "static void ch_object_free(")]
    objects = "".join(parts)
    need("hos_compile_heap_pause()" in block(src, "static void *ch_object_allocation(") and "if (scope)" in objects,
         "the pipeline objects' allocator does not pause the compile arena for what outlives the call")
    bare = re.sub(r"\b(mspace_\w+|__real_ps5_heap_\w+|__wrap_ps5_heap_\w+|ps5_heap_\w+)\(", "X(", functools.reduce(lambda t, b: t.replace(b, ""), parts, src))
    for name in ("malloc(", "calloc(", "realloc(", "free(", "new ", "strdup("):
        need(name not in bare, "ps5_compile_heap.c calls %s" % name)
    # the routing: a compile arena or the heap's own, a refusal falls back on the thread's arena
    for fn, real in (("void *__wrap_ps5_heap_malloc(", "__real_ps5_heap_malloc(bytes)"),
                     ("void *__wrap_ps5_heap_calloc(", "__real_ps5_heap_calloc(count, bytes)"),
                     ("void *__wrap_ps5_heap_memalign(", "__real_ps5_heap_memalign(alignment, bytes)")):
        b = block(src, fn)
        need("ch_route()" in b and b.index("ch_route()") < b.index("mspace_") < b.index(real), "%s does not route, then fall back" % fn)
    route = block(src, "static void *ch_route(void)")
    need("s->depth <= 0 || s->paused" in route, "a paused call or a thread outside a compile is routed")
    # the driver's calls: a scope around the entry points, a pause around what a compile keeps
    for fn, real, pair in (("int32_t __wrap_radv_CreateGraphicsPipelines(", "__real_radv_CreateGraphicsPipelines(", ("hos_compile_heap_enter()", "hos_compile_heap_leave(t)")),
                           ("int32_t __wrap_radv_CreateComputePipelines(", "__real_radv_CreateComputePipelines(", ("hos_compile_heap_enter()", "hos_compile_heap_leave(t)")),
                           ("void *__wrap_radv_shader_create(", "__real_radv_shader_create(", ("hos_compile_heap_pause()", "hos_compile_heap_resume()")),
                           ("void __wrap_radv_pipeline_cache_insert(", "__real_radv_pipeline_cache_insert(", ("hos_compile_heap_pause()", "hos_compile_heap_resume()")),
                           ("void *__wrap_vk_pipeline_cache_lookup_object(", "__real_vk_pipeline_cache_lookup_object(", ("hos_compile_heap_pause()", "hos_compile_heap_resume()"))):
        b = block(src, fn)
        need(b.index(pair[0]) < b.index(real) < b.index(pair[1]), "%s does not bracket the driver's call" % fn)
        if "Pipelines" in fn:
            need("hos_compile_heap_allocator(allocator)" in b, "%s does not give the driver the pipeline objects' allocator" % fn)
    need("#if defined(__PROSPERO__)\n/* ---- the driver's calls" in SRC.read_text(), "the driver's wraps are not the console's only")
    # the guards in ps5_platform.c's wraps
    m = block(platform, "void *__wrap_ps5p_heap_map(size_t bytes)")
    need("hos_compile_heap_guard_take(&slot)" in m and "heap_map_counted(bytes + 2 * guard)" in m and
         "hos_compile_heap_guard_set(slot, " in m, "the heap's map does not take the guards around what dlmalloc is given")
    need("always_inline" in platform.split("heap_map_counted(size_t bytes) {")[0][-200:],
         "the map's body is not inlined into the wrap: the growth by site would count one frame more")
    u = block(platform, "int __wrap_ps5p_heap_unmap(void *address, size_t bytes)")
    need("hos_compile_heap_guard_find(address, bytes, &at, &length)" in u and "__real_ps5p_heap_unmap(at, length)" in u and
         "if (r == 0) hos_compile_heap_guard_forget(slot)" in u, "the heap's unmap does not hand a guarded segment back whole")
    need("int hos_compile_heap_enter(void);" in header and "struct hos_compile_heap_stats" in header, "ps5.h lacks the declarations")
    # the link and the build
    tc = TOOLCHAIN.read_text()
    m = re.search(r"COMPILE_HEAP_WRAPS = \(([^)]*)\)", tc)
    need(m and tuple(re.findall(r'"(\w+)"', m.group(1))) == WRAPS, "nx2/toolchain_ps5.py's COMPILE_HEAP_WRAPS is not the list")
    need('["--wrap=" + n for n in COMPILE_HEAP_WRAPS]' in tc, "the link does not pass COMPILE_HEAP_WRAPS")
    need("mspace_malloc\"" not in tc.split("COMPILE_HEAP_WRAPS")[1][:400], "dlmalloc's mspace_malloc is wrapped")
    need('"ps5_compile_heap.c"' in BUILD.read_text(), "nx2/build.py does not build host/posix/ps5_compile_heap.c for the console")


def check_signatures():
    """the wrapped functions as the console's RADV has them (game-align, bdb1031), when its source is above the repository"""
    mesa = above("mesa-work", "game-align", "src")
    if not mesa:
        print("note: mesa-work/game-align is not above the repository: the driver's signatures are not checked")
        return
    def proto(path, name):
        t = code((mesa / path).read_text())
        m = re.search(r"\n[\w \*]*?\b%s\(([^)]*)\)\s*\{" % name, t)
        need(m, "%s not found in %s" % (name, path))
        return [p.strip() for p in m.group(1).split(",")]
    g = proto("amd/vulkan/radv_pipeline_graphics.c", "radv_CreateGraphicsPipelines")
    need(len(g) == 6 and g[0].startswith("VkDevice") and g[1].startswith("VkPipelineCache") and g[2].startswith("uint32_t"),
         "radv_CreateGraphicsPipelines: %s" % g)
    c = proto("amd/vulkan/radv_pipeline_compute.c", "radv_CreateComputePipelines")
    need(len(c) == 6 and c[2].startswith("uint32_t"), "radv_CreateComputePipelines: %s" % c)
    s = proto("amd/vulkan/radv_pipeline_cache.c", "radv_shader_create")
    need(len(s) == 5 and s[3].startswith("bool") and "*" in s[0] and "*" in s[1] and "*" in s[2] and "*" in s[4],
         "radv_shader_create: %s" % s)
    i = proto("amd/vulkan/radv_pipeline_cache.c", "radv_pipeline_cache_insert")
    need(len(i) == 3 and all("*" in p for p in i), "radv_pipeline_cache_insert: %s" % i)
    k = proto("vulkan/runtime/vk_pipeline_cache.c", "vk_pipeline_cache_lookup_object")
    need(len(k) == 5 and k[2].startswith("size_t") and k[4].startswith("bool"), "vk_pipeline_cache_lookup_object: %s" % k)


def run_harness(cc):
    if not shutil.which(cc):
        print("SKIP: no %s: the harness is not built" % cc)
        return
    dl = above("ps5-stack", "PS5_PayloadSDK", "platform", "src", "dlmalloc")
    if not dl or not (dl / "malloc.c").is_file():
        print("note: the SDK's dlmalloc (ps5-stack/PS5_PayloadSDK/platform/src/dlmalloc) is not above the repository: the harness is left out")
        return
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "dlm.c").write_text(DLMALLOC_CONFIG)
        subprocess.run([cc, "-O1", "-g", "-std=gnu11", "-w", "-I", str(dl), "-c", str(tmp / "dlm.c"), "-o", str(tmp / "dlm.o")], check=True)
        exe = tmp / "compile_heap_test"
        # (ps5_compile_heap.c's allocator for the pipeline objects calls malloc, realloc, free and posix_memalign: the model's here)
        subprocess.run([cc, "-O1", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", "-DHOOKSHOT_PS5_HOST_MODEL",
                        "-Dmalloc=test_malloc", "-Dfree=test_free", "-Drealloc=test_realloc", "-Dposix_memalign=test_posix_memalign",
                        "-I", str(ROOT / "host" / "posix"), "-c", str(SRC), "-o", str(tmp / "src.o")], check=True)
        subprocess.run([cc, "-O1", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", "-DHOOKSHOT_PS5_HOST_MODEL",
                        "-I", str(ROOT / "host" / "posix"), str(HARNESS), str(tmp / "src.o"), str(tmp / "dlm.o"), "-o", str(exe)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith("HOOKSHOT_PS5_COMPILE_")}
        off = subprocess.run([str(exe), "-q", "0"], capture_output=True, text=True, timeout=600, env=env)
        print(off.stderr.strip())
        need(off.returncode == 0, "the harness failed with the setting unset")
        on = subprocess.run([str(exe), "-q", "1"], capture_output=True, text=True, timeout=600,
                            env=dict(env, HOOKSHOT_PS5_COMPILE_HEAP="1", HOOKSHOT_PS5_COMPILE_KEEP_MB="0"))
        print(on.stderr.strip())
        need(on.returncode == 0, "the harness failed with the setting on, trimmed after every compile")
        dflt = subprocess.run([str(exe), "-q", "1"], capture_output=True, text=True, timeout=600, env=dict(env, HOOKSHOT_PS5_COMPILE_HEAP="1"))
        print(dflt.stderr.strip())
        need(dflt.returncode == 0, "the harness failed with the setting on and its default threshold (64 MiB)")
        two = subprocess.run([str(exe), "-q", "1"], capture_output=True, text=True, timeout=600,
                             env=dict(env, HOOKSHOT_PS5_COMPILE_HEAP="1", HOOKSHOT_PS5_COMPILE_ARENAS="2"))
        print(two.stderr.strip())
        need(two.returncode == 0, "the harness failed with two compile arenas")
        a = int(re.search(r"off_after_mib (\d+)", off.stdout).group(1))
        b = int(re.search(r"on_after_mib (\d+)", on.stdout).group(1))
        c = int(re.search(r"on_after_mib (\d+)", dflt.stdout).group(1))
        print("the heap after a compile of 300 MiB mixed with another thread's kept blocks: %d MiB off, %d MiB on (%d with the "
              "default threshold)" % (a, b, c))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.environ.get("CC") or "cc")
    a = ap.parse_args()
    check_text()
    check_signatures()
    run_harness(a.cc)
    print("ok")


if __name__ == "__main__":
    main()
