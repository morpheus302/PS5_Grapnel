/* (ps5) host/posix/ps5_heap_pieces.c over a model of the console's direct memory: tests/test_heap_pieces.py builds and runs it.

   The model: 2 GiB of "direct memory" in a memfd, so that each piece is a range of its own that a mapping really points at; a
   unit of 64 KiB is free or allocated, and counts the pages of the heap's range that map it now (a release of a unit still
   mapped is an error, as is a mapping of a unit not allocated, a mapping over a page not reserved, or a start of
   ps5_heap_pieces.c's reaching the "kernel"). The heap's range: 8 GiB reserved, a table of what each page maps.
   The heap: libps5platform.a's heap.o as disassembled on 11/11 (first fit in its range, 1024 segments, each one allocation
   of direct memory (0 to the size, 64 KiB aligned, type 12) mapped fixed, released when the mapping fails; an unmap of whole
   segments: unmapped, reserved again, released), and over it, when tests/test_heap_pieces.py found the SDK's dlmalloc
   (-DHAVE_DLMALLOC), dlmalloc 2.8.6 configured as the archive's mspace.o is (16 MiB granularity, 32 MiB mmap threshold,
   64 KiB pages, footers, locks).
   The console's glue, as host/posix/ps5_platform.c has it: the allocation's refusal handed to hos_heap_pieces_after_refusal,
   a start of the file's routed to hos_heap_pieces_map and hos_heap_pieces_release, the heap's map call marked
   (hos_heap_mapper_enter / leave). */
#define _GNU_SOURCE
#include "ps5.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...)                                                    \
    do {                                                                 \
        if (!(c)) {                                                      \
            fails++;                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);         \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

/* ---- the model of the direct memory ---- */
#define U ((size_t)1 << 16)
#define DM_UNITS 32768u   /* 2 GiB */
#define DM_BYTES ((size_t)DM_UNITS * U)
#define REFUSED ((int32_t)0x80020023)
static int dm_fd = -1;
static uint8_t dm_used[DM_UNITS], dm_saved[DM_UNITS];
static int32_t dm_maps[DM_UNITS];
static pthread_mutex_t dm_mu = PTHREAD_MUTEX_INITIALIZER;
static long dm_bad, map_calls, alloc_calls;
static int map_fail_at;   /* the n-th map from now fails (0: none) */

#define RANGE_BYTES ((size_t)8 << 30)
#define RANGE_PAGES (RANGE_BYTES / U)
static char *range;
static int64_t v2p[RANGE_PAGES];
static uint8_t v_reserved[RANGE_PAGES];

static void bad(const char *what, long a, long b) {
    __atomic_add_fetch(&dm_bad, 1, __ATOMIC_RELAXED);
    fprintf(stderr, "model: %s (%#lx, %#lx)\n", what, a, b);
}

static int32_t model_alloc(int64_t lo, int64_t hi, size_t length, size_t alignment, int64_t *start) {
    __atomic_add_fetch(&alloc_calls, 1, __ATOMIC_RELAXED);
    if (lo < 0 || hi > (int64_t)DM_BYTES || length == 0 || (length & (U - 1)) || !start) return REFUSED;
    const size_t n = length / U, a = alignment > U ? alignment / U : 1;
    pthread_mutex_lock(&dm_mu);
    size_t u = ((size_t)lo / U + a - 1) / a * a;
    const size_t end = (size_t)hi / U;
    while (u + n <= end) {
        size_t k = 0;
        while (k < n && !dm_used[u + k]) k++;
        if (k == n) {
            memset(dm_used + u, 1, n);
            pthread_mutex_unlock(&dm_mu);
            *start = (int64_t)(u * U);
            return 0;
        }
        u = (u + k + 1 + a - 1) / a * a;
    }
    pthread_mutex_unlock(&dm_mu);
    return REFUSED;
}
static int32_t model_release(int64_t start, size_t length) {
    if (start < 0 || (start & (int64_t)(U - 1)) || (size_t)start + length > DM_BYTES || (length & (U - 1))) {
        bad("a release out of the direct memory", (long)start, (long)length);
        return REFUSED;
    }
    pthread_mutex_lock(&dm_mu);
    for (size_t u = (size_t)start / U; u < ((size_t)start + length) / U; u++) {
        if (!dm_used[u]) bad("a release of a free unit", (long)u, 0);
        if (dm_maps[u]) bad("a release of a unit still mapped", (long)u, dm_maps[u]);
        dm_used[u] = 0;
    }
    pthread_mutex_unlock(&dm_mu);
    return 0;
}
static size_t page_of(const void *p) { return (size_t)((const char *)p - range) / U; }
static int in_range(const void *p, size_t length) {
    return (const char *)p >= range && (const char *)p + length <= range + RANGE_BYTES && !(((uintptr_t)p) & (U - 1));
}
static int32_t model_map(void **address, size_t length, int protection, int flags, int64_t start, size_t alignment) {
    (void)protection, (void)alignment;
    __atomic_add_fetch(&map_calls, 1, __ATOMIC_RELAXED);
    if (map_fail_at && !--map_fail_at) return REFUSED;
    if (!address || !in_range(*address, length) || !(flags & 0x10) || (length & (U - 1))) {
        bad("a map out of the heap's range", (long)(address ? *address : 0), (long)length);
        return REFUSED;
    }
    if (start < 0 || (size_t)start + length > DM_BYTES || (start & (int64_t)(U - 1))) {
        bad("a map of a start that is not direct memory", (long)start, (long)length);
        return REFUSED;
    }
    pthread_mutex_lock(&dm_mu);
    const size_t p0 = page_of(*address), u0 = (size_t)start / U;
    for (size_t i = 0; i < length / U; i++) {
        if (!dm_used[u0 + i]) bad("a map of a unit not allocated", (long)(u0 + i), 0);
        if (!v_reserved[p0 + i] || v2p[p0 + i] >= 0) bad("a map over a page not reserved", (long)(p0 + i), (long)v2p[p0 + i]);
    }
    void *got = mmap(*address, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, dm_fd, (off_t)start);
    if (got != *address) {
        pthread_mutex_unlock(&dm_mu);
        bad("mmap", (long)got, errno);
        return REFUSED;
    }
    for (size_t i = 0; i < length / U; i++) v2p[p0 + i] = (int64_t)(u0 + i), v_reserved[p0 + i] = 0, dm_maps[u0 + i]++;
    pthread_mutex_unlock(&dm_mu);
    return 0;
}
static void model_munmap(void *address, size_t length) {
    if (!in_range(address, length)) {
        bad("an unmap out of the heap's range", (long)address, (long)length);
        return;
    }
    pthread_mutex_lock(&dm_mu);
    const size_t p0 = page_of(address);
    for (size_t i = 0; i < length / U; i++) {
        if (v2p[p0 + i] >= 0) dm_maps[v2p[p0 + i]]--;
        v2p[p0 + i] = -1, v_reserved[p0 + i] = 0;
    }
    mmap(address, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    pthread_mutex_unlock(&dm_mu);
}
static void model_reserve(void *address, size_t length) {
    pthread_mutex_lock(&dm_mu);
    const size_t p0 = page_of(address);
    for (size_t i = 0; i < length / U; i++) {
        if (v2p[p0 + i] >= 0) bad("a reservation over a mapped page", (long)(p0 + i), 0);
        v_reserved[p0 + i] = 1;
    }
    pthread_mutex_unlock(&dm_mu);
}
static uint64_t model_largest(void) {
    pthread_mutex_lock(&dm_mu);
    size_t best = 0, run = 0;
    for (size_t u = 0; u < DM_UNITS; u++) {
        run = dm_used[u] ? 0 : run + 1;
        if (run > best) best = run;
    }
    pthread_mutex_unlock(&dm_mu);
    return (uint64_t)best * U;
}
static size_t model_free_units(void) {
    size_t n = 0;
    for (size_t u = 0; u < DM_UNITS; u++) n += !dm_used[u];
    return n;
}

/* ---- the console's side that ps5_heap_pieces.c calls (ps5_platform.c's), over the model ---- */
int hos_ps5_tid(void) { return (int)syscall(SYS_gettid); }
double hos_ps5_since_start(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int quiet;
static long trail_lines;
void hos_ps5_trail(const char *fmt, ...) {
    __atomic_add_fetch(&trail_lines, 1, __ATOMIC_RELAXED);
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("  trail: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
int32_t hos_ps5_piece_take(int64_t search_end, size_t length, size_t alignment, int32_t type, int64_t *start) {
    (void)type;
    return model_alloc(0, search_end, length, alignment, start);
}
int32_t hos_ps5_piece_give(int64_t start, size_t length) { return model_release(start, length); }
int32_t hos_ps5_piece_map(void **address, size_t length, int protection, int flags, int64_t start, size_t alignment) {
    return model_map(address, length, protection, flags, start, alignment);
}
void hos_ps5_piece_unmap(void *address, size_t length, int reserve) {
    model_munmap(address, length);
    if (reserve) model_reserve(address, length);
}
uint64_t hos_ps5_piece_largest(void) { return model_largest(); }

/* the wraps of the three kernel calls, as ps5_platform.c's: what the heap calls */
static int32_t kAllocate(int64_t lo, int64_t hi, size_t length, size_t alignment, int type, int64_t *start) {
    const int32_t r = model_alloc(lo, hi, length, alignment, start);
    return r == 0 ? 0 : hos_heap_pieces_after_refusal(r, lo, hi, length, alignment, type, start);
}
static int32_t kMap(void **address, size_t length, int protection, int flags, int64_t start, size_t alignment) {
    if (hos_heap_pieces_is(start)) return hos_heap_pieces_map(address, length, protection, flags, start, alignment);
    return model_map(address, length, protection, flags, start, alignment);
}
static int32_t kRelease(int64_t start, size_t length) {
    if (hos_heap_pieces_is(start)) return hos_heap_pieces_release(start, length);
    return model_release(start, length);
}

/* ---- the heap's map and unmap, as libps5platform.a's heap.o does them ---- */
#define HEAP_MAX_SEGMENTS 1024
static struct { size_t offset, bytes; int64_t direct_start; } segments[HEAP_MAX_SEGMENTS];
static unsigned segment_count;
static size_t mapped_bytes;
static long refusals;
static pthread_mutex_t segment_lock = PTHREAD_MUTEX_INITIALIZER;
static void *real_heap_map(size_t bytes) {
    pthread_mutex_lock(&segment_lock);
    void *result = (void *)-1;
    bytes = (bytes + U - 1) & ~(U - 1);
    if (bytes && segment_count < HEAP_MAX_SEGMENTS) {
        unsigned slot = 0;
        size_t offset = 0;
        for (; slot < segment_count; slot++) {
            if (segments[slot].offset - offset >= bytes) break;
            offset = segments[slot].offset + segments[slot].bytes;
        }
        int64_t start = -1;
        if (offset <= RANGE_BYTES && RANGE_BYTES - offset >= bytes && kAllocate(0, (int64_t)DM_BYTES, bytes, U, 12, &start) == 0) {
            void *const at = range + offset;
            void *mapped = at;
            if (kMap(&mapped, bytes, 3, 0x10, start, U) != 0 || mapped != at) {
                kRelease(start, bytes);
            } else {
                memmove(&segments[slot + 1], &segments[slot], (segment_count - slot) * sizeof segments[0]);
                segments[slot].offset = offset, segments[slot].bytes = bytes, segments[slot].direct_start = start;
                segment_count++;
                mapped_bytes += bytes;
                result = at;
            }
        }
    }
    pthread_mutex_unlock(&segment_lock);
    return result;
}
int ps5p_heap_unmap(void *address, size_t bytes) {
    pthread_mutex_lock(&segment_lock);
    int result = -1;
    if ((char *)address >= range) {
        const size_t offset = (size_t)((char *)address - range);
        unsigned first = 0;
        while (first < segment_count && segments[first].offset < offset) first++;
        if (first < segment_count && segments[first].offset == offset) {
            unsigned last = first;
            size_t covered = 0;
            while (last < segment_count && covered < bytes && segments[last].offset == offset + covered) covered += segments[last++].bytes;
            if (covered == bytes) {
                for (unsigned i = first; i < last; i++) {
                    void *at = range + segments[i].offset;
                    model_munmap(at, segments[i].bytes);
                    model_reserve(at, segments[i].bytes);
                    kRelease(segments[i].direct_start, segments[i].bytes);
                    mapped_bytes -= segments[i].bytes;
                }
                memmove(&segments[first], &segments[last], (segment_count - last) * sizeof segments[0]);
                segment_count -= last - first;
                result = 0;
            }
        }
    }
    pthread_mutex_unlock(&segment_lock);
    return result;
}
/* ps5_platform.c's wrap of it: the thread marked as the heap's mapper, a refusal counted */
void *ps5p_heap_map(size_t bytes) {
    const int slot = hos_heap_mapper_enter();
    void *const at = real_heap_map(bytes);
    hos_heap_mapper_leave(slot);
    if (at == (void *)-1) __atomic_add_fetch(&refusals, 1, __ATOMIC_RELAXED);
    return at;
}

/* ---- set-ups ---- */
static void model_reset(int all_used) {
    CHECK(segment_count == 0, "the heap holds %u segments at a reset", segment_count);
    memset(dm_used, all_used ? 1 : 0, sizeof dm_used);
    memset(dm_maps, 0, sizeof dm_maps);
}
static void hole(size_t unit, size_t units) {
    for (size_t u = unit; u < unit + units && u < DM_UNITS; u++) dm_used[u] = 0;
}
/* 30 pieces of 15 MiB, 60 of 1 MiB, 400 of 64 KiB, the rest taken (the run of 11/11: 304 MiB free, the largest 15) */
static void fragment(void) {
    model_reset(1);
    size_t u = 64;
    for (int i = 0; i < 30; i++, u += 240 + 17) hole(u, 240);
    for (int i = 0; i < 60; i++, u += 16 + 3) hole(u, 16);
    for (int i = 0; i < 400; i++, u += 2) hole(u, 1);
}
static void pieces(const char *kb) {
    if (kb) setenv("HOOKSHOT_PS5_HEAP_PIECES_KB", kb, 1);
    else unsetenv("HOOKSHOT_PS5_HEAP_PIECES_KB");
}
static void save(void) { memcpy(dm_saved, dm_used, sizeof dm_used); }
static int same_as_saved(void) { return !memcmp(dm_saved, dm_used, sizeof dm_used); }
static int all_reserved(void) {
    for (size_t p = 0; p < RANGE_PAGES; p++)
        if (!v_reserved[p] || v2p[p] >= 0) return 0;
    return 1;
}
static struct hos_heap_pieces_stats stats(void) {
    struct hos_heap_pieces_stats s;
    hos_heap_pieces_stats(&s);
    return s;
}
/* a word a 4-byte slot of the block holds: its index and the block's seed */
static void fill(void *p, size_t bytes, uint32_t seed) {
    uint32_t *w = p;
    const size_t n = bytes / 4;
    for (size_t i = 0; i + 1 < n; i += 61) w[i] = (uint32_t)i * 2654435761u ^ seed;
    if (n) w[n - 1] = 0xabcd1234u ^ seed;   /* (the last word: the end of the allocation written too) */
}
static int filled(const void *p, size_t bytes, uint32_t seed) {
    const uint32_t *w = p;
    const size_t n = bytes / 4;
    for (size_t i = 0; i + 1 < n; i += 61)
        if (w[i] != ((uint32_t)i * 2654435761u ^ seed)) return 0;
    return !n || w[n - 1] == (0xabcd1234u ^ seed);
}
/* what the block's pages map, read back from the "direct memory" itself: the writes landed in distinct units */
static int backed_as_written(const void *p, size_t bytes, uint32_t seed) {
    for (size_t off = 0; off < bytes; off += U) {
        const int64_t u = v2p[page_of((const char *)p + off)];
        if (u < 0) return 0;
        uint32_t first;
        const size_t i = off / 4;
        if (pread(dm_fd, &first, 4, (off_t)(u * (int64_t)U)) != 4) return 0;
        if (i % 61 == 0 && first != ((uint32_t)i * 2654435761u ^ seed)) return 0;
    }
    /* and no unit twice */
    for (size_t a = 0; a < bytes; a += U)
        for (size_t b = a + U; b < bytes; b += U)
            if (v2p[page_of((const char *)p + a)] == v2p[page_of((const char *)p + b)]) return 0;
    return 1;
}

static void test_abundant(void) {
    fprintf(stderr, "abundant direct memory: blocks of one piece, as before\n");
    model_reset(0);
    pieces("1024");
    save();
    const struct hos_heap_pieces_stats s0 = stats();
    void *a = ps5p_heap_map(16 << 20), *b = ps5p_heap_map(33 << 20);
    CHECK(a != (void *)-1 && b != (void *)-1, "maps refused with all of the memory free");
    const struct hos_heap_pieces_stats s1 = stats();
    CHECK(s1.made == s0.made && s1.blocks == 0, "a block of pieces made where one piece was there");
    CHECK(segments[0].direct_start >= 0 && segments[1].direct_start >= 0, "a start not the kernel's");
    fill(a, 16 << 20, 1), fill(b, 33 << 20, 2);
    CHECK(filled(a, 16 << 20, 1) && filled(b, 33 << 20, 2), "the blocks do not hold what was written");
    CHECK(ps5p_heap_unmap(a, 16 << 20) == 0 && ps5p_heap_unmap(b, 33 << 20) == 0, "unmaps refused");
    CHECK(same_as_saved() && all_reserved(), "the direct memory or the range not as before");
}

static void test_fragmented_off(void) {
    fprintf(stderr, "direct memory in pieces of 15 MiB at most, HOOKSHOT_PS5_HEAP_PIECES_KB unset: refused, as before\n");
    fragment();
    pieces(NULL);
    save();
    const long r0 = refusals;
    CHECK(ps5p_heap_map(16 << 20) == (void *)-1, "a block of 16 MiB made with no piece of 16 MiB and the setting off");
    CHECK(ps5p_heap_map(33 << 20) == (void *)-1, "a block of 33 MiB made with no piece of 33 MiB and the setting off");
    pieces("0");
    CHECK(ps5p_heap_map(16 << 20) == (void *)-1, "a block made with HOOKSHOT_PS5_HEAP_PIECES_KB=0");
    CHECK(refusals == r0 + 3, "refusals not counted");
    CHECK(same_as_saved() && all_reserved(), "a refusal changed the direct memory or the range");
}

static void test_fragmented_on(void) {
    fprintf(stderr, "the same pieces, HOOKSHOT_PS5_HEAP_PIECES_KB=1024: blocks of 16, 33 and 80 MiB made of pieces\n");
    fragment();
    pieces("1024");
    save();
    const size_t free0 = model_free_units();
    const struct hos_heap_pieces_stats s0 = stats();
    void *a = ps5p_heap_map(16 << 20);
    CHECK(a != (void *)-1, "a block of 16 MiB refused with 30 pieces of 15 MiB free");
    struct hos_heap_pieces_stats s1 = stats();
    CHECK(s1.made == s0.made + 1 && s1.blocks == 1 && s1.pieces == 2, "16 MiB: %llu made, %llu now, %llu pieces (15 + 1 expected)",
          (unsigned long long)(s1.made - s0.made), (unsigned long long)s1.blocks, (unsigned long long)s1.pieces);
    void *b = ps5p_heap_map(33 << 20), *c = ps5p_heap_map(80 << 20);
    CHECK(b != (void *)-1 && c != (void *)-1, "blocks of 33 and 80 MiB refused with 450 MiB free in pieces of 15");
    s1 = stats();
    CHECK(s1.blocks == 3, "%llu blocks of pieces now", (unsigned long long)s1.blocks);
    CHECK(model_free_units() == free0 - (129u << 4), "the blocks took %zu units, not %u", free0 - model_free_units(), 129u << 4);
    fill(a, 16 << 20, 3), fill(b, 33 << 20, 4), fill(c, 80 << 20, 5);
    CHECK(filled(a, 16 << 20, 3) && filled(b, 33 << 20, 4) && filled(c, 80 << 20, 5), "a block of pieces does not hold what was written");
    CHECK(backed_as_written(a, 16 << 20, 3) && backed_as_written(b, 33 << 20, 4) && backed_as_written(c, 80 << 20, 5),
          "the writes did not land in distinct units of the direct memory");
    CHECK(ps5p_heap_unmap(b, 33 << 20) == 0, "the unmap of a block of pieces refused");
    CHECK(ps5p_heap_unmap(a, 16 << 20) == 0 && ps5p_heap_unmap(c, 80 << 20) == 0, "unmaps refused");
    s1 = stats();
    CHECK(s1.blocks == 0 && s1.pieces == 0 && s1.given_back == s0.given_back + 3, "blocks not given back: %llu now",
          (unsigned long long)s1.blocks);
    CHECK(same_as_saved() && all_reserved(), "the direct memory or the range not as before the blocks");
}

static void test_map_fails(void) {
    fprintf(stderr, "a piece's mapping refused part way: nothing kept, the range reserved again\n");
    fragment();
    pieces("1024");
    save();
    const struct hos_heap_pieces_stats s0 = stats();
    map_fail_at = 2;   /* (the second piece's) */
    CHECK(ps5p_heap_map(16 << 20) == (void *)-1, "a block made though a piece's mapping failed");
    map_fail_at = 0;
    const struct hos_heap_pieces_stats s1 = stats();
    CHECK(s1.map_failed == s0.map_failed + 1 && s1.blocks == 0, "the failure not counted, or the block kept");
    CHECK(same_as_saved() && all_reserved(), "a failed mapping left units taken or the range unreserved");
    void *a = ps5p_heap_map(16 << 20);
    CHECK(a != (void *)-1, "the next block refused after a failed one");
    if (a != (void *)-1) CHECK(ps5p_heap_unmap(a, 16 << 20) == 0, "unmap refused");
    CHECK(same_as_saved(), "units left");
}

static void test_small_pieces(void) {
    fprintf(stderr, "only pieces of 512 KiB: refused at 1024, made at 512; only pieces of 64 KiB: more than 64 pieces refused\n");
    model_reset(1);
    for (size_t u = 64; u + 8 < DM_UNITS && u < 64 + 600 * 9; u += 9) hole(u, 8);
    save();
    pieces("1024");
    CHECK(ps5p_heap_map(16 << 20) == (void *)-1, "a block made of pieces smaller than the least asked");
    CHECK(same_as_saved(), "a refused block left units taken");
    pieces("512");
    void *a = ps5p_heap_map(16 << 20);
    CHECK(a != (void *)-1 && stats().pieces == 32, "16 MiB in pieces of 512 KiB: %llu pieces", (unsigned long long)stats().pieces);
    if (a != (void *)-1) {
        fill(a, 16 << 20, 6);
        CHECK(filled(a, 16 << 20, 6) && backed_as_written(a, 16 << 20, 6), "the block of 32 pieces does not hold what was written");
        CHECK(ps5p_heap_unmap(a, 16 << 20) == 0, "unmap refused");
    }
    CHECK(same_as_saved(), "units left");
    model_reset(1);
    for (size_t u = 64; u + 1 < DM_UNITS; u += 2) hole(u, 1);
    save();
    pieces("64");
    CHECK(ps5p_heap_map(16 << 20) == (void *)-1, "a block of 256 pieces made (64 at most)");
    CHECK(same_as_saved(), "a refused block of too many pieces left units taken");
    void *b = ps5p_heap_map(4 << 20);   /* (64 pieces of 64 KiB: the most) */
    CHECK(b != (void *)-1 && stats().pieces == 64, "4 MiB in 64 pieces of 64 KiB refused");
    if (b != (void *)-1) CHECK(ps5p_heap_unmap(b, 4 << 20) == 0, "unmap refused");
    CHECK(same_as_saved(), "units left");
}

static void test_slots(void) {
    fprintf(stderr, "every slot of blocks in use: the next refused, then made once one is given back\n");
    model_reset(1);
    for (size_t u = 64; u + 1 < DM_UNITS; u += 2) hole(u, 1);
    save();
    pieces("64");
    static void *at[300];
    int made = 0;
    for (; made < 300; made++) {
        at[made] = ps5p_heap_map(128 << 10);
        if (at[made] == (void *)-1) break;
    }
    CHECK(made == 256, "%d blocks of pieces made, 256 slots", made);
    CHECK(stats().full >= 1, "the full table not counted");
    if (made == 256) {
        CHECK(ps5p_heap_unmap(at[10], 128 << 10) == 0, "unmap refused");
        at[10] = ps5p_heap_map(128 << 10);
        CHECK(at[10] != (void *)-1, "a block refused after a slot was given back");
    }
    for (int i = 0; i < made; i++)
        if (at[i] != (void *)-1) CHECK(ps5p_heap_unmap(at[i], 128 << 10) == 0, "unmap %d refused", i);
    CHECK(same_as_saved() && stats().blocks == 0, "units or blocks left");
}

static void test_not_the_heap(void) {
    fprintf(stderr, "an allocation that is not the heap's map call: never made of pieces\n");
    fragment();
    pieces("1024");
    save();
    int64_t s = -1;
    CHECK(kAllocate(0, (int64_t)DM_BYTES, 16 << 20, U, 12, &s) != 0, "an allocation outside the heap's map call made of pieces");
    const int slot = hos_heap_mapper_enter();
    CHECK(kAllocate(0, (int64_t)DM_BYTES, 16 << 20, (size_t)2 << 20, 12, &s) != 0, "an allocation aligned to 2 MiB made of pieces");
    CHECK(kAllocate(1 << 20, (int64_t)DM_BYTES, 16 << 20, U, 12, &s) != 0, "an allocation of a part of the range made of pieces");
    hos_heap_mapper_leave(slot);
    CHECK(same_as_saved() && stats().blocks == 0, "units or blocks left");
    CHECK(!hos_heap_pieces_is(0) && !hos_heap_pieces_is((int64_t)DM_BYTES - (int64_t)U) && !hos_heap_pieces_is((int64_t)16 << 30) &&
              !hos_heap_pieces_is(-1),
          "a start of direct memory taken for a block of pieces");
    void *p = range;
    CHECK(kMap(&p, U, 3, 0x10, (int64_t)((uint64_t)0x7a << 40), U) != 0, "a map of a start never made accepted");
    CHECK(kRelease((int64_t)((uint64_t)0x7a << 40) + ((int64_t)5 << 32), U) != 0, "a release of a start never made accepted");
}

#ifdef HAVE_DLMALLOC
void *create_mspace(size_t capacity, int locked);
size_t destroy_mspace(void *msp);
void *mspace_malloc(void *msp, size_t bytes);
void mspace_free(void *msp, void *mem);
int mspace_trim(void *msp, size_t pad);
size_t mspace_footprint(void *msp);

static unsigned lcg(unsigned *s) { return *s = *s * 1103515245u + 12345u, *s >> 8; }

static void test_dlmalloc(void) {
    fprintf(stderr, "dlmalloc as the archive configures it, over the same pieces\n");
    fragment();
    save();
    pieces(NULL);
    void *m = create_mspace(0, 1);
    CHECK(!m, "an arena made with no piece of 16 MiB and the setting off");
    if (m) destroy_mspace(m);
    pieces("1024");
    m = create_mspace(0, 1);
    CHECK(m != NULL, "no arena with the setting on");
    if (!m) return;
    hos_heap_arena_note(m);
    enum { N = 3000 };
    static void *p[N];
    static size_t sz[N];
    unsigned seed = 7, nulls = 0;
    for (int i = 0; i < N; i++) {
        const unsigned r = lcg(&seed);
        sz[i] = i % 1000 == 999 ? (size_t)33 << 20 : i % 700 == 3 ? (size_t)20 << 20 : 32 + r % (64 << 10);
        p[i] = mspace_malloc(m, sz[i]);
        if (!p[i]) { nulls++; continue; }
        fill(p[i], sz[i], (uint32_t)i);
    }
    CHECK(!nulls, "%u allocations refused with %zu MiB free in pieces", nulls, model_free_units() >> 4);
    for (int i = 0; i < N; i++)
        if (p[i]) CHECK(filled(p[i], sz[i], (uint32_t)i), "allocation %d (%zu bytes) does not hold what was written", i, sz[i]);
    const struct hos_heap_pieces_stats s1 = stats();
    fprintf(stderr, "  %llu blocks of pieces now (%llu MiB in %llu pieces), %u segments, %zu MiB mapped\n", (unsigned long long)s1.blocks,
            (unsigned long long)(s1.bytes >> 20), (unsigned long long)s1.pieces, segment_count, mapped_bytes >> 20);
    CHECK(s1.blocks > 0, "no block of pieces made");
    for (int i = 0; i < N; i += 2) mspace_free(m, p[i]), p[i] = NULL;
    for (int i = 0; i < N; i += 2) {
        sz[i] = 32 + lcg(&seed) % (128 << 10);
        p[i] = mspace_malloc(m, sz[i]);
        if (p[i]) fill(p[i], sz[i], (uint32_t)i + 77);
        else nulls++;
    }
    CHECK(!nulls, "%u allocations refused the second time", nulls);
    for (int i = 0; i < N; i++)
        if (p[i]) CHECK(filled(p[i], sz[i], (uint32_t)i + (i % 2 ? 0 : 77)), "allocation %d does not hold what was written", i);
    for (int i = 0; i < N; i++) mspace_free(m, p[i]);
    const size_t before = mapped_bytes;
    hos_heap_trim();
    fprintf(stderr, "  all freed: %zu MiB mapped, %zu MiB after the trim\n", before >> 20, mapped_bytes >> 20);
    CHECK(mapped_bytes <= before, "the trim mapped more");
    destroy_mspace(m);
    CHECK(segment_count == 0 && mapped_bytes == 0, "%u segments left after the arena's end", segment_count);
    CHECK(same_as_saved() && all_reserved() && stats().blocks == 0, "the direct memory or the range not as before the arena");
}

struct worker { void *m; unsigned seed; unsigned nulls, bad; };
static void *worker_main(void *arg) {
    struct worker *w = arg;
    enum { LIVE = 200 };
    void *p[LIVE] = {0};
    size_t sz[LIVE] = {0};
    uint32_t tag[LIVE] = {0};
    for (int it = 0; it < 6000; it++) {
        const unsigned k = lcg(&w->seed) % LIVE;
        if (p[k]) {
            if (!filled(p[k], sz[k], tag[k])) w->bad++;
            mspace_free(w->m, p[k]);
            p[k] = NULL;
        }
        const unsigned r = lcg(&w->seed);
        sz[k] = r % 997 == 0 ? (size_t)(24 + r % 16) << 20 : 16 + r % (48 << 10);
        tag[k] = r ^ (uint32_t)it;
        p[k] = mspace_malloc(w->m, sz[k]);
        if (!p[k]) { w->nulls++; continue; }
        fill(p[k], sz[k], tag[k]);
    }
    for (int k = 0; k < LIVE; k++) {
        if (p[k] && !filled(p[k], sz[k], tag[k])) w->bad++;
        mspace_free(w->m, p[k]);
    }
    return NULL;
}
static void test_threads(void) {
    fprintf(stderr, "four threads, an arena each, over the pieces\n");
    fragment();
    save();
    pieces("1024");
    struct worker w[4];
    pthread_t t[4];
    for (int i = 0; i < 4; i++) {
        w[i].m = create_mspace(0, 1), w[i].seed = 1000u + (unsigned)i, w[i].nulls = w[i].bad = 0;
        CHECK(w[i].m != NULL, "arena %d not made", i);
        hos_heap_arena_note(w[i].m);
    }
    for (int i = 0; i < 4; i++)
        if (w[i].m) pthread_create(&t[i], NULL, worker_main, &w[i]);
    unsigned trims = 0;
    for (int k = 0; k < 20; k++) {   /* (the memory watch's trim, meanwhile) */
        usleep(2000);
        trims += hos_heap_trim();
    }
    for (int i = 0; i < 4; i++)
        if (w[i].m) pthread_join(t[i], NULL);
    for (int i = 0; i < 4; i++) {
        CHECK(!w[i].bad, "thread %d: %u allocations did not hold what was written", i, w[i].bad);
        CHECK(!w[i].nulls, "thread %d: %u allocations refused", i, w[i].nulls);
    }
    const struct hos_heap_pieces_stats s = stats();
    fprintf(stderr, "  %llu blocks of pieces made in all, %llu now; %u trims gave something back\n", (unsigned long long)s.made,
            (unsigned long long)s.blocks, trims);
    for (int i = 0; i < 4; i++)
        if (w[i].m) destroy_mspace(w[i].m);
    CHECK(segment_count == 0, "%u segments left", segment_count);
    CHECK(same_as_saved() && all_reserved() && stats().blocks == 0, "the direct memory or the range not as before the threads");
}

static void test_trim(void) {
    fprintf(stderr, "what dlmalloc gives back by itself after a burst, and what a trim adds\n");
    model_reset(0);
    save();
    pieces(NULL);
    void *m = create_mspace(0, 1), *keep = mspace_malloc(m, 100);
    enum { N = 4000 };
    static void *p[N];
    for (int i = 0; i < N; i++) p[i] = mspace_malloc(m, 48 << 10);
    void *late = mspace_malloc(m, 100);   /* (a small allocation made after the burst, kept) */
    const size_t peak = mapped_bytes;
    for (int i = 0; i < N; i++) mspace_free(m, p[i]);
    const size_t freed = mapped_bytes;
    mspace_trim(m, 0);
    const size_t trimmed = mapped_bytes;
    fprintf(stderr, "  burst of %d x 48 KiB: %zu MiB mapped at the peak, %zu after the frees (dlmalloc by itself), %zu after the trim\n",
            N, peak >> 20, freed >> 20, trimmed >> 20);
    CHECK(trimmed <= freed, "the trim mapped more");
    mspace_free(m, late);
    mspace_free(m, keep);
    destroy_mspace(m);
    CHECK(segment_count == 0 && same_as_saved(), "segments or units left");
}
#endif

int main(int argc, char **argv) {
    quiet = argc > 1 && !strcmp(argv[1], "-q");
    dm_fd = memfd_create("direct", 0);
    if (dm_fd < 0 || ftruncate(dm_fd, (off_t)DM_BYTES)) return perror("memfd"), 2;
    range = mmap(NULL, RANGE_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (range == MAP_FAILED) return perror("mmap"), 2;
    if ((uintptr_t)range & (U - 1)) {   /* (64 KiB aligned, as the heap's range) */
        munmap(range, RANGE_BYTES);
        char *r = mmap(NULL, RANGE_BYTES + U, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        range = (char *)(((uintptr_t)r + U - 1) & ~(uintptr_t)(U - 1));
    }
    for (size_t p = 0; p < RANGE_PAGES; p++) v2p[p] = -1, v_reserved[p] = 1;
    test_abundant();
    test_fragmented_off();
    test_fragmented_on();
    test_map_fails();
    test_small_pieces();
    test_slots();
    test_not_the_heap();
#ifdef HAVE_DLMALLOC
    test_dlmalloc();
    test_threads();
    test_trim();
#else
    fprintf(stderr, "SKIP: dlmalloc's tests (the SDK's src/dlmalloc/malloc.c not found)\n");
#endif
    CHECK(dm_bad == 0, "the model saw %ld wrong calls", dm_bad);
    fprintf(stderr, "%s: %d failures; %ld allocations and %ld maps asked of the model, %ld trail lines\n", fails ? "FAIL" : "ok", fails,
            alloc_calls, map_calls, trail_lines);
    return fails ? 1 : 0;
}
