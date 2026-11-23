/* (ps5) host/posix/ps5_compile_heap.c on a model of the console's title heap (tests/test_compile_heap.py builds it with the SDK's
   dlmalloc 2.8.6 configured as the archive's mspace.o: 16 MiB granularity, 32 MiB mmap threshold, FOOTERS).

   The model: heap.c's range (first fit, 1,024 segments, a return only of whole recorded segments, exactly), two thread arenas
   given round robin as heap.c gives its eight, ps5_platform.c's wraps of ps5p_heap_map and ps5p_heap_unmap (the guards). The
   checks: off by default; on, a compile's allocations come from the compile arena, a paused call's from the thread's, a free from
   any thread goes back where the block came from; a compile arena's segments are never side by side and go back whole with their
   guards; a compile left by a jump is found stale at the next one; and the case of 15/11 in small: a compile mixed with another
   thread's kept allocations in one arena leaves its high-water mark in the heap, and with the setting on gives it back. Then
   threads compiling, pausing and freeing each other's blocks at once. */
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ps5.h"

void *create_mspace(size_t capacity, int locked);
void *mspace_malloc(void *msp, size_t bytes);
void *mspace_calloc(void *msp, size_t count, size_t bytes);
void *mspace_memalign(void *msp, size_t alignment, size_t bytes);
void *mspace_realloc(void *msp, void *p, size_t bytes);
void mspace_free(void *msp, void *p);
size_t mspace_footprint(void *msp);
int mspace_trim(void *msp, size_t pad);
void *__wrap_ps5_heap_malloc(size_t n);
void *__wrap_ps5_heap_calloc(size_t c, size_t n);
void *__wrap_ps5_heap_memalign(size_t a, size_t n);

static int quiet, failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define SAY(...) do { if (!quiet) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- heap.c's range ---- */
#define RANGE ((size_t)8 << 30)
#define UNIT ((size_t)1 << 16)
#define MAX_SEGMENTS 1024
static struct { size_t offset, bytes; } segs[MAX_SEGMENTS];
static unsigned nseg;
static uintptr_t base;
static size_t mapped;
static pthread_mutex_t seg_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t unmap_refused, guarded_refused;

static void *heap_map(size_t bytes) {
    bytes = (bytes + UNIT - 1) & ~(UNIT - 1);
    pthread_mutex_lock(&seg_lock);
    void *at = (void *)-1;
    unsigned slot = 0;
    size_t offset = 0;
    for (; slot < nseg; slot++) {
        if (segs[slot].offset - offset >= bytes) break;
        offset = segs[slot].offset + segs[slot].bytes;
    }
    if (nseg < MAX_SEGMENTS && offset <= RANGE && RANGE - offset >= bytes &&
        mmap((void *)(base + offset), bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED) {
        memmove(&segs[slot + 1], &segs[slot], (nseg - slot) * sizeof segs[0]);
        segs[slot].offset = offset, segs[slot].bytes = bytes;
        nseg++, mapped += bytes;
        at = (void *)(base + offset);
    }
    pthread_mutex_unlock(&seg_lock);
    return at;
}
static int heap_unmap(void *address, size_t bytes) {
    pthread_mutex_lock(&seg_lock);
    int r = -1;
    const size_t offset = (uintptr_t)address - base;
    unsigned first = 0;
    while (first < nseg && segs[first].offset < offset) first++;
    if ((uintptr_t)address >= base && first < nseg && segs[first].offset == offset) {
        unsigned last = first;
        size_t covered = 0;
        while (last < nseg && covered < bytes && segs[last].offset == offset + covered) covered += segs[last++].bytes;
        if (covered == bytes) {
            for (unsigned i = first; i < last; i++) {
                mmap((void *)(base + segs[i].offset), segs[i].bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
                mapped -= segs[i].bytes;
            }
            memmove(&segs[first], &segs[last], (nseg - last) * sizeof segs[0]);
            nseg -= last - first, r = 0;
        }
    }
    if (r) unmap_refused++;
    pthread_mutex_unlock(&seg_lock);
    return r;
}
/* ps5_platform.c's wraps (dlmalloc's MMAP and MUNMAP here) */
void *ps5p_heap_map(size_t bytes) {
    int slot = -1;
    const size_t guard = hos_compile_heap_guard_take(&slot);
    return hos_compile_heap_guard_set(slot, heap_map(bytes + 2 * guard), bytes);
}
int ps5p_heap_unmap(void *address, size_t bytes) {
    void *at = address;
    size_t length = bytes;
    const int slot = hos_compile_heap_guard_find(address, bytes, &at, &length);
    const int r = heap_unmap(at, length);
    if (r == 0) hos_compile_heap_guard_forget(slot);
    else if (slot >= 0) guarded_refused++;   /* (a guarded segment's whole return refused: never right) */
    return r;
}

/* ---- heap.c's arenas: two, given round robin at a thread's first allocation (three threads or more share one) ---- */
#define ARENAS 2
static void *arenas[ARENAS];
static pthread_key_t arena_key;
static unsigned arena_next;
static void *thread_arena(void) {
    uintptr_t slot = (uintptr_t)pthread_getspecific(arena_key);
    if (!slot) {
        slot = __atomic_fetch_add(&arena_next, 1, __ATOMIC_RELAXED) % ARENAS + 1;
        pthread_setspecific(arena_key, (void *)slot);
    }
    return arenas[slot - 1];
}
void *__real_ps5_heap_malloc(size_t n) { return mspace_malloc(thread_arena(), n); }
void *__real_ps5_heap_calloc(size_t c, size_t n) { return mspace_calloc(thread_arena(), c, n); }
void *__real_ps5_heap_memalign(size_t a, size_t n) { return mspace_memalign(thread_arena(), a, n); }
static void heap_free(void *p) { if (p) mspace_free(arenas[0], p); }   /* (FOOTERS: the block's own arena) */
/* heap_wrap.c's malloc, realloc, free and posix_memalign over the model, for ps5_compile_heap.c's allocator of the pipeline
   objects (test_compile_heap.py builds it with -Dmalloc=test_malloc and the others) */
void *test_malloc(size_t n) { return __wrap_ps5_heap_malloc(n); }
void test_free(void *p) { heap_free(p); }
void *test_realloc(void *p, size_t n) { return p ? mspace_realloc(arenas[0], p, n) : __wrap_ps5_heap_malloc(n); }
int test_posix_memalign(void **out, size_t a, size_t n) { *out = __wrap_ps5_heap_memalign(a, n); return *out ? 0 : 12; }
struct callbacks {   /* (VkAllocationCallbacks) */
    void *user;
    void *(*allocation)(void *user, size_t size, size_t alignment, int scope);
    void *(*reallocation)(void *user, void *original, size_t size, size_t alignment, int scope);
    void (*free)(void *user, void *memory);
    void *internal_allocation, *internal_free;
};

static uint64_t trail_lines;
void hos_ps5_trail(const char *fmt, ...) {
    trail_lines++;
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  trail: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
int hos_ps5_tid(void) { return (int)gettid(); }

static size_t compile_footprint(void) {
    struct hos_compile_heap_stats st;
    hos_compile_heap_stats(&st);
    return st.footprint;
}
static size_t arena_bytes(int i) { return mspace_footprint(arenas[i]); }
/* whether a block is in one of the compile arenas' segments (all guarded in these checks) */
static int in_compile_arena(const void *p) {
    int in = 0;
    pthread_mutex_lock(&seg_lock);
    for (unsigned i = 0; i < nseg && !in; i++) {
        const uintptr_t at = base + segs[i].offset;
        void *a;
        size_t l;
        if ((uintptr_t)p >= at && (uintptr_t)p < at + segs[i].bytes)
            in = hos_compile_heap_guard_find((void *)(at + UNIT), segs[i].bytes - 2 * UNIT, &a, &l) >= 0;
    }
    pthread_mutex_unlock(&seg_lock);
    return in;
}

/* a compile in small: many blocks of NIR's and ACO's sizes, most freed at the end; `keep` blocks made while paused (what the
   compile keeps), returned to the caller */
static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) { rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17; return rng; }
static void compile_like(size_t total, void **kept, int keep, void (*between)(void)) {
    enum { MAXB = 200000 };
    static void *blocks[MAXB];
    int n = 0;
    size_t got = 0;
    int k = 0;
    const int token = hos_compile_heap_enter();
    while (got < total && n < MAXB) {
        const uint64_t x = next();
        size_t sz = (size_t)32 << (x % 12);   /* 32 B to 64 KiB */
        if (x % 97 == 0) sz = (size_t)(1 + x % 3) << 20;   /* now and then 1 to 3 MiB (reallocs of NIR's arrays) */
        void *p = (x & 1) ? __wrap_ps5_heap_malloc(sz) : __wrap_ps5_heap_calloc(1, sz);
        if (!p) break;
        memset(p, 0xa5, sz < 64 ? sz : 64);
        blocks[n++] = p, got += sz;
        if (k < keep && n % (MAXB / 64) == 7) {
            hos_compile_heap_pause();
            kept[k++] = __wrap_ps5_heap_malloc(1500 + (size_t)(x % 4000));
            hos_compile_heap_resume();
        }
        if (between && n % 512 == 0) between();
    }
    while (k < keep) {
        hos_compile_heap_pause();
        kept[k++] = __wrap_ps5_heap_malloc(2048);
        hos_compile_heap_resume();
    }
    for (int i = 0; i < n; i++) heap_free(blocks[i]);
    hos_compile_heap_leave(token);
}

/* another thread of the compile's arena, which keeps some of what it allocates (here: called between the compile's
   allocations, on the compile's thread but outside its scope, as heap.c gives the arena to both) */
static void *kept_by_others[100000];
static int kept_by_others_n;
static void other_thread_allocates(void) {
    hos_compile_heap_pause();   /* (outside the compile: what the game's threads allocate, in the arena they share) */
    for (int i = 0; i < 4 && kept_by_others_n < 100000; i++) kept_by_others[kept_by_others_n++] = __wrap_ps5_heap_malloc(96 + (next() % 2000));
    hos_compile_heap_resume();
}

__attribute__((noinline)) static void inner_compile(struct hos_compile_heap_stats *after) {
    volatile char deeper[256];
    deeper[0] = 1;
    const int b = hos_compile_heap_enter();
    void *p = __wrap_ps5_heap_malloc(100 + deeper[0]);
    heap_free(p);
    hos_compile_heap_leave(b);
    hos_compile_heap_stats(after);
}

static jmp_buf jump;
__attribute__((noinline)) static void compile_that_faults(void) {
    const int token = hos_compile_heap_enter();
    (void)token;
    void *p = __wrap_ps5_heap_malloc(4096);
    (void)p;
    longjmp(jump, 1);   /* (the renderer's guard jumps out of the driver: the scope is never left) */
}

/* ---- threads at once ---- */
struct worker { int id, rounds; void **out; int nout; };
static void *shared_blocks[4096];
static int shared_lock;
static void *worker_main(void *arg) {
    struct worker *w = arg;
    uint64_t x = 0x1234567ull * (uint64_t)(w->id + 1);
    for (int r = 0; r < w->rounds; r++) {
        const int t = hos_compile_heap_enter();
        void *mine[512];
        for (int i = 0; i < 512; i++) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            mine[i] = (x & 3) == 0 ? __wrap_ps5_heap_memalign(64 << (x % 4), 100 + x % 9000) : __wrap_ps5_heap_malloc(16 + x % 30000);
            if (mine[i]) memset(mine[i], w->id, 16);
            if (i % 64 == 0) {   /* a block another thread frees, and one it made that this one frees */
                while (__atomic_exchange_n(&shared_lock, 1, __ATOMIC_ACQUIRE)) ;
                const unsigned j = (unsigned)(x >> 20) % 4096;
                void *theirs = shared_blocks[j];
                shared_blocks[j] = __wrap_ps5_heap_malloc(256);
                __atomic_store_n(&shared_lock, 0, __ATOMIC_RELEASE);
                heap_free(theirs);
            }
            if (i % 100 == 50) {
                hos_compile_heap_pause();
                void *k = __wrap_ps5_heap_malloc(1000);
                hos_compile_heap_resume();
                heap_free(k);
            }
            if (i % 128 == 3) mine[i] = mspace_realloc(arenas[0], mine[i], 40000);   /* (a block resized grows where it was) */
        }
        for (int i = 0; i < 512; i++) heap_free(mine[i]);
        hos_compile_heap_leave(t);
    }
    return NULL;
}

int main(int argc, char **argv) {
    quiet = argc > 1 && !strcmp(argv[1], "-q");
    void *r = mmap(NULL, RANGE + UNIT, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) return 2;
    base = ((uintptr_t)r + UNIT - 1) & ~(uintptr_t)(UNIT - 1);
    pthread_key_create(&arena_key, NULL);
    for (int i = 0; i < ARENAS; i++) arenas[i] = create_mspace(0, 1);
    const int mode = argc > 2 ? atoi(argv[2]) : 0;   /* (0: the off checks then the on checks, in two processes: test_compile_heap.py) */

    if (mode == 0) {
        /* ---- off (unset) ---- */
        CHECK(!hos_compile_heap_enter(), "the setting unset, a compile scope is entered");
        CHECK(!hos_compile_heap_allocator(NULL), "the setting unset, the driver is given an allocator");
        void *kept[8];
        const size_t before = mapped;
        kept_by_others_n = 0;
        compile_like((size_t)300 << 20, kept, 8, other_thread_allocates);
        const size_t after = mapped;
        SAY("off: the heap %zu MiB before a compile of 300 MiB mixed with another thread's kept blocks, %zu MiB after", before >> 20, after >> 20);
        CHECK(compile_footprint() == 0, "the setting unset, a compile arena was made");
        /* dlmalloc's own trim: what it gives back of an arena where the other thread kept blocks */
        for (int i = 0; i < ARENAS; i++) mspace_trim(arenas[i], 0);
        SAY("off: after dlmalloc's trim of the arenas, %zu MiB", mapped >> 20);
        CHECK(mapped >= before + ((size_t)150 << 20), "off: the model does not keep the compile's high-water mark (%zu -> %zu MiB): "
              "the case it stands for is not made", before >> 20, mapped >> 20);
        printf("off_after_mib %zu\n", mapped >> 20);
        for (int i = 0; i < 8; i++) heap_free(kept[i]);
        for (int i = 0; i < kept_by_others_n; i++) heap_free(kept_by_others[i]);
        return failures ? 1 : 0;
    }

    /* ---- on ---- */
    CHECK(getenv("HOOKSHOT_PS5_COMPILE_HEAP") && atoi(getenv("HOOKSHOT_PS5_COMPILE_HEAP")) == 1, "mode 1 runs with the setting at 1");
    /* (HOOKSHOT_PS5_COMPILE_KEEP_MB: an arena holding that or less after a compile is not trimmed; 64 by default) */
    const char *keep_env = getenv("HOOKSHOT_PS5_COMPILE_KEEP_MB");
    const size_t keep = (size_t)(keep_env && *keep_env ? atoi(keep_env) : 64) << 20;
    {   /* a compile's allocations come from the compile arena; a paused call's from the thread's arena */
        const size_t a0 = arena_bytes(0) + arena_bytes(1);
        const int t = hos_compile_heap_enter();
        CHECK(t == 1, "the setting on, the scope is not entered");
        void *big[40];
        for (int i = 0; i < 40; i++) big[i] = __wrap_ps5_heap_malloc((size_t)1 << 20);
        CHECK(compile_footprint() >= ((size_t)40 << 20), "40 MiB allocated in a compile: the compile arena holds %zu MiB",
              compile_footprint() >> 20);
        CHECK(arena_bytes(0) + arena_bytes(1) == a0, "a compile's allocations grew a thread's arena");
        hos_compile_heap_pause();
        void *k = __wrap_ps5_heap_malloc((size_t)20 << 20);
        hos_compile_heap_resume();
        CHECK(arena_bytes(0) + arena_bytes(1) >= a0 + ((size_t)20 << 20), "a paused call's allocation is not in the thread's arena");
        /* guards: no two of the compile arena's segments side by side; each one recorded */
        struct hos_compile_heap_stats st;
        hos_compile_heap_stats(&st);
        CHECK(st.guarded >= 3, "the compile arena's segments are not guarded (%llu)", (unsigned long long)st.guarded);
        int adjacent = 0;
        pthread_mutex_lock(&seg_lock);
        for (unsigned i = 0; i + 1 < nseg; i++) {
            void *a;
            size_t l;
            const int g1 = hos_compile_heap_guard_find((void *)(base + segs[i].offset + UNIT), segs[i].bytes - 2 * UNIT, &a, &l) >= 0;
            if (g1 && segs[i].offset + segs[i].bytes == segs[i + 1].offset) {
                /* (side by side in the range is fine: the guards are between what dlmalloc was given) */
                const uintptr_t end_given = base + segs[i].offset + segs[i].bytes - UNIT, next_given = base + segs[i + 1].offset + UNIT;
                if (end_given == next_given) adjacent++;
            }
        }
        pthread_mutex_unlock(&seg_lock);
        CHECK(!adjacent, "two guarded segments touch");
        for (int i = 0; i < 40; i++) heap_free(big[i]);
        heap_free(k);
        hos_compile_heap_leave(t);
        hos_compile_heap_stats(&st);
        CHECK(st.footprint <= ((size_t)34 << 20) + keep, "after the compile, its arena holds %llu MiB (its first and newest segments "
              "at most, and what it may keep)", (unsigned long long)(st.footprint >> 20));
        CHECK(keep || st.trims >= 1, "the compile's end did not trim its arena");
        CHECK(!guarded_refused, "the heap refused %llu whole returns of guarded segments", (unsigned long long)guarded_refused);
    }
    {   /* the pipeline object's allocator: given when the renderer gives none; what outlives the call in the thread's arena,
           a command's temporaries in the compile arena; realloc keeps a block where it is; free from it frees it */
        int mine;
        CHECK(hos_compile_heap_allocator(&mine) == &mine, "the renderer's own allocator is not passed through");
        const struct callbacks *cb = hos_compile_heap_allocator(NULL);
        CHECK(cb && cb->allocation && cb->reallocation && cb->free && !cb->internal_allocation, "no allocator for the driver's objects");
        if (cb) {
            const int t = hos_compile_heap_enter();
            void *object = cb->allocation(NULL, (size_t)20 << 20, 8, 1);   /* VK_SYSTEM_ALLOCATION_SCOPE_OBJECT */
            void *small = cb->allocation(NULL, 3000, 8, 1);
            CHECK(object && small && !in_compile_arena(object) && !in_compile_arena(small), "a pipeline object went to the compile arena");
            void *command = cb->allocation(NULL, (size_t)20 << 20, 64, 0);   /* VK_SYSTEM_ALLOCATION_SCOPE_COMMAND, aligned */
            CHECK(command && !((uintptr_t)command & 63) && in_compile_arena(command), "a command's temporary is not in the compile arena, or not aligned");
            void *plain = __wrap_ps5_heap_malloc(5000);
            CHECK(plain && in_compile_arena(plain), "the compile's own allocation is not in the compile arena");
            heap_free(plain);
            object = cb->reallocation(NULL, object, (size_t)21 << 20, 8, 1);
            CHECK(object && !in_compile_arena(object), "a pipeline object's realloc went to the compile arena");
            cb->free(NULL, small);
            cb->free(NULL, command);
            cb->free(NULL, object);
            hos_compile_heap_leave(t);
        }
    }
    {   /* the case of 15/11 in small, with the setting on */
        void *kept[8];
        const size_t before = mapped;
        kept_by_others_n = 0;
        compile_like((size_t)300 << 20, kept, 8, other_thread_allocates);
        const size_t after = mapped;
        SAY("on: the heap %zu MiB before the same compile, %zu MiB after (the compile arena %zu MiB)", before >> 20, after >> 20,
            compile_footprint() >> 20);
        CHECK(after <= before + ((size_t)48 << 20) + keep, "on: the compile left %zu MiB in the heap", (after - before) >> 20);
        printf("on_after_mib %zu\n", after >> 20);
        for (int i = 0; i < 8; i++) heap_free(kept[i]);
        for (int i = 0; i < kept_by_others_n; i++) heap_free(kept_by_others[i]);
    }
    {   /* a compile left by a jump: stale at the next one, and the arena's count right again */
        struct hos_compile_heap_stats st0, st1;
        hos_compile_heap_stats(&st0);
        if (!setjmp(jump)) compile_that_faults();
        const int t = hos_compile_heap_enter();
        void *p = __wrap_ps5_heap_malloc(1000);
        heap_free(p);
        hos_compile_heap_leave(t);
        hos_compile_heap_stats(&st1);
        CHECK(st1.stale == st0.stale + 1, "a scope left by a jump is not found stale");
        CHECK(keep || st1.trims > st0.trims, "after a stale scope, the next compile's end does not trim (the arena's count is off)");
        hos_compile_heap_pause();   /* (outside any scope: nothing to pause) */
        hos_compile_heap_resume();
    }
    {   /* nested scopes (an inner one from a deeper frame, as the driver calling its own entry point): one trim, at the outer end */
        struct hos_compile_heap_stats st0, st1;
        hos_compile_heap_stats(&st0);
        const int a = hos_compile_heap_enter();
        inner_compile(&st1);
        CHECK(st1.trims == st0.trims && st1.stale == st0.stale, "the inner scope's end trimmed, or the inner scope was taken as stale");
        hos_compile_heap_leave(a);
        hos_compile_heap_stats(&st1);
        CHECK(keep || st1.trims == st0.trims + 1, "the outer scope's end did not trim");
    }
    {   /* threads at once */
        enum { N = 4 };
        pthread_t th[N];
        struct worker w[N];
        const size_t before = mapped;
        struct hos_compile_heap_stats st0;
        hos_compile_heap_stats(&st0);
        for (int i = 0; i < N; i++) w[i] = (struct worker){.id = i + 1, .rounds = 60}, pthread_create(&th[i], NULL, worker_main, &w[i]);
        for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
        for (int i = 0; i < 4096; i++) heap_free(shared_blocks[i]), shared_blocks[i] = NULL;
        for (int i = 0; i < ARENAS; i++) mspace_trim(arenas[i], 0);
        const int t = hos_compile_heap_enter();
        hos_compile_heap_leave(t);
        struct hos_compile_heap_stats st;
        hos_compile_heap_stats(&st);
        SAY("threads: the heap %zu MiB before, %zu MiB after %d threads x 60 compiles (compile arenas %llu MiB, %llu scopes, %llu stale)",
            before >> 20, mapped >> 20, N, (unsigned long long)(st.footprint >> 20), (unsigned long long)st.scopes,
            (unsigned long long)(st.stale - st0.stale));
        CHECK(st.stale == st0.stale, "threads at once: stale scopes (%llu)", (unsigned long long)(st.stale - st0.stale));
        CHECK(st.scopes >= st0.scopes + N * 60, "threads at once: %llu scopes", (unsigned long long)(st.scopes - st0.scopes));
        CHECK(!guarded_refused, "threads at once: the heap refused %llu whole returns of guarded segments", (unsigned long long)guarded_refused);
        CHECK(mapped <= before + ((size_t)64 << 20) + keep, "threads at once: %zu MiB more in the heap after", (mapped - before) >> 20);
    }
    if (!quiet || failures) fprintf(stderr, "%s (%llu trail lines)\n", failures ? "FAILED" : "all checks passed", (unsigned long long)trail_lines);
    return failures ? 1 : 0;
}
