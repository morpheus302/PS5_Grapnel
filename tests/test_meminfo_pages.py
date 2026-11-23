"""The pages the game maps count as used memory, as longshot's a4c77174 has it (host/hos_mem.c, host/hos_svc.c).

QueryMemoryInfo told the game a constant used memory (its program and 16 MB), so ~9.6 GB free whatever it held, and OoT's
resource memory kept mapping pages for its caches: on the console the address regions held ~4.1 GiB of the window where the
memory floor ended the run. Now AllocateMemoryPages and FreeMemoryPages keep a bit per 4 KB page and the used memory told is
program + heap + mapped pages, at most the console's total. These checks keep, on the real host/hos_mem.c compiled over a
stand-in for host/hos.h (no guest window: the addresses are only counted, the commits and decommits only noted):
- pages mapped twice, or over pages already mapped, count once; freed twice, once; a part of a page is the page; a range
  across two GB of the window (two leaves of bits); a free where nothing was ever mapped changes nothing;
- the used memory told by QueryMemoryInfo (+0x08) and hos_used_memory (svc GetInfo 7 and 22) at each step, and never more
  than the total, so the game's free = total - used never wraps;
- the "mem: the game's mapped pages" line at each 256 MB step, with the peak;
- HOS_MEMINFO_OLD read as longshot's env_on reads a switch: "1", "yes", " on " give the old figures (QueryMemoryInfo program
  + 16 MB, svc GetInfo program + heap), "0", "off", " No ", "FALSE" and "" the new ones; the pages are counted either way;
- HOS_PAGES_LIMIT=1: AllocateMemoryPages refused (0xCA01, nothing committed, nothing counted) past the total of heap and
  pages, pages already mapped mapped again at the limit;
- an address region given back takes the pages still mapped in it out of the count, and says so; one freed first does not;
- eight threads mapping and freeing their own pages while others ask for the used memory: the count is the sum of theirs;
- (the console's) the memory line's note: the heap's and the regions' backed bytes asked of the window over the right ranges,
  the mapped pages and their peak, the used memory told, "(HOS_MEMINFO_OLD)"; nothing past its buffer;
- in the source: svc GetInfo 7 and 22 and QueryMemoryInfo call hos_used_memory with their old figure; the count is made and
  its lock given back before the commit or the decommit; the note takes no lock, allocates nothing, logs nothing, is the
  console's alone and is called weakly at the end of hos_ps5_memory_where, without the words the run summaries look for.
Then each of a few sabotages of host/hos_mem.c has to make a case fail.

python -I -B tests/test_meminfo_pages.py [--cc cc] [--no-mutations]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MEM = ROOT / "host" / "hos_mem.c"
SVC = ROOT / "host" / "hos_svc.c"
HOS_H = ROOT / "host" / "hos.h"
PLATFORM = ROOT / "host" / "posix" / "ps5_platform.c"


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


def code(text):
    """the text without its comments (their lines kept)"""
    return re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)


FAKE_HOS_H = r"""
#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t x[32]; } Ctx;
#define HLE(sym) void imp_##sym(Ctx *c)
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(uintptr_t)(a))   /* (the test's guest addresses: never read or written but QueryMemoryInfo's, a host buffer) */
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
#define HOS_SYSTEM_BASE 0x01000000ULL
#define HOS_SYSTEM_END 0x07000000ULL
#define HOS_HEAP_BASE 0x1000000000ULL
#define HOS_GUEST_SPACE (1ULL << 39)
#define HOS_SPACE_END 0x8000000000ULL
#define HOS_TRACE_CALL(name) ((void)0)
#define HOS_WRITTEN(p, n) ((void)0)
extern const uint64_t hos_image_base, hos_image_end;   /* (hos_image_end: the control bridge's hos_mem_region, since the merge) */
static inline void *hos_guest_range(uint64_t a, uint64_t n) { (void)n; return GP(a); }   /* (the test's guest addresses are its own) */
void hos_log(const char *fmt, ...);
void hos_fatal(Ctx *c, const char *fmt, ...);
void hos_commit(uint64_t addr, uint64_t size);
int64_t hos_now_ns(void);
uint64_t hos_region_alloc(uint64_t size, uint64_t align);
extern uint64_t hos_heap_size, hos_heap_used, hos_total_memory, hos_program_size;
@USED@
"""

FAKE_PS5_H = r"""
#include <stdint.h>
#include <stddef.h>
uint64_t hos_ps5_guest_backed_bytes(const void *p, size_t n, uint64_t *pinned_bytes, uint64_t *free_bytes);
"""

HARNESS = r"""
#include "hos.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint64_t hos_image_base = 0x80000000ULL, hos_image_end = 0x80000000ULL;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char logs[1 << 16];
static size_t logn;
void hos_log(const char *fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&log_mu);
    const size_t n = strlen(line);
    if (logn + n + 2 < sizeof logs) memcpy(logs + logn, line, n), logn += n, logs[logn++] = '\n', logs[logn] = 0;
    pthread_mutex_unlock(&log_mu);
}
void hos_fatal(Ctx *c, const char *fmt, ...) {
    (void)c;
    fprintf(stderr, "hos_fatal: %s\n", fmt);
    abort();
}
static uint64_t commits;
void hos_commit(uint64_t addr, uint64_t size) { (void)addr, (void)size; __atomic_add_fetch(&commits, 1, __ATOMIC_RELAXED); }
int64_t hos_now_ns(void) { return 0; }
int rx_guest_imported(uint64_t lo, uint64_t hi);
int rx_guest_imported(uint64_t lo, uint64_t hi) { (void)lo, (void)hi; return 0; }
int hos_posix_guest_discard(void *p, size_t n);
int hos_posix_guest_discard(void *p, size_t n) { (void)p, (void)n; return 0; }

void hos_mem_init(uint64_t image_end);
void imp__ZN2nn2os17SetMemoryHeapSizeEm(Ctx *c);
void imp__ZN2nn2os19AllocateMemoryPagesEmm(Ctx *c);
void imp__ZN2nn2os15FreeMemoryPagesEmm(Ctx *c);
void imp__ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE(Ctx *c);
void imp__ZN2nn2os21AllocateAddressRegionEPmm(Ctx *c);
void imp__ZN2nn2os17FreeAddressRegionEm(Ctx *c);

#define MB (1ULL << 20)
#define GB (1ULL << 30)
#define PROG (96 * MB)
#define BASE (PROG + 16 * MB)
#define HEAP (6894 * MB)
#define TOTAL (9809 * MB)
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL (line %d): ", __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); failures++; } } while (0)

static uint64_t call2(void (*f)(Ctx *), uint64_t a, uint64_t b) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = a, c.x[1] = b;
    f(&c);
    return c.x[0];
}
static uint64_t pages_alloc(uint64_t a, uint64_t n) { return call2(imp__ZN2nn2os19AllocateMemoryPagesEmm, a, n); }
static uint64_t pages_free(uint64_t a, uint64_t n) { return call2(imp__ZN2nn2os15FreeMemoryPagesEmm, a, n); }
static void heap(uint64_t size) { CHECK(call2(imp__ZN2nn2os17SetMemoryHeapSizeEm, size, 0) == 0, "SetMemoryHeapSize refused"); }
static uint64_t told_total;
static uint64_t told(void) {
    uint64_t info[8] = {0};
    call2(imp__ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE, (uint64_t)(uintptr_t)info, 0);
    told_total = info[0];
    return info[1];
}
static uint64_t region(uint64_t size) {
    uint64_t out = 0;
    call2(imp__ZN2nn2os21AllocateAddressRegionEPmm, (uint64_t)(uintptr_t)&out, size);
    return out;
}
static void region_free(uint64_t a) { call2(imp__ZN2nn2os17FreeAddressRegionEm, a, 0); }
static int lines_with(const char *s) {
    int n = 0;
    pthread_mutex_lock(&log_mu);
    for (const char *p = logs; (p = strstr(p, s)) != NULL; p++) n++;
    pthread_mutex_unlock(&log_mu);
    return n;
}
static uint64_t min_u(uint64_t a, uint64_t b) { return a < b ? a : b; }
#define TOLD_IS(want) do { const uint64_t t_ = told(), w_ = (want); CHECK(t_ == w_, "told %llu KB used, want %llu KB", \
    (unsigned long long)(t_ >> 10), (unsigned long long)(w_ >> 10)); } while (0)

static void count_case(void) {
    TOLD_IS(BASE);
    CHECK(told_total == TOTAL, "a total of %llu MB told", (unsigned long long)(told_total >> 20));
    heap(HEAP);
    TOLD_IS(BASE + HEAP);
    const uint64_t a = 492 * GB;
    CHECK(pages_alloc(a, 256 * MB) == 0, "AllocateMemoryPages refused");
    TOLD_IS(BASE + HEAP + 256 * MB);
    CHECK(lines_with("mem: the game's mapped pages 256 MB (peak 256); heap 6894 MB; the console's total 9809 MB") == 1, "no line at 256 MB:\n%s", logs);
    pages_alloc(a, 256 * MB);   /* (mapped again: counted once) */
    TOLD_IS(BASE + HEAP + 256 * MB);
    pages_alloc(a + 128 * MB, 256 * MB);   /* (half over pages already mapped) */
    TOLD_IS(BASE + HEAP + 384 * MB);
    pages_alloc(a + 512 * MB + 0x10, 0x10);   /* (a part of a page is the page) */
    pages_alloc(a + 512 * MB + 0xFF0, 0x20);   /* (that page and the next) */
    TOLD_IS(BASE + HEAP + 384 * MB + 8192);
    const uint64_t across = 495 * GB - 8192;   /* (two pages in one GB of the window, two in the next) */
    pages_alloc(across, 16384);
    TOLD_IS(BASE + HEAP + 384 * MB + 8192 + 16384);
    pages_free(across, 16384);
    pages_free(across, 16384);   /* (freed twice: once) */
    TOLD_IS(BASE + HEAP + 384 * MB + 8192);
    pages_free(a + 512 * MB, 8192);
    pages_free(300 * GB, 64 * MB);   /* (nothing ever mapped in that GB) */
    TOLD_IS(BASE + HEAP + 384 * MB);
    CHECK(hos_used_memory(PROG, PROG + HEAP) == PROG + HEAP + 384 * MB, "svc GetInfo's used memory is not program + heap + pages");
    pages_free(a, 384 * MB);
    TOLD_IS(BASE + HEAP);
    CHECK(lines_with("mem: the game's mapped pages 0 MB (peak 384)") == 1, "no line back under 256 MB:\n%s", logs);
    const uint64_t b = 480 * GB;
    for (uint64_t k = 1; k <= 16; k++) {   /* (4 GB of pages: past the total from the 11th 256 MB) */
        CHECK(pages_alloc(b + (k - 1) * 256 * MB, 256 * MB) == 0, "AllocateMemoryPages refused with HOS_PAGES_LIMIT unset");
        TOLD_IS(min_u(BASE + HEAP + k * 256 * MB, TOTAL));   /* (never past the total: the game's free does not wrap) */
    }
    CHECK(hos_used_memory(PROG, PROG + HEAP) == TOTAL, "svc GetInfo's used memory past the total");
    CHECK(lines_with("mapped pages 4096 MB (peak 4096)") == 1, "no line at 4096 MB:\n%s", logs);
    for (uint64_t k = 0; k < 16; k++) pages_free(b + k * 256 * MB, 256 * MB);
    TOLD_IS(BASE + HEAP);
    CHECK(commits > 16, "AllocateMemoryPages above 4 GB no longer commits (%llu commits)", (unsigned long long)commits);
    CHECK(lines_with("refused") == 0 && lines_with("FreeAddressRegion") == 0, "a refusal or a region's line:\n%s", logs);
}

static void old_case(void) {
    heap(HEAP);
    pages_alloc(492 * GB, 512 * MB);
    TOLD_IS(BASE);
    CHECK(hos_used_memory(PROG, PROG + HEAP) == PROG + HEAP, "svc GetInfo's old used memory is not program + heap");
    CHECK(lines_with("mem: the game's mapped pages 512 MB") == 1, "the pages are no longer counted with the old report:\n%s", logs);
}

static void limit_case(void) {
    heap(HEAP);
    const uint64_t b = 480 * GB;
    for (uint64_t k = 0; k < 12; k++) {   /* (11 x 256 MB fit beside the heap: 2816 + 6894 <= 9809; not 12) */
        const uint64_t before = commits, r = pages_alloc(b + k * 256 * MB, 256 * MB);
        if (k < 11) CHECK(r == 0, "AllocateMemoryPages refused under the total (%llu MB)", (unsigned long long)(k * 256));
        else CHECK(r == 0xCA01 && commits == before, "AllocateMemoryPages past the total: %#llx, %llu commits", (unsigned long long)r,
                   (unsigned long long)(commits - before));
    }
    CHECK(lines_with("refused: 2816 MB mapped + 6894 MB heap would pass the console's 9809 MB") == 1, "the refusal's line:\n%s", logs);
    CHECK(pages_alloc(b + 256 * MB, 256 * MB) == 0, "pages already mapped refused at the limit");
    pages_free(b, 256 * MB);
    TOLD_IS(BASE + HEAP + 2560 * MB);   /* (the refused 256 MB were not counted) */
}

static void region_case(void) {
    heap(HEAP);
    const uint64_t r = region(1024 * MB);
    CHECK(r >= HOS_HEAP_BASE + HEAP, "a region at %#llx", (unsigned long long)r);
    pages_alloc(r, 512 * MB);
    TOLD_IS(BASE + HEAP + 512 * MB);
    region_free(r);
    TOLD_IS(BASE + HEAP);
    char want[160];
    snprintf(want, sizeof want, "mem: FreeAddressRegion(%#llx): 524288 KB of pages the game mapped there and did not free", (unsigned long long)r);
    CHECK(lines_with(want) == 1, "the region's line:\n%s", logs);
    pages_free(r, 512 * MB);   /* (after its region: nothing left to take out) */
    TOLD_IS(BASE + HEAP);
    const uint64_t r2 = region(256 * MB);
    pages_alloc(r2, 128 * MB);
    pages_free(r2, 128 * MB);
    region_free(r2);
    TOLD_IS(BASE + HEAP);
    CHECK(lines_with("FreeAddressRegion") == 1, "a region freed empty is said:\n%s", logs);
}

/* (thread t owns every eighth page of one span: each 64-bit word of the bits holds pages of all eight, so a count made outside
   the lock would lose some) */
enum { THREADS = 8, OWN_PAGES = 16384 };   /* (64 MB a thread: 512 MB in all, under the total beside the heap) */
static uint64_t thread_mapped[THREADS];
static void *thread_main(void *arg) {
    const int t = (int)(intptr_t)arg;
    static uint8_t mine[THREADS][OWN_PAGES];
    const uint64_t span = 400 * GB - 64 * MB;   /* (across two GB of the window: two leaves of bits) */
    uint64_t x = 0x9E3779B97F4A7C15ULL * (uint64_t)(t + 1), held = 0;
    for (int i = 0; i < 20000; i++) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        const uint64_t own = x % OWN_PAGES, page = span + (own * THREADS + (uint64_t)t) * 4096, bytes = 1 + (x >> 24) % 4096;
        const int map = (x >> 40) & 1;
        if (map) pages_alloc(page, bytes);   /* (a part of the page, or all of it: the page) */
        else pages_free(page, bytes);
        held += map && !mine[t][own] ? 4096 : 0;
        held -= !map && mine[t][own] ? 4096 : 0;
        mine[t][own] = (uint8_t)map;
        if (!(i % 64)) {
            uint64_t info[8] = {0};
            call2(imp__ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE, (uint64_t)(uintptr_t)info, 0);
            CHECK(info[1] >= BASE + HEAP && info[1] <= BASE + HEAP + THREADS * (uint64_t)OWN_PAGES * 4096, "told %llu MB meanwhile",
                  (unsigned long long)(info[1] >> 20));
            (void)hos_used_memory(PROG, PROG + HEAP);
        }
    }
    thread_mapped[t] = held;
    return NULL;
}
static void threads_case(void) {
    heap(HEAP);
    pthread_t th[THREADS];
    for (int t = 0; t < THREADS; t++) pthread_create(&th[t], NULL, thread_main, (void *)(intptr_t)t);
    uint64_t sum = 0;
    for (int t = 0; t < THREADS; t++) pthread_join(th[t], NULL), sum += thread_mapped[t];
    CHECK(sum > 0, "the threads hold nothing");
    TOLD_IS(BASE + HEAP + sum);
}

#ifdef __PROSPERO__
void hos_mem_pages_note(char *buf, size_t cap);
static uint64_t asked[8][2];
static int nasked;
uint64_t hos_ps5_guest_backed_bytes(const void *p, size_t n, uint64_t *pinned_bytes, uint64_t *free_bytes) {
    CHECK(!pinned_bytes && !free_bytes, "the note asks for what it does not say");
    if (nasked < 8) asked[nasked][0] = (uint64_t)(uintptr_t)p, asked[nasked][1] = n;
    nasked++;
    return (uint64_t)(uintptr_t)p == HOS_HEAP_BASE ? 3474 * MB : 4058 * MB;
}
static void note_case(int old) {
    char buf[300];
    hos_mem_pages_note(buf, sizeof buf);   /* (no heap, no region yet: nothing asked of the window) */
    CHECK(nasked == 0 && strstr(buf, "the game's heap 0 backed of 0, its address regions 0 backed, its mapped pages 0 (peak 0)"),
          "before the heap: %s (%d asked)", buf, nasked);
    heap(HEAP);
    hos_mem_pages_note(buf, sizeof buf);
    CHECK(nasked == 1 && asked[0][0] == HOS_HEAP_BASE && asked[0][1] == HEAP, "the heap's range asked: %d", nasked);
    const uint64_t r = region(2048 * MB);
    pages_alloc(r, 1024 * MB);
    pages_alloc(r + 1024 * MB, 512 * MB);
    pages_free(r + 1024 * MB, 512 * MB);
    nasked = 0;
    hos_mem_pages_note(buf, sizeof buf);
    CHECK(nasked == 2 && asked[1][0] == r && asked[1][1] == HOS_SPACE_END - r, "the regions' range asked: %d, %#llx + %#llx", nasked,
          (unsigned long long)asked[1][0], (unsigned long long)asked[1][1]);
    char want[200];
    if (old) snprintf(want, sizeof want, "the game's heap 3474 backed of 6894, its address regions 4058 backed, its mapped pages 1024 (peak 1536), told 112 used of 9809 (HOS_MEMINFO_OLD)");
    else snprintf(want, sizeof want, "the game's heap 3474 backed of 6894, its address regions 4058 backed, its mapped pages 1024 (peak 1536), told 8030 used of 9809");
    CHECK(!strcmp(buf, want), "the note: %s", buf);
    memset(buf, 'x', sizeof buf);
    hos_mem_pages_note(buf, 16);
    CHECK(buf[15] == 0 && buf[16] == 'x', "the note past its buffer");
}
#endif

int main(int argc, char **argv) {
    hos_mem_init(hos_image_base + PROG);
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "count")) count_case();
    else if (!strcmp(mode, "old")) old_case();
    else if (!strcmp(mode, "limit")) limit_case();
    else if (!strcmp(mode, "region")) region_case();
    else if (!strcmp(mode, "threads")) threads_case();
#ifdef __PROSPERO__
    else if (!strcmp(mode, "note")) note_case(0);
    else if (!strcmp(mode, "note-old")) note_case(1);
#endif
    else { fprintf(stderr, "no case %s\n", mode); return 2; }
    return failures ? 1 : 0;
}
"""

# (case, environment, compiled for the console): every one has to pass
CASES = [
    ("count", {}, False),
    ("count", {"HOS_MEMINFO_OLD": "0"}, False),
    ("count", {"HOS_MEMINFO_OLD": "off"}, False),
    ("count", {"HOS_MEMINFO_OLD": " No "}, False),
    ("count", {"HOS_MEMINFO_OLD": "FALSE"}, False),
    ("count", {"HOS_MEMINFO_OLD": ""}, False),
    ("count", {"HOS_PAGES_LIMIT": "0"}, False),
    ("old", {"HOS_MEMINFO_OLD": "1"}, False),
    ("old", {"HOS_MEMINFO_OLD": "yes"}, False),
    ("old", {"HOS_MEMINFO_OLD": " on "}, False),
    ("limit", {"HOS_PAGES_LIMIT": "1"}, False),
    ("region", {}, False),
    ("threads", {}, False),
    ("count", {}, True),
    ("note", {}, True),
    ("note-old", {"HOS_MEMINFO_OLD": "1"}, True),
]

# (what is changed in host/hos_mem.c, into what, the cases one of which has to fail)
MUTATIONS = [
    ("else if (map == 1 && !was) *word |= bit, changed += 0x1000;", "else if (map == 1) *word |= bit, changed += 0x1000;", "count"),
    ("else if (map == 0 && was) *word &= ~bit, changed += 0x1000;", "else if (map == 0) *word &= ~bit, changed += 0x1000;", "count"),
    ("return used < hos_total_memory ? used : hos_total_memory;", "return used;", "count"),
    ("if (meminfo_old()) return old;", "", "old"),
    ("if (old < 0) old = switch_on(\"HOS_MEMINFO_OLD\");", "if (old < 0) old = getenv(\"HOS_MEMINFO_OLD\") != NULL;", "count"),
    ("    mapped_bytes -= (uint64_t)pages_count_locked(ARG(0), ARG(1), 0);\n", "", "count"),
    ("if (limit && hos_heap_size + mapped_bytes + (uint64_t)pages_count_locked(a, n, 2) > hos_total_memory) {",
     "if (limit && hos_heap_size + mapped_bytes > hos_total_memory) {", "limit"),
    ("            left = pages_forget_locked(regions[i].addr, regions[i].size);\n", "", "region"),
]


def check_source(mem, svc, hos_h, platform):
    need("uint64_t hos_used_memory(uint64_t base, uint64_t old);" in hos_h, "host/hos.h does not declare hos_used_memory(base, old)")
    for case in ("7", "22"):
        need(re.search(r"case " + case + r": return hos_used_memory\(hos_program_size, hos_program_size \+ hos_heap_size\);", svc),
             "svc GetInfo %s does not tell hos_used_memory, with program + heap as its old figure" % case)
    need("hos_program_size + hos_heap_size;" not in code(svc), "svc GetInfo still tells program + heap somewhere")
    query = block(mem, "HLE(_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE)")
    need("G64(info + 0x08) = hos_used_memory(hos_program_size + (16ULL << 20), hos_program_size + (16ULL << 20));" in query,
         "QueryMemoryInfo's used memory is not hos_used_memory's, with program + system as its old figure")
    alloc = code(block(mem, "HLE(_ZN2nn2os19AllocateMemoryPagesEmm)"))
    need(alloc.index("mapped_bytes += (uint64_t)pages_count_locked(a, n, 1);") < alloc.index("UNLOCK();\n    }") < alloc.index("hos_commit(a, n);"),
         "AllocateMemoryPages commits before it counts, or under its lock")
    need('limit = switch_on("HOS_PAGES_LIMIT")' in alloc, "HOS_PAGES_LIMIT is not read at AllocateMemoryPages' first call")
    free = code(block(mem, "HLE(_ZN2nn2os15FreeMemoryPagesEmm)"))
    need(free.index("LOCK();") < free.index("pages_count_locked(ARG(0), ARG(1), 0)") < free.index("UNLOCK();") < free.index("hos_posix_guest_discard"),
         "FreeMemoryPages decommits before it uncounts, or under its lock")
    need('old = switch_on("HOS_MEMINFO_OLD")' in block(mem, "static int meminfo_old(void)"), "HOS_MEMINFO_OLD is not read at first use")
    used = code(block(mem, "uint64_t hos_used_memory(uint64_t base, uint64_t old)"))
    need(used.index("if (meminfo_old()) return old;") < used.index("LOCK();"), "the old report takes the lock")
    region_free = code(block(mem, "void hos_region_free(uint64_t addr)"))
    need(region_free.index("LOCK();") < region_free.index("pages_forget_locked(") < region_free.index("UNLOCK();") <
         region_free.index("discard_pages(freed, freed_size)"), "a region's pages are not taken out of the count under the lock, before its discard")
    # the console's note
    m = re.search(r"#ifdef __PROSPERO__\n/\* \(ps5, 2026-11-14\) For the memory line.*?\n#endif\n", mem, re.S)
    need(m and "void hos_mem_pages_note(char *buf, size_t cap) {" in m.group(0), "hos_mem_pages_note is not the console's alone")
    note = code(block(mem, "void hos_mem_pages_note(char *buf, size_t cap) {"))
    for bad in ("LOCK()", "alloc(", "hos_log(", "pages_count_locked("):
        need(bad not in note, "the memory line's note calls " + bad + " (it is written from inside allocations, any lock held)")
    fmt = re.search(r'snprintf\(buf, cap, ((?:"[^"]*"\s*)+),', note)
    need(fmt, "the note's format is not found")
    for word in ("stacks", "the rest", "the guest window", "the host heap"):
        need(word not in fmt.group(1), "the note says '%s', which the run summaries read on that line" % word)
    need(re.search(r"^void hos_mem_pages_note\(char \*buf, size_t cap\) __attribute__\(\(weak\)\);", platform, re.M),
         "ps5_platform.c does not declare the note weak (the platform probe links it without hos_mem.c)")
    where = code(block(platform, "void hos_ps5_memory_where(char *buf, size_t cap, uint64_t used_mb)"))
    need(where.index('snprintf(buf, cap, "the guest window') < where.index("if (hos_mem_pages_note) {") < where.index("hos_mem_pages_note(buf + at + 2, cap - at - 2);"),
         "the note is not at the end of the memory line, after its own figures")


def build(tmp, mem_text, hos_h, console, cc, name, werror=True):
    d = Path(tmp) / name
    d.mkdir()
    used = re.search(r"^uint64_t hos_used_memory\(.*?\);.*$", hos_h, re.M).group(0)
    (d / "hos.h").write_text(FAKE_HOS_H.replace("@USED@", used), encoding="utf-8")
    (d / "ps5.h").write_text(FAKE_PS5_H, encoding="utf-8")
    (d / "hos_mem.c").write_text(mem_text, encoding="utf-8")
    (d / "harness.c").write_text(HARNESS, encoding="utf-8")
    # (hos_mem.c includes ../gpu/vk/compat/longshot_env.h, the switches' one rule, since the Longshot merge: there beside the copy)
    env_h = Path(tmp) / "gpu" / "vk" / "compat"
    env_h.mkdir(parents=True, exist_ok=True)
    (env_h / "longshot_env.h").write_text((ROOT / "gpu" / "vk" / "compat" / "longshot_env.h").read_text(encoding="utf-8"), encoding="utf-8")
    exe = d / "run"
    flags = (["-D__PROSPERO__"] if console else []) + (["-Werror"] if werror else ["-w"])   # (a sabotage may leave a function unused)
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-pthread", *flags, "-I", str(d), str(d / "hos_mem.c"),
                    str(d / "harness.c"), "-o", str(exe)], check=True)
    return exe


def run(exe, case, extra):
    env = {k: v for k, v in os.environ.items()
           if k not in ("HOS_MEMINFO_OLD", "HOS_PAGES_LIMIT", "HOS_MEMORY_MB", "HOS_KEEP_FREED_PAGES", "HOS_ZERO_ALL_PAGES", "HOS_PREFAULT")}
    env.update(extra)
    return subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=120)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    p.add_argument("--no-mutations", action="store_true")
    a = p.parse_args()
    mem, hos_h = MEM.read_text(encoding="utf-8"), HOS_H.read_text(encoding="utf-8")
    check_source(mem, SVC.read_text(encoding="utf-8"), hos_h, PLATFORM.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="meminfo-pages-") as tmp:
        exes = {False: build(tmp, mem, hos_h, False, a.cc, "pc"), True: build(tmp, mem, hos_h, True, a.cc, "console")}
        for case, env, console in CASES:
            r = run(exes[console], case, env)
            need(r.returncode == 0, "case %s %s (%s): %s%s" % (case, env, "console" if console else "pc", r.stdout, r.stderr))
        print("%d cases passed" % len(CASES))
        if a.no_mutations:
            return
        for i, (old, new, case) in enumerate(MUTATIONS):
            need(mem.count(old) == 1, "a sabotage's text is not once in host/hos_mem.c: " + old.strip())
            exe = build(tmp, mem.replace(old, new), hos_h, False, a.cc, "mutation-%d" % i, werror=False)
            caught = any(run(exe, c, e).returncode != 0 for c, e, console in CASES if c == case and not console)
            need(caught, "a sabotage of host/hos_mem.c went through the %s cases: %s -> %s" % (case, old.strip(), new.strip() or "(nothing)"))
        print("%d sabotages caught" % len(MUTATIONS))


if __name__ == "__main__":
    main()
