/* (ps5) host/posix/ps5.h on the payload SDK fork's platform layer: reservations and stacks, threads, futexes, the
   linked-in "modules", paths and /data, and the shell's exit. The guest window is host/posix/ps5_window.c, open files
   host/posix/ps5_files.c, the storage broker host/posix/ps5_broker_client.c. Built with RX_TARGET=ps5 only.

   Phase 3 (2026-11-09) compiled and archived it; branch ps5-plat (phase 4) adds the futex fallback, /data and the platform
   probe title PPSA99147 (scripts/ps5/plat-probe), which runs each part on the console. What each part rests on is the
   platform layer's measurements (PS5_PayloadSDK platform/docs/PROBE.md) and PS5_VulkanTemplate's ps5/src/platform.c; what
   is not measured yet is said where it is used, and listed in docs/ps5-port.md ("Census"). */
#ifdef __PROSPERO__
#include "ps5.h"
#define HOS_TSC_CLOCK_START   /* (hos_ps5_tsc_clock_start, below) */
#include "tsc_clock.h"
#include "ps5_data_folder.h"   /* (lot 80: /data/grapnel, and /data/hookshot taken over) */
#ifndef LONGSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../../gpu/vk/compat/longshot_env.h"
#endif

#include <ps5platform/elevation.h>
#include <ps5platform/fp.h>
#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>
#include <ps5platform/libc.h>
#include <ps5platform/shm.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/cpuset.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/umtx.h>
#include <unistd.h>

int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceKernelUsleep(uint32_t microseconds);
int32_t scePthreadGetaffinity(pthread_t thread, uint64_t *mask);

/* ---- the guest window: host/posix/ps5_window.c ---- */
#define UNIT ((uint64_t)1 << 16)

/* ---- other reservations: placed in direct memory, outside the GPU window (the platform's vrange) ---- */
/* (2026-11-10: where the direct memory goes, for the memory line: the scene's first frames ran it out with 4.6 GiB taken
   outside the guest window. Bytes this file backed: reservations made backed, commits asked later, the stacks' tops;
   releases are counted, their bytes are not known here) */
static uint64_t acct_reserved_backed, acct_commits, acct_stacks, acct_stack_n, acct_releases;
void *hos_ps5_reserve(size_t size, int commit) {
    void *base = NULL;
    size = (size + UNIT - 1) & ~(size_t)(UNIT - 1);
    if (ps5_vrange_reserve(size, NULL, UNIT, &base)) return NULL;
    if (commit && ps5_vrange_commit(base, size, PS5_SHM_READ | PS5_SHM_WRITE)) {
        ps5_vrange_release(base, size);
        return NULL;
    }
    if (commit) __atomic_add_fetch(&acct_reserved_backed, size, __ATOMIC_RELAXED);
    return base;
}
int hos_ps5_commit(void *p, size_t n) {
    const int r = ps5_vrange_commit(p, n, PS5_SHM_READ | PS5_SHM_WRITE);
    if (!r) __atomic_add_fetch(&acct_commits, n, __ATOMIC_RELAXED);
    return r;
}
/* (2026-11-14) host/hos_mem.c: of the window, the game's heap and its address regions, beside the pages it mapped and the used
   memory it is told. Weak: a probe that links this file without host/hos_mem.c leaves the note out */
void hos_mem_pages_note(char *buf, size_t cap) __attribute__((weak));
/* " window W, heap H (peak P, N libc fallbacks), stacks S (n), other reservations R + C committed later (n released), the
   write-watch table 128; the rest X (the driver, the system's own); the game's heap B backed of H, its address regions R
   backed, its mapped pages M (peak P), told U used of T" in MiB, for a line that knows the total in use */
void hos_ps5_memory_where(char *buf, size_t cap, uint64_t used_mb) {
    struct hos_ps5_guest_stats w;
    struct ps5_heap_stats h;
    memset(&w, 0, sizeof w), memset(&h, 0, sizeof h);
    hos_ps5_guest_stats(&w);
    ps5_heap_stats(&h);
    const uint64_t win = w.backed_bytes >> 20, heap = h.mapped_bytes >> 20, st = acct_stacks >> 20, rb = acct_reserved_backed >> 20,
                   cm = acct_commits >> 20;
    uint64_t table_refused = 0;
    const uint64_t table_kb = hos_ps5_dirty_bytes(&table_refused) >> 10;
    const uint64_t known = win + heap + st + rb + cm + (table_kb >> 10);
    snprintf(buf, cap, "the guest window %llu, the host heap %llu (peak %llu, %llu libc fallbacks), stacks %llu (%llu of them), other "
                       "reservations %llu backed + %llu committed later (%llu releases), the write-watch table %llu.%llu (%llu parts refused); the rest %lld (the "
                       "driver's memory, thread stacks made by pthread_create, the system's own)",
             (unsigned long long)win, (unsigned long long)heap, (unsigned long long)(h.peak_bytes >> 20), h.libc_fallbacks,
             (unsigned long long)st, (unsigned long long)acct_stack_n, (unsigned long long)rb, (unsigned long long)cm,
             (unsigned long long)acct_releases, (unsigned long long)(table_kb >> 10), (unsigned long long)((table_kb & 1023) * 10 / 1024),
             (unsigned long long)table_refused, (long long)used_mb - (long long)known);
    if (hos_mem_pages_note) {   /* (at the end: the run summaries on the PC read the figures above by their place) */
        const size_t at = strlen(buf);
        if (at + 3 < cap) {
            buf[at] = ';', buf[at + 1] = ' ', buf[at + 2] = 0;
            hos_mem_pages_note(buf + at + 2, cap - at - 2);
        }
    }
}
/* (2026-11-10) The title heap's large blocks. An allocation of 32 MiB and more is a segment of direct memory of its own
   (PS5_PayloadSDK's platform/src/heap.c, dlmalloc's DIRECT_MMAP), mapped at once: 275 MiB went between two looks of the memory
   watch and the title stopped with 235 MiB free where its floor is 512. The link wraps the heap's map call
   (--wrap=ps5p_heap_map, nx2/toolchain_ps5.py): a block of that size is written in the trail with who asked (the stack's words
   that point into the eboot, to read against link-game/llvm-pie.map), and is not made when it would take the free direct memory
   under the floor: the title stops there, before the memory is gone. No heap call from here: the caller holds its arena's lock. */
/* The same floor under every allocation of direct memory of 16 MiB and more, whoever asks (the heap above, the Vulkan
   driver's device memory: an image of 288 MB is one allocation, the guest window's are 64 KiB): the link wraps the kernel's
   call (--wrap=sceKernelAllocateDirectMemory). What is left to the watch's 50 ms is then what small allocations take
   meanwhile, about 70 MiB at the fastest load seen. */
int32_t __real_sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment, int32_t type,
                                             int64_t *start_out);
int32_t __real_sceKernelReleaseDirectMemory(int64_t start, size_t length);

/* ---- (2026-11-10) the direct memory by who asked for it ----
   Every allocation and release of direct memory goes through the two wraps here, so each 64 KiB unit can carry the site that
   allocated it: the first four words of the caller's stack that point into the eboot (its return addresses, to read against
   link-game/llvm-pie.map with tools/trail-symbols.py). The memory watch writes the largest sites every 30 s and at a stop:
   what the driver holds outside VMA's blocks, about 500 MiB, had no name. The title's heap is one site here; its growth has a
   table of its own (heap_site, by the words past the allocator's frames). Tables in .bss, a spin lock, no heap call. */
#define ACC_UNITS ((size_t)1 << 18)   /* 16 GiB of direct memory, 64 KiB each */
#define ACC_SITES 250   /* (a unit's site is a byte; 64 at first, which one run filled: below) */
#define ACC_DEPTH 4
struct acc_site { uintptr_t a[ACC_DEPTH]; uint64_t units, peak, allocs, frees; };
static uint8_t acc_unit_site[ACC_UNITS];   /* the unit's site + 1; 0: not counted */
static struct acc_site acc_sites[ACC_SITES], heap_sites[ACC_SITES];
static unsigned acc_sites_n, heap_sites_n;
static int acc_lock;
static void acc_take(void) { while (__atomic_exchange_n(&acc_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void acc_give(void) { __atomic_store_n(&acc_lock, 0, __ATOMIC_RELEASE); }
/* the stack's words into the eboot from a frame, `skip` of them left out, ACC_DEPTH kept.
   (2026-11-10, after the first run: only words into the host's and the driver's code. Any word between 4 MiB and the image's end
   counted before, and a size is such a word (16 MiB, 0x1000000: the lifted game's code starts at 4 MiB): one caller with the
   sizes beside it on the stack made a site of each, the table of 64 was full at once and 10.8 GiB of 11.5 fell in "the rest".
   The link places the host and the driver after the game's code, to the end of .text: from 1 MiB before this function to the
   unwind table's header that follows .text (PS5_Vulkan's script provides the symbol). The code itself is not read, so a
   function's address kept on the stack still counts as a word.) */
extern const char __eh_frame_hdr_start[];
int hos_ps5_guest_fault2(const void *p, int write);   /* (ps5_window.c) */
static void acc_words(const void *frame, int skip, uintptr_t out[ACC_DEPTH]) {
    const uintptr_t here = (uintptr_t)&acc_words, lo = here - (1u << 20), hi = (uintptr_t)__eh_frame_hdr_start;
    const uintptr_t sp = (uintptr_t)frame;
    int n = 0;
    for (int i = 0; i < ACC_DEPTH; i++) out[i] = 0;
    if (!sp || (sp & 7)) return;
    for (int i = 1; i < 1024 && n < ACC_DEPTH + skip; i++) {
        if (((sp + (uintptr_t)i * 8) >> 16) != (sp >> 16)) break;   /* (to the end of the stack's 64 KiB unit, no further) */
        const uintptr_t v = ((const uintptr_t *)sp)[i];
        if (v > lo && v < hi && n++ >= skip) out[n - 1 - skip] = v;
        /* (2026-11-10: nothing past the window's fault handler. What lies above it on the stack is whatever the faulting
           thread was running, a different word each time: the faults made a site of each and filled the 250 before the
           driver's first allocation, whose 4 GiB then fell in "the rest") */
        if (v - (uintptr_t)&hos_ps5_guest_fault2 < 0x1000) break;
    }
}
/* the site of those words in a table (the lock held): found, made, or the last one when the table is full */
static struct acc_site *acc_site_of(struct acc_site *table, unsigned *count, const uintptr_t a[ACC_DEPTH]) {
    for (unsigned i = 0; i < *count; i++)
        if (!memcmp(table[i].a, a, sizeof table[i].a)) return &table[i];
    if (*count < ACC_SITES - 1) {
        struct acc_site *s = &table[(*count)++];
        memcpy(s->a, a, sizeof s->a);
        return s;
    }
    if (*count < ACC_SITES) (*count)++;   /* (the rest together: its words stay 0) */
    return &table[ACC_SITES - 1];
}
/* "<MiB> MiB (peak <MiB>, <n> allocations, <n> releases): <words>; ..." for the sites of rank first to first + count - 1, by size now */
static void acc_say(const struct acc_site *table, unsigned total, char *buf, size_t cap, unsigned first, unsigned count) {
    uint8_t order[ACC_SITES];
    unsigned n = total < ACC_SITES ? total : ACC_SITES;
    for (unsigned i = 0; i < n; i++) order[i] = (uint8_t)i;
    for (unsigned i = 0; i < n; i++)   /* (64 at most) */
        for (unsigned j = i + 1; j < n; j++)
            if (table[order[j]].units > table[order[i]].units) { const uint8_t t = order[i]; order[i] = order[j]; order[j] = t; }
    size_t at = 0;
    buf[0] = 0;
    for (unsigned i = first; i < n && i < first + count && at + 160 < cap; i++) {
        const struct acc_site *s = &table[order[i]];
        at += (size_t)snprintf(buf + at, cap - at, "%s%llu MiB (peak %llu, %llu allocations, %llu releases): %#lx %#lx %#lx %#lx",
                               i > first ? "; " : "", (unsigned long long)(s->units >> 4), (unsigned long long)(s->peak >> 4),
                               (unsigned long long)s->allocs, (unsigned long long)s->frees, (unsigned long)s->a[0],
                               (unsigned long)s->a[1], (unsigned long)s->a[2], (unsigned long)s->a[3]);
    }
}
void hos_ps5_direct_sites(char *buf, size_t cap, unsigned first, unsigned count) {
    acc_take();
    acc_say(acc_sites, acc_sites_n, buf, cap, first, count);
    acc_give();
}
void hos_ps5_heap_sites(char *buf, size_t cap, unsigned first, unsigned count) {
    acc_take();
    acc_say(heap_sites, heap_sites_n, buf, cap, first, count);
    acc_give();
}
uintptr_t hos_ps5_sites_image(void) { return (uintptr_t)&acc_words; }   /* (the function the lines' addresses are read against) */

/* (2026-11-11) a release, counted: the wrap's own work, and a piece of a block of the heap's made of pieces (ps5_heap_pieces.c) */
static int32_t release_counted(int64_t start, size_t length) {
    const int32_t r = __real_sceKernelReleaseDirectMemory(start, length);
    if (r == 0) __atomic_fetch_add(&hos_ps5_free_estimate, (int64_t)length, __ATOMIC_RELAXED);
    if (r == 0 && start >= 0) {
        const size_t lo = (size_t)start >> 16, n = (length + 0xFFFF) >> 16;
        acc_take();
        for (size_t u = lo; u < lo + n && u < ACC_UNITS; u++) {
            const uint8_t s = acc_unit_site[u];
            if (!s) continue;
            if (acc_sites[s - 1].units) acc_sites[s - 1].units--;
            if (u == lo) acc_sites[s - 1].frees++;
            acc_unit_site[u] = 0;
        }
        acc_give();
    }
    return r;
}
int32_t __wrap_sceKernelReleaseDirectMemory(int64_t start, size_t length) {
    /* (2026-11-11) a block of the heap's made of pieces: its pieces go back (ps5_heap_pieces.c); any other start as before */
    if (hos_heap_pieces_is(start)) return hos_heap_pieces_release(start, length);
    return release_counted(start, length);
}
int32_t hos_ps5_piece_give(int64_t start, size_t length) { return release_counted(start, length); }
/* ---- (2026-11-10) where an allocation of direct memory goes ----
   Direct memory is allocated in one piece, and the title's is one range. With an image in a memory object of its own
   (RX_VK_IMAGE_DEDICATED_KB), thousands of allocations of every size came and went among the window's 64 KiB units, which stay:
   after 13 minutes of play 575 MiB were free in 682 pieces, the largest 7 MiB. The heap grows by 16 MiB: it found no piece,
   fell back on libc 17,182 times, and the title ended on std::bad_alloc. Images of 4.7 MiB were not made from 664 s on.
   So the two kinds are kept apart. An allocation of HOOKSHOT_PS5_DIRECT_LARGE_KB KiB and more (256 by default; 0: all as
   before, the kernel's choice) is looked for from the top of the range, in the last 512 MiB, then the last 1024, and so on down
   to the whole range. A smaller one is looked for below the lowest address a large one was ever given, and anywhere only when
   nothing is free there. The small ones, which the kernel places from the bottom up, no longer land in what a large one
   leaves free. Every caller asks for the whole range (0 to the direct memory's size): any other range is passed as it is. */
static uint8_t acc_unit_large[ACC_UNITS >> 3];   /* a bit per unit: of a large allocation (the lock held) */
static int64_t direct_large_floor = INT64_MAX;   /* the lowest start a large allocation was given */
static uint64_t direct_large_tries, direct_large_n, direct_small_spills;
static int32_t direct_place(int64_t search_start, int64_t search_end, size_t length, size_t alignment, int32_t type, int64_t *start_out,
                            int *large) {
    *large = 0;
    if (search_start != 0 || search_end < ((int64_t)1 << 30))
        return __real_sceKernelAllocateDirectMemory(search_start, search_end, length, alignment, type, start_out);
    if (length >= ((size_t)64 << 10) + 1) {   /* (above the window's unit: the setting is read, which the window's faults need not) */
        const char *e = getenv("HOOKSHOT_PS5_DIRECT_LARGE_KB");
        const size_t from = (size_t)(e ? strtoull(e, NULL, 0) : 256) << 10;
        if (from && length >= from) {
            const int64_t band = (int64_t)512 << 20;
            int32_t r = -1;
            *large = 1;
            __atomic_fetch_add(&direct_large_n, 1, __ATOMIC_RELAXED);
            for (int64_t lo = (search_end - band) & ~(((int64_t)2 << 20) - 1);; lo -= band) {
                if (lo < 0) lo = 0;
                __atomic_fetch_add(&direct_large_tries, 1, __ATOMIC_RELAXED);
                r = __real_sceKernelAllocateDirectMemory(lo, search_end, length, alignment, type, start_out);
                if (r == 0 || lo == 0) break;
            }
            if (r == 0 && start_out) {
                int64_t was = __atomic_load_n(&direct_large_floor, __ATOMIC_RELAXED);
                while (*start_out < was &&
                       !__atomic_compare_exchange_n(&direct_large_floor, &was, *start_out, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
            }
            return r;
        }
    }
    const int64_t below = __atomic_load_n(&direct_large_floor, __ATOMIC_RELAXED);
    if (below > 0 && below < search_end) {
        const int32_t r = __real_sceKernelAllocateDirectMemory(0, below, length, alignment, type, start_out);
        if (r == 0) return 0;
        __atomic_fetch_add(&direct_small_spills, 1, __ATOMIC_RELAXED);
    }
    return __real_sceKernelAllocateDirectMemory(search_start, search_end, length, alignment, type, start_out);
}
/* "<GiB>: <small MiB>/<large MiB>; ..." by GiB of the direct memory, of what the wraps saw allocated; and the large ones' tries */
void hos_ps5_direct_layout(char *buf, size_t cap) {
    size_t at = 0;
    if (cap) buf[0] = 0;
    acc_take();
    for (size_t g = 0; g < ACC_UNITS >> 14 && at + 40 < cap; g++) {
        unsigned small = 0, big = 0;
        for (size_t u = g << 14; u < (g + 1) << 14; u++)
            if (acc_unit_site[u]) { if (acc_unit_large[u >> 3] >> (u & 7) & 1) big++; else small++; }
        if (small || big) at += (size_t)snprintf(buf + at, cap - at, "%s%zu: %u/%u", at ? "; " : "", g, small >> 4, big >> 4);
    }
    acc_give();
    const int64_t floor = __atomic_load_n(&direct_large_floor, __ATOMIC_RELAXED);
    if (at + 200 < cap)
        snprintf(buf + at, cap - at, "; large ones: %llu, %llu tries, the lowest at %lld MiB; small ones placed above that: %llu",
                 (unsigned long long)__atomic_load_n(&direct_large_n, __ATOMIC_RELAXED),
                 (unsigned long long)__atomic_load_n(&direct_large_tries, __ATOMIC_RELAXED),
                 floor == INT64_MAX ? -1LL : (long long)(floor >> 20),
                 (unsigned long long)__atomic_load_n(&direct_small_spills, __ATOMIC_RELAXED));
}
/* an allocation placed and counted (by site, in the free estimate): the wrap's own work, and a piece of a block of the heap's
   made of pieces (ps5_heap_pieces.c). Inlined into its two callers, whose frame the site's words are read from, as before */
static inline __attribute__((always_inline)) int32_t direct_counted(int64_t search_start, int64_t search_end, size_t length,
                                                                    size_t alignment, int32_t type, int64_t *start_out) {
    int large = 0;
    const int32_t r = direct_place(search_start, search_end, length, alignment, type, start_out, &large);
    if (r == 0) __atomic_fetch_sub(&hos_ps5_free_estimate, (int64_t)length, __ATOMIC_RELAXED);
    if (r == 0 && start_out && *start_out >= 0) {   /* (whose it is: above) */
        uintptr_t a[ACC_DEPTH];
        acc_words(__builtin_frame_address(0), 0, a);
        const size_t lo = (size_t)*start_out >> 16, n = (length + 0xFFFF) >> 16;
        acc_take();
        struct acc_site *s = acc_site_of(acc_sites, &acc_sites_n, a);
        const uint8_t id = (uint8_t)(s - acc_sites + 1);
        for (size_t u = lo; u < lo + n && u < ACC_UNITS; u++) {
            acc_unit_site[u] = id;
            if (large) acc_unit_large[u >> 3] |= (uint8_t)(1u << (u & 7));
            else acc_unit_large[u >> 3] &= (uint8_t)~(1u << (u & 7));
        }
        s->units += n, s->allocs++;
        if (s->units > s->peak) s->peak = s->units;
        acc_give();
    }
    return r;
}
int32_t hos_ps5_piece_take(int64_t search_end, size_t length, size_t alignment, int32_t type, int64_t *start) {
    return direct_counted(0, search_end, length, alignment, type, start);
}
int32_t __wrap_sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment, int32_t type,
                                             int64_t *start_out) {
    /* (2026-11-10) one of 16 MiB and more is measured against the floor before it is made; and one of 1 MiB and more when the
       free memory as last measured, less what was allocated since, comes within 64 MiB of that: an image has a memory object
       of its own now (RX_VK_IMAGE_DEDICATED_KB), most of them under 16 MiB, where the 16 MiB blocks were each measured */
    if (length >= ((size_t)1 << 20)) {
        const char *e = getenv("HOOKSHOT_PS5_MEMORY_FLOOR_MB");
        const uint64_t floor_mb = e ? strtoull(e, NULL, 0) : 768, want_mb = (length + (1u << 20) - 1) >> 20;
        uint64_t direct = 0, flexible = 0, largest = 0;
        const int64_t est = __atomic_load_n(&hos_ps5_free_estimate, __ATOMIC_RELAXED);
        const int measure = length >= ((size_t)16 << 20) || est < (int64_t)((floor_mb + want_mb + 64) << 20);
        if (measure && floor_mb && hos_ps5_memory_free(&direct, &flexible, &largest) && direct < floor_mb + want_mb) {
            hos_ps5_trail("memory: STOPPING THE TITLE: an allocation of %llu MiB of direct memory (asked from %p, thread %d) would leave "
                          "%lld MiB free (floor %llu, %llu free now): the title ends itself before the console runs out "
                          "(HOOKSHOT_PS5_MEMORY_FLOOR_MB)",
                          (unsigned long long)want_mb, __builtin_return_address(0), hos_ps5_tid(), (long long)direct - (long long)want_mb,
                          (unsigned long long)floor_mb, (unsigned long long)direct);
            char wh[600];
            hos_ps5_memory_where(wh, sizeof wh, hos_ps5_memory_total_mb() > direct ? hos_ps5_memory_total_mb() - direct : 0);
            hos_ps5_trail("memory: at the stop, %llu MiB in use: %s", (unsigned long long)(hos_ps5_memory_total_mb() - direct), wh);
            abort();
        }
    }
    const int32_t r = direct_counted(search_start, search_end, length, alignment, type, start_out);
    /* (2026-11-11) refused, and asked by the heap's map call: made of pieces when HOOKSHOT_PS5_HEAP_PIECES_KB allows it */
    return r == 0 ? 0 : hos_heap_pieces_after_refusal(r, search_start, search_end, length, alignment, type, start_out);
}
/* ---- (2026-11-11) the heap's blocks made of pieces (host/posix/ps5_heap_pieces.c): the calls it makes ----
   The heap maps a block's start with sceKernelMapDirectMemory (and nothing else): a start of ps5_heap_pieces.c's is mapped
   piece by piece there; any other goes to the kernel as before (the window's units, the driver's memory: one compare more) */
int32_t __real_sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags, int64_t start, size_t alignment);
int32_t __wrap_sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags, int64_t start, size_t alignment) {
    if (hos_heap_pieces_is(start)) return hos_heap_pieces_map(address, length, protection, flags, start, alignment);
    return __real_sceKernelMapDirectMemory(address, length, protection, flags, start, alignment);
}
int32_t hos_ps5_piece_map(void **address, size_t length, int protection, int flags, int64_t start, size_t alignment) {
    return __real_sceKernelMapDirectMemory(address, length, protection, flags, start, alignment);
}
void hos_ps5_piece_unmap(void *address, size_t length, int reserve) {
    sceKernelMunmap(address, length);
    if (reserve) {   /* (as the heap's unmap does: the range stays its own) */
        void *at = address;
        sceKernelReserveVirtualRange(&at, length, PS5_KERNEL_MAP_FIXED, PS5_KERNEL_PAGE_SIZE);
    }
}
uint64_t hos_ps5_piece_largest(void) {
    /* (the largest free piece: one question, as ps5_memory_query asks first; 0x8002000c: none free) */
    const int64_t total = sceKernelGetDirectMemorySize();
    int64_t start = 0;
    size_t bytes = 0;
    if (total <= 0 || sceKernelAvailableDirectMemorySize(0, total, (size_t)1 << 16, &start, &bytes) != 0) return 0;
    return bytes;
}
/* (2026-11-11) the heap's arenas, for hos_heap_trim: the link wraps dlmalloc's create_mspace (--wrap=create_mspace, from
   heap.c's make_arena) */
void *__real_create_mspace(size_t capacity, int locked);
void *__wrap_create_mspace(size_t capacity, int locked) {
    void *const m = __real_create_mspace(capacity, locked);
    hos_heap_arena_note(m);
    return m;
}
/* (2026-11-11) the heap's refusals: a block its map call could not have (in one piece, nor of pieces when they are allowed).
   The heap then falls back on libc, and an allocation that libc cannot make either is a std::bad_alloc: the last refusal is
   what the stop line below says. No heap call here (the caller holds its arena's lock); the free memory walked for the
   first 40 only */
static struct { uint64_t n, bytes, at_ms, free_mb, largest_mb; int tid; } heap_refusal;
static void heap_refusal_note(size_t bytes) {
    const uint64_t n = __atomic_add_fetch(&heap_refusal.n, 1, __ATOMIC_RELAXED);
    heap_refusal.bytes = bytes, heap_refusal.tid = hos_ps5_tid(), heap_refusal.at_ms = (uint64_t)(hos_ps5_since_start() * 1000.0);
    if (n > 40 && n % 100) return;
    uint64_t direct = 0, flexible = 0, largest = 0;
    if (hos_ps5_memory_free(&direct, &flexible, &largest)) heap_refusal.free_mb = direct, heap_refusal.largest_mb = largest;
    struct ps5_heap_stats h;
    memset(&h, 0, sizeof h);
    ps5_heap_stats(&h);
    hos_ps5_trail("heap: a block of %llu KiB refused on thread %d (refusal %llu): direct %llu MiB free, largest piece %llu MiB; the heap "
                  "%llu MiB in %u segments, %llu libc fallbacks; the allocation falls back on libc%s",
                  (unsigned long long)(bytes >> 10), heap_refusal.tid, (unsigned long long)n, (unsigned long long)direct,
                  (unsigned long long)largest, (unsigned long long)(h.mapped_bytes >> 20), h.segments, h.libc_fallbacks,
                  hos_heap_pieces_min() ? "" : " (HOOKSHOT_PS5_HEAP_PIECES_KB is off)");
}
/* (2026-11-11) the stop line for an allocation nobody caught (gpu/vk/vk_vma.cpp's terminate handler, on a std::bad_alloc): the
   floor guard's words, with what the heap was last refused and the memory now; HOOKSHOT_PS5_HEAP_STOP=0: not written */
void hos_ps5_heap_stop_line(const char *what) {
    const char *s = getenv("HOOKSHOT_PS5_HEAP_STOP");
    if (s && *s && !atoi(s)) return;
    const char *e = getenv("HOOKSHOT_PS5_MEMORY_FLOOR_MB");
    const uint64_t floor_mb = e ? strtoull(e, NULL, 0) : 768;
    uint64_t direct = 0, flexible = 0, largest = 0;
    const int known = hos_ps5_memory_free(&direct, &flexible, &largest);
    struct ps5_heap_stats h;
    memset(&h, 0, sizeof h);
    ps5_heap_stats(&h);
    struct hos_heap_pieces_stats p;
    hos_heap_pieces_stats(&p);
    const uint64_t n = __atomic_load_n(&heap_refusal.n, __ATOMIC_RELAXED);
    const double ago = n ? hos_ps5_since_start() - heap_refusal.at_ms / 1000.0 : 0;
    hos_ps5_trail("memory: STOPPING THE TITLE: an allocation the host heap could not make was not caught (%s, thread %d): the heap's "
                  "last refused block %llu KiB (thread %d, %.1f s before, %llu refusals), direct %llu MiB free%s (largest piece %llu, "
                  "floor %llu), flexible %llu MiB free; the heap %llu MiB (peak %llu, %llu libc fallbacks), %llu blocks of pieces "
                  "(%llu MiB); the title ends itself (HOOKSHOT_PS5_HEAP_STOP)",
                  what ? what : "?", hos_ps5_tid(), (unsigned long long)(heap_refusal.bytes >> 10), heap_refusal.tid, ago,
                  (unsigned long long)n, (unsigned long long)direct, known ? "" : " (no answer)", (unsigned long long)largest,
                  (unsigned long long)floor_mb, (unsigned long long)flexible, (unsigned long long)(h.mapped_bytes >> 20),
                  (unsigned long long)(h.peak_bytes >> 20), h.libc_fallbacks, (unsigned long long)p.blocks,
                  (unsigned long long)(p.bytes >> 20));
    char wh[600];
    hos_ps5_memory_where(wh, sizeof wh, hos_ps5_memory_total_mb() > direct ? hos_ps5_memory_total_mb() - direct : 0);
    hos_ps5_trail("memory: at the stop, %llu MiB in use: %s", (unsigned long long)(hos_ps5_memory_total_mb() - direct), wh);
}
/* (2026-11-11) "<n> now (<MiB> in <n> pieces) ..." for the memory watch's line, or "" when no block was ever made of pieces
   and the heap was never refused */
void hos_ps5_heap_pieces_line(char *buf, size_t cap) {
    struct hos_heap_pieces_stats p;
    hos_heap_pieces_stats(&p);
    const uint64_t n = __atomic_load_n(&heap_refusal.n, __ATOMIC_RELAXED);
    if (cap) buf[0] = 0;
    if (!p.made && !p.refused && !p.full && !p.map_failed && !p.bad && !n) return;
    snprintf(buf, cap, "%llu now (%llu MiB in %llu pieces), %llu made (%llu pieces, the smallest %llu KiB), %llu given back; not made: "
                       "%llu (no pieces large enough), %llu (no slot), %llu (a piece not mapped), %llu calls refused; the heap refused "
                       "%llu blocks (the last %llu KiB)",
             (unsigned long long)p.blocks, (unsigned long long)(p.bytes >> 20), (unsigned long long)p.pieces, (unsigned long long)p.made,
             (unsigned long long)p.pieces_made, (unsigned long long)p.smallest_kib, (unsigned long long)p.given_back,
             (unsigned long long)p.refused, (unsigned long long)p.full, (unsigned long long)p.map_failed, (unsigned long long)p.bad,
             (unsigned long long)n, (unsigned long long)(heap_refusal.bytes >> 10));
}
/* (2026-11-11) dlmalloc's trim of every arena (ps5_heap_pieces.c), for the memory watch: the bytes the heap holds less after
   it, the arenas that gave something back, the heap's bytes after */
uint64_t hos_ps5_heap_trim_now(unsigned *arenas, uint64_t *after) {
    struct ps5_heap_stats h;
    memset(&h, 0, sizeof h);
    ps5_heap_stats(&h);
    const uint64_t before = h.mapped_bytes;
    const unsigned gave = hos_heap_trim();
    memset(&h, 0, sizeof h);
    ps5_heap_stats(&h);
    if (arenas) *arenas = gave;
    if (after) *after = h.mapped_bytes;
    return before > h.mapped_bytes ? before - h.mapped_bytes : 0;
}
void *__real_ps5p_heap_map(size_t bytes);
static inline __attribute__((always_inline)) void *heap_map_counted(size_t bytes);
/* (2026-11-15) a segment mapped by a thread inside the driver's compile (ps5_compile_heap.c, HOOKSHOT_PS5_COMPILE_HEAP=1) gets
   64 KiB of guard each side: dlmalloc is given the middle, never next to another segment, and its return of exactly that is
   handed to the heap as the whole block (__wrap_ps5p_heap_unmap). Everything else as before */
void *__wrap_ps5p_heap_map(size_t bytes) {
    int slot = -1;
    const size_t guard = hos_compile_heap_guard_take(&slot);
    return hos_compile_heap_guard_set(slot, heap_map_counted(bytes + 2 * guard), bytes);
}
int __real_ps5p_heap_unmap(void *address, size_t bytes);
int __wrap_ps5p_heap_unmap(void *address, size_t bytes) {
    void *at = address;
    size_t length = bytes;
    const int slot = hos_compile_heap_guard_find(address, bytes, &at, &length);
    const int r = __real_ps5p_heap_unmap(at, length);
    if (r == 0) hos_compile_heap_guard_forget(slot);
    return r;
}
/* (inlined into the wrap: the growth by site below skips the allocator's own two frames, as before) */
static inline __attribute__((always_inline)) void *heap_map_counted(size_t bytes) {
    if (bytes >= ((size_t)32 << 20)) {
        const char *e = getenv("HOOKSHOT_PS5_MEMORY_FLOOR_MB");
        const uint64_t floor_mb = e ? strtoull(e, NULL, 0) : 768, want_mb = (bytes + (1u << 20) - 1) >> 20;
        uint64_t direct = 0, flexible = 0, largest = 0;
        const int known = hos_ps5_memory_free(&direct, &flexible, &largest);
        static volatile int told;
        const int stop = known && floor_mb && direct < floor_mb + want_mb;
        if (stop || __atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 120) {
            char stack[500];
            int n = 0;
            stack[0] = 0;
            const uintptr_t here = (uintptr_t)&__wrap_ps5p_heap_map, lo = 4u << 20, hi = here + (64u << 20);
            const uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
            if (sp && !(sp & 7))
                for (int i = 0; i < 2048 && n < 440; i++) {
                    if (((sp + (uintptr_t)i * 8) >> 16) != (sp >> 16)) break;   /* (to the end of the stack's 64 KiB unit, no further) */
                    const uintptr_t v = ((const uintptr_t *)sp)[i];
                    if (v > lo && v < hi) n += snprintf(stack + n, sizeof stack - (size_t)n, " %#lx", (unsigned long)v);
                }
            hos_ps5_trail("heap: a block of %llu MiB asked on thread %d with %llu MiB of direct memory free (largest piece %llu); image: "
                          "__wrap_ps5p_heap_map at %#lx; the stack's words into the eboot:%s",
                          (unsigned long long)want_mb, hos_ps5_tid(), (unsigned long long)direct, (unsigned long long)largest,
                          (unsigned long)here, stack);
        }
        if (stop) {
            hos_ps5_trail("memory: STOPPING THE TITLE: a heap block of %llu MiB would leave %lld MiB of direct memory free (floor %llu): "
                          "the title ends itself before the console runs out (HOOKSHOT_PS5_MEMORY_FLOOR_MB)",
                          (unsigned long long)want_mb, (long long)direct - (long long)want_mb, (unsigned long long)floor_mb);
            char wh[600];
            hos_ps5_memory_where(wh, sizeof wh, hos_ps5_memory_total_mb() > direct ? hos_ps5_memory_total_mb() - direct : 0);
            hos_ps5_trail("memory: at the stop, %llu MiB in use: %s", (unsigned long long)(hos_ps5_memory_total_mb() - direct), wh);
            abort();
        }
    }
    {   /* (the heap's growth by who asked: the words past the allocator's own two frames; never taken back here) */
        uintptr_t a[ACC_DEPTH];
        acc_words(__builtin_frame_address(0), 2, a);
        acc_take();
        struct acc_site *s = acc_site_of(heap_sites, &heap_sites_n, a);
        s->units += (bytes + 0xFFFF) >> 16, s->allocs++;
        s->peak = s->units;
        acc_give();
    }
    /* (2026-11-11) this thread inside the heap's map call: its allocation of direct memory may be made of pieces when it is
       refused (ps5_heap_pieces.c); and a refusal noted */
    const int slot = hos_heap_mapper_enter();
    void *const at = __real_ps5p_heap_map(bytes);
    hos_heap_mapper_leave(slot);
    if (at == (void *)-1) heap_refusal_note(bytes);
    return at;
}
void *hos_ps5_stack(size_t size) {
    static size_t backed_bytes;
    if (!backed_bytes) {
        const char *e = getenv("HOOKSHOT_PS5_STACK_MB");
        backed_bytes = (size_t)(e && atoi(e) > 0 ? atoi(e) : 8) << 20;
    }
    size = (size + UNIT - 1) & ~(size_t)(UNIT - 1);
    void *base = hos_ps5_reserve(size, 0);
    if (!base) return NULL;
    const size_t top = size < backed_bytes + UNIT ? size - UNIT : backed_bytes;   /* (at least one unit left as the guard) */
    if (ps5_vrange_commit((char *)base + size - top, top, PS5_SHM_READ | PS5_SHM_WRITE)) {
        ps5_vrange_release(base, size);
        return NULL;
    }
    __atomic_add_fetch(&acct_stacks, top, __ATOMIC_RELAXED), __atomic_add_fetch(&acct_stack_n, 1, __ATOMIC_RELAXED);
    return base;
}
int hos_ps5_release(void *p, size_t size) { __atomic_add_fetch(&acct_releases, 1, __ATOMIC_RELAXED); return ps5_vrange_release(p, (size + UNIT - 1) & ~(size_t)(UNIT - 1)); }
/* (2026-11-10) A block of direct memory of its own, backed at once, that is given back later (host/nn_video.c: a movie's
   pictures, which the heap kept 16 MiB blocks for long after the movie): a reservation made backed, and its bytes taken out of
   the memory line's "reservations backed" when it goes, which hos_ps5_release cannot do for a reservation it knows nothing of */
void *hos_ps5_block(size_t size) { return hos_ps5_reserve(size, 1); }
void hos_ps5_block_free(void *p, size_t size) {
    size = (size + UNIT - 1) & ~(size_t)(UNIT - 1);
    if (p && !hos_ps5_release(p, size)) __atomic_sub_fetch(&acct_reserved_backed, size, __ATOMIC_RELAXED);
}

/* ---- threads ---- */
int hos_ps5_tid(void) { return pthread_getthreadid_np(); }
/* (the platform's pthread_create wrap gives a thread its creator's MXCSR, but host/posix's CreateThread asks for its stack
   size, and the guest threads' 64 MiB are not the wrap's 2 MiB: set here again, on every thread win32_posix.c starts) */
void hos_ps5_thread_started(void) { ps5_fp_ieee(); }
int hos_ps5_cpus(uint64_t *mask) {
    uint64_t m = 0;
    if (scePthreadGetaffinity(pthread_self(), &m) || !m) m = 0x1fff;   /* (PROBE.md: CPUs 0 to 12) */
    if (mask) *mask = m;
    return __builtin_popcountll(m);
}
void hos_ps5_set_name(void *t, const char *name) { pthread_set_name_np((pthread_t)t, name); }

/* ---- scheduling (2026-11-09: the game's first run crawled, logs/game-run2). A title's threads are real-time: EglantinePS5
   (headless/performance.cpp, GPL-3.0: "threads of one priority are not time-sliced here") pins its workers for it, and
   the platform layer's own heap blocks rather than spins so as not to starve an owner at real-time priority (PS5_PayloadSDK
   platform/src/heap.c, src/dlmalloc/mspace.c). FreeBSD's ULE runs a SCHED_FIFO thread until it blocks, yields or a better
   one comes; SCHED_RR gives threads of one priority a slice each. Hookshot's threads all have one priority (SetThreadPriority
   changes nothing here: win32_posix.c), and the game's forty-odd threads on six virtual cores (host/hos_thread.c) need a
   thread that is woken to run: in that run a thread waited 26 s for a core whose limit is 250 ms. HOOKSHOT_PS5_SCHED:
     auto (default)  SCHED_FIFO made SCHED_RR, the priority at the same place in RR's range; any other policy kept
     rr              SCHED_RR;  other: SCHED_OTHER (time-shared: below every real-time thread);  fifo: SCHED_FIFO
     keep            the console's own, never changed
   for every thread win32_posix.c starts (CreateThread: the game's threads, the renderer's) or adopts (a thread that calls a
   Win32 thread function: the main thread, std::thread's that name themselves). RADV's own threads keep the console's. What
   the first thread was and became, and the counts, are hos_ps5_sched_note's line (stage 6 and the scheduling report) ---- */
int sceKernelGetCurrentCpu(void);
uint64_t hos_ps5_new_thread_cpus;
/* 0 keep, 1 auto, 2 rr, 3 other, 4 fifo (read each time: a thread may start before stage 4 loads hookshot_env.txt) */
static int sched_mode_get(void) {
    const char *e = getenv("HOOKSHOT_PS5_SCHED");
    int m = 1;
    if (e && *e) {
        if (!strcasecmp(e, "keep") || !strcmp(e, "0")) m = 0;
        else if (!strcasecmp(e, "rr")) m = 2;
        else if (!strcasecmp(e, "other")) m = 3;
        else if (!strcasecmp(e, "fifo")) m = 4;
    }
    return m;
}
const char *hos_ps5_sched_mode(void) {
    static const char *const names[] = {"keep", "auto", "rr", "other", "fifo"};
    return names[sched_mode_get()];
}
const char *hos_ps5_policy_name(int policy) {
    return policy == SCHED_FIFO ? "fifo" : policy == SCHED_RR ? "rr" : policy == SCHED_OTHER ? "other" : "?";
}
int hos_ps5_sched_of(void *t, int *policy, int *priority) {
    struct sched_param p;
    memset(&p, 0, sizeof p);
    int pol = -1;
    const int r = pthread_getschedparam((pthread_t)t, &pol, &p);
    if (policy) *policy = r ? -1 : pol;
    if (priority) *priority = r ? 0 : p.sched_priority;
    return r;
}
/* the priority at the same place in the new policy's range (the console's FIFO and RR ranges may well be one) */
static int sched_map(int from, int to, int prio) {
    const int lo1 = sched_get_priority_min(from), hi1 = sched_get_priority_max(from);
    const int lo2 = sched_get_priority_min(to), hi2 = sched_get_priority_max(to);
    if (lo2 > hi2 || lo2 < -1000000 || hi2 > 1000000) return prio;   /* (a range the call refused: as it was) */
    if (lo1 == lo2 && hi1 == hi2) return prio;
    if (hi1 > lo1 && prio >= lo1 && prio <= hi1)
        return lo2 + (int)((int64_t)(prio - lo1) * (hi2 - lo2) / (hi1 - lo1));
    return prio < lo2 ? lo2 : prio > hi2 ? hi2 : prio;
}
static struct {
    volatile int claimed, done;
    int policy, priority, new_policy, new_priority, result;
} sched_first;
static volatile int sched_changed, sched_refused, sched_left;
int hos_ps5_sched_set(int policy) {
    int pol = -1, prio = 0;
    int r = hos_ps5_sched_of((void *)pthread_self(), &pol, &prio);
    if (r) return r;
    struct sched_param p;
    memset(&p, 0, sizeof p);
    p.sched_priority = sched_map(pol, policy, prio);
    return pthread_setschedparam(pthread_self(), policy, &p);
}
int hos_ps5_sched_apply(void) {
    const int m = sched_mode_get();
    int pol = -1, prio = 0;
    const int got = hos_ps5_sched_of((void *)pthread_self(), &pol, &prio);
    int want = -1;
    if (!got) {
        switch (m) {
        case 1: want = pol == SCHED_FIFO ? SCHED_RR : -1; break;
        case 2: want = SCHED_RR; break;
        case 3: want = SCHED_OTHER; break;
        case 4: want = SCHED_FIFO; break;
        default: break;
        }
        if (want == pol) want = -1;
    }
    int r = 0;
    if (want >= 0) r = hos_ps5_sched_set(want);
    __atomic_add_fetch(want < 0 ? &sched_left : r ? &sched_refused : &sched_changed, 1, __ATOMIC_RELAXED);
    if (!__atomic_exchange_n(&sched_first.claimed, 1, __ATOMIC_ACQ_REL)) {
        sched_first.policy = pol, sched_first.priority = prio, sched_first.result = got ? got : r;
        hos_ps5_sched_of((void *)pthread_self(), &sched_first.new_policy, &sched_first.new_priority);
        __atomic_store_n(&sched_first.done, 1, __ATOMIC_RELEASE);
    }
    return want < 0 ? 0 : r ? -r : 1;
}
void hos_ps5_sched_note(char *buf, size_t cap) {
    int n = snprintf(buf, cap, "HOOKSHOT_PS5_SCHED=%s: ", hos_ps5_sched_mode());
    if (n < 0 || (size_t)n >= cap) return;
    if (__atomic_load_n(&sched_first.done, __ATOMIC_ACQUIRE))
        n += snprintf(buf + n, cap - (size_t)n, "the first thread was %s %d, is %s %d (%d); ", hos_ps5_policy_name(sched_first.policy),
                      sched_first.priority, hos_ps5_policy_name(sched_first.new_policy), sched_first.new_priority, sched_first.result);
    if ((size_t)n >= cap) return;
    snprintf(buf + n, cap - (size_t)n, "%d threads changed, %d refused, %d left as they were; ranges fifo %d..%d, rr %d..%d, other %d..%d",
             sched_changed, sched_refused, sched_left, sched_get_priority_min(SCHED_FIFO), sched_get_priority_max(SCHED_FIFO),
             sched_get_priority_min(SCHED_RR), sched_get_priority_max(SCHED_RR), sched_get_priority_min(SCHED_OTHER),
             sched_get_priority_max(SCHED_OTHER));
}
int64_t hos_ps5_thread_cpu_ns(void *t) {
    clockid_t id;
    const int r = pthread_getcpuclockid((pthread_t)t, &id);
    if (r) return -(int64_t)r;
    struct timespec ts;
    if (clock_gettime(id, &ts)) return -(int64_t)errno;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
int64_t hos_ps5_process_cpu_ns(void) {
    struct timespec ts;
    if (!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts)) return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    struct rusage u;
    if (getrusage(RUSAGE_SELF, &u)) return -(int64_t)errno;
    return ((int64_t)u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000000000LL + ((int64_t)u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1000;
}
int hos_ps5_current_cpu(void) { return sceKernelGetCurrentCpu(); }
uint64_t hos_ps5_affinity_of(void *t) {
    uint64_t m = 0;
    return scePthreadGetaffinity((pthread_t)t, &m) ? 0 : m;
}
int hos_ps5_process_cpuset(uint64_t *which, int *which_error, uint64_t *root, int *root_error) {
    /* (8 bytes, as EglantinePS5 asks and as pthread_getaffinity_np answers: the console refuses FreeBSD's 32-byte cpuset_t) */
    cpuset_t s;
    CPU_ZERO(&s);
    *which_error = cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, 8, &s) ? errno : 0;
    memcpy(which, &s, 8);
    CPU_ZERO(&s);
    *root_error = cpuset_getaffinity(CPU_LEVEL_ROOT, CPU_WHICH_PID, -1, 8, &s) ? errno : 0;
    memcpy(root, &s, 8);
    return *which_error;
}

/* ---- futexes: FreeBSD's umtx wait and wake on a 32-bit word, process-private. libkernel_web exports _umtx_op; that a
   title may call it is the probe's to prove (the platform's probe measured kevent, not umtx). Every lock and condition
   variable of host/posix rests on it, so should the console refuse it (ENOSYS, EPERM, EINVAL on a well-formed call), a
   table of condition variables by address takes over: a waiter checks the word under its bucket's mutex and sleeps on the
   bucket's condition, a waker broadcasts under the same mutex, so no wake is lost between the check and the sleep ---- */
static volatile int futex_mode;   /* 0 not known yet, 1 _umtx_op, 2 the table */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
} buckets[64];
static pthread_once_t buckets_once = PTHREAD_ONCE_INIT;
static void buckets_init(void) {
    for (int i = 0; i < 64; i++) {
        pthread_mutex_init(&buckets[i].mu, NULL);
        pthread_condattr_t a;
        pthread_condattr_init(&a);
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        pthread_cond_init(&buckets[i].cv, &a);
        pthread_condattr_destroy(&a);
    }
}
static unsigned bucket_of(volatile uint32_t *a) { return (unsigned)(((uintptr_t)a >> 2) * 0x9e3779b1u >> 26) & 63; }
static long table_wait(volatile uint32_t *a, uint32_t v, const struct timespec *rel) {
    pthread_once(&buckets_once, buckets_init);
    const unsigned b = bucket_of(a);
    struct timespec until;
    if (rel) {
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_sec += rel->tv_sec, until.tv_nsec += rel->tv_nsec;
        if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    }
    int r = 0;
    pthread_mutex_lock(&buckets[b].mu);
    if (__atomic_load_n(a, __ATOMIC_SEQ_CST) == v)
        r = rel ? pthread_cond_timedwait(&buckets[b].cv, &buckets[b].mu, &until) : pthread_cond_wait(&buckets[b].cv, &buckets[b].mu);
    pthread_mutex_unlock(&buckets[b].mu);
    if (r == ETIMEDOUT) { errno = ETIMEDOUT; return -1; }
    return 0;
}
static long table_wake(volatile uint32_t *a) {
    pthread_once(&buckets_once, buckets_init);
    const unsigned b = bucket_of(a);
    pthread_mutex_lock(&buckets[b].mu);
    pthread_cond_broadcast(&buckets[b].cv);   /* (a bucket is shared by many words: all of its waiters look again) */
    pthread_mutex_unlock(&buckets[b].mu);
    return 0;
}
static int umtx_refused(long r) { return r == -1 && (errno == ENOSYS || errno == EPERM || errno == EINVAL || errno == EFAULT); }
long hos_ps5_futex_wait(volatile uint32_t *a, uint32_t v, const struct timespec *rel) {
    if (futex_mode != 2) {
        /* (a relative timeout: uaddr is the size of a struct timespec, uaddr2 the timespec) */
        const long r = _umtx_op((void *)a, UMTX_OP_WAIT_UINT_PRIVATE, v, rel ? (void *)(uintptr_t)sizeof *rel : NULL, (void *)rel);
        if (!umtx_refused(r)) {
            if (!futex_mode) futex_mode = 1;
            return r;
        }
        if (futex_mode != 1) {   /* (refused from the start: the table from now on, said once) */
            futex_mode = 2;
            fprintf(stderr, "ps5: _umtx_op refused (errno %d): futexes on condition variables\n", errno);
        } else {
            return r;   /* (umtx worked before: this call's own error) */
        }
    }
    return table_wait(a, v, rel);
}
long hos_ps5_futex_wake(volatile uint32_t *a, int n) {
    if (futex_mode == 2) return table_wake(a);
    const long r = _umtx_op((void *)a, UMTX_OP_WAKE_PRIVATE, (u_long)n, NULL, NULL);
    if (umtx_refused(r) && futex_mode != 1) {
        futex_mode = 2;
        fprintf(stderr, "ps5: _umtx_op refused (errno %d): futexes on condition variables\n", errno);
        return table_wake(a);
    }
    if (!futex_mode && r != -1) futex_mode = 1;
    return r;
}
int hos_ps5_futex_mode(void) { return futex_mode ? futex_mode : 1; }
void hos_ps5_futex_force_fallback(int on) { futex_mode = on ? 2 : 0; }

/* ---- (lot 62) QueryPerformanceCounter on the time stamp counter, HOS_TSC_CLOCK=1 (host/posix/tsc_clock.h, windows.h). The
   rate is measured from a pair taken by a constructor, before main, to the settings: ~0.2 s at stage 4, the eboot's
   start in between ---- */
volatile int hos_tsc_qpc;   /* (1: QueryPerformanceCounter reads hos_tsc_clk; set once, after it) */
HosTscClock hos_tsc_clk;
static HosTscPair tsc_early;
__attribute__((constructor)) static void tsc_clock_early(void) { hos_tsc_pair(&tsc_early, 8); }
void hos_ps5_tsc_clock_start(char *line, size_t size) {
    static volatile int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) {
        snprintf(line, size, "clock: already started (QueryPerformanceCounter on the time stamp counter: %s)",
                 __atomic_load_n(&hos_tsc_qpc, __ATOMIC_ACQUIRE) ? "yes" : "no");
        return;
    }
    HosTscClock clk;
    if (hos_tsc_clock_start(env_on("HOS_TSC_CLOCK"), &tsc_early, 100000000, &clk, line, size)) {
        hos_tsc_clk = clk;
        __atomic_store_n(&hos_tsc_qpc, 1, __ATOMIC_RELEASE);
    }
}

/* ---- modules: what Linux loads at run time, linked in. Weak, so that host/ links without the renderer (a test); the
   eboot's link names them with -u so their archive members are pulled in (a weak reference pulls in nothing:
   nx2/toolchain_ps5.py link_plan) ---- */
typedef struct { const char *name; } Module;
static Module vk_module = {"hookshot_vk"}, vulkan_module = {"vulkan-1"}, self_module = {"eboot"};
extern const void *mkwvk_init(const void *host) __attribute__((weak));
extern void *vk_icdGetInstanceProcAddr(void *instance, const char *name) __attribute__((weak));
void *hos_ps5_module(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    /* (longshot_vk too: since upstream's rename (e2a4d453) host/rx_backend.c asks for longshot_vk.dll. The table knew the old
       name alone, and the merged eboot's first run on the console drew nothing: "rx: /data/hookshot/longshot_vk.so did not
       load (): nothing is drawn" (logs/udp/20261116-235031.log). tests/test_ps5_renderer_module_name.py) */
    if (!strncasecmp(base, "hookshot_vk.", 12) || !strncasecmp(base, "longshot_vk.", 12)) return mkwvk_init ? &vk_module : NULL;
    if (!strncasecmp(base, "vulkan-1.", 9) || !strncasecmp(base, "libvulkan.", 10))
        return vk_icdGetInstanceProcAddr ? &vulkan_module : NULL;
    return NULL;   /* (SDL, xinput, kernel32...: not on the console) */
}
void *hos_ps5_symbol(void *m, const char *name) {
    if (m == &vk_module && !strcmp(name, "mkwvk_init")) return (void *)mkwvk_init;
    /* (the Vulkan loader's entry is the ICD's: RADV linked whole, PS5_VulkanTemplate ps5/src/main.cpp does the same) */
    if (m == &vulkan_module && !strcmp(name, "vkGetInstanceProcAddr")) return (void *)vk_icdGetInstanceProcAddr;
    return NULL;
}
void *hos_ps5_self(void) { return &self_module; }

/* ---- paths ---- */
/* (the game folder, where a PC build keeps image.bin, romfs/, user/, the options, the logs and the caches beside the
   executable: the data folder when the title sees /data, else the title's own folder, /app0 (/data/homebrew/<TITLE_ID> over
   FTP and ps5upload: GRPN00001 since lot 80). A title sees only /app0 and /download0 unless its sandbox has /data:
   ShadowMountPlus 1.7beta3 and later mount /data into the titles it registers (bizkut/ps5-forever ps5/probes/psmkit-wine/
   forever-paths.h; Forever and Savage Wind keep their data in /data/forever and /data/savagewind so), or the Lapy daemon
   elevates the process (PS5_PayloadSDK platform/docs/ELEVATION.md: experimental on 12.60; asked for only when
   /app0/hookshot-elevate exists, since it waits ten seconds when no daemon runs). The title's big writes go through the storage broker
   (host/posix/ps5_broker_client.c), a payload the console does not throttle. The eboot itself is /app0/eboot.bin;
   GetModuleFileNameA answers <root>/eboot.bin so that every "beside the executable" path lands in the root.
   (lot 80) The data folder is /data/grapnel, the port's public name; /data/hookshot, the folder before, is renamed to it when
   it is the only one there, or used in place when that rename fails: host/posix/ps5_data_folder.h, and why a rename) */
static int data_granted;
static char data_root[64];
static char data_status[400] = "not checked";
static int data_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static int data_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}
static int data_rename(const char *from, const char *to) { return rename(from, to); }
static int data_mkdir(const char *path) {
    if (mkdir(path, 0777)) return -1;
    chmod(path, 0777);   /* (FTP's too: files-and-io.md) */
    return 0;
}
static const struct ps5_data_ops data_ops = {data_is_dir, data_exists, data_rename, data_mkdir};
/* the folder, chosen once: its line in data_status ("<how>: <what was done>"), the trail told of a rename (its file was in
   the folder: the same file under the new name) */
static int data_choose(const char *how_prefix) {
    char line[360];
    int err = 0;
    const enum ps5_data_how how = ps5_data_folder_choose(PS5_DATA_PARENT, &data_ops, data_root, sizeof data_root, line,
                                                         sizeof line, &err);
    snprintf(data_status, sizeof data_status, "%s%s: %s", how_prefix, ps5_data_how_name(how), line);
    if (how == PS5_DATA_TAKEN_OVER) hos_ps5_trail_renamed(PS5_DATA_OLD_DIR, PS5_DATA_DIR);
    hos_ps5_trail("ps5: data folder: %s", data_status);
    errno = err;
    return how != PS5_DATA_NONE;
}
int hos_ps5_data_init(void) {
    static int done;
    if (done) return data_granted;
    done = 1;
    struct stat st;
    if (data_choose("")) {
        data_granted = 1;
        return 1;
    }
    const int seen = errno;
    if (stat("/app0/hookshot-elevate", &st) == 0) {
        const enum ps5_elevation_status e = ps5_elevation_request(PS5_ELEVATION_FILESYSTEM);
        if (e == PS5_ELEVATION_OK && data_choose("elevated (Lapy), ")) {
            data_granted = 1;
            return 1;
        }
        snprintf(data_status + strlen(data_status), sizeof data_status - strlen(data_status), "; no /data: errno %d; Lapy: %s",
                 seen, ps5_elevation_status_name(e));
        return 0;
    }
    snprintf(data_status + strlen(data_status), sizeof data_status - strlen(data_status), "; no /data: errno %d", seen);
    return 0;
}
const char *hos_ps5_data_status(void) { return data_status; }
const char *hos_ps5_data_root(void) {
    const char *e = getenv("HOOKSHOT_PS5_ROOT");
    return e && *e ? e : data_granted ? data_root : "/app0";
}
const char *hos_ps5_exe_path(void) {
    static char path[1024];
    if (!path[0]) snprintf(path, sizeof path, "%s/eboot.bin", hos_ps5_data_root());
    return path;
}
/* (no getcwd for a title: libc's faults, PROBE.md. The runtime asks for one only to start in the executable's folder:
   kept here, and relative paths are not used on the console) */
static char cwd[1024] = "/app0";
const char *hos_ps5_cwd(void) { return cwd; }
int hos_ps5_chdir(const char *dir) {
    if (!dir || dir[0] != '/' || strlen(dir) >= sizeof cwd) { errno = EINVAL; return -1; }
    snprintf(cwd, sizeof cwd, "%s", dir);
    return 0;
}

/* ---- libc ---- */
/* (merged upstream v2.2.0) libc++'s std::filesystem, which the back end now uses (its play caches, driver_cache_bytes,
   pcache_stamp.h, pipeline_cache_file.h), comes in whole with its operations.cpp, and that imports pathconf: no module a
   title loads exports it, a null import the title's link refuses (RX_VK_SKIP_FILE took stat() for this reason). libc++
   asks it only for _PC_PATH_MAX, in current_path, which Hookshot does not call; the console's paths fit in 1024 bytes */
long pathconf(const char *path, int name) {
    (void)path;
    if (name == _PC_PATH_MAX) return 1024;
    if (name == _PC_NAME_MAX) return 255;
    errno = EINVAL;
    return -1;
}
time_t hos_ps5_timegm(struct tm *tm) {
    /* (days from the civil date, Howard Hinnant's algorithm: UTC, no time zone, as timegm. The fields are normalised by
       the arithmetic, not written back: nn_applet.c reads only the result) */
    long long y = (long long)tm->tm_year + 1900, mon = tm->tm_mon;
    y += mon / 12, mon %= 12;
    if (mon < 0) mon += 12, y--;
    const long long m = mon + 1;
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const long long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + tm->tm_mday - 1;
    const long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return (time_t)(((days * 24 + tm->tm_hour) * 60 + tm->tm_min) * 60 + tm->tm_sec);
}
int hos_ps5_fnmatch_casefold(const char *p, const char *n) {
    /* (0 on a match, as fnmatch: Windows' FindFirstFile patterns, '*' and '?' only, case folded) */
    for (; *p; p++, n++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 0;
            for (; *n; n++)
                if (!hos_ps5_fnmatch_casefold(p + 1, n)) return 0;
            return 1;
        }
        if (!*n) return 1;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*n)) return 1;
    }
    return *n ? 1 : 0;
}

/* ---- standard error's last lines (2026-11-10, the eighth run: an abort with no word of why; what the aborting code wrote
   went to klog only). After klog's capture, standard error is one more pipe: its reader gives every line on to klog's
   stream as before and keeps the last 4 KiB of the lines that are not hos_log's (those are in hookshot_log.txt) ---- */
static FILE *err_klog;
static int err_read = -1;
static char err_ring[4096];
static unsigned long err_n;   /* (bytes ever kept: the ring's write position is err_n % size) */
static void err_keep(const char *line, size_t n) {
    for (size_t i = 0; i < n; i++) err_ring[__atomic_fetch_add(&err_n, 1, __ATOMIC_RELAXED) % sizeof err_ring] = line[i];
}
static void *err_main(void *unused) {
    (void)unused;
    char line[1200];
    size_t len = 0;
    for (;;) {
        char chunk[1024];
        const ssize_t got = read(err_read, chunk, sizeof chunk);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        for (ssize_t i = 0; i < got; i++) {
            if (len < sizeof line - 2) line[len++] = chunk[i];
            if (chunk[i] != '\n' && len < sizeof line - 2) continue;
            if (line[len - 1] != '\n') line[len++] = '\n';
            line[len] = 0;
            fputs(line, err_klog);
            fflush(err_klog);
            /* (hos_log's lines: "[  12.345] [hos ...") */
            if (!strstr(line, "] [hos ") && strncmp(line, "[hos ", 5)) err_keep(line, len);
            len = 0;
        }
    }
    return NULL;
}
void hos_ps5_stderr_tail(char *out, size_t cap) {
    if (!cap) return;
    const unsigned long end = __atomic_load_n(&err_n, __ATOMIC_RELAXED);
    unsigned long from = end > sizeof err_ring ? end - sizeof err_ring : 0;
    if (end - from > cap - 1) from = end - (cap - 1);
    size_t n = 0;
    for (unsigned long i = from; i < end && n + 4 < cap; i++) {
        const char c = err_ring[i % sizeof err_ring];
        if (c == '\n') out[n++] = ' ', out[n++] = '|', out[n++] = ' ';
        else out[n++] = c;
    }
    out[n] = 0;
}
static void err_capture(void) {
    int ends[2];
    if (pipe(ends)) return;
    FILE *const stream = fdopen(ends[1], "w");
    if (!stream) { close(ends[0]), close(ends[1]); return; }
    setvbuf(stream, NULL, _IOLBF, 0);
    err_read = ends[0];
    fflush(stderr);
    err_klog = stderr;
    pthread_t t;
    if (pthread_create(&t, NULL, err_main, NULL)) { fclose(stream), close(ends[0]); return; }
    pthread_detach(t);
    stderr = stream;
}

/* ---- the process ---- */
void hos_ps5_start(void) {
    /* (/data first: the Lapy daemon's request must come before any thread exists, and klog's capture is a thread) */
    hos_ps5_data_init();
    ps5_fp_ieee();   /* (the lifted ARM code's floating point: IEEE denormals, round to nearest) */
    ps5_klog_capture_stderr("[hookshot] ");
    if (!env_on("HOOKSHOT_PS5_NO_STDERR_TAIL")) err_capture();
    sceSystemServiceHideSplashScreen();
    fprintf(stderr, "ps5: /data: %s; the game folder is %s\n", data_status, hos_ps5_data_root());
    /* (main reserved the window before this: how, for klog. A shorter window means the console refused its top) */
    struct hos_ps5_guest_stats w;
    hos_ps5_guest_stats(&w);
    static const char *const how[] = {"NOT RESERVED", "one reservation", "adjacent pieces", "the pieces granted from the base up"};
    fprintf(stderr, "ps5: the guest window: %llu GiB at %#llx, %s (one reservation answered %#x, %u pieces); the guest's address space "
                    "ends at %llu GiB\n", (unsigned long long)(w.size >> 30), (unsigned long long)w.base, how[w.mode & 3],
            (unsigned)w.whole_result, w.pieces, (unsigned long long)(w.space_end >> 30));
}
/* (PS5_Vulkan's CRT calls it when main returns, in place of exit: tooling/native/app_crt.cpp) */
void catchReturnFromMain(int status) { hos_ps5_exit(status); }
void hos_ps5_exit(int code) {
    hos_ps5_trail("exit: hos_ps5_exit(%d): the title asks the shell to close it", code);
    fprintf(stderr, "ps5: exit %d: the shell closes the title\n", code);
    fflush(NULL);
    (void)sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(100000);
}
#endif /* __PROSPERO__ */
