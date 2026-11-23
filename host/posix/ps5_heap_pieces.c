/* (ps5, 2026-11-11) The host heap's blocks when the direct memory is in pieces.

   The title heap (PS5_PayloadSDK's platform/src/heap.c and dlmalloc, linked from the SDK's libps5platform.a) grows by
   segments it maps from one reserved range, each one allocation of direct memory, so each one piece of it: 16 MiB at a time
   (the archive's dlmalloc granularity), and a request of 32 MiB and more as a segment of its own size. At the end of the run
   of 11/11 at 20:04 (logs/udp/essai-syncshaders-spare32.txt) 304 MiB of direct memory were free but in 360 pieces, the
   largest 15 MiB: the heap could not grow by 16 MiB, fell back on libc 276 times, and the title ended on an uncaught
   std::bad_alloc. Before that, ACO's single allocations of 33 to 80 MiB (aco::optimize_postRA, a compile of 6.8 s) had been
   refused with 760 MiB free in pieces of 36 MiB at most: a compile faulted out of the driver, keeping 464 MiB, and three
   background jobs failed.

   The CPU reads and writes the heap's memory through its addresses only, which need not be one piece of direct memory: the
   guest window backs its range with units of 64 KiB mapped side by side. So, when the heap's own allocation of a segment's
   direct memory is refused, and only then, the segment is made of several pieces, as large as the free ones are, mapped
   side by side at the segment's place:
   - HOOKSHOT_PS5_HEAP_PIECES_KB=<n>: pieces of n KiB and more (rounded up to 64 KiB; the last one may be smaller, the rest of
     the length); 0 or unset (the default): as before, the heap's allocation fails and the heap falls back on libc. Read at
     each refusal, never before: hookshot_env.txt is loaded in main.
   - The heap keeps the segment's start of direct memory and gives it back to the two calls it makes with it, the mapping and
     the release: host/posix/ps5_platform.c's wraps of sceKernelAllocateDirectMemory, sceKernelMapDirectMemory and
     sceKernelReleaseDirectMemory hand a start of this file's (HP_TAG: past the 16 GiB of direct memory, never a real one)
     to hos_heap_pieces_map and hos_heap_pieces_release, and every other start to the kernel as before.
   - Only the heap's own allocation is made of pieces: the thread inside the heap's map call (ps5_platform.c's wrap of
     ps5p_heap_map marks it, hos_heap_mapper_enter), the whole range searched, 64 KiB aligned. The driver's memory, the
     window's units and the other reservations never are.
   - A piece is allocated as any allocation of direct memory is (ps5_platform.c: placed, counted by site, the free estimate
     kept); the floor was looked at for the whole block before the heap's first try.
   - A mapping that fails part way unmaps what it mapped and reserves it again, as the heap does when it unmaps a segment;
     the heap then releases the block, which gives every piece back.
   No heap call here (the callers hold the heap's arena lock and its segment lock): tables in .bss, a spin lock.

   Also here: the heap's arenas (ps5_platform.c's wrap of create_mspace notes them) and hos_heap_trim, dlmalloc's own trim of
   each (mspace_trim: the free top of an arena's last segment and its other segments that are all free go back), which the
   memory watch calls every HOOKSHOT_PS5_HEAP_TRIM_S seconds (host/hos_thread.c; 0 or unset: never). */
#if defined(__PROSPERO__) || defined(HOOKSHOT_PS5_HOST_MODEL)
#include "ps5.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* What it calls (ps5.h): hos_ps5_tid, hos_ps5_trail and the hos_ps5_piece_* calls, the console's (host/posix/ps5_platform.c)
   or a test's model of it (tests/test_heap_pieces.py): a piece of direct memory taken and given back as any allocation is
   (placed, counted by site), the kernel's sceKernelMapDirectMemory, an unmap that reserves the range again (the heap's range
   stays reserved), the largest free piece now in bytes (0: none, or no answer).
   And dlmalloc's trim (src/dlmalloc of the SDK, in libps5platform.a's mspace.o); weak: a link without the heap trims nothing */
int mspace_trim(void *msp, size_t pad) __attribute__((weak));

#define HP_UNIT ((size_t)1 << 16)
#define HP_FIXED 0x10   /* (PS5_KERNEL_MAP_FIXED) */
#define HP_BLOCKS 256
#define HP_PIECES 64
#define HP_MAPPERS 32
#define HP_ARENAS 16
/* a block's start as the heap keeps it: 0x7a << 40 plus its slot << 32 (the direct memory's offsets end at 16 GiB) */
#define HP_TAG ((uint64_t)0x7a << 40)
#define HP_ERROR ((int32_t)0x80020016)   /* (what a refused call answers here: the kernel's EINVAL) */

struct hp_block {
    int state;   /* 0 free, 1 being made, 2 made (the heap holds its start) */
    unsigned n;
    size_t length;
    int64_t start[HP_PIECES];
    uint32_t units[HP_PIECES];
};
static struct hp_block hp_blocks[HP_BLOCKS];
static int hp_lock;
static volatile int hp_mappers[HP_MAPPERS];   /* the threads inside the heap's map call (tid; 0: a free slot) */
static uint64_t hp_made, hp_given, hp_refused, hp_full, hp_map_failed, hp_bad, hp_pieces_made, hp_smallest;
static uint64_t hp_now, hp_bytes_now, hp_pieces_now;

static void hp_take_lock(void) { while (__atomic_exchange_n(&hp_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void hp_give_lock(void) { __atomic_store_n(&hp_lock, 0, __ATOMIC_RELEASE); }

int hos_heap_pieces_is(int64_t start) { return ((uint64_t)start >> 40) == (HP_TAG >> 40); }

/* the block a start names, made (state 2), or NULL */
static struct hp_block *hp_block_of(int64_t start) {
    if (!hos_heap_pieces_is(start) || ((uint64_t)start & 0xffffffffu)) return NULL;
    const uint64_t i = ((uint64_t)start - HP_TAG) >> 32;
    if (i >= HP_BLOCKS) return NULL;
    struct hp_block *b = &hp_blocks[i];
    return __atomic_load_n(&b->state, __ATOMIC_ACQUIRE) == 2 ? b : NULL;
}

int hos_heap_mapper_enter(void) {
    const int tid = hos_ps5_tid();
    for (int i = 0; i < HP_MAPPERS; i++) {
        int none = 0;
        if (!hp_mappers[i] && __atomic_compare_exchange_n(&hp_mappers[i], &none, tid, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return i;
    }
    return -1;   /* (more threads mapping at once than slots: this one's refusal stays a refusal) */
}
void hos_heap_mapper_leave(int slot) {
    if (slot >= 0 && slot < HP_MAPPERS) __atomic_store_n(&hp_mappers[slot], 0, __ATOMIC_RELEASE);
}
static int hp_is_mapper(int tid) {
    for (int i = 0; i < HP_MAPPERS; i++)
        if (__atomic_load_n(&hp_mappers[i], __ATOMIC_ACQUIRE) == tid) return 1;
    return 0;
}

/* HOOKSHOT_PS5_HEAP_PIECES_KB in bytes, rounded up to 64 KiB; 0: off */
size_t hos_heap_pieces_min(void) {
    const char *e = getenv("HOOKSHOT_PS5_HEAP_PIECES_KB");
    const unsigned long long kb = e && *e ? strtoull(e, NULL, 0) : 0;
    if (!kb || kb > ((unsigned long long)1 << 30)) return 0;
    return (((size_t)kb << 10) + HP_UNIT - 1) & ~(HP_UNIT - 1);
}

/* (the wrap of sceKernelAllocateDirectMemory, after the kernel's answer `refused`, non-zero) the block made of pieces when the
   heap's map call asked and the setting allows it: 0 and *start_out set; else `refused` as it was */
int32_t hos_heap_pieces_after_refusal(int32_t refused, int64_t search_start, int64_t search_end, size_t length, size_t alignment,
                                      int32_t type, int64_t *start_out) {
    if (!refused || !start_out || search_start != 0 || search_end <= 0 || alignment > HP_UNIT || length < 2 * HP_UNIT ||
        (length & (HP_UNIT - 1)))
        return refused;
    if (!hp_is_mapper(hos_ps5_tid())) return refused;
    const size_t least = hos_heap_pieces_min();
    if (!least) return refused;
    struct hp_block *b = NULL;
    unsigned slot = 0;
    hp_take_lock();
    for (; slot < HP_BLOCKS; slot++)
        if (!hp_blocks[slot].state) {
            b = &hp_blocks[slot];
            b->state = 1;
            break;
        }
    hp_give_lock();
    if (!b) {
        if (__atomic_fetch_add(&hp_full, 1, __ATOMIC_RELAXED) < 8)
            hos_ps5_trail("heap: a block of %llu KiB not made of pieces: all %d blocks of pieces are in use", (unsigned long long)(length >> 10),
                          HP_BLOCKS);
        return refused;
    }
    const size_t align = alignment ? alignment : HP_UNIT;
    uint64_t cap = hos_ps5_piece_largest() & ~(uint64_t)(HP_UNIT - 1);
    const uint64_t largest_then = cap;
    size_t left = length;
    unsigned n = 0, tries = 0;
    while (left && n < HP_PIECES && tries < 4 * HP_PIECES) {
        size_t size = left < cap ? left : (size_t)cap;
        size &= ~(HP_UNIT - 1);
        if (size < (left < least ? left : least)) break;   /* (no free piece of the least size, or of what is left) */
        int64_t at = -1;
        tries++;
        if (hos_ps5_piece_take(search_end, size, align, type, &at) == 0 && at >= 0) {
            b->start[n] = at, b->units[n] = (uint32_t)(size >> 16), n++;
            left -= size;
            continue;
        }
        /* (that size was not there: another thread took it, or it was never one piece) the largest one now, smaller than this */
        const uint64_t now = hos_ps5_piece_largest() & ~(uint64_t)(HP_UNIT - 1);
        cap = now < size ? now : (uint64_t)((size >> 1) & ~(HP_UNIT - 1));
    }
    if (left) {
        for (unsigned i = 0; i < n; i++) hos_ps5_piece_give(b->start[i], (size_t)b->units[i] << 16);
        __atomic_store_n(&b->state, 0, __ATOMIC_RELEASE);
        if (__atomic_fetch_add(&hp_refused, 1, __ATOMIC_RELAXED) < 16)
            hos_ps5_trail("heap: a block of %llu KiB could not be made of pieces of %llu KiB and more on thread %d: %llu KiB found in %u "
                          "pieces, the largest free piece %llu KiB (HOOKSHOT_PS5_HEAP_PIECES_KB)",
                          (unsigned long long)(length >> 10), (unsigned long long)(least >> 10), hos_ps5_tid(),
                          (unsigned long long)((length - left) >> 10), n, (unsigned long long)(largest_then >> 10));
        return refused;
    }
    b->n = n, b->length = length;
    uint32_t big = 0, small = ~0u;
    for (unsigned i = 0; i < n; i++) {
        if (b->units[i] > big) big = b->units[i];
        if (b->units[i] < small) small = b->units[i];
    }
    __atomic_store_n(&b->state, 2, __ATOMIC_RELEASE);
    __atomic_add_fetch(&hp_now, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&hp_bytes_now, length, __ATOMIC_RELAXED);
    __atomic_add_fetch(&hp_pieces_now, n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&hp_pieces_made, n, __ATOMIC_RELAXED);
    {
        uint64_t was = __atomic_load_n(&hp_smallest, __ATOMIC_RELAXED);
        while ((!was || small < was) &&
               !__atomic_compare_exchange_n(&hp_smallest, &was, (uint64_t)small, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    }
    if (__atomic_fetch_add(&hp_made, 1, __ATOMIC_RELAXED) < 40)
        hos_ps5_trail("heap: a block of %llu KiB made of %u pieces of direct memory on thread %d (the largest %llu KiB, the smallest "
                      "%llu KiB; the largest free piece was %llu KiB): the direct memory had no piece of that size "
                      "(HOOKSHOT_PS5_HEAP_PIECES_KB=%llu)",
                      (unsigned long long)(length >> 10), n, hos_ps5_tid(), (unsigned long long)big << 6, (unsigned long long)small << 6,
                      (unsigned long long)(largest_then >> 10), (unsigned long long)(least >> 10));
    *start_out = (int64_t)(HP_TAG + ((uint64_t)slot << 32));
    return 0;
}

/* (the wrap of sceKernelMapDirectMemory, for a start of this file's) each piece at its place, side by side from *address */
int32_t hos_heap_pieces_map(void **address, size_t length, int protection, int flags, int64_t start, size_t alignment) {
    struct hp_block *b = hp_block_of(start);
    if (!b || b->length != length || !address || !*address || !(flags & HP_FIXED) || ((uintptr_t)*address & (HP_UNIT - 1))) {
        if (__atomic_fetch_add(&hp_bad, 1, __ATOMIC_RELAXED) < 8)
            hos_ps5_trail("heap: a map of the block of pieces %#llx (%llu KiB at %p, flags %#x) refused: not one this file made so",
                          (unsigned long long)start, (unsigned long long)(length >> 10), address ? *address : NULL, flags);
        return HP_ERROR;
    }
    char *const at = (char *)*address;
    size_t off = 0;
    for (unsigned i = 0; i < b->n; i++) {
        const size_t size = (size_t)b->units[i] << 16;
        void *p = at + off;
        const int32_t r = hos_ps5_piece_map(&p, size, protection, flags, b->start[i], alignment);
        if (r != 0 || p != at + off) {
            if (r == 0) hos_ps5_piece_unmap(p, size, 0);   /* (placed elsewhere though fixed: given up) */
            if (off) hos_ps5_piece_unmap(at, off, 1);
            if (__atomic_fetch_add(&hp_map_failed, 1, __ATOMIC_RELAXED) < 8)
                hos_ps5_trail("heap: the block of pieces at %p (%llu KiB): its piece %u of %u not mapped (%#x, at %p): the block is not made",
                              (void *)at, (unsigned long long)(length >> 10), i + 1, b->n, (unsigned)r, p);
            return r ? r : HP_ERROR;
        }
        off += size;
    }
    return 0;
}

/* (the wrap of sceKernelReleaseDirectMemory, for a start of this file's) every piece given back, the block's slot free */
int32_t hos_heap_pieces_release(int64_t start, size_t length) {
    struct hp_block *b = hp_block_of(start);
    if (!b || b->length != length) {
        if (__atomic_fetch_add(&hp_bad, 1, __ATOMIC_RELAXED) < 8)
            hos_ps5_trail("heap: a release of the block of pieces %#llx (%llu KiB) refused: not one this file made so",
                          (unsigned long long)start, (unsigned long long)(length >> 10));
        return HP_ERROR;
    }
    for (unsigned i = 0; i < b->n; i++) hos_ps5_piece_give(b->start[i], (size_t)b->units[i] << 16);
    __atomic_sub_fetch(&hp_now, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&hp_bytes_now, b->length, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&hp_pieces_now, b->n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&hp_given, 1, __ATOMIC_RELAXED);
    b->n = 0, b->length = 0;
    __atomic_store_n(&b->state, 0, __ATOMIC_RELEASE);
    return 0;
}

/* (ps5.h's struct: made and held now; since the start) */
void hos_heap_pieces_stats(struct hos_heap_pieces_stats *s) {
    s->blocks = __atomic_load_n(&hp_now, __ATOMIC_RELAXED), s->bytes = __atomic_load_n(&hp_bytes_now, __ATOMIC_RELAXED);
    s->pieces = __atomic_load_n(&hp_pieces_now, __ATOMIC_RELAXED), s->made = __atomic_load_n(&hp_made, __ATOMIC_RELAXED);
    s->given_back = __atomic_load_n(&hp_given, __ATOMIC_RELAXED), s->refused = __atomic_load_n(&hp_refused, __ATOMIC_RELAXED);
    s->full = __atomic_load_n(&hp_full, __ATOMIC_RELAXED), s->map_failed = __atomic_load_n(&hp_map_failed, __ATOMIC_RELAXED);
    s->bad = __atomic_load_n(&hp_bad, __ATOMIC_RELAXED), s->pieces_made = __atomic_load_n(&hp_pieces_made, __ATOMIC_RELAXED);
    s->smallest_kib = __atomic_load_n(&hp_smallest, __ATOMIC_RELAXED) << 6;
}

/* ---- the heap's arenas, and dlmalloc's trim of them ---- */
static void *hp_arenas[HP_ARENAS];
static unsigned hp_arena_n;
void hos_heap_arena_note(void *mspace) {
    if (!mspace) return;
    const unsigned i = __atomic_fetch_add(&hp_arena_n, 1, __ATOMIC_RELAXED);
    if (i < HP_ARENAS) __atomic_store_n(&hp_arenas[i], mspace, __ATOMIC_RELEASE);
}
/* mspace_trim(arena, 0) on each arena made so far (each takes its arena's lock: never called with one held); the arenas
   that gave something back */
unsigned hos_heap_trim(void) {
    if (!mspace_trim) return 0;
    unsigned n = __atomic_load_n(&hp_arena_n, __ATOMIC_RELAXED), gave = 0;
    if (n > HP_ARENAS) n = HP_ARENAS;
    for (unsigned i = 0; i < n; i++) {
        void *const m = __atomic_load_n(&hp_arenas[i], __ATOMIC_ACQUIRE);
        if (m && mspace_trim(m, 0)) gave++;
    }
    return gave;
}
/* HOOKSHOT_PS5_HEAP_TRIM_S: seconds between two trims by the memory watch; 0 or unset: never */
unsigned hos_heap_trim_every(void) {
    const char *e = getenv("HOOKSHOT_PS5_HEAP_TRIM_S");
    const long s = e && *e ? strtol(e, NULL, 0) : 0;
    return s > 0 && s < 3600 ? (unsigned)s : 0;
}
#endif /* __PROSPERO__ || HOOKSHOT_PS5_HOST_MODEL */
