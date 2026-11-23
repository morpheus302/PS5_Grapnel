/* (ps5, 2026-11-15) The driver's compiles in arenas of their own, given back to the console after each.

   Why: the run of 15/11 (logs/udp/essai-lot33-1.txt, 75 min) ended at the memory floor with the host heap at 1,848 MiB. Its
   last step was the driver compiling graphics pipelines met in Castle Town (4,368 to 4,390 s, two workers at once): the heap
   went from 1,544 to 1,816 MiB (peak 1,928) and kept it. A compile asks for a lot and frees nearly all of it at its end
   (NIR's copy propagation, nir_opt_copy_prop_vars, and ACO: hundreds of MiB), but the title heap (PS5_PayloadSDK's heap.c
   over dlmalloc 2.8.6, eight arenas shared round robin by the title's ~126 threads, segments of 16 MiB) gives a segment back
   only when it is all free and is not its arena's first or newest: the compile's memory is mixed with what the other threads
   of its arena allocate meanwhile and keep, and dlmalloc joins a segment mapped next to the one before into one, which the
   arena's state then pins (notes-agents/tas-hote-fragmentation.md). So each compile left its high-water mark in the heap.

   What this does, with HOOKSHOT_PS5_COMPILE_HEAP=1 (0 or unset, the default: nothing changes; read at the first compile):
   - a compile is a call of the driver's vkCreateGraphicsPipelines or vkCreateComputePipelines (the link wraps RADV's
     radv_CreateGraphicsPipelines and radv_CreateComputePipelines, nx2/toolchain_ps5.py): while a thread is inside one, what it
     allocates comes from a compile arena (an mspace of this file's, from the same heap range, HOOKSHOT_PS5_COMPILE_ARENAS of
     them, 1 to 4, 1 by default, given to the compiling threads in turn), not from its own arena. The link wraps the heap's
     ps5_heap_malloc, ps5_heap_calloc and ps5_heap_memalign (heap_wrap.o's malloc, calloc and posix_memalign call them);
     a block keeps its arena (dlmalloc's FOOTERS): freed or resized from any thread, it goes back to the arena it came from.
     Not dlmalloc's mspace_malloc itself: its own memalign and realloc call it for the arena they work in, and lld wraps a
     call inside the object that defines the symbol too (tested 15/11: a wrapped foo called by bar beside it went through
     the wrap);
   - what a compile makes and keeps is allocated in the thread's own arena as before: inside the driver's radv_shader_create
     (a compiled shader and its copy in the driver's cache), radv_pipeline_cache_insert (the pipeline's entry in a cache) and
     vk_pipeline_cache_lookup_object (a shader read back from a cache, on a hit), the compile arena is paused; and the pipeline
     object itself, which the driver allocates with the call's VkAllocationCallbacks: when the renderer gives none (it never
     does), the wraps give it one of this file's, the device's own calls (malloc, realloc, free) with the compile arena paused
     for anything but a command's temporaries. What is left in a compile arena after a compile is what nobody kept;
   - every segment a compiling thread maps (not paused) is mapped with 64 KiB of guard on each side (ps5_platform.c's wraps of
     ps5p_heap_map and ps5p_heap_unmap ask the heap for 128 KiB more and hand dlmalloc the middle): two segments of the
     compile arena are never side by side, so dlmalloc never joins them, and a segment all free goes back whole;
   - dlmalloc gives a compile arena's free segments back by itself (each 4,095 frees, and when its top is large); and when the
     last compile in an arena ends with the arena holding more than HOOKSHOT_PS5_COMPILE_KEEP_MB (64 MiB by default; 0: after
     every compile), dlmalloc's trim of that arena (mspace_trim) gives back every other segment that is all free. What stays:
     its first segment (dlmalloc's state is in it), its newest one, and up to the threshold. (Measured on the PC, 15/11: a trim
     after every compile made the compiles slower, +30% in the one measure on a quiet PC, the arena mapped again and touched
     afresh each time; with 64 MiB kept, only the heavy compiles are followed by a trim, for 32 MiB more held.)
   A compile that faults out of the driver (the renderer's guard jumps out of the call, vk_draw.cpp guarded_graphics) never
   leaves its scope: the thread's next compile finds the scope from a frame it is not inside and starts again from 0; until
   then, what that thread allocates goes to the compile arena (still a valid block of the heap, freed as any).
   No allocation here (the callers allocate through it): tables in .bss, spin locks, pthread_getspecific as heap.c itself
   uses on every allocation. */
#if defined(__PROSPERO__) || defined(HOOKSHOT_PS5_HOST_MODEL)
#include "ps5.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* dlmalloc (the SDK's mspace.o): create_mspace is wrapped by ps5_platform.c (its arenas noted for hos_heap_trim) */
void *create_mspace(size_t capacity, int locked);
void *mspace_malloc(void *msp, size_t bytes);
void *mspace_calloc(void *msp, size_t count, size_t bytes);
void *mspace_memalign(void *msp, size_t alignment, size_t bytes);
int mspace_trim(void *msp, size_t pad) __attribute__((weak));
size_t mspace_footprint(void *msp) __attribute__((weak));
/* the heap's own (heap.o), under the link's --wrap */
void *__real_ps5_heap_malloc(size_t bytes);
void *__real_ps5_heap_calloc(size_t count, size_t bytes);
void *__real_ps5_heap_memalign(size_t alignment, size_t bytes);

#define CH_SLOTS 64     /* threads that ever compiled (the workers, the presentation thread for RADV's own pipelines, ...) */
#define CH_ARENAS 4
#define CH_GUARD ((size_t)1 << 16)
#define CH_GUARDED 512  /* segments with guards held at once (a compile maps tens) */

struct ch_slot {
    int arena;          /* the compile arena this thread uses */
    int depth;          /* compile scopes it is inside */
    int paused;         /* inside a call that makes what the compile keeps */
    int making;         /* making its arena: its first segment is guarded too */
    uintptr_t frame;    /* the outermost scope's frame */
    uint64_t t0_ns;
};
static struct ch_slot ch_slots[CH_SLOTS];
static unsigned ch_slot_n;
static pthread_key_t ch_key;
static int ch_state;    /* 0 not read, 1 off, 2 on, 3 being read */
static unsigned ch_arena_n = 1;
static size_t ch_keep;   /* HOOKSHOT_PS5_COMPILE_KEEP_MB: an arena holding this or less after a compile is not trimmed */

struct ch_arena {
    void *m;
    int state;          /* 0 none, 1 being made, 2 made, 3 refused */
    int active;         /* threads inside a compile scope on it */
    size_t peak;        /* its footprint's most, after a compile */
};
static struct ch_arena ch_arenas[CH_ARENAS];

static struct { uintptr_t at; size_t bytes; int state; } ch_guarded[CH_GUARDED];   /* state 0 free, 1 taken, 2 holds a segment */
static int ch_guarded_n;

static struct {
    uint64_t scopes, stale, trims, trims_gave, given_bytes, guarded_made, guarded_none, routed_refused, compile_ns;
    uint64_t told_at_ns, told;
} ch_st;

static uint64_t ch_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* the setting, once, by the first thread that compiles (hookshot_env.txt is loaded in main, before the renderer starts) */
static int ch_ready(void) {
    int s = __atomic_load_n(&ch_state, __ATOMIC_ACQUIRE);
    if (s == 1 || s == 2) return s == 2;
    if (s == 0 && __atomic_compare_exchange_n(&ch_state, &s, 3, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        const char *e = getenv("HOOKSHOT_PS5_COMPILE_HEAP");
        int on = e && *e && atoi(e) > 0;
        const char *n = getenv("HOOKSHOT_PS5_COMPILE_ARENAS");
        const int arenas = n && *n ? atoi(n) : 1;
        ch_arena_n = arenas < 1 ? 1 : arenas > CH_ARENAS ? CH_ARENAS : (unsigned)arenas;
        const char *k = getenv("HOOKSHOT_PS5_COMPILE_KEEP_MB");
        ch_keep = (size_t)(k && *k ? (atoi(k) > 0 ? atoi(k) : 0) : 64) << 20;   /* (64 MiB by default; 0: trimmed after every compile) */
        if (on && pthread_key_create(&ch_key, NULL) != 0) on = 0;
        __atomic_store_n(&ch_state, on ? 2 : 1, __ATOMIC_RELEASE);
        if (on)
            hos_ps5_trail("heap: the driver's compiles allocate from %u arena(s) of their own, trimmed after a compile when one holds "
                          "more than %llu MiB; their segments guarded by 64 KiB each side; what a compile keeps (shaders, pipeline "
                          "objects, cache entries) stays in the thread's arena (HOOKSHOT_PS5_COMPILE_HEAP=1, "
                          "HOOKSHOT_PS5_COMPILE_ARENAS, HOOKSHOT_PS5_COMPILE_KEEP_MB)", ch_arena_n, (unsigned long long)(ch_keep >> 20));
        return on;
    }
    while ((s = __atomic_load_n(&ch_state, __ATOMIC_ACQUIRE)) == 3) {
        const struct timespec pause = {0, 50000};
        nanosleep(&pause, NULL);
    }
    return s == 2;
}

static struct ch_slot *ch_slot_peek(void) {
    if (__atomic_load_n(&ch_state, __ATOMIC_ACQUIRE) != 2) return NULL;
    const uintptr_t v = (uintptr_t)pthread_getspecific(ch_key);
    return v ? &ch_slots[v - 1] : NULL;
}

/* the compile arena this thread allocates from now, or NULL: the hot path of every allocation once a compile ran */
static void *ch_route(void) {
    const struct ch_slot *s = ch_slot_peek();
    if (!s || s->depth <= 0 || s->paused) return NULL;
    return __atomic_load_n(&ch_arenas[s->arena].m, __ATOMIC_ACQUIRE);
}

void *__wrap_ps5_heap_malloc(size_t bytes) {
    void *const m = ch_route();
    if (m) {
        void *const p = mspace_malloc(m, bytes);
        if (p) return p;
        __atomic_add_fetch(&ch_st.routed_refused, 1, __ATOMIC_RELAXED);   /* (no segment for it: the thread's arena tries) */
    }
    return __real_ps5_heap_malloc(bytes);
}
void *__wrap_ps5_heap_calloc(size_t count, size_t bytes) {
    void *const m = ch_route();
    if (m) {
        void *const p = mspace_calloc(m, count, bytes);
        if (p) return p;
        __atomic_add_fetch(&ch_st.routed_refused, 1, __ATOMIC_RELAXED);
    }
    return __real_ps5_heap_calloc(count, bytes);
}
void *__wrap_ps5_heap_memalign(size_t alignment, size_t bytes) {
    void *const m = ch_route();
    if (m) {
        void *const p = mspace_memalign(m, alignment, bytes);
        if (p) return p;
        __atomic_add_fetch(&ch_st.routed_refused, 1, __ATOMIC_RELAXED);
    }
    return __real_ps5_heap_memalign(alignment, bytes);
}

/* this thread's slot, given at its first compile (pthread_setspecific may allocate: no lock is held here) */
static struct ch_slot *ch_slot_self(void) {
    struct ch_slot *s = ch_slot_peek();
    if (s) return s;
    const unsigned i = __atomic_fetch_add(&ch_slot_n, 1, __ATOMIC_RELAXED);
    if (i >= CH_SLOTS) return NULL;   /* (no slot: this thread's compiles stay in its own arena, as before) */
    ch_slots[i].arena = (int)(i % ch_arena_n);
    if (pthread_setspecific(ch_key, (void *)(uintptr_t)(i + 1)) != 0) return NULL;
    return &ch_slots[i];
}

/* the arena, made by the first thread that needs it (its first segment guarded: the thread is marked as making it) */
static void *ch_arena_made(struct ch_slot *s) {
    struct ch_arena *a = &ch_arenas[s->arena];
    int st = __atomic_load_n(&a->state, __ATOMIC_ACQUIRE);
    if (st == 0 && __atomic_compare_exchange_n(&a->state, &st, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        s->making = 1;
        void *const m = create_mspace(0, 1);
        s->making = 0;
        __atomic_store_n(&a->m, m, __ATOMIC_RELEASE);
        __atomic_store_n(&a->state, m ? 2 : 3, __ATOMIC_RELEASE);
        if (!m) hos_ps5_trail("heap: the driver's compile arena %d could not be made: compiles stay in their threads' arenas", s->arena);
        return m;
    }
    while ((st = __atomic_load_n(&a->state, __ATOMIC_ACQUIRE)) == 1) {
        const struct timespec pause = {0, 50000};
        nanosleep(&pause, NULL);
    }
    return st == 2 ? __atomic_load_n(&a->m, __ATOMIC_ACQUIRE) : NULL;
}

/* (noinline: its frame is the scope's mark, below its caller's) */
__attribute__((noinline)) int hos_compile_heap_enter(void) {
    if (!ch_ready()) return 0;
    struct ch_slot *const s = ch_slot_self();
    if (!s) return 0;
    const uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    if (s->depth > 0 && frame >= s->frame) {
        /* (a scope left by a jump out of the driver: not one this call is inside) */
        __atomic_add_fetch(&ch_st.stale, 1, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&ch_arenas[s->arena].active, 1, __ATOMIC_ACQ_REL);
        s->depth = 0, s->paused = 0;
    }
    if (s->depth == 0) {
        if (!ch_arena_made(s)) return 0;
        s->frame = frame, s->t0_ns = ch_now_ns();
        __atomic_add_fetch(&ch_arenas[s->arena].active, 1, __ATOMIC_ACQ_REL);
        __atomic_add_fetch(&ch_st.scopes, 1, __ATOMIC_RELAXED);
    }
    s->depth++;
    return 1;
}

static void ch_trimmed(struct ch_arena *a, int index, size_t before, size_t after) {
    if (after > a->peak) a->peak = after;
    __atomic_add_fetch(&ch_st.trims, 1, __ATOMIC_RELAXED);
    if (before > after) {
        __atomic_add_fetch(&ch_st.trims_gave, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&ch_st.given_bytes, before - after, __ATOMIC_RELAXED);
    }
    /* a line now and then: the first 8 trims that gave 16 MiB or more back, then at most one each 60 s */
    const uint64_t now = ch_now_ns();
    uint64_t last = __atomic_load_n(&ch_st.told_at_ns, __ATOMIC_RELAXED);
    const int big = before >= after + ((size_t)16 << 20);
    if ((big && ch_st.told < 8) || (last && now - last >= 60000000000ull) || !last) {
        if (!__atomic_compare_exchange_n(&ch_st.told_at_ns, &last, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
        ch_st.told++;
        hos_ps5_trail("heap: the driver's compile arena %d: %llu MiB after a compile (it held %llu; its most %llu); so far %llu "
                      "compiles in %.1f s, %llu MiB given back by %llu of %llu trims, %llu guarded segments (%llu without a guard), "
                      "%llu scopes left by a jump, %llu allocations it could not make",
                      index, (unsigned long long)(after >> 20), (unsigned long long)(before >> 20),
                      (unsigned long long)(a->peak >> 20), (unsigned long long)ch_st.scopes, ch_st.compile_ns / 1e9,
                      (unsigned long long)(ch_st.given_bytes >> 20), (unsigned long long)ch_st.trims_gave,
                      (unsigned long long)ch_st.trims, (unsigned long long)ch_st.guarded_made,
                      (unsigned long long)ch_st.guarded_none, (unsigned long long)ch_st.stale,
                      (unsigned long long)ch_st.routed_refused);
    }
}

void hos_compile_heap_leave(int token) {
    if (!token) return;
    struct ch_slot *const s = ch_slot_peek();
    if (!s || s->depth <= 0 || --s->depth) return;
    s->paused = 0;
    __atomic_add_fetch(&ch_st.compile_ns, ch_now_ns() - s->t0_ns, __ATOMIC_RELAXED);
    struct ch_arena *const a = &ch_arenas[s->arena];
    if (__atomic_sub_fetch(&a->active, 1, __ATOMIC_ACQ_REL) || !a->m || !mspace_trim) return;
    const size_t before = mspace_footprint ? mspace_footprint(a->m) : 0;
    if (ch_keep && before <= ch_keep) return;   /* (small enough: kept for the next compile, not mapped again) */
    mspace_trim(a->m, 0);
    ch_trimmed(a, s->arena, before, mspace_footprint ? mspace_footprint(a->m) : 0);
}

/* around a call that makes what the compile keeps: the thread's own arena meanwhile */
void hos_compile_heap_pause(void) {
    struct ch_slot *const s = ch_slot_peek();
    if (s && s->depth > 0) s->paused++;
}
void hos_compile_heap_resume(void) {
    struct ch_slot *const s = ch_slot_peek();
    if (s && s->paused > 0) s->paused--;
}

/* ---- the pipeline object: the driver allocates it with the allocator vkCreate*Pipelines is given (vk_zalloc2), the device's
   when none (Mesa's vk_default_allocator: malloc, realloc, free). Measured on the PC (build/compile-heap, 15/11): with the
   shaders paused, what a compile left in its arena was 602 blocks of 1.2 MiB in all, the pipelines' own objects (385 graphics,
   212 compute), and they held 256 MiB of segments. When the renderer gives no allocator, the wraps give the driver this one:
   the same heap and calls as the device's, so a pipeline is freed by vkDestroyPipeline with none, as the renderer does; what
   outlives the call (every scope but VK_SYSTEM_ALLOCATION_SCOPE_COMMAND, 0) allocated in the thread's own arena ---- */
struct ch_allocation_callbacks {   /* (VkAllocationCallbacks: its layout, the host's code has no Vulkan headers) */
    void *user;
    void *(*allocation)(void *user, size_t size, size_t alignment, int scope);
    void *(*reallocation)(void *user, void *original, size_t size, size_t alignment, int scope);
    void (*free)(void *user, void *memory);
    void (*internal_allocation)(void *user, size_t size, int type, int scope);
    void (*internal_free)(void *user, size_t size, int type, int scope);
};
static void *ch_object_allocation(void *user, size_t size, size_t alignment, int scope) {
    (void)user;
    if (scope) hos_compile_heap_pause();
    void *p = NULL;
    if (alignment <= 32) p = malloc(size);   /* (the heap's blocks are 32-byte aligned: dlmalloc's MALLOC_ALIGNMENT) */
    else if (posix_memalign(&p, alignment, size)) p = NULL;
    if (scope) hos_compile_heap_resume();
    return p;
}
static void *ch_object_reallocation(void *user, void *original, size_t size, size_t alignment, int scope) {
    (void)alignment;   /* (as Mesa's vk_default_realloc: realloc, whatever the alignment) */
    if (!original) return ch_object_allocation(user, size, alignment, scope);
    if (scope) hos_compile_heap_pause();
    void *p = realloc(original, size);
    if (scope) hos_compile_heap_resume();
    return p;
}
static void ch_object_free(void *user, void *memory) {
    (void)user;
    free(memory);
}
static const struct ch_allocation_callbacks ch_objects = {NULL, ch_object_allocation, ch_object_reallocation, ch_object_free, NULL, NULL};
/* what the driver's call is given: this allocator when the renderer gave none and the setting is on, else the renderer's */
const void *hos_compile_heap_allocator(const void *given) {
    return !given && __atomic_load_n(&ch_state, __ATOMIC_ACQUIRE) == 2 ? (const void *)&ch_objects : given;
}

/* ---- the guards (ps5_platform.c's wraps of ps5p_heap_map and ps5p_heap_unmap; the callers hold an arena's lock) ---- */
/* the guard a segment mapped now gets on each side (0: none) and the record it is kept in */
size_t hos_compile_heap_guard_take(int *slot) {
    *slot = -1;
    const struct ch_slot *const s = ch_slot_peek();
    if (!s || ((s->depth <= 0 || s->paused) && !s->making)) return 0;   /* (a compile arena's segments only: not a paused call's) */
    for (int i = 0; i < CH_GUARDED; i++) {
        int free_ = 0;
        if (__atomic_compare_exchange_n(&ch_guarded[i].state, &free_, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            *slot = i;
            return CH_GUARD;
        }
    }
    __atomic_add_fetch(&ch_st.guarded_none, 1, __ATOMIC_RELAXED);   /* (every record held: mapped without guards) */
    return 0;
}
/* the heap mapped `bytes` + both guards at `at` ((void *)-1: refused): what dlmalloc is given */
void *hos_compile_heap_guard_set(int slot, void *at, size_t bytes) {
    if (slot < 0) return at;
    if (at == (void *)-1) {
        __atomic_store_n(&ch_guarded[slot].state, 0, __ATOMIC_RELEASE);
        return at;
    }
    ch_guarded[slot].at = (uintptr_t)at + CH_GUARD, ch_guarded[slot].bytes = bytes;
    __atomic_store_n(&ch_guarded[slot].state, 2, __ATOMIC_RELEASE);
    __atomic_add_fetch(&ch_guarded_n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ch_st.guarded_made, 1, __ATOMIC_RELAXED);
    return (char *)at + CH_GUARD;
}
/* dlmalloc returns [address, address + bytes): the record of a guarded segment exactly that (-1: none), and what the heap
   mapped for it */
int hos_compile_heap_guard_find(void *address, size_t bytes, void **mapped_at, size_t *mapped_bytes) {
    if (!__atomic_load_n(&ch_guarded_n, __ATOMIC_ACQUIRE)) return -1;
    for (int i = 0; i < CH_GUARDED; i++)
        if (__atomic_load_n(&ch_guarded[i].state, __ATOMIC_ACQUIRE) == 2 && ch_guarded[i].at == (uintptr_t)address &&
            ch_guarded[i].bytes == bytes) {
            *mapped_at = (char *)address - CH_GUARD, *mapped_bytes = bytes + 2 * CH_GUARD;
            return i;
        }
    return -1;
}
/* the heap gave it back */
void hos_compile_heap_guard_forget(int slot) {
    if (slot < 0) return;
    __atomic_sub_fetch(&ch_guarded_n, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&ch_guarded[slot].state, 0, __ATOMIC_RELEASE);
}

/* the compile arena this thread allocates from now (NULL: its own arena), for a test or a model to say where a block went */
void *hos_compile_heap_arena_now(void) { return ch_route(); }

void hos_compile_heap_stats(struct hos_compile_heap_stats *st) {
    memset(st, 0, sizeof *st);
    st->on = __atomic_load_n(&ch_state, __ATOMIC_ACQUIRE) == 2;
    st->scopes = ch_st.scopes, st->stale = ch_st.stale, st->trims = ch_st.trims, st->given_bytes = ch_st.given_bytes;
    st->guarded = (uint64_t)__atomic_load_n(&ch_guarded_n, __ATOMIC_RELAXED), st->guarded_made = ch_st.guarded_made;
    st->guarded_none = ch_st.guarded_none, st->routed_refused = ch_st.routed_refused, st->compile_ns = ch_st.compile_ns;
    for (unsigned i = 0; i < CH_ARENAS; i++) {
        void *const m = __atomic_load_n(&ch_arenas[i].m, __ATOMIC_ACQUIRE);
        if (!m) continue;
        st->arenas++;
        st->footprint += mspace_footprint ? mspace_footprint(m) : 0;
        st->peak += ch_arenas[i].peak;
    }
}

#if defined(__PROSPERO__)
/* ---- the driver's calls (the link's --wrap: RADV's entry points, and the three calls that make what a compile keeps) ----
   Their Vulkan types as the ABI passes them: a VkDevice, a VkPipelineCache and the other handles are pointers on this
   target, VkResult a 32-bit enum */
int32_t __real_radv_CreateGraphicsPipelines(void *device, void *cache, uint32_t count, const void *infos, const void *allocator,
                                            void *pipelines);
int32_t __wrap_radv_CreateGraphicsPipelines(void *device, void *cache, uint32_t count, const void *infos, const void *allocator,
                                            void *pipelines) {
    const int t = hos_compile_heap_enter();
    const int32_t r = __real_radv_CreateGraphicsPipelines(device, cache, count, infos, hos_compile_heap_allocator(allocator), pipelines);
    hos_compile_heap_leave(t);
    return r;
}
int32_t __real_radv_CreateComputePipelines(void *device, void *cache, uint32_t count, const void *infos, const void *allocator,
                                           void *pipelines);
int32_t __wrap_radv_CreateComputePipelines(void *device, void *cache, uint32_t count, const void *infos, const void *allocator,
                                           void *pipelines) {
    const int t = hos_compile_heap_enter();
    const int32_t r = __real_radv_CreateComputePipelines(device, cache, count, infos, hos_compile_heap_allocator(allocator), pipelines);
    hos_compile_heap_leave(t);
    return r;
}
/* radv_pipeline_cache.c: struct radv_shader *radv_shader_create(struct radv_device *, struct vk_pipeline_cache *,
   const struct radv_shader_binary *, bool skip_cache, struct radv_shader_debug_info *) */
void *__real_radv_shader_create(void *device, void *cache, const void *binary, _Bool skip_cache, void *debug);
void *__wrap_radv_shader_create(void *device, void *cache, const void *binary, _Bool skip_cache, void *debug) {
    hos_compile_heap_pause();
    void *const r = __real_radv_shader_create(device, cache, binary, skip_cache, debug);
    hos_compile_heap_resume();
    return r;
}
/* radv_pipeline_cache.c: void radv_pipeline_cache_insert(struct radv_device *, struct vk_pipeline_cache *, struct radv_pipeline *) */
void __real_radv_pipeline_cache_insert(void *device, void *cache, void *pipeline);
void __wrap_radv_pipeline_cache_insert(void *device, void *cache, void *pipeline) {
    hos_compile_heap_pause();
    __real_radv_pipeline_cache_insert(device, cache, pipeline);
    hos_compile_heap_resume();
}
/* vk_pipeline_cache.c: struct vk_pipeline_cache_object *vk_pipeline_cache_lookup_object(struct vk_pipeline_cache *,
   const void *key_data, size_t key_size, const struct vk_pipeline_cache_object_ops *, bool *cache_hit) */
void *__real_vk_pipeline_cache_lookup_object(void *cache, const void *key, size_t key_size, const void *ops, _Bool *hit);
void *__wrap_vk_pipeline_cache_lookup_object(void *cache, const void *key, size_t key_size, const void *ops, _Bool *hit) {
    hos_compile_heap_pause();
    void *const r = __real_vk_pipeline_cache_lookup_object(cache, key, key_size, ops, hit);
    hos_compile_heap_resume();
    return r;
}
#endif
#endif /* __PROSPERO__ || HOOKSHOT_PS5_HOST_MODEL */
