/* host/posix/ps5_window.c on a PC (scripts/ps5/build-plat-probe.sh host-test), over PS5_PayloadSDK's model of the console's
   kernel (platform/tests/host_kernel.c: direct memory a memfd handed out with a pattern, reservations PROT_NONE mappings).
   What it proves is the window's own logic: the reservation's three ways, backing at once and from a real SIGSEGV handler on
   several threads (with a watchdog: a deadlock fails the test, it does not hang it), zeroed units, decommit, a refused
   allocation part way, the protection of units already backed left alone, a fault taken with locks held, which ranges
   answer "untouched" (the renderer's question before it uploads a texture: tests/test_zero_upload.py runs this too), and
   the reclaim of zero units (hos_ps5_guest_reclaim: its switches, its holds, a write placed at each moment of a unit's
   sequence, and four threads writing beside it with every byte checked: tests/test_zero_reclaim.py runs this too). What
   the console itself does is the platform probe title's to prove (PPSA99147). SPDX-License-Identifier: GPL-3.0-or-later */
#include "ps5.h"
#include "host_kernel.h"

#include <ps5platform/kernel.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <time.h>
#include <unistd.h>

#define BASE 0x8000000000ull
#define GIB (1ull << 30)
#define MIB (1ull << 20)
#define G(a) ((uint8_t *)(uintptr_t)(BASE + (a)))

static int failures;
#define CHECK(cond, ...) do { if (cond) { printf("PASS "); } else { printf("FAIL "); failures++; } printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static volatile uint64_t fault_seen;   /* (the address of the last fault the handler was entered for: the reclaim's checks) */
static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    __atomic_store_n(&fault_seen, (uint64_t)(uintptr_t)si->si_addr, __ATOMIC_RELEASE);
    if (hos_ps5_guest_fault(si->si_addr)) return;
    signal(sig, SIG_DFL);   /* (not ours: the fault comes again and ends the test) */
}
static void install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* ---- (2026-11-12) the kernel's answer to "what protection has this address", as the console gives one. The model's own
   always refuses (platform/tests/host_kernel.c), and the reclaim of zero units now leaves a unit alone unless the kernel says,
   for all of it, that it is plain CPU read-write. Linked with --wrap (scripts/ps5/build-plat-probe.sh host-test): the mapping's
   range and its CPU bits from Linux (PROCMAP_QUERY on /proc/self/maps: system calls alone, so the fault handler may ask too,
   as it does on the console), the GPU bits of the ranges a check says an import granted (grants[]: RADV's
   radv_ps5_memory_grant_gpu played; the console's range ends where a grant does, so the answer is cut there too), and a
   refusal for the addresses a check names (refuse_lo..refuse_hi) ---- */
#define GRANTS 4
static struct { volatile uintptr_t lo, hi; } grants[GRANTS];
static volatile uintptr_t refuse_lo, refuse_hi, watch_lo, watch_hi, writable_lo, writable_hi;
static volatile unsigned long query_asked, query_refused, query_watched;
int32_t __real_sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);
int32_t __wrap_sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);
int32_t __wrap_sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection) {
    const uintptr_t a = (uintptr_t)address;
    __atomic_add_fetch(&query_asked, 1, __ATOMIC_RELAXED);
    if (a >= watch_lo && a < watch_hi) __atomic_add_fetch(&query_watched, 1, __ATOMIC_RELAXED);
    if (a >= refuse_lo && a < refuse_hi) {
        __atomic_add_fetch(&query_refused, 1, __ATOMIC_RELAXED);
        return __real_sceKernelQueryMemoryProtection(address, start, end, protection);   /* (the model's refusal) */
    }
    struct procmap_query q;
    memset(&q, 0, sizeof q);
    q.size = sizeof q;
    q.query_addr = (uint64_t)a;
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);   /* (this process's: a child asks about its own) */
    const int r = fd < 0 ? -1 : ioctl(fd, PROCMAP_QUERY, &q);
    if (fd >= 0) close(fd);
    if (r) return __real_sceKernelQueryMemoryProtection(address, start, end, protection);
    uintptr_t lo = (uintptr_t)q.vma_start, hi = (uintptr_t)q.vma_end;
    uint32_t prot = (q.vma_flags & PROCMAP_QUERY_VMA_READABLE ? PS5_KERNEL_PROT_CPU_READ : 0) |
                    (q.vma_flags & PROCMAP_QUERY_VMA_WRITABLE ? PS5_KERNEL_PROT_CPU_WRITE : 0);
    for (unsigned i = 0; i < GRANTS; i++) {
        const uintptr_t glo = grants[i].lo, ghi = grants[i].hi;
        if (glo >= ghi) continue;
        if (a >= glo && a < ghi) {
            prot |= PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE;
            if (glo > lo) lo = glo;
            if (ghi < hi) hi = ghi;
        } else if (glo > a && glo < hi) hi = glo;
        else if (ghi <= a && ghi > lo) lo = ghi;
    }
    if (a >= writable_lo && a < writable_hi) prot |= PS5_KERNEL_PROT_CPU_WRITE;   /* (a kernel whose read-only does not take) */
    if (start) *start = (void *)lo;
    if (end) *end = (void *)hi;
    if (protection) *protection = prot;
    return 0;
}

/* ---- the reservation's ways, each in a child of its own (a window is reserved once a process) ---- */
static int child(int (*fn)(void)) {
    const pid_t pid = fork();
    if (!pid) _exit(fn());
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 100 + WTERMSIG(status);
}
static int way_pieces(void) {
    setenv("HOOKSHOT_PS5_WINDOW", "pieces", 1);
    struct hos_ps5_guest_stats s;
    const int ok = hos_ps5_guest_reserve(BASE, 512 * GIB);
    hos_ps5_guest_stats(&s);
    return ok && s.mode == HOS_PS5_WINDOW_PIECES && s.pieces == 32 && s.space_end == 512 * GIB ? 0 : 1;
}
static int way_prefix(void) {
    /* (the console's suspected case: the window's top 16 GiB taken, as a user map ending at 0xFC_0000_0000 would) */
    void *top = mmap((void *)(uintptr_t)(BASE + 496 * GIB), 16 * GIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (top == MAP_FAILED) return 2;
    struct hos_ps5_guest_stats s;
    const int ok = hos_ps5_guest_reserve(BASE, 512 * GIB);
    hos_ps5_guest_stats(&s);
    printf("  prefix: ok %d mode %d pieces %u whole %#x space end %llu GiB, hos_space_end %llu GiB\n", ok, s.mode, s.pieces,
           (unsigned)s.whole_result, (unsigned long long)(s.space_end >> 30), (unsigned long long)(hos_space_end >> 30));
    if (!ok || s.mode != HOS_PS5_WINDOW_PREFIX || s.space_end != 496 * GIB || hos_space_end != 496 * GIB) return 1;
    /* the part past the end is not the window's: a fault there is not answered */
    return hos_ps5_guest_fault(G(500 * GIB)) == 0 && hos_ps5_in_guest(G(495 * GIB), 1) && !hos_ps5_in_guest(G(497 * GIB), 1) ? 0 : 3;
}
static int way_too_short(void) {
    /* (taken from 300 GiB up: less than the heap and the VAMM region need, so nothing is kept) */
    void *mid = mmap((void *)(uintptr_t)(BASE + 304 * GIB), 16 * GIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (mid == MAP_FAILED) return 2;
    const int ok = hos_ps5_guest_reserve(BASE, 512 * GIB);
    /* (and every piece given back: the range is free again but for the blocker) */
    void *again = mmap((void *)(uintptr_t)BASE, 16 * GIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    return !ok && again == (void *)(uintptr_t)BASE ? 0 : 1;
}

/* ---- faults from threads ---- */
struct job { uint64_t at, bytes, stride, first; unsigned id; volatile uint64_t done; volatile int finished, bad; };
static void *touch(void *p) {
    struct job *j = p;
    for (uint64_t u = j->first; u * 65536 < j->bytes; u += j->stride) {
        for (uint64_t off = u * 65536; off < (u + 1) * 65536; off += 4096) {
            volatile uint64_t *w = (volatile uint64_t *)(G(j->at) + off);
            if (*w != 0) j->bad++;   /* (a unit backed for this test reads zero first: the model hands out 0xa5) */
            *w = ((uint64_t)j->id << 56) | off;
        }
        j->done += 65536;
    }
    j->finished = 1;
    return NULL;
}
static int run(struct job *jobs, unsigned n) {
    pthread_t t[64];
    for (unsigned i = 0; i < n; i++) pthread_create(&t[i], NULL, touch, &jobs[i]);
    uint64_t last = 0;
    for (int still = 0; still < 100;) {   /* (10 s with no progress at all: a deadlock) */
        uint64_t sum = 0;
        unsigned left = 0;
        for (unsigned i = 0; i < n; i++) sum += jobs[i].done, left += !jobs[i].finished;
        if (!left) break;
        still = sum == last ? still + 1 : 0;
        last = sum;
        usleep(100000);
        if (still == 100) { printf("FAIL no progress for 10 s: deadlock\n"); fflush(stdout); _exit(9); }
    }
    int bad = 0;
    for (unsigned i = 0; i < n; i++) pthread_join(t[i], NULL), bad += jobs[i].bad;
    return bad;
}
static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
static void *fault_holding(void *p) {
    (void)p;
    pthread_mutex_lock(&held);
    const uint64_t v = *(volatile uint64_t *)G(0x2800000000ull);
    pthread_mutex_unlock(&held);
    return (void *)(uintptr_t)(v == 0);
}

static int perms_of(uint64_t at, char out[5]) {   /* the unit's protection, from /proc/self/maps */
    FILE *f = fopen("/proc/self/maps", "r");
    char line[256];
    int found = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[5];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) == 3 && at >= lo && at < hi) { memcpy(out, perms, 5); found = 1; break; }
    }
    if (f) fclose(f);
    return found;
}

/* ---- ps5_fiber.c's switch: two fibers and the thread, 3,000 switches, registers and stacks kept, MXCSR per fiber ---- */
static void *rsp_of[3];
static volatile unsigned rounds, bad_fiber;
static unsigned mxcsr_now(void) { unsigned m; __asm__ volatile("stmxcsr %0" : "=m"(m)); return m; }
static void fiber_body(void *p) {
    const unsigned me = (unsigned)(uintptr_t)p;
    volatile uint64_t mark[32];
    for (unsigned i = 0; i < 32; i++) mark[i] = (uint64_t)me << 32 | i;
    if (me == 2) { unsigned m = 0x1f80 | 0x6000; __asm__ volatile("ldmxcsr %0" :: "m"(m)); }   /* (fiber 2 rounds toward zero) */
    for (;;) {
        hos_ps5_fiber_switch(&rsp_of[me], rsp_of[me == 1 ? 2 : 0]);
        for (unsigned i = 0; i < 32; i++) if (mark[i] != ((uint64_t)me << 32 | i)) bad_fiber++;
        if (me == 2 && (mxcsr_now() & 0x6000) != 0x6000) bad_fiber++;
        if (me == 1 && (mxcsr_now() & 0x6000)) bad_fiber++;
    }
}
static int fiber_test(void) {
    static uint8_t s1[256 << 10], s2[256 << 10];
    rsp_of[1] = hos_ps5_fiber_init(s1, sizeof s1, fiber_body, (void *)1);
    rsp_of[2] = hos_ps5_fiber_init(s2, sizeof s2, fiber_body, (void *)2);
    volatile uint64_t mine[32];
    for (unsigned i = 0; i < 32; i++) mine[i] = 0xf00d0000u + i;
    for (unsigned r = 0; r < 1000; r++) {
        hos_ps5_fiber_switch(&rsp_of[0], rsp_of[1]);
        rounds++;
        for (unsigned i = 0; i < 32; i++) if (mine[i] != 0xf00d0000u + i) bad_fiber++;
        if (mxcsr_now() & 0x6000) bad_fiber++;
    }
    return rounds == 1000 && !bad_fiber;
}

/* ---- pinned units (hos_ps5_guest_pin: what an import was granted is never given back) ---- */
#define PIN_AT 0x6000000000ull
static int pin_off(void) {   /* (a child: the setting is read once) */
    setenv("HOOKSHOT_PS5_GPU_PIN", "0", 1);
    if (hos_ps5_guest_pin(G(PIN_AT + GIB), 2 * MIB)) return 1;
    size_t run_bytes = 0;
    if (!hos_ps5_guest_committed(G(PIN_AT + GIB), &run_bytes) || run_bytes != 2 * MIB) return 2;   /* (backed all the same) */
    if (hos_ps5_guest_pinned(G(PIN_AT + GIB), 2 * MIB)) return 3;
    hos_ps5_guest_decommit(G(PIN_AT + GIB), 2 * MIB);
    return hos_ps5_guest_committed(G(PIN_AT + GIB), NULL) ? 4 : 0;
}
static volatile int churn_stop;
static volatile unsigned long churn_rounds;
static void *churn(void *p) {   /* (the game's side: frees the range and touches it again, as fast as it can) */
    (void)p;
    while (!churn_stop) {
        hos_ps5_guest_decommit(G(PIN_AT + 2 * GIB), 8 * MIB);
        for (uint64_t off = 0; off < 8 * MIB; off += 65536) *(volatile uint8_t *)(G(PIN_AT + 2 * GIB) + off) = 1;
        churn_rounds++;
    }
    return NULL;
}
static void pin_tests(void) {
    struct hos_ps5_guest_stats s0, s1;
    CHECK(child(pin_off) == 0, "HOOKSHOT_PS5_GPU_PIN=0: a pin backs its range and pins nothing, a decommit gives it back");
    /* 1 MiB the game uses, in a 4 MiB range; the import takes the first 2 MiB (its second never touched) */
    hos_ps5_guest_commit(G(PIN_AT), MIB);
    hos_ps5_guest_commit(G(PIN_AT + 2 * MIB), 2 * MIB);
    memset(G(PIN_AT), 0x5a, MIB);
    memset(G(PIN_AT + 2 * MIB), 0x5a, 2 * MIB);
    hos_ps5_guest_stats(&s0);
    CHECK(hos_ps5_guest_pin(G(PIN_AT), 2 * MIB) == 0, "2 MiB pinned");
    hos_ps5_guest_stats(&s1);
    size_t run_bytes = 0;
    CHECK(hos_ps5_guest_committed(G(PIN_AT), &run_bytes) && run_bytes == 4 * MIB && s1.pin_backed_bytes - s0.pin_backed_bytes == MIB &&
          s1.pinned_bytes - s0.pinned_bytes == 2 * MIB && s1.pin_mode == 1,
          "the pin backed the MiB nothing had touched (%llu KiB) and pinned 2 MiB", (unsigned long long)((s1.pin_backed_bytes - s0.pin_backed_bytes) >> 10));
    CHECK(*(volatile uint8_t *)G(PIN_AT) == 0x5a && *(volatile uint8_t *)(G(PIN_AT) + MIB) == 0, "what was there stays, the new units read zero");
    CHECK(hos_ps5_guest_pinned(G(PIN_AT + MIB), 4096) && !hos_ps5_guest_pinned(G(PIN_AT + 2 * MIB), 2 * MIB), "pinned says which");
    char before[5] = "?", after[5] = "?";
    sceKernelMprotect(G(PIN_AT), 2 * MIB, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE);
    perms_of(BASE + PIN_AT, before);
    const long long a0 = host_direct_allocations();
    hos_ps5_guest_stats(&s0);
    CHECK(hos_ps5_guest_decommit(G(PIN_AT), 4 * MIB) == 0, "a decommit of the 4 MiB");
    hos_ps5_guest_stats(&s1);
    perms_of(BASE + PIN_AT, after);
    int zero = 1;
    for (uint64_t off = 0; off < 2 * MIB; off += 4096) zero &= *(volatile uint64_t *)(G(PIN_AT) + off) == 0;
    struct hos_ps5_guest_stats s2;
    hos_ps5_guest_stats(&s2);
    CHECK(hos_ps5_guest_committed(G(PIN_AT), &run_bytes) && run_bytes == 2 * MIB && zero && !strcmp(before, after) &&
          s1.pinned_kept_units == 32 && s1.pinned_kept_calls == 1 && a0 - host_direct_allocations() == 32 && s2.faults == s1.faults,
          "kept its 2 pinned MiB, zeroed, mapped as they were (%s), with no fault to read them, and gave the other 2 back (%lld units)",
          after, a0 - host_direct_allocations());
    /* MEM_RESET's pair (win32_posix.c): a decommit and a commit */
    memset(G(PIN_AT), 0x33, 2 * MIB);
    hos_ps5_guest_decommit(G(PIN_AT), 2 * MIB);
    hos_ps5_guest_commit(G(PIN_AT), 2 * MIB);
    CHECK(*(volatile uint8_t *)(G(PIN_AT) + MIB) == 0 && host_direct_allocations() == a0 - 32, "a reset of pinned units zeroes them in place");
    /* the race the renderer's own check leaves: the game frees and touches 8 MiB in a loop while the import pins it */
    pthread_t t;
    pthread_create(&t, NULL, churn, NULL);
    while (churn_rounds < 20) sched_yield();
    const int r = hos_ps5_guest_pin(G(PIN_AT + 2 * GIB), 8 * MIB);
    hos_ps5_guest_stats(&s0);
    const long long a1 = host_direct_allocations();
    const unsigned long at = churn_rounds;
    int holes = 0;
    while (churn_rounds < at + 200) {
        size_t n = 0;
        holes += !hos_ps5_guest_committed(G(PIN_AT + 2 * GIB), &n) || n != 8 * MIB;
    }
    churn_stop = 1;
    pthread_join(t, NULL);
    hos_ps5_guest_stats(&s1);
    CHECK(r == 0 && !holes && s1.decommit_units == s0.decommit_units && host_direct_allocations() == a1 && s1.pinned_kept_units - s0.pinned_kept_units >= 200 * 128,
          "8 MiB pinned under a thread freeing and touching it: no unit given back in %lu more rounds (%llu kept)", churn_rounds - at,
          (unsigned long long)(s1.pinned_kept_units - s0.pinned_kept_units));
    /* a range that cannot be backed: the pin says so (the import is then not made) */
    hos_ps5_guest_stats(&s0);
    host_fail(HOST_CALL_ALLOCATE, 20);
    const int refused = hos_ps5_guest_pin(G(PIN_AT + 3 * GIB), 4 * MIB);
    host_fail(HOST_CALL_NONE, 0);
    hos_ps5_guest_stats(&s1);
    CHECK(refused != 0 && s1.pin_failures == s0.pin_failures + 1, "a pin whose 20th allocation is refused answers %#x", (unsigned)refused);
    CHECK(hos_ps5_guest_pin(G(PIN_AT + 3 * GIB), 4 * MIB) == 0 && hos_ps5_guest_committed(G(PIN_AT + 3 * GIB), &run_bytes) && run_bytes == 4 * MIB,
          "asked again with memory to give, it backs the rest");
    CHECK(hos_ps5_guest_pin(G(0), 0) == 0 && hos_ps5_guest_pin((void *)(uintptr_t)0x1000, 65536) != 0, "nothing to pin is fine; a range outside the window is refused");
}

/* ---- storage nothing wrote (hos_ps5_guest_untouched: the renderer clears a texture whose storage answers 1, in place of
   staging and copying its zeros; gpu/vk/vk_texture.cpp upload_zeros) ---- */
#define ZERO_AT 0x7000000000ull
int hos_ps5_guest_fault2(const void *p, int write);   /* (ps5_window.c; mkw_glue.c's handler gives it the access' sense) */
static volatile int fill_stop, fill_full;
static volatile unsigned long fill_round;
static void *fill(void *p) {   /* (the game's side: writes all of 4 MiB, leaves it written a while, frees it) */
    (void)p;
    while (!fill_stop) {
        for (uint64_t off = 0; off < 4 * MIB; off += 65536) *(volatile uint8_t *)(G(ZERO_AT + 8 * GIB) + off) = 1;
        fill_full = 1;
        for (int k = 0; k < 64; k++) sched_yield();
        fill_full = 0;
        fill_round++;
        hos_ps5_guest_decommit(G(ZERO_AT + 8 * GIB), 4 * MIB);
    }
    return NULL;
}
static void untouched_tests(void) {
    struct hos_ps5_guest_stats s0, s1;
    hos_ps5_guest_settings();   /* (the unit of zeros read faults map is made once the settings are read) */
    hos_ps5_guest_stats(&s0);
    CHECK(s0.zero_mode == 1 && s0.fault_bytes == MIB, "read faults map the shared zeros (mode %d), a write fault backs 1 MiB", s0.zero_mode);
    const long long a0 = host_direct_allocations();
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT), 288 * MIB) == 1 && host_direct_allocations() == a0 && !hos_ps5_guest_committed(G(ZERO_AT), NULL),
          "288 MiB nothing touched: untouched, and asking backed nothing");
    /* read: the shared zeros, no memory of its own, still untouched */
    int mapped = 1, zero = 1;
    for (uint64_t off = 0; off < 8 * MIB; off += 65536) {
        mapped &= hos_ps5_guest_fault2(G(ZERO_AT) + off, 0) == 1;
        zero &= *(volatile uint64_t *)(G(ZERO_AT) + off) == 0 && *(volatile uint64_t *)(G(ZERO_AT) + off + 65528) == 0;
    }
    hos_ps5_guest_stats(&s1);
    CHECK(mapped && zero && s1.zero_maps - s0.zero_maps == 128 && host_direct_allocations() == a0 && hos_ps5_guest_untouched(G(ZERO_AT), 288 * MIB) == 1,
          "8 MiB of it read through the shared zeros (%llu units, no memory of their own): still untouched", (unsigned long long)(s1.zero_maps - s0.zero_maps));
    /* one byte written (a real fault on a unit of the shared zeros): its block gets memory, the range is not untouched */
    G(ZERO_AT)[3 * MIB + 5] = 0x7e;
    hos_ps5_guest_stats(&s0);
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT), 288 * MIB) == 0 && hos_ps5_guest_untouched(G(ZERO_AT) + 3 * MIB + 5, 1) == 0 && s0.zero_upgrades - s1.zero_upgrades == 16 &&
          G(ZERO_AT)[3 * MIB + 5] == 0x7e && G(ZERO_AT)[3 * MIB + 6] == 0,
          "one byte written in it: not untouched (the fault's block of %llu units given memory, the byte kept)", (unsigned long long)(s0.zero_upgrades - s1.zero_upgrades));
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT), 3 * MIB) == 1 && hos_ps5_guest_untouched(G(ZERO_AT) + 4 * MIB, 284 * MIB) == 1,
          "what lies before and after that block still is");
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT) + 3 * MIB - 4096, 4096) == 1 && hos_ps5_guest_untouched(G(ZERO_AT) + 3 * MIB - 4096, 4097) == 0 &&
          hos_ps5_guest_untouched(G(ZERO_AT) + 4 * MIB - 1, 1) == 0 && hos_ps5_guest_untouched(G(ZERO_AT) + 4 * MIB, 1) == 1,
          "a range is its units: one byte into a backed unit is enough, one byte short of it is not");
    /* given back: untouched again */
    hos_ps5_guest_decommit(G(ZERO_AT), 288 * MIB);
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT), 288 * MIB) == 1 && *(volatile uint8_t *)(G(ZERO_AT) + 3 * MIB + 5) == 0 && hos_ps5_guest_untouched(G(ZERO_AT), 288 * MIB) == 0,
          "given back, it is untouched again; read through the test's handler (which backs: every fault a write to it), it no longer is");
    hos_ps5_guest_decommit(G(ZERO_AT), 288 * MIB);
    /* backed and never written (a commit, a pin): 0, the caller reads it as before */
    hos_ps5_guest_commit(G(ZERO_AT) + 16 * MIB, 65536);
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT), 288 * MIB) == 0 && hos_ps5_guest_untouched(G(ZERO_AT), 16 * MIB) == 1 &&
          hos_ps5_guest_untouched(G(ZERO_AT) + 16 * MIB + 65536, MIB) == 1, "a unit committed and never written answers 0 (the caller reads it), its neighbours 1");
    hos_ps5_guest_decommit(G(ZERO_AT) + 16 * MIB, 65536);
    hos_ps5_guest_pin(G(ZERO_AT) + 32 * MIB, 65536);
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT) + 31 * MIB, 2 * MIB) == 0 && hos_ps5_guest_untouched(G(ZERO_AT) + 31 * MIB, MIB) == 1, "a unit an import pinned answers 0");
    /* a range over two GiB of the window (two locks) */
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT + 2 * GIB - 2 * MIB), 4 * MIB) == 1, "4 MiB across a GiB's end: untouched");
    G(ZERO_AT + 2 * GIB)[0] = 1;
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT + 2 * GIB - 2 * MIB), 4 * MIB) == 0 && hos_ps5_guest_untouched(G(ZERO_AT + 2 * GIB - 2 * MIB), 2 * MIB) == 1,
          "a byte written past the GiB's end: the range across it is not, the part before it is");
    CHECK(hos_ps5_guest_untouched(G(ZERO_AT), 0) == 0 && hos_ps5_guest_untouched((void *)(uintptr_t)0x1000, 65536) == 0 &&
          hos_ps5_guest_untouched(G(512 * GIB - 65536), 2 * 65536) == 0, "no range, and a range not all inside the window: 0");
    /* against a thread that writes all of 4 MiB, leaves it so a while and frees it: asked while it is written throughout, the
       answer is never 1; and neither side waits for the other for good (the GiB's lock) */
    pthread_t t;
    pthread_create(&t, NULL, fill, NULL);
    unsigned long asked = 0, wrong = 0, sure = 0, yes = 0;
    while (fill_round < 300) {
        const unsigned long r0 = fill_round;
        const int full0 = fill_full;
        const int u = hos_ps5_guest_untouched(G(ZERO_AT + 8 * GIB), 4 * MIB);
        const int full1 = fill_full;
        asked++, yes += u;
        if (full0 && full1 && fill_round == r0) sure++, wrong += u;
    }
    fill_stop = 1;
    pthread_join(t, NULL);
    CHECK(!wrong && sure > 0, "asked %lu times beside a thread writing and freeing the range: never untouched in the %lu asks made while all of it was written "
          "(%lu answered 1 in all)", asked, sure, yes);
}

/* ---- what a range holds of the window (hos_ps5_guest_backed_bytes: the renderer's report of the textures' storage,
   gpu/vk/vk_texture.cpp storage_report; tests/test_tex_storage.py runs this too) ---- */
#define BACK_AT 0x7400000000ull
#define U 65536ull
static void backed_tests(void) {
    struct hos_ps5_guest_stats s0, s1;
    uint64_t pinned = 7, freeable = 7;
    const long long a0 = host_direct_allocations();
    CHECK(hos_ps5_guest_backed_bytes(G(BACK_AT), 64 * MIB, &pinned, &freeable) == 0 && !pinned && !freeable && host_direct_allocations() == a0 &&
          !hos_ps5_guest_committed(G(BACK_AT), NULL), "64 MiB nothing touched: no byte backed, and asking backed nothing");
    /* three units of eight */
    hos_ps5_guest_commit(G(BACK_AT) + 2 * U, 3 * U);
    CHECK(hos_ps5_guest_backed_bytes(G(BACK_AT), 8 * U, &pinned, &freeable) == 3 * U && !pinned && freeable == 3 * U,
          "three units committed of eight: their bytes, all of them a decommit's to give back");
    CHECK(hos_ps5_guest_backed_bytes(G(BACK_AT) + 2 * U + 4096, U - 4096 + 100, &pinned, &freeable) == U - 4096 + 100 && !freeable &&
          hos_ps5_guest_backed_bytes(G(BACK_AT) + U + 100, 2 * U, &pinned, &freeable) == U + 100 && freeable == U &&
          hos_ps5_guest_backed_bytes(G(BACK_AT) + 2 * U - 1, U + 2, &pinned, &freeable) == U + 1 && freeable == U,
          "a unit partly inside the range counts for its part, and only one whole inside it is a decommit's to give back");
    /* read through the shared zeros: no memory of its own */
    CHECK(hos_ps5_guest_fault2(G(BACK_AT) + 6 * U, 0) == 1 && *(volatile uint8_t *)(G(BACK_AT) + 6 * U) == 0 &&
          hos_ps5_guest_backed_bytes(G(BACK_AT), 8 * U, NULL, NULL) == 3 * U, "a unit read through the shared zeros is not counted");
    /* pinned: counted, never a decommit's */
    hos_ps5_guest_pin(G(BACK_AT) + 3 * U, U);
    hos_ps5_guest_pin(G(BACK_AT) + 7 * U, U);
    CHECK(hos_ps5_guest_backed_bytes(G(BACK_AT), 8 * U, &pinned, &freeable) == 4 * U && pinned == 2 * U && freeable == 2 * U &&
          hos_ps5_guest_backed_bytes(G(BACK_AT) + 3 * U + 5, 10, &pinned, &freeable) == 10 && pinned == 10 && !freeable,
          "pinned units are counted as backed and as pinned, not as what a decommit gives back");
    /* a long range across a GiB's end, eight units a load in its middle: against the units one by one */
    const uint64_t at = BACK_AT + 2 * GIB - 40 * U;
    hos_ps5_guest_commit(G(at) + 3 * U, 37 * U);
    hos_ps5_guest_commit(G(at) + 61 * U, 9 * U);
    hos_ps5_guest_pin(G(at) + 16 * U, 5 * U);
    uint64_t want = 0, want_pinned = 0, want_free = 0;
    const uint64_t lo = at + 1000, hi = at + 80 * U - 1000;
    for (uint64_t u = at; u < at + 80 * U; u += U) {
        if (!hos_ps5_guest_committed(G(u), NULL)) continue;
        const uint64_t part = (u + U < hi ? u + U : hi) - (u > lo ? u : lo);
        const int pin = hos_ps5_guest_pinned(G(u), 1);
        want += part, want_pinned += pin ? part : 0, want_free += !pin && part == U ? U : 0;
    }
    const uint64_t got = hos_ps5_guest_backed_bytes(G(lo), (size_t)(hi - lo), &pinned, &freeable);
    CHECK(got == want && pinned == want_pinned && freeable == want_free && got == 46 * U && pinned == 5 * U && freeable == 41 * U,
          "80 units across a GiB's end, 46 backed and 5 of them pinned: %llu KiB backed, %llu pinned, %llu a decommit's",
          (unsigned long long)(got >> 10), (unsigned long long)(pinned >> 10), (unsigned long long)(freeable >> 10));
    /* and a decommit of that range gives back what was said, no more */
    hos_ps5_guest_stats(&s0);
    hos_ps5_guest_decommit(G(lo), (size_t)(hi - lo));
    hos_ps5_guest_stats(&s1);
    CHECK((s1.decommit_units - s0.decommit_units) * U == want_free && hos_ps5_guest_backed_bytes(G(lo), (size_t)(hi - lo), &pinned, &freeable) == 5 * U &&
          pinned == 5 * U && !freeable, "a decommit of the range gave back those %llu units; the pinned ones stay, nothing more to give",
          (unsigned long long)(s1.decommit_units - s0.decommit_units));
    pinned = freeable = 7;
    CHECK(hos_ps5_guest_backed_bytes(G(BACK_AT), 0, &pinned, &freeable) == 0 && !pinned && !freeable &&
          hos_ps5_guest_backed_bytes((void *)(uintptr_t)0x1000, U, &pinned, NULL) == 0 &&
          hos_ps5_guest_backed_bytes(G(512 * GIB - U), 2 * U, NULL, &freeable) == 0, "no range, and a range not all inside the window: 0 backed");
    hos_ps5_guest_decommit(G(BACK_AT), 8 * U);
}

/* ---- zero units put back behind the shared zeros (hos_ps5_guest_reclaim: host/posix/ps5_window.c's header has the
   sequence). Passes are 30 s apart on the console; here one follows another, and what is counted in passes (a commit's
   hold, the wait after a write soon after a reclaim) is checked in passes ---- */
#define RECLAIM_AT 0x7800000000ull
#define UNIT_AT(base, k) ((base) + (uint64_t)(k) * 65536)
extern void (*hos_ps5_reclaim_hook)(int step, uint64_t guest);   /* (ps5_window.c, this build alone) */

/* the switches, each in a child (they are read once): 1 MiB committed and left zero, five passes. main sets
   HOOKSHOT_PS5_ZERO_RECLAIM=1 for the whole test (2026-11-12: off unless set); the first child unsets it */
static const char *off_name;
static int off_state;
static int reclaim_is_off(void) {
    if (off_name) setenv(off_name, "0", 1);
    else unsetenv("HOOKSHOT_PS5_ZERO_RECLAIM");
    hos_ps5_guest_settings();
    /* (HOOKSHOT_PS5_GPU_PIN=0 is read at the first import: one is asked for) */
    if (off_state == HOS_PS5_RECLAIM_NO_PINS ? hos_ps5_guest_pin(G(RECLAIM_AT), MIB) : hos_ps5_guest_commit(G(RECLAIM_AT), MIB)) return 1;
    const long long a0 = host_direct_allocations();
    for (int i = 0; i < 5; i++) {
        struct hos_ps5_guest_reclaim r;
        if (hos_ps5_guest_reclaim(&r) || r.state != off_state || r.reclaimed || r.looked) return 2;
    }
    size_t run_bytes = 0;
    if (host_direct_allocations() != a0 || !hos_ps5_guest_committed(G(RECLAIM_AT), &run_bytes) || run_bytes != MIB) return 3;
    char perms[5] = "?";
    perms_of(BASE + RECLAIM_AT, perms);
    return strncmp(perms, "rw-", 3) ? 4 : 0;
}
static int reclaim_is_on(void) {
    hos_ps5_guest_settings();
    if (hos_ps5_guest_commit(G(RECLAIM_AT), MIB)) return 1;
    const long long a0 = host_direct_allocations();
    struct hos_ps5_guest_reclaim r;
    for (int i = 0; i < 2; i++)
        if (!hos_ps5_guest_reclaim(&r) || r.reclaimed) return 2;
    if (!hos_ps5_guest_reclaim(&r) || r.reclaimed != 16 || a0 - host_direct_allocations() != 16 || r.pass != 3) return 3;
    return hos_ps5_guest_committed(G(RECLAIM_AT), NULL) ? 4 : 0;
}
/* a call the kernel refuses: the reclaim says which and turns itself off, and the units are as they were */
static int refused_step;
static int reclaim_refused(void) {
    hos_ps5_guest_settings();
    if (hos_ps5_guest_commit(G(RECLAIM_AT), MIB)) return 1;
    struct hos_ps5_guest_reclaim r;
    hos_ps5_guest_reclaim(&r);
    hos_ps5_guest_reclaim(&r);   /* (the commit's hold) */
    const long long a0 = host_direct_allocations();
    host_fail(refused_step == 1 ? HOST_CALL_PROTECT : HOST_CALL_MAP, 1);
    const int ran = hos_ps5_guest_reclaim(&r);
    host_fail(HOST_CALL_NONE, 0);
    if (!ran || r.reclaimed || r.error_step != refused_step || !r.error) return 2;
    struct hos_ps5_guest_stats s0, s1;
    hos_ps5_guest_stats(&s0);
    if (hos_ps5_guest_reclaim(&r) || r.state != HOS_PS5_RECLAIM_REFUSED || s0.reclaim_mode != -2 || s0.reclaim_failures != 1) return 3;
    size_t run_bytes = 0;
    if (host_direct_allocations() != a0 || !hos_ps5_guest_committed(G(RECLAIM_AT), &run_bytes) || run_bytes != MIB) return 4;
    for (unsigned k = 0; k < 16; k++) {   /* (zero, and written with no fault: read-write as before) */
        if (G(UNIT_AT(RECLAIM_AT, k))[k]) return 5;
        G(UNIT_AT(RECLAIM_AT, k))[k] = (uint8_t)(k + 1);
    }
    hos_ps5_guest_stats(&s1);
    return s1.faults == s0.faults && s1.fault_retries == s0.fault_retries && G(UNIT_AT(RECLAIM_AT, 15))[15] == 16 ? 0 : 6;
}
/* the protection refuses to give the write back to a unit that was written meanwhile: its memory is mapped over it again */
static void hook_restore(int step, uint64_t guest) {
    if (step || guest != UNIT_AT(RECLAIM_AT, 3)) return;
    G(guest)[100] = 0x5a;               /* (between the pass's look and its lock) */
    host_fail(HOST_CALL_PROTECT, 2);    /* (the next protection, read-only, is made; the one after, read-write again, is refused) */
}
static int reclaim_restore(void) {
    hos_ps5_guest_settings();
    if (hos_ps5_guest_commit(G(RECLAIM_AT), MIB)) return 1;
    struct hos_ps5_guest_reclaim r;
    hos_ps5_guest_reclaim(&r);
    hos_ps5_guest_reclaim(&r);
    hos_ps5_reclaim_hook = hook_restore;
    const int ran = hos_ps5_guest_reclaim(&r);
    hos_ps5_reclaim_hook = NULL;
    if (!ran || r.written != 1 || r.reclaimed != 15 || r.error_step) return 2;
    char perms[5] = "?";
    perms_of(BASE + UNIT_AT(RECLAIM_AT, 3), perms);
    struct hos_ps5_guest_stats s0, s1;
    hos_ps5_guest_stats(&s0);
    G(UNIT_AT(RECLAIM_AT, 3))[101] = 0x5b;
    hos_ps5_guest_stats(&s1);
    if (strncmp(perms, "rw-", 3) || s1.faults != s0.faults || s1.fault_retries != s0.fault_retries) return 3;
    if (G(UNIT_AT(RECLAIM_AT, 3))[100] != 0x5a || G(UNIT_AT(RECLAIM_AT, 3))[101] != 0x5b || G(UNIT_AT(RECLAIM_AT, 3))[102]) return 4;
    return hos_ps5_guest_reclaim(&r) && r.state == HOS_PS5_RECLAIM_RAN ? 0 : 5;   /* (and the reclaim goes on) */
}
/* the read-only protection made, and the kernel still saying the unit is writable: read-write again, the reclaim off */
static int reclaim_still_writable(void) {
    hos_ps5_guest_settings();
    if (hos_ps5_guest_commit(G(RECLAIM_AT), MIB)) return 1;
    struct hos_ps5_guest_reclaim r;
    hos_ps5_guest_reclaim(&r);
    hos_ps5_guest_reclaim(&r);   /* (the commit's hold) */
    const long long a0 = host_direct_allocations();
    writable_lo = (uintptr_t)G(RECLAIM_AT), writable_hi = (uintptr_t)G(RECLAIM_AT + MIB);
    const int ran = hos_ps5_guest_reclaim(&r);
    writable_lo = writable_hi = 0;
    if (!ran || r.reclaimed || r.error_step != 4) return 2;
    if (hos_ps5_guest_reclaim(&r) || r.state != HOS_PS5_RECLAIM_REFUSED) return 3;
    size_t run_bytes = 0;
    char perms[5] = "?";
    perms_of(BASE + RECLAIM_AT, perms);
    if (host_direct_allocations() != a0 || !hos_ps5_guest_committed(G(RECLAIM_AT), &run_bytes) || run_bytes != MIB || strncmp(perms, "rw-", 3)) return 4;
    G(RECLAIM_AT)[5] = 5;   /* (written with no fault) */
    return G(RECLAIM_AT)[5] == 5 ? 0 : 5;
}
static void reclaim_switches(void) {   /* (before the parent's first commit, import and pass: the children read the settings themselves) */
    off_name = NULL, off_state = HOS_PS5_RECLAIM_OFF;
    CHECK(child(reclaim_is_off) == 0, "HOOKSHOT_PS5_ZERO_RECLAIM unset: off, as by default (no pass, 1 MiB of zeros stays backed)");
    off_name = "HOOKSHOT_PS5_ZERO_RECLAIM", off_state = HOS_PS5_RECLAIM_OFF;
    CHECK(child(reclaim_is_off) == 0, "HOOKSHOT_PS5_ZERO_RECLAIM=0: no pass, 1 MiB of zeros stays backed");
    off_name = "HOOKSHOT_PS5_ZERO_READS", off_state = HOS_PS5_RECLAIM_NO_ZEROS;
    CHECK(child(reclaim_is_off) == 0, "HOOKSHOT_PS5_ZERO_READS=0 (no shared zeros): no pass");
    off_name = "HOOKSHOT_PS5_GPU_PIN", off_state = HOS_PS5_RECLAIM_NO_PINS;
    CHECK(child(reclaim_is_off) == 0, "HOOKSHOT_PS5_GPU_PIN=0 (the window does not know what the GPU reaches): no pass");
    CHECK(child(reclaim_is_on) == 0, "HOOKSHOT_PS5_ZERO_RECLAIM=1 and none of them off: the 1 MiB is held two passes after its commit and put back by the third (16 units)");
    refused_step = 1;
    CHECK(child(reclaim_refused) == 0, "the read-only protection refused: the reclaim says so and turns itself off, the units backed and read-write as before");
    refused_step = 3;
    CHECK(child(reclaim_refused) == 0, "the shared zeros' mapping refused: the unit is made read-write again, the reclaim off");
    CHECK(child(reclaim_restore) == 0, "the protection refusing to make a written unit read-write again: its own memory mapped over it, the byte kept, the reclaim goes on");
    CHECK(child(reclaim_still_writable) == 0, "the read-only protection made but the kernel still saying the unit is writable: read-write again, backed, the reclaim off (step 4)");
}

/* a write, or a commit, placed at each moment of a unit's sequence (the hook: step 0 the unit read zero and its lock is
   not taken yet, step 1 it is read-only and the lock is held) */
struct race {
    uint64_t unit;
    int write_before, write_during, commit_before, commit_during;
    sem_t go;
    volatile int about, met;
    int commit_result;
    ssize_t read_result;
};
static struct race races[5];
static int race_fd = -1;
static void race_hook(int step, uint64_t guest) {
    for (unsigned i = 0; i < 5; i++) {
        struct race *c = &races[i];
        if (c->unit != guest) continue;
        if (step == 0 && c->write_before) G(guest)[100] = 0x5a;
        if (step == 0 && c->commit_before) c->commit_result = hos_ps5_guest_commit(G(guest), 4096);
        if (step == 1 && (c->write_during || c->commit_during)) {
            sem_post(&c->go);
            /* (until the writer is in its fault handler for this very byte, the committer about to ask; then the time to
               reach the lock) */
            for (int k = 0; k < 5000 && !c->met; k++) {
                if (c->write_during ? __atomic_load_n(&fault_seen, __ATOMIC_ACQUIRE) == (uint64_t)(uintptr_t)(G(guest) + 200) : c->about) c->met = 1;
                else usleep(1000);
            }
            usleep(20000);
        }
    }
}
static void *race_thread(void *p) {
    struct race *c = p;
    sem_wait(&c->go);
    if (c->write_during) G(c->unit)[200] = 0x77;   /* (a fault: the unit is read-only) */
    if (c->commit_during) {
        c->about = 1;
        c->commit_result = hos_ps5_guest_commit(G(c->unit), 4096);   /* (waits for the unit's lock) */
        c->read_result = pread(race_fd, G(c->unit), 4096, 0);        /* (the kernel's write into it) */
    }
    return NULL;
}
static int only_bytes(uint64_t unit, unsigned off_a, uint8_t a, unsigned off_b, uint8_t b) {   /* the unit holds these and zeros */
    for (unsigned off = 0; off < 65536; off++) {
        const uint8_t want = off == off_a ? a : off == off_b ? b : 0;
        if (G(unit)[off] != want) return 0;
    }
    return 1;
}

/* four threads write beside the passes: each owns a quarter of every unit (so a unit is zero only when all four left
   theirs zero, and a fault on a unit is often two threads'), keeps a copy of what it wrote and compares before and after
   every write */
#define STRESS_AT (RECLAIM_AT + 4 * GIB)
#define STRESS_UNITS 128
#define STRESS_THREADS 4
#define SLICE (65536 / STRESS_THREADS)
struct stress { unsigned id; uint64_t seed; uint8_t *copy; volatile uint64_t ops; volatile unsigned long bad; };
static volatile int stress_stop;
static uint64_t next_random(uint64_t *s) { *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17; return *s; }
static void *stress_writer(void *p) {
    struct stress *w = p;
    while (!stress_stop) {
        const unsigned unit = (unsigned)(next_random(&w->seed) % STRESS_UNITS);
        uint8_t *mine = G(UNIT_AT(STRESS_AT, unit)) + w->id * SLICE, *copy = w->copy + (size_t)unit * SLICE;
        if (memcmp(mine, copy, SLICE)) w->bad++;
        const unsigned what = (unsigned)(next_random(&w->seed) % 100);
        if (what < 60) {   /* (its quarter cleared: the unit may be all zeros now) */
            memset(mine, 0, SLICE);
            memset(copy, 0, SLICE);
        } else if (what < 95) {   /* (a few bytes, any value) */
            const unsigned off = (unsigned)(next_random(&w->seed) % SLICE), len = 1 + (unsigned)(next_random(&w->seed) % 64);
            for (unsigned i = off; i < off + len && i < SLICE; i++) {
                const uint8_t v = (uint8_t)(next_random(&w->seed) >> 24);
                copy[i] = v, mine[i] = v;
            }
        } else {   /* (all of it) */
            for (unsigned i = 0; i < SLICE; i += 8) {
                const uint64_t v = next_random(&w->seed);
                memcpy(copy + i, &v, 8);
                memcpy(mine + i, &v, 8);
            }
        }
        if (memcmp(mine, copy, SLICE)) w->bad++;
        w->ops++;
    }
    return NULL;
}

static void reclaim_tests(void) {
    struct hos_ps5_guest_stats s0, s1;
    struct hos_ps5_guest_reclaim r;
    size_t run_bytes = 0;
    char perms[5] = "?", perms2[5] = "?";

    /* what the checks above left backed: three passes, and the pinned units are where they were */
    const long long left = host_direct_allocations();
    uint64_t got = 0;
    int ran = 0;
    for (int i = 0; i < 3; i++) ran += hos_ps5_guest_reclaim(&r), got += r.reclaimed;
    hos_ps5_guest_stats(&s1);
    CHECK(ran == 3 && r.state == HOS_PS5_RECLAIM_RAN && got >= 15 && (long long)got == left - host_direct_allocations() && s1.reclaim_mode == 1 &&
          s1.reclaim_units == got, "three passes over what the checks above left: %llu units of zeros put back, each one's memory given back", (unsigned long long)got);
    perms_of(BASE + PIN_AT, perms);
    size_t run2 = 0, run3 = 0;
    CHECK(r.pinned >= 32 + 128 + 64 + 1 && hos_ps5_guest_committed(G(PIN_AT), &run_bytes) && run_bytes == 2 * MIB && !strncmp(perms, "rw-", 3) &&
          hos_ps5_guest_committed(G(PIN_AT + 2 * GIB), &run2) && run2 == 8 * MIB && hos_ps5_guest_committed(G(PIN_AT + 3 * GIB), &run3) && run3 == 4 * MIB &&
          hos_ps5_guest_pinned(G(ZERO_AT) + 32 * MIB, 1) && hos_ps5_guest_committed(G(ZERO_AT) + 32 * MIB, NULL),
          "the pinned units stay, all zeros or not (%llu met, mapped %s)", (unsigned long long)r.pinned, perms);

    /* 4 MiB committed, a byte written in three of its 64 units */
    const uint64_t R = RECLAIM_AT;
    const long long clean = host_direct_allocations();
    CHECK(hos_ps5_guest_commit(G(R), 4 * MIB) == 0 && host_direct_allocations() == clean + 64 + 1, "4 MiB committed (64 units and the GiB's table)");
    G(R)[65535] = 0x11;                 /* (unit 0: its last byte) */
    G(UNIT_AT(R, 5))[30000] = 0x22;     /* (unit 5: one in the middle) */
    G(UNIT_AT(R, 63))[0] = 0x33;        /* (unit 63: its first) */
    const long long a0 = host_direct_allocations();
    hos_ps5_guest_stats(&s0);
    uint64_t early = 0, held = 0;
    for (int i = 0; i < 2; i++) hos_ps5_guest_reclaim(&r), early += r.reclaimed, held = r.held;
    CHECK(early == 0 && held == 64 && host_direct_allocations() == a0, "a commit holds its units through the two passes that follow (%llu held, %llu put back)",
          (unsigned long long)held, (unsigned long long)early);
    hos_ps5_guest_reclaim(&r);
    hos_ps5_guest_stats(&s1);
    CHECK(r.reclaimed == 61 && r.zeros == 61 && r.written == 0 && a0 - host_direct_allocations() == 61 && s0.backed_bytes - s1.backed_bytes == 61 * 65536ull &&
          s1.zero_bytes - s0.zero_bytes == 61 * 65536ull && s1.reclaim_units - s0.reclaim_units == 61,
          "the third pass put the 61 units of zeros back behind the shared zeros and gave their memory back (%llu; the longest lock %llu us)",
          (unsigned long long)r.reclaimed, (unsigned long long)(r.lock_ns_max / 1000));
    perms_of(BASE + UNIT_AT(R, 1), perms);
    perms_of(BASE + R, perms2);
    const long long a1 = host_direct_allocations();
    int zero = 1;
    hos_ps5_guest_stats(&s0);
    for (unsigned k = 0; k < 64; k++)
        if (k != 0 && k != 5 && k != 63)
            for (uint64_t off = 0; off < 65536; off += 8) zero &= *(volatile uint64_t *)(G(UNIT_AT(R, k)) + off) == 0;
    hos_ps5_guest_stats(&s1);
    size_t run4 = 0;
    CHECK(zero && s1.faults == s0.faults && host_direct_allocations() == a1 && !strncmp(perms, "r--", 3) && !strncmp(perms2, "rw-", 3) &&
          G(R)[65535] == 0x11 && G(UNIT_AT(R, 5))[30000] == 0x22 && G(UNIT_AT(R, 63))[0] == 0x33 && only_bytes(R, 65535, 0x11, 65535, 0x11) &&
          hos_ps5_guest_committed(G(R), &run_bytes) && run_bytes == 65536 && !hos_ps5_guest_committed(G(UNIT_AT(R, 1)), &run4) && run4 == 4 * 65536,
          "they read zero with no fault and no memory of their own (mapped %s); the three written units are as they were (%s)", perms, perms2);
    CHECK(hos_ps5_guest_untouched(G(UNIT_AT(R, 1)), 4 * 65536) == 1 && hos_ps5_guest_untouched(G(R), 4 * MIB) == 0,
          "untouched answers for them as for units nothing wrote");

    /* a byte written into one of them: a fault, memory of its own again, for that unit alone */
    hos_ps5_guest_stats(&s0);
    G(UNIT_AT(R, 10))[7] = 0x44;
    hos_ps5_guest_stats(&s1);
    CHECK(only_bytes(UNIT_AT(R, 10), 7, 0x44, 7, 0x44) && host_direct_allocations() == a1 + 1 && s1.faults - s0.faults == 1 &&
          s1.zero_upgrades - s0.zero_upgrades == 1 && s1.reclaim_rebacked - s0.reclaim_rebacked == 1 && s1.reclaim_flaps - s0.reclaim_flaps == 1 &&
          !hos_ps5_guest_committed(G(UNIT_AT(R, 9)), NULL) && !hos_ps5_guest_committed(G(UNIT_AT(R, 11)), NULL),
          "a byte written into one: that unit alone gets memory again (the rest of the fault's block stays behind the zeros), the byte kept");
    /* zero again at once: written all the time, as far as the reclaim can tell. Left alone 8 passes, then 16 */
    G(UNIT_AT(R, 10))[7] = 0;
    uint64_t soon = 0, counted = 0;
    for (int i = 0; i < 8; i++) hos_ps5_guest_reclaim(&r), soon += r.reclaimed, counted += r.rebacked;
    const int kept8 = hos_ps5_guest_committed(G(UNIT_AT(R, 10)), NULL);
    hos_ps5_guest_reclaim(&r);
    CHECK(soon == 0 && kept8 && counted == 1 && r.reclaimed == 1 && !hos_ps5_guest_committed(G(UNIT_AT(R, 10)), NULL),
          "written within two passes of being put back, and zero again: left alone for 8 passes, put back by the 9th (the pass after the write "
          "counted %llu unit given memory again)", (unsigned long long)counted);
    G(UNIT_AT(R, 10))[9] = 0x45;
    G(UNIT_AT(R, 10))[9] = 0;
    soon = 0;
    for (int i = 0; i < 16; i++) hos_ps5_guest_reclaim(&r), soon += r.reclaimed;
    const int kept16 = hos_ps5_guest_committed(G(UNIT_AT(R, 10)), NULL);
    hos_ps5_guest_reclaim(&r);
    CHECK(soon == 0 && kept16 && r.reclaimed == 1 && !hos_ps5_guest_committed(G(UNIT_AT(R, 10)), NULL), "the same again: 16 passes, put back by the 17th");
    hos_ps5_guest_stats(&s0);
    G(UNIT_AT(R, 20))[1] = 9;   /* (put back 28 passes ago) */
    G(UNIT_AT(R, 20))[1] = 0;
    hos_ps5_guest_reclaim(&r);
    hos_ps5_guest_stats(&s1);
    CHECK(r.reclaimed == 1 && r.rebacked == 1 && s1.reclaim_flaps == s0.reclaim_flaps && !hos_ps5_guest_committed(G(UNIT_AT(R, 20)), NULL),
          "a unit written long after it was put back, and zero again, goes back at the next pass");

    /* a unit behind the zeros gets memory zeroed elsewhere first; the reservation laid back over that address refused once */
    hos_ps5_guest_stats(&s0);
    host_fail(HOST_CALL_RESERVE, 1);
    G(UNIT_AT(R, 21))[3] = 7;
    host_fail(HOST_CALL_NONE, 0);
    hos_ps5_guest_stats(&s1);
    G(UNIT_AT(R, 22))[4] = 8;
    struct hos_ps5_guest_stats s2;
    hos_ps5_guest_stats(&s2);
    CHECK(only_bytes(UNIT_AT(R, 21), 3, 7, 3, 7) && only_bytes(UNIT_AT(R, 22), 4, 8, 4, 8) && s1.rereserve_fallbacks - s0.rereserve_fallbacks == 1 &&
          s2.rereserve_fallbacks == s1.rereserve_fallbacks && s2.zero_upgrades - s0.zero_upgrades == 2,
          "the address a unit's new memory is zeroed at: its reservation refused once, unmapped and reserved again; the next unit as usual");

    /* a system call's write: no fault to answer, so a commit comes first, and holds */
    uint8_t page[4096];
    memset(page, 0xc3, sizeof page);
    race_fd = memfd_create("reclaim-read", 0);
    const int filled = race_fd >= 0 && write(race_fd, page, sizeof page) == (ssize_t)sizeof page;
    errno = 0;
    const ssize_t refused = pread(race_fd, G(UNIT_AT(R, 30)), 4096, 0);
    const int why = errno;
    CHECK(filled && refused == -1 && why == EFAULT, "a read() into a unit behind the shared zeros is refused by the kernel (errno %d), with no fault to answer: "
          "a commit comes first", why);
    const int committed = hos_ps5_guest_commit(G(UNIT_AT(R, 30)), 2 * 65536);
    const ssize_t first = pread(race_fd, G(UNIT_AT(R, 30)), 4096, 0);
    uint64_t taken = 0;
    for (int i = 0; i < 2; i++) hos_ps5_guest_reclaim(&r), taken += r.reclaimed;
    const ssize_t later = pread(race_fd, G(UNIT_AT(R, 31)) + 4096, 4096, 0);   /* (the unit beside it: committed, still all zeros) */
    CHECK(committed == 0 && first == 4096 && later == 4096 && taken == 0 && G(UNIT_AT(R, 30))[4095] == 0xc3 && G(UNIT_AT(R, 31))[4096] == 0xc3 &&
          G(UNIT_AT(R, 31))[4095] == 0, "after the commit the read lands; two passes later one still lands in the unit beside it, zero until then");

    /* an import over units behind the zeros: backed and pinned under the lock, and no pass takes them */
    hos_ps5_guest_stats(&s0);
    const int pin = hos_ps5_guest_pin(G(UNIT_AT(R, 40)), 8 * 65536);
    hos_ps5_guest_stats(&s1);
    taken = 0;
    for (int i = 0; i < 4; i++) hos_ps5_guest_reclaim(&r), taken += r.reclaimed;
    perms_of(BASE + UNIT_AT(R, 40), perms);
    CHECK(pin == 0 && s1.pin_backed_bytes - s0.pin_backed_bytes == 8 * 65536 && taken == 0 && hos_ps5_guest_committed(G(UNIT_AT(R, 40)), &run_bytes) &&
          run_bytes == 8 * 65536 && !strncmp(perms, "rw-", 3), "an import over 8 of them backs and pins them: four passes leave them (%s)", perms);
    /* a decommit: what has memory gives it back, what is behind the zeros has none to give */
    hos_ps5_guest_decommit(G(R), 4 * MIB);
    CHECK(host_direct_allocations() == clean + 1 + 8, "a decommit of the 4 MiB leaves the 8 pinned units and the GiB's table (%lld allocations more than before)",
          host_direct_allocations() - clean);

    /* a write, and a commit, at each moment of a unit's sequence */
    const uint64_t S = R + 16 * MIB;
    hos_ps5_guest_commit(G(S), MIB);
    hos_ps5_guest_reclaim(&r);
    hos_ps5_guest_reclaim(&r);   /* (the commit's hold) */
    memset(races, 0, sizeof races);
    races[0].unit = UNIT_AT(S, 3), races[0].write_before = 1;
    races[1].unit = UNIT_AT(S, 6), races[1].write_during = 1;
    races[2].unit = UNIT_AT(S, 9), races[2].write_before = 1, races[2].write_during = 1;
    races[3].unit = UNIT_AT(S, 12), races[3].commit_before = 1, races[3].commit_result = -1;
    races[4].unit = UNIT_AT(S, 14), races[4].commit_during = 1, races[4].commit_result = -1;
    pthread_t rt[5];
    for (unsigned i = 0; i < 5; i++) {
        sem_init(&races[i].go, 0, 0);
        if (races[i].write_during || races[i].commit_during) pthread_create(&rt[i], NULL, race_thread, &races[i]);
    }
    const long long a2 = host_direct_allocations();
    hos_ps5_guest_stats(&s0);
    hos_ps5_reclaim_hook = race_hook;
    hos_ps5_guest_reclaim(&r);
    hos_ps5_reclaim_hook = NULL;
    for (unsigned i = 0; i < 5; i++)
        if (races[i].write_during || races[i].commit_during) pthread_join(rt[i], NULL);
    hos_ps5_guest_stats(&s1);
    const ssize_t after = pread(race_fd, G(races[3].unit), 4096, 0);
    CHECK(r.zeros == 16 && r.reclaimed == 13 && r.written == 2 && r.changed == 1, "one pass over 16 units of zeros with a write or a commit placed in five of them: "
          "%llu put back, %llu found written and kept, %llu found committed and left", (unsigned long long)r.reclaimed, (unsigned long long)r.written,
          (unsigned long long)r.changed);
    perms_of(BASE + races[0].unit, perms);
    CHECK(only_bytes(races[0].unit, 100, 0x5a, 100, 0x5a) && hos_ps5_guest_committed(G(races[0].unit), NULL) && !strncmp(perms, "rw-", 3),
          "a byte written between the pass's look and its lock: the unit is kept, the byte there (%s)", perms);
    CHECK(races[1].met && only_bytes(races[1].unit, 200, 0x77, 200, 0x77) && hos_ps5_guest_committed(G(races[1].unit), NULL) &&
          s1.reclaim_rebacked - s0.reclaim_rebacked == 2,
          "a write that faults while the unit is read-only and all zeros: it waits, the unit is put back, then given memory for the write: the byte is there");
    CHECK(races[2].met && only_bytes(races[2].unit, 100, 0x5a, 200, 0x77) && hos_ps5_guest_committed(G(races[2].unit), NULL) &&
          s1.fault_retries - s0.fault_retries >= 1 && s1.fault_refused == s0.fault_refused,
          "a write that faults while a written unit is read-only: it waits, the unit is kept, the write is retried: both bytes there (%llu retried, none refused)",
          (unsigned long long)(s1.fault_retries - s0.fault_retries));
    CHECK(races[3].commit_result == 0 && after == 4096 && G(races[3].unit)[4095] == 0xc3 && hos_ps5_guest_committed(G(races[3].unit), NULL),
          "a commit between the pass's look and its lock: the unit is left read-write, a read() into it lands");
    CHECK(races[4].met && races[4].commit_result == 0 && races[4].read_result == 4096 && G(races[4].unit)[4095] == 0xc3 && G(races[4].unit)[4096] == 0 &&
          hos_ps5_guest_committed(G(races[4].unit), NULL),
          "a commit while the unit is read-only: it waits, backs the unit the pass put back, and the read() that follows lands");
    CHECK(a2 - host_direct_allocations() == 13 - 2, "13 units' memory given back, 2 allocated again (%lld fewer allocations)", a2 - host_direct_allocations());

    /* four threads writing 8 MiB while passes run one after another */
    hos_ps5_guest_commit(G(STRESS_AT), (size_t)STRESS_UNITS * 65536);
    static struct stress writers[STRESS_THREADS];
    pthread_t wt[STRESS_THREADS];
    for (unsigned i = 0; i < STRESS_THREADS; i++) {
        writers[i] = (struct stress){.id = i, .seed = 0x9e3779b97f4a7c15ull * (i + 1), .copy = calloc(STRESS_UNITS, SLICE)};
        pthread_create(&wt[i], NULL, stress_writer, &writers[i]);
    }
    hos_ps5_guest_stats(&s0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t n_passes = 0, put_back = 0, written = 0, lock_max = 0, pass_max = 0, last_ops = 0;
    double stalled = 0, at = 0, last_at = 0;
    while (at < 3.0) {
        hos_ps5_guest_reclaim(&r);
        n_passes++, put_back += r.reclaimed, written += r.written;
        if (r.lock_ns_max > lock_max) lock_max = r.lock_ns_max;
        if (r.ns > pass_max) pass_max = r.ns;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        at = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        uint64_t ops = 0;
        for (unsigned i = 0; i < STRESS_THREADS; i++) ops += writers[i].ops;
        if (ops != last_ops) last_ops = ops, last_at = at;
        stalled = at - last_at;
        if (stalled > 10) { printf("FAIL no write for 10 s beside the passes: deadlock\n"); fflush(stdout); _exit(9); }
    }
    stress_stop = 1;
    unsigned long bad = 0;
    uint64_t ops = 0;
    for (unsigned i = 0; i < STRESS_THREADS; i++) pthread_join(wt[i], NULL), bad += writers[i].bad, ops += writers[i].ops;
    hos_ps5_guest_stats(&s1);
    unsigned long wrong = 0;   /* (every byte of the 8 MiB against the four copies, the writers stopped) */
    for (unsigned unit = 0; unit < STRESS_UNITS; unit++)
        for (unsigned i = 0; i < STRESS_THREADS; i++)
            wrong += memcmp(G(UNIT_AT(STRESS_AT, unit)) + i * SLICE, writers[i].copy + (size_t)unit * SLICE, SLICE) != 0;
    CHECK(!bad && !wrong && ops > 10000 && put_back > 100 && s1.reclaim_rebacked - s0.reclaim_rebacked > 100 && s1.fault_refused == s0.fault_refused &&
          s1.fault_failures == s0.fault_failures,
          "4 threads writing 8 MiB beside %llu passes: %llu writes, every byte as written (%lu wrong); %llu units put back, %llu given memory again "
          "(%llu soon after), %llu found written under the lock and kept; no fault refused",
          (unsigned long long)n_passes, (unsigned long long)ops, bad + wrong, (unsigned long long)put_back,
          (unsigned long long)(s1.reclaim_rebacked - s0.reclaim_rebacked), (unsigned long long)(s1.reclaim_flaps - s0.reclaim_flaps),
          (unsigned long long)written);
    printf("  the reclaim beside the writers: the longest pass %.1f ms, the longest lock %.0f us; %llu faults, %llu retried\n", pass_max / 1e6, lock_max / 1e3,
           (unsigned long long)(s1.faults - s0.faults), (unsigned long long)(s1.fault_retries - s0.fault_retries));
    for (unsigned i = 0; i < STRESS_THREADS; i++) free(writers[i].copy);
}

/* ---- (2026-11-12, the review on ps5-next-20) what keeps a unit the GPU may reach out of the reclaim besides its pin: the
   kernel's answer, for the whole unit. Units granted GPU access with no pin (an import the window did not hear of), a unit
   with a grant over half of it, a unit whose question the kernel refuses: each left alone, read-write and backed, and not
   looked at again this session ---- */
#define FENCE_AT (RECLAIM_AT + 8 * GIB)
static void reclaim_fences(void) {
    struct hos_ps5_guest_reclaim r;
    const uint64_t F = FENCE_AT;
    CHECK(hos_ps5_guest_commit(G(F), 16 * 65536) == 0, "fences: 16 units committed, left zero");
    void *s = NULL, *e = NULL;
    uint32_t prot = 0;
    const int asked = sceKernelQueryMemoryProtection(G(F), &s, &e, &prot);
    CHECK(asked == 0 && (uintptr_t)s <= (uintptr_t)G(F) && (uintptr_t)e >= (uintptr_t)G(F) + 65536 && prot == 3,
          "the model's kernel answers the question as the console does (%#x: %p..%p, protection %#x)", (unsigned)asked, s, e, (unsigned)prot);
    hos_ps5_guest_reclaim(&r);
    hos_ps5_guest_reclaim(&r);   /* (the commit's hold) */
    grants[0].lo = (uintptr_t)G(UNIT_AT(F, 2)), grants[0].hi = (uintptr_t)G(UNIT_AT(F, 3));            /* (a whole unit) */
    grants[1].lo = (uintptr_t)G(UNIT_AT(F, 5)) + 32768, grants[1].hi = (uintptr_t)G(UNIT_AT(F, 6));   /* (its second half) */
    refuse_lo = (uintptr_t)G(UNIT_AT(F, 9)), refuse_hi = (uintptr_t)G(UNIT_AT(F, 10));
    watch_lo = (uintptr_t)G(F), watch_hi = (uintptr_t)G(UNIT_AT(F, 16));
    const unsigned long refused0 = query_refused, watched0 = query_watched;
    hos_ps5_guest_reclaim(&r);   /* (units elsewhere that earlier checks left may go back in the same pass: the counts are >=) */
    const int kept = hos_ps5_guest_committed(G(UNIT_AT(F, 2)), NULL) && hos_ps5_guest_committed(G(UNIT_AT(F, 5)), NULL) &&
                     hos_ps5_guest_committed(G(UNIT_AT(F, 9)), NULL);
    unsigned others_back = 0;
    for (unsigned k = 0; k < 16; k++) others_back += k != 2 && k != 5 && k != 9 && !hos_ps5_guest_committed(G(UNIT_AT(F, k)), NULL);
    char p2[5] = "?", p5[5] = "?", p9[5] = "?";
    perms_of(BASE + UNIT_AT(F, 2), p2), perms_of(BASE + UNIT_AT(F, 5), p5), perms_of(BASE + UNIT_AT(F, 9), p9);
    CHECK(kept && others_back == 13 && r.foreign >= 3 && r.unanswered >= 1 && query_refused == refused0 + 1 && query_watched - watched0 == 13 * 2 + 3 &&
          !strncmp(p2, "rw-", 3) && !strncmp(p5, "rw-", 3) && !strncmp(p9, "rw-", 3),
          "a unit granted GPU access with no pin, one with a grant over its second half, one the kernel does not answer for: the three "
          "left read-write and backed (%s %s %s), the 13 others put back (%u; the pass: %llu not the CPU's alone, %llu unanswered; %lu "
          "questions, twice for a unit put back: before and after its protection)", p2, p5, p9, others_back, (unsigned long long)r.foreign,
          (unsigned long long)r.unanswered, query_watched - watched0);
    grants[0].lo = grants[0].hi = grants[1].lo = grants[1].hi = 0;
    refuse_lo = refuse_hi = 0;
    const unsigned long watched1 = query_watched;
    for (int i = 0; i < 4; i++) hos_ps5_guest_reclaim(&r);
    char q2[5] = "?", q9[5] = "?";
    perms_of(BASE + UNIT_AT(F, 2), q2), perms_of(BASE + UNIT_AT(F, 9), q9);
    CHECK(query_watched == watched1 && hos_ps5_guest_committed(G(UNIT_AT(F, 2)), NULL) && hos_ps5_guest_committed(G(UNIT_AT(F, 5)), NULL) &&
          hos_ps5_guest_committed(G(UNIT_AT(F, 9)), NULL) && !strncmp(q2, "rw-", 3) && !strncmp(q9, "rw-", 3),
          "the grants gone and the kernel answering again, four passes leave the three alone, not asked about again this session (%lu questions)",
          query_watched - watched1);
    watch_lo = watch_hi = 0;
    hos_ps5_guest_decommit(G(F), 16 * 65536);
}

/* ---- (2026-11-12, the review on ps5-next-20) the free pieces of direct memory: the memory watch gives up walking them past
   8192 questions and then takes the largest for the free memory (an early stop at the floor), and each unit put back may be a
   piece of its own. The watch's count is ps5_early.c's on the console (not linked here): this test gives it ---- */
static volatile uint64_t test_pieces, test_no_walk;
uint64_t hos_ps5_memory_pieces(uint64_t *no_walk) {
    if (no_walk) *no_walk = test_no_walk;
    return test_pieces;
}
#define VALVE_AT (RECLAIM_AT + 9 * GIB)
static unsigned valve_back(void) {   /* (the valve's units put back so far) */
    unsigned n = 0;
    for (unsigned k = 0; k < 64; k++) n += !hos_ps5_guest_committed(G(UNIT_AT(VALVE_AT, k)), NULL);
    return n;
}
static void reclaim_valve(void) {
    struct hos_ps5_guest_reclaim r;
    hos_ps5_guest_commit(G(VALVE_AT), 64 * 65536);
    hos_ps5_guest_reclaim(&r);
    hos_ps5_guest_reclaim(&r);   /* (the commit's hold) */
    test_pieces = 5000;
    hos_ps5_guest_reclaim(&r);
    const unsigned none = valve_back();
    CHECK(r.crowded && r.pieces == 5000 && !r.reclaimed && !none, "the watch counting 5000 free pieces: the pass stops at the first unit of zeros, "
          "puts none back and says why (%llu pieces, %u put back)", (unsigned long long)r.pieces, none);
    test_pieces = 2990;
    hos_ps5_guest_reclaim(&r);
    const unsigned few = valve_back();
    CHECK(r.crowded && r.reclaimed <= 10 && few <= 10 && few >= 1, "2990: the pass puts back units until 3000 pieces could be reached, and stops "
          "(%u of the 64 put back, %llu in the pass)", few, (unsigned long long)r.reclaimed);
    test_pieces = 100, test_no_walk++;
    hos_ps5_guest_reclaim(&r);
    const unsigned after_failed = valve_back();
    hos_ps5_guest_reclaim(&r);
    const unsigned all = valve_back();
    CHECK(after_failed == few && all == 64 && !r.crowded, "a walk of the pieces failed since the last pass: that pass puts none back; the next "
          "one, with no new failure and 100 pieces, puts the rest back (%u, then %u of 64)", after_failed, all);
    test_pieces = 0;
}

/* ---- (2026-11-12, the review on ps5-next-20) the reclaim beside everything at once. 8 writers over 256 units, each owning an
   eighth of every unit (a unit is all zeros only when the eight left theirs so); every word they write says who wrote it
   where, so 2 readers going round the same units can tell a stale or foreign word (the model's fresh memory reads 0xa5) from
   a written one or a zero. A thread whose file reads (a commit, then pread: the kernel's write, no fault) land in units it
   zeroes again after; one that pins units the passes had put back, one by one, each of which must stay backed and read-write
   through every pass after; passes one after another for 3 s. The hold after a commit: the window's pass numbers move with
   s2_started (counted here before each pass), so a commit made while it read S has until >= S + 2, and a read() refused
   before pass S + 2 started is a defect; after, it is the hold's design (two passes are a minute on the console, a few ms
   here) ---- */
#define S2_AT (RECLAIM_AT + 10 * GIB)
#define S2_UNITS 256
#define S2_WRITERS 8
#define S2_SLICE (65536 / S2_WRITERS)
#define IO_AT (S2_AT + GIB)
#define IO_UNITS 64
#define PINS_AT (S2_AT + 2 * GIB)
#define PIN_UNITS 256
static volatile int s2_stop;
static volatile uint64_t s2_started;
struct s2_writer { unsigned id; uint64_t seed; uint32_t gen; uint64_t *copy; volatile uint64_t ops; volatile unsigned long bad; };
static uint64_t s2_word(unsigned id, unsigned unit, unsigned w, uint32_t gen) {
    return (uint64_t)(id + 1) << 56 | (uint64_t)unit << 40 | (uint64_t)w << 24 | (gen & 0xffffffu);
}
static void *s2_write(void *p) {
    struct s2_writer *w = p;
    while (!s2_stop) {
        const unsigned unit = (unsigned)(next_random(&w->seed) % S2_UNITS), first = w->id * (S2_SLICE / 8);
        volatile uint64_t *mine = (volatile uint64_t *)G(UNIT_AT(S2_AT, unit)) + first;
        uint64_t *copy = w->copy + (size_t)unit * (S2_SLICE / 8);
        for (unsigned i = 0; i < S2_SLICE / 8; i++) w->bad += mine[i] != copy[i];
        const unsigned what = (unsigned)(next_random(&w->seed) % 100);
        if (what < 80) {   /* (its eighth cleared) */
            for (unsigned i = 0; i < S2_SLICE / 8; i++) mine[i] = 0, copy[i] = 0;
        } else {
            const unsigned from = what < 97 ? (unsigned)(next_random(&w->seed) % (S2_SLICE / 8)) : 0;
            const unsigned n = what < 97 ? 1 + (unsigned)(next_random(&w->seed) % 16) : S2_SLICE / 8;
            w->gen++;
            for (unsigned i = from; i < from + n && i < S2_SLICE / 8; i++) {
                const uint64_t v = s2_word(w->id, unit, first + i, w->gen);
                copy[i] = v, mine[i] = v;
            }
        }
        for (unsigned i = 0; i < S2_SLICE / 8; i++) w->bad += mine[i] != copy[i];
        w->ops++;
    }
    return NULL;
}
struct s2_reader { uint64_t seed; volatile uint64_t units; volatile unsigned long bad; uint64_t first_bad; };
static void *s2_read(void *p) {
    struct s2_reader *rd = p;
    while (!s2_stop) {
        const unsigned unit = (unsigned)(next_random(&rd->seed) % S2_UNITS);
        const volatile uint64_t *w = (const volatile uint64_t *)G(UNIT_AT(S2_AT, unit));
        for (unsigned i = 0; i < 65536 / 8; i++) {
            const uint64_t v = w[i];
            if (!v) continue;
            const unsigned id = (unsigned)(v >> 56) - 1;
            if (id >= S2_WRITERS || ((v >> 40) & 0xffff) != unit || ((v >> 24) & 0xffff) != i || i / (S2_SLICE / 8) != id) {
                if (!rd->bad++) rd->first_bad = v;
            }
        }
        rd->units++;
    }
    return NULL;
}
struct s2_io { int fd; uint64_t seed; volatile uint64_t reads; volatile unsigned long early, late, wrong; };
static void *s2_copy(void *p) {
    struct s2_io *io = p;
    while (!s2_stop) {
        const unsigned unit = (unsigned)(next_random(&io->seed) % IO_UNITS), page = (unsigned)(next_random(&io->seed) % 16);
        uint8_t *at = G(UNIT_AT(IO_AT, unit)) + page * 4096;
        const uint64_t before = __atomic_load_n(&s2_started, __ATOMIC_SEQ_CST);
        hos_ps5_guest_commit(at, 4096);
        errno = 0;
        const ssize_t got = pread(io->fd, at, 4096, 0);
        const uint64_t after = __atomic_load_n(&s2_started, __ATOMIC_SEQ_CST);
        if (got != 4096) {
            if (after < before + 2) io->early++;   /* (refused within the hold: a defect) */
            else io->late++;
        } else {
            for (unsigned i = 0; i < 4096; i++) io->wrong += at[i] != (uint8_t)(0x5a ^ i);
            memset(at, 0, 4096);   /* (zero again: the passes may take the unit back) */
        }
        io->reads++;
    }
    return NULL;
}
struct s2_pin { volatile unsigned pinned; volatile unsigned long lost, not_rw, failed; };
static void *s2_pins(void *p) {
    struct s2_pin *pn = p;
    for (unsigned k = 0; k < PIN_UNITS && !s2_stop; k++) {
        if (hos_ps5_guest_pin(G(UNIT_AT(PINS_AT, k)), 65536)) pn->failed++;
        pn->pinned = k + 1;
        for (unsigned j = 0; j <= k; j += (k > 64 ? 7 : 1)) {   /* (all pinned so far, a sample once there are many) */
            void *s = NULL, *e = NULL;
            uint32_t prot = 0;
            pn->lost += !hos_ps5_guest_committed(G(UNIT_AT(PINS_AT, j)), NULL);
            pn->not_rw += sceKernelQueryMemoryProtection(G(UNIT_AT(PINS_AT, j)), &s, &e, &prot) != 0 || (prot & 3) != 3;
        }
        usleep(2000);
    }
    return NULL;
}
static void reclaim_stress(void) {
    struct hos_ps5_guest_stats s0, s1;
    struct hos_ps5_guest_reclaim r;
    hos_ps5_guest_commit(G(S2_AT), (size_t)S2_UNITS * 65536);
    hos_ps5_guest_commit(G(IO_AT), (size_t)IO_UNITS * 65536);
    hos_ps5_guest_commit(G(PINS_AT), (size_t)PIN_UNITS * 65536);
    for (int i = 0; i < 3; i++) hos_ps5_guest_reclaim(&r);   /* (past the commits' hold: the pins' range goes back behind the zeros) */
    const int pins_back = !hos_ps5_guest_committed(G(PINS_AT), NULL);
    uint8_t page[4096];
    for (unsigned i = 0; i < 4096; i++) page[i] = (uint8_t)(0x5a ^ i);
    struct s2_io io = {.fd = memfd_create("reclaim-io", 0), .seed = 0x2545f4914f6cdd1dull};
    const int filled = io.fd >= 0 && write(io.fd, page, sizeof page) == (ssize_t)sizeof page;
    static struct s2_writer wr[S2_WRITERS];
    static struct s2_reader rd[2];
    struct s2_pin pn = {0};
    pthread_t tw[S2_WRITERS], trd[2], tio, tpin;
    s2_stop = 0;
    hos_ps5_guest_stats(&s0);
    for (unsigned i = 0; i < S2_WRITERS; i++) {
        wr[i] = (struct s2_writer){.id = i, .seed = 0x9e3779b97f4a7c15ull * (i + 7), .copy = calloc(S2_UNITS, S2_SLICE)};
        pthread_create(&tw[i], NULL, s2_write, &wr[i]);
    }
    for (unsigned i = 0; i < 2; i++) rd[i] = (struct s2_reader){.seed = 0xd1b54a32d192ed03ull * (i + 1)}, pthread_create(&trd[i], NULL, s2_read, &rd[i]);
    pthread_create(&tio, NULL, s2_copy, &io);
    pthread_create(&tpin, NULL, s2_pins, &pn);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t passes = 0, put_back = 0, kept_written = 0, last = 0, pass_max = 0, lock_max = 0;
    double at = 0, last_at = 0;
    while (at < 3.0) {
        __atomic_add_fetch(&s2_started, 1, __ATOMIC_SEQ_CST);
        hos_ps5_guest_reclaim(&r);
        passes++, put_back += r.reclaimed, kept_written += r.written;
        if (r.ns > pass_max) pass_max = r.ns;
        if (r.lock_ns_max > lock_max) lock_max = r.lock_ns_max;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        at = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        uint64_t ops = io.reads;
        for (unsigned i = 0; i < S2_WRITERS; i++) ops += wr[i].ops;
        if (ops != last) last = ops, last_at = at;
        if (at - last_at > 10) { printf("FAIL nothing done for 10 s beside the passes: deadlock\n"); fflush(stdout); _exit(9); }
    }
    s2_stop = 1;
    for (unsigned i = 0; i < S2_WRITERS; i++) pthread_join(tw[i], NULL);
    for (unsigned i = 0; i < 2; i++) pthread_join(trd[i], NULL);
    pthread_join(tio, NULL);
    pthread_join(tpin, NULL);
    hos_ps5_guest_stats(&s1);
    unsigned long bad = 0, wrong = 0, read_bad = 0;
    uint64_t ops = 0, units_read = 0, first_bad = 0;
    for (unsigned i = 0; i < S2_WRITERS; i++) bad += wr[i].bad, ops += wr[i].ops;
    for (unsigned i = 0; i < 2; i++) read_bad += rd[i].bad, units_read += rd[i].units, first_bad = first_bad ? first_bad : rd[i].first_bad;
    for (unsigned unit = 0; unit < S2_UNITS; unit++)   /* (every word against the eight copies, the writers stopped) */
        for (unsigned i = 0; i < S2_WRITERS; i++)
            wrong += memcmp(G(UNIT_AT(S2_AT, unit)) + i * S2_SLICE, (uint8_t *)wr[i].copy + (size_t)unit * S2_SLICE, S2_SLICE) != 0;
    unsigned long pins_lost = 0;
    for (unsigned k = 0; k < pn.pinned; k++) pins_lost += !hos_ps5_guest_committed(G(UNIT_AT(PINS_AT, k)), NULL) || !hos_ps5_guest_pinned(G(UNIT_AT(PINS_AT, k)), 1);
    CHECK(!bad && !wrong && ops > 10000 && put_back > 100 && s1.reclaim_rebacked - s0.reclaim_rebacked > 100 && s1.fault_refused == s0.fault_refused &&
          s1.fault_failures == s0.fault_failures, "8 threads writing 16 MiB beside %llu passes: %llu writes, every word as written (%lu wrong); %llu units "
          "put back, %llu given memory again, %llu found written under the lock and kept; no fault refused", (unsigned long long)passes,
          (unsigned long long)ops, bad + wrong, (unsigned long long)put_back, (unsigned long long)(s1.reclaim_rebacked - s0.reclaim_rebacked),
          (unsigned long long)kept_written);
    CHECK(!read_bad && units_read > 1000, "2 readers going round the same units: %llu units read whole, every word zero or one a writer wrote there "
          "(%lu not: the first %#llx)", (unsigned long long)units_read, read_bad, (unsigned long long)first_bad);
    CHECK(filled && !io.early && !io.wrong && io.reads > 1000, "a thread's file reads into units the passes take back: %llu reads, %lu refused within "
          "their commit's hold, %lu bytes wrong (%lu refused once the hold was over: its design, two passes are a minute on the console)",
          (unsigned long long)io.reads, io.early, io.wrong, io.late);
    CHECK(pins_back && pn.pinned > 50 && !pn.lost && !pn.not_rw && !pn.failed && !pins_lost, "%u units put back by the passes, then pinned one by one "
          "beside them: every one stays backed, pinned and read-write through the passes after (%lu lost, %lu not read-write)",
          pn.pinned, pn.lost + pins_lost, pn.not_rw);
    printf("  the reclaim beside all of them: the longest pass %.1f ms, the longest lock %.0f us; %llu faults, %llu retried; %lu questions to "
           "the kernel\n", pass_max / 1e6, lock_max / 1e3, (unsigned long long)(s1.faults - s0.faults),
           (unsigned long long)(s1.fault_retries - s0.fault_retries), query_asked);
    for (unsigned i = 0; i < S2_WRITERS; i++) free(wr[i].copy);
    if (io.fd >= 0) close(io.fd);
}

/* ---- (2026-11-12) cold units on the SSD (HOOKSHOT_PS5_COLD: ps5_window.c's header). Each case in a child of its own: the
   settings are read once a process, and what cold_fatal does (it ends the process) is a case's expected end. The children
   work in GiB 300 to 307, which nothing else here backs; their files go in HOOKSHOT_TEST_COLD_DIR (tests/test_cold.py gives
   one on disk; never /tmp, which is memory here) ---- */
int hos_ps5_cold_park_now(void *p);
unsigned hos_ps5_cold_state(const void *p, uint32_t *slot);
extern void (*hos_ps5_cold_writer_hook)(int step, const uint64_t *units, unsigned n);
extern void (*hos_ps5_cold_fatal_hook)(const char *what, uint64_t guest, int32_t error);
enum { CS_PARKED = 1, CS_SPILLED = 2, CS_CLEAN = 4, CS_FOREIGN = 8, CS_ZERO = 16, CS_STAGED = 32, CS_DEAD = 64 };
#define CU(gib, i) (G((uint64_t)(gib) * GIB + (uint64_t)(i) * 65536))
static char cold_dir[400];
static void on_segv_rw(int sig, siginfo_t *si, void *uc) {   /* (the console's: the access' sense from the error code, mkw_glue.c) */
    const ucontext_t *c = (const ucontext_t *)uc;
    if (hos_ps5_guest_fault2(si->si_addr, (c->uc_mcontext.gregs[REG_ERR] & 2) != 0)) return;
    signal(sig, SIG_DFL);
}
static uint64_t cw(uint64_t unit_addr, unsigned w, uint32_t gen) {   /* (a word that says where it is and which round wrote it) */
    return (unit_addr >> 16) << 36 ^ (uint64_t)w << 18 ^ gen ^ 0x5a00000000000000ull;
}
static void cold_fill(uint8_t *unit, uint32_t gen) {
    uint64_t *const w = (uint64_t *)unit;
    for (unsigned i = 0; i < 8192; i++) w[i] = cw((uint64_t)(uintptr_t)unit, i, gen);
}
static int cold_right(const uint8_t *unit, uint32_t gen) {   /* every word of the unit as cold_fill(gen) wrote it */
    const volatile uint64_t *const w = (const volatile uint64_t *)unit;
    for (unsigned i = 0; i < 8192; i++)
        if (w[i] != cw((uint64_t)(uintptr_t)unit, i, gen)) return 0;
    return 1;
}
static int kernel_reads(const void *p) {   /* 1: the kernel can read 4 KiB there (a system call given an unreachable unit: EFAULT) */
    int fds[2];
    if (pipe(fds)) return -1;
    const ssize_t n = write(fds[1], p, 4096);
    close(fds[0]), close(fds[1]);
    return n == 4096;
}
static void cold_env(const char *mode, const char *sub) {
    failures = 0;   /* (a child's own count: the parent's so far came with the fork) */
    install();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv_rw;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    char d[480];
    snprintf(d, sizeof d, "%s/%s", cold_dir, sub);
    mkdir(d, 0777);
    setenv("HOOKSHOT_PS5_COLD", mode, 1);
    setenv("HOOKSHOT_PS5_COLD_DIR", d, 1);
    setenv("HOOKSHOT_PS5_COLD_LO_GIB", "300", 1);
    setenv("HOOKSHOT_PS5_COLD_HI_GIB", "308", 1);
    setenv("HOOKSHOT_PS5_COLD_AFTER_S", "0", 1);
    setenv("HOOKSHOT_PS5_COLD_HOT_S", "0", 1);
    if (!getenv("HOOKSHOT_PS5_COLD_HOLD_S")) setenv("HOOKSHOT_PS5_COLD_HOLD_S", "0", 1);
    /* (the sweep off but where a case asks for it: the cases park by hand, hos_ps5_cold_park_now, so that what goes to the SSD
       is what they say) */
    if (!getenv("HOOKSHOT_PS5_COLD_PARK_RATE")) setenv("HOOKSHOT_PS5_COLD_PARK_RATE", "0", 1);
    setenv("HOOKSHOT_PS5_COLD_WRITE_MB_S", "100000", 1);
    setenv("HOOKSHOT_PS5_COLD_RESERVE_MB", "1", 1);
    setenv("HOOKSHOT_PS5_COLD_SEG_MB", "4", 1);
    hos_ps5_guest_pin(G(299 * GIB), 65536);   /* (the pins' setting read, as the renderer's first import reads it) */
}
static int cold_start_child(const char *mode, const char *sub) {
    cold_env(mode, sub);
    char said[600];
    const int m = hos_ps5_cold_start(said, sizeof said);
    printf("  [%s] %s\n", sub, said);
    return m;
}
/* the cold thread's steps until `cond` holds or `max` steps (calm, no free memory: everything cold may go) */
#define COLD_TICKS(cond, max) do { for (int k_ = 0; k_ < (max) && !(cond); k_++) { hos_ps5_cold_tick(1, 0); usleep(2000); } } while (0)
static uint64_t cold_now(uint64_t (*f)(const struct hos_ps5_cold *)) { struct hos_ps5_cold c; hos_ps5_cold_stats(&c); return f(&c); }
static uint64_t c_spilled(const struct hos_ps5_cold *c) { return c->spilled; }
static uint64_t c_written(const struct hos_ps5_cold *c) { return c->written; }
static uint64_t c_reloads(const struct hos_ps5_cold *c) { return c->reloads; }
static int file_slot_right(uint32_t slot1, const uint8_t *unit, uint32_t gen) {   /* the copy on the SSD of the unit's bytes */
    char path[560];
    snprintf(path, sizeof path, "%s/hookshot-cold-%02u.bin", getenv("HOOKSHOT_PS5_COLD_DIR"), (unsigned)((slot1 - 1) / 64));
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    static uint64_t buf[8192];
    const ssize_t n = pread(fd, buf, 65536, (off_t)(((slot1 - 1) % 64) * 65536ull));
    close(fd);
    if (n != 65536) return 0;
    for (unsigned i = 0; i < 8192; i++)
        if (buf[i] != cw((uint64_t)(uintptr_t)unit, i, gen)) return 0;
    return 1;
}

static int cold_off_child(void) {   /* unset: nothing starts, no notes, the paths as before */
    failures = 0;
    install();
    unsetenv("HOOKSHOT_PS5_COLD");
    char said[600];
    const int m = hos_ps5_cold_start(said, sizeof said);
    cold_fill(CU(300, 0), 1);
    for (int k = 0; k < 20; k++) hos_ps5_cold_tick(1, 0);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    CHECK(m == HOS_PS5_COLD_OFF && strstr(said, "off") && c.parks == 0 && c.ticks == 0 && hos_ps5_cold_state(CU(300, 0), NULL) == 0 &&
          hos_ps5_cold_park_now(CU(300, 0)) == 0 && cold_right(CU(300, 0), 1),
          "cold units: HOOKSHOT_PS5_COLD unset: off (\"%s\"), no step taken, no unit parked", said);
    return failures;
}

static int cold_typo_child(void) {   /* (2026-11-13) a value not understood ("mesure" for "measure"): off, said so, never the real mode */
    cold_env("mesure", "typo");
    char said[600];
    const int m = hos_ps5_cold_start(said, sizeof said);
    cold_fill(CU(300, 0), 1);
    for (int k = 0; k < 20; k++) hos_ps5_cold_tick(1, 0);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    CHECK(m == HOS_PS5_COLD_OFF && strstr(said, "not understood") && c.parks == 0 && hos_ps5_cold_park_now(CU(300, 0)) == 0 &&
          hos_ps5_cold_state(CU(300, 0), NULL) == 0 && cold_right(CU(300, 0), 1),
          "cold units: HOOKSHOT_PS5_COLD=mesure (not a value it knows): off (\"%s\"), nothing parked", said);
    return failures;
}

static int cold_rules_child(void) {
    if (cold_start_child("1", "rules") != HOS_PS5_COLD_ON) return 100;
    const long long a0 = host_direct_allocations();
    for (unsigned i = 0; i < 64; i++) cold_fill(CU(300, i), 1);
    const long long a1 = host_direct_allocations();
    int parked = 0;
    for (unsigned i = 0; i < 64; i++) parked += hos_ps5_cold_park_now(CU(300, i));
    char perms[5] = "?";
    perms_of(BASE + 300 * GIB, perms);
    size_t run = 0;
    CHECK(parked == 64 && host_direct_allocations() == a1 + 1 && hos_ps5_cold_state(CU(300, 5), NULL) == CS_PARKED && kernel_reads(CU(300, 5)) == 0 &&
          !strncmp(perms, "---", 3) && hos_ps5_guest_committed(CU(300, 0), &run) && run >= 64 * 65536 && !hos_ps5_guest_untouched(CU(300, 0), 65536),
          "64 units parked: their memory held (%lld allocations since, the GiB's notes), unreachable (%s, a kernel read refused), committed, not untouched",
          host_direct_allocations() - a1, perms);
    uint64_t pin_b = 0, free_b = 0;
    CHECK(hos_ps5_guest_backed_bytes(CU(300, 0), 64 * 65536, &pin_b, &free_b) == 64 * 65536 && free_b == 64 * 65536 && pin_b == 0,
          "parked units count as memory held, which a decommit would give back");
    /* to the SSD */
    COLD_TICKS(cold_now(c_spilled) >= 64, 200);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    uint32_t slot = 0;
    const unsigned st5 = hos_ps5_cold_state(CU(300, 5), &slot);
    CHECK(c.spilled == 64 && c.written == 64 * 65536ull && c.spills_written == 64 && st5 == CS_SPILLED && slot && file_slot_right(slot, CU(300, 5), 1),
          "the 64 on the SSD: %llu MiB written in %llu writes, unit 5's copy in slot %u is its bytes", (unsigned long long)(c.written >> 20),
          (unsigned long long)c.writes, slot - 1);
    CHECK(host_direct_allocations() == a1 + 1 + 16 - 64, "their memory given back: %lld allocations since the fill (the notes, a reserve of 16)",
          host_direct_allocations() - a1);
    /* a read brings a unit back read-only, its copy kept */
    const uint64_t first = *(volatile uint64_t *)CU(300, 0);
    perms_of(BASE + 300 * GIB, perms);
    hos_ps5_cold_stats(&c);
    CHECK(first == cw((uint64_t)(uintptr_t)CU(300, 0), 0, 1) && cold_right(CU(300, 0), 1) && !strncmp(perms, "r--", 3) &&
          hos_ps5_cold_state(CU(300, 0), &slot) == CS_CLEAN && slot && c.reloads == 1 && c.clean == 1,
          "a read of unit 0 brought it back from the SSD, every word right, read-only (%s) with its copy", perms);
    /* a write then makes it writable and forgets the copy */
    *(volatile uint64_t *)(CU(300, 0) + 8) = 0x1234;
    perms_of(BASE + 300 * GIB, perms);
    CHECK(!strncmp(perms, "rw-", 3) && hos_ps5_cold_state(CU(300, 0), &slot) == 0 && slot == 0 && *(volatile uint64_t *)(CU(300, 0) + 8) == 0x1234,
          "a write into it then: read-write (%s), its copy forgotten", perms);
    cold_fill(CU(300, 0), 2);
    /* written once while it does not change: unit 1 read (clean), parked and cold again: back to the SSD with no write */
    CHECK(cold_right(CU(300, 1), 1) && hos_ps5_cold_state(CU(300, 1), NULL) == CS_CLEAN, "unit 1 read back: clean");
    const uint64_t w0 = cold_now(c_written);
    CHECK(hos_ps5_cold_park_now(CU(300, 1)) == 1, "the clean unit parked (the kernel says read-only, as it must be)");
    COLD_TICKS(hos_ps5_cold_state(CU(300, 1), NULL) & CS_SPILLED, 200);
    hos_ps5_cold_stats(&c);
    CHECK((hos_ps5_cold_state(CU(300, 1), NULL) & CS_SPILLED) && c.written == w0 && c.spills_again == 1,
          "parked again and cold: on the SSD with no new write (%llu bytes written before, %llu after)", (unsigned long long)w0, (unsigned long long)c.written);
    CHECK(cold_right(CU(300, 1), 1), "and read back right");
    /* unit 0, written since: written again */
    CHECK(hos_ps5_cold_park_now(CU(300, 0)) == 1, "unit 0 (written since it came back) parked");
    COLD_TICKS(hos_ps5_cold_state(CU(300, 0), NULL) & CS_SPILLED, 200);
    hos_ps5_cold_stats(&c);
    CHECK(c.written == w0 + 65536 && cold_right(CU(300, 0), 2), "written once more, read back with what was written last");
    /* a write fault on a unit on the SSD: read-write at once, its copy forgotten */
    *(volatile uint64_t *)CU(300, 2) = 77;
    CHECK(hos_ps5_cold_state(CU(300, 2), &slot) == 0 && slot == 0 && *(volatile uint64_t *)CU(300, 2) == 77 &&
          ((volatile uint64_t *)CU(300, 2))[1] == cw((uint64_t)(uintptr_t)CU(300, 2), 1, 1), "a write into a unit on the SSD: brought back read-write, the rest of it right");
    /* zeros: nothing written */
    memset(CU(300, 3), 0, 65536);
    CHECK(hos_ps5_cold_park_now(CU(300, 3)) == 1, "a unit of zeros parked");
    const uint64_t w1 = cold_now(c_written);
    COLD_TICKS(hos_ps5_cold_state(CU(300, 3), NULL) & CS_SPILLED, 200);
    hos_ps5_cold_stats(&c);
    int zeros = 1;
    for (unsigned i = 0; i < 8192; i++) zeros &= ((volatile uint64_t *)CU(300, 3))[i] == 0;
    CHECK(c.written == w1 && c.spills_zero == 1 && zeros, "it went to the SSD with nothing written, and reads zeros");
    /* a commit (what ReadFile makes before its system call): brought back read-write, a kernel write into it goes through */
    CHECK((hos_ps5_cold_state(CU(300, 10), NULL) & CS_SPILLED) && (hos_ps5_cold_state(CU(300, 11), NULL) & CS_SPILLED), "units 10 and 11 on the SSD");
    (void)*(volatile uint64_t *)CU(300, 12);   /* (unit 12: read, clean) */
    CHECK(hos_ps5_guest_commit(CU(300, 10), 3 * 65536) == 0, "a commit over two units on the SSD and a clean one");
    perms_of(BASE + 300 * GIB + 12 * 65536, perms);
    int fds[2];
    static uint8_t outside[4096];
    if (pipe(fds) || write(fds[1], outside, 4096) != 4096) return 101;   /* (a kernel write into guest memory, from a pipe) */
    const ssize_t got = read(fds[0], CU(300, 11) + 200, 4096);
    close(fds[0]), close(fds[1]);
    CHECK(got == 4096 && !strncmp(perms, "rw-", 3) && cold_right(CU(300, 10), 1) && hos_ps5_cold_state(CU(300, 12), NULL) == 0,
          "after it: a kernel write into one of them went through (%zd bytes), the clean one read-write (%s), all right", got, perms);
    /* a pin: brought back read-write and pinned; never parked again */
    CHECK(hos_ps5_guest_pin(CU(300, 20), 2 * 65536) == 0 && hos_ps5_guest_pinned(CU(300, 20), 65536) && cold_right(CU(300, 20), 1) &&
          hos_ps5_cold_state(CU(300, 20), NULL) == 0 && hos_ps5_cold_park_now(CU(300, 20)) == 0 && hos_ps5_cold_park_now(CU(300, 21)) == 0,
          "a pin over units on the SSD: back read-write, pinned, and the park refuses them");
    /* decommit: a whole unit on the SSD forgotten unread; a part of one brought back and zeroed */
    const uint64_t r0 = cold_now(c_reloads);
    const long long a2 = host_direct_allocations();
    CHECK(hos_ps5_guest_decommit(CU(300, 40), 4 * 65536) == 0 && cold_now(c_reloads) == r0 && host_direct_allocations() == a2 &&
          !hos_ps5_guest_committed(CU(300, 40), NULL) && hos_ps5_cold_state(CU(300, 40), NULL) == 0,
          "a decommit of 4 units on the SSD: nothing read back, nothing to give back, not committed");
    hos_ps5_guest_decommit(CU(300, 50) + 4096, 8192);
    CHECK(cold_now(c_reloads) == r0 + 1 && CU(300, 50)[4096] == 0 && CU(300, 50)[12287] == 0 &&
          ((volatile uint64_t *)CU(300, 50))[0] == cw((uint64_t)(uintptr_t)CU(300, 50), 0, 1) &&
          ((volatile uint64_t *)CU(300, 50))[2000] == cw((uint64_t)(uintptr_t)CU(300, 50), 2000, 1),
          "a decommit of 8 KiB inside one: brought back, those bytes zero, the rest right");
    /* units the GPU may reach, or the kernel does not answer for: never parked */
    cold_fill(CU(301, 0), 1), cold_fill(CU(301, 1), 1), cold_fill(CU(301, 2), 1);
    grants[0].lo = (uintptr_t)CU(301, 0), grants[0].hi = (uintptr_t)CU(301, 1);
    grants[1].lo = (uintptr_t)CU(301, 1) + 32768, grants[1].hi = (uintptr_t)CU(301, 2);
    refuse_lo = (uintptr_t)CU(301, 2), refuse_hi = (uintptr_t)CU(301, 3);
    const int p0 = hos_ps5_cold_park_now(CU(301, 0)), p1 = hos_ps5_cold_park_now(CU(301, 1)), p2 = hos_ps5_cold_park_now(CU(301, 2));
    hos_ps5_cold_stats(&c);
    CHECK(!p0 && !p1 && !p2 && hos_ps5_cold_state(CU(301, 0), NULL) == CS_FOREIGN && hos_ps5_cold_state(CU(301, 1), NULL) == CS_FOREIGN &&
          hos_ps5_cold_state(CU(301, 2), NULL) == CS_FOREIGN && c.foreign == 3 && c.unanswered == 1,
          "a unit with GPU access nobody pinned, one with it over half, one the kernel does not answer for: never parked (%llu, %llu unanswered)",
          (unsigned long long)c.foreign, (unsigned long long)c.unanswered);
    grants[0].lo = grants[0].hi = grants[1].lo = grants[1].hi = 0, refuse_lo = refuse_hi = 0;
    /* every unit still right, through faults */
    int right = 1;
    for (unsigned i = 4; i < 64; i++)
        if (i != 10 && i != 11 && i != 12 && i != 20 && i != 21 && !(i >= 40 && i < 44) && i != 50) right &= cold_right(CU(300, i), 1);
    char line[1600];
    hos_ps5_cold_line(line, sizeof line);
    printf("  [rules] %s\n", line);
    CHECK(right && strstr(line, "on: ") && strstr(line, "on the SSD now"), "every other unit read back right; the line says it");
    (void)a0;
    return failures;
}

static int cold_spill_some(unsigned gib, unsigned n, uint32_t gen) {   /* n units filled, parked and on the SSD */
    for (unsigned i = 0; i < n; i++) cold_fill(CU(gib, i), gen);
    int parked = 0;
    for (unsigned i = 0; i < n; i++) parked += hos_ps5_cold_park_now(CU(gib, i));
    COLD_TICKS(cold_now(c_spilled) >= n, 300);
    return parked == (int)n && cold_now(c_spilled) == n;
}
static int seg_fd(void) {   /* the descriptor of hookshot-cold-00.bin in this process */
    char p[64], l[600];
    for (int fd = 3; fd < 1024; fd++) {
        snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
        const ssize_t n = readlink(p, l, sizeof l - 1);
        if (n > 0) { l[n] = 0; if (strstr(l, "hookshot-cold-00.bin")) return fd; }
    }
    return -1;
}
static int cold_open_fails_child(void) {   /* the files cannot be made: nothing goes, nothing is lost */
    char d[480];
    snprintf(d, sizeof d, "%s/no-such-folder/deeper", cold_dir);
    cold_env("1", "unused");
    setenv("HOOKSHOT_PS5_COLD_DIR", d, 1);
    char said[600];
    if (hos_ps5_cold_start(said, sizeof said) != HOS_PS5_COLD_ON) return 100;
    int parked = 0;
    for (unsigned i = 0; i < 32; i++) cold_fill(CU(302, i), 3), parked += hos_ps5_cold_park_now(CU(302, i));
    for (int k = 0; k < 20; k++) hos_ps5_cold_tick(1, 0);
    if (parked != 32 || hos_ps5_cold_state(CU(302, 3), NULL) != CS_PARKED) return 101;
    int right = 1;
    for (unsigned i = 0; i < 32; i++) right &= cold_right(CU(302, i), 3);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    CHECK(c.spilled == 0 && c.writes_stopped && c.stop_why == 2 && right && c.unparks == 32,
          "the SSD's file cannot be made: no unit went (writes stopped), all 32 came back from parking right (%llu on the SSD, stopped %d "
          "why %d, right %d, %llu came back)", (unsigned long long)c.spilled, c.writes_stopped, c.stop_why, right, (unsigned long long)c.unparks);
    return failures;
}
static int cold_enospc_child(void) {
    if (cold_start_child("1", "enospc") != HOS_PS5_COLD_ON) return 100;
    if (!cold_spill_some(302, 16, 4)) return 101;
    const int fd = seg_fd(), full = open("/dev/full", O_WRONLY);
    if (fd < 0 || full < 0 || dup2(full, fd) < 0) return 102;   /* (the SSD full from here on) */
    for (unsigned i = 16; i < 48; i++) cold_fill(CU(302, i), 4), hos_ps5_cold_park_now(CU(302, i));
    for (int k = 0; k < 20; k++) hos_ps5_cold_tick(1, 0);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    int right = 1;
    for (unsigned i = 16; i < 48; i++) right &= cold_right(CU(302, i), 4);
    CHECK(c.write_errors == 1 && c.writes_stopped && c.stop_why == 3 && c.spilled == 16 && right,
          "a write refused (ENOSPC): writes stop, the units of that batch stay parked and come back right (%llu on the SSD from before)",
          (unsigned long long)c.spilled);
    return failures;
}
static int cold_fatal_case(int how) {   /* a unit that cannot come back: the process ends (77) with the line, never runs on */
    if (cold_start_child("1", how == 0 ? "short" : how == 1 ? "sum" : how == 2 ? "nomem" : "sum-end") != HOS_PS5_COLD_ON) return 100;
    if (!cold_spill_some(303, 40, 5)) return 101;
    const int fd = seg_fd();
    if (fd < 0) return 102;
    if (how == 0 && ftruncate(fd, 0)) return 103;            /* the SSD gives nothing back */
    if (how == 1) {                                           /* a byte of unit 7's copy changed */
        uint32_t slot = 0;
        hos_ps5_cold_state(CU(303, 7), &slot);
        const uint8_t x = 0xee;
        if (pwrite(fd, &x, 1, (off_t)((slot - 1) % 64 * 65536ull + 1234)) != 1) return 104;
    }
    if (how == 3) {   /* (2026-11-13) the last byte of unit 7's copy changed: the sum covers the whole unit */
        uint32_t slot = 0;
        hos_ps5_cold_state(CU(303, 7), &slot);
        uint8_t x = 0;
        const off_t at = (off_t)((slot - 1) % 64 * 65536ull + 65535);
        if (pread(fd, &x, 1, at) != 1) return 104;
        x = (uint8_t)~x;
        if (pwrite(fd, &x, 1, at) != 1) return 104;
    }
    if (how == 2) {   /* no direct memory: the reserve's 16 units, then nothing */
        host_fail(HOST_CALL_ALLOCATE, -1);
        int right = 1;
        for (unsigned i = 20; i < 36; i++) right &= cold_right(CU(303, i), 5);
        struct hos_ps5_cold c;
        hos_ps5_cold_stats(&c);
        printf("  [nomem] the reserve: %llu units used, %llu left, every word right: %d\n", (unsigned long long)c.from_reserve,
               (unsigned long long)c.reserve_now, right);
        if (!right || c.from_reserve != 16) return 105;
    }
    (void)*(volatile uint64_t *)CU(303, 7);   /* (should end the process) */
    return 106;
}
static int cold_fatal_short(void) { return cold_fatal_case(0); }
static int cold_fatal_sum(void) { return cold_fatal_case(1); }
static int cold_fatal_nomem(void) { return cold_fatal_case(2); }
static int cold_fatal_sum_end(void) { return cold_fatal_case(3); }
static int cold_park_refused_child(void) {
    if (cold_start_child("1", "refused") != HOS_PS5_COLD_ON) return 100;
    cold_fill(CU(304, 0), 6);
    hos_ps5_cold_park_now(CU(304, 1));   /* (the GiB's notes made first) */
    host_fail(HOST_CALL_RESERVE, 1);
    const int r = hos_ps5_cold_park_now(CU(304, 0));
    host_fail(HOST_CALL_NONE, 0);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    char perms[5] = "?";
    perms_of(BASE + 304 * GIB, perms);
    CHECK(r == -1 && c.park_refused == 1 && hos_ps5_cold_state(CU(304, 0), NULL) == 0 && !strncmp(perms, "rw-", 3) && cold_right(CU(304, 0), 6),
          "a park the kernel refuses: the unit mapped back as it was (%s), every word right", perms);
    return failures;
}
static int cold_hold_child(void) {   /* a commit holds its units: no park until hold_s has passed */
    setenv("HOOKSHOT_PS5_COLD_HOLD_S", "2", 1);
    if (cold_start_child("1", "hold") != HOS_PS5_COLD_ON) return 100;
    cold_fill(CU(305, 0), 7);
    const int fresh = hos_ps5_cold_park_now(CU(305, 0));   /* (the GiB's notes made now: every unit held for 2 s) */
    hos_ps5_guest_commit(CU(305, 1), 65536);
    cold_fill(CU(305, 1), 7);
    usleep(3100000);   /* (2 s, and the clock's whole seconds) */
    const int later = hos_ps5_cold_park_now(CU(305, 0));
    hos_ps5_guest_commit(CU(305, 1), 65536);
    const int held = hos_ps5_cold_park_now(CU(305, 1));
    usleep(3100000);
    const int after = hos_ps5_cold_park_now(CU(305, 1));
    CHECK(fresh == 0 && later == 1 && held == 0 && after == 1 && cold_right(CU(305, 0), 7) && cold_right(CU(305, 1), 7),
          "HOOKSHOT_PS5_COLD_HOLD_S=2: a GiB's units held 2 s once its notes are made, a committed unit held 2 s after the commit");
    return failures;
}
static int cold_writer_case;
static void writer_hook(int step, const uint64_t *units, unsigned n) {
    /* between the staging and the write (0), or the write and the giving back (1): the game touches, commits or frees units */
    if (n < 4 || cold_writer_case) return;
    if (step == 0) *(volatile uint64_t *)(uintptr_t)units[0] = 0xabcd;   /* a write: back read-write */
    if (step == 0) hos_ps5_guest_commit((void *)(uintptr_t)units[3], 65536);     /* a commit */
    if (step == 1) (void)*(volatile uint64_t *)(uintptr_t)units[1];      /* a read: back */
    if (step == 1) hos_ps5_guest_decommit((void *)(uintptr_t)units[2], 65536);   /* freed */
    if (step == 1) cold_writer_case = 1;
}
static int cold_writer_child(void) {   /* units that move while the writer has them: left as they are, their memory theirs */
    if (cold_start_child("1", "writer") != HOS_PS5_COLD_ON) return 100;
    for (unsigned i = 0; i < 16; i++) cold_fill(CU(306, i), 8), hos_ps5_cold_park_now(CU(306, i));
    hos_ps5_cold_writer_hook = writer_hook;
    const long long a0 = host_direct_allocations();
    for (int k = 0; k < 4; k++) hos_ps5_cold_tick(1, 0);
    hos_ps5_cold_writer_hook = NULL;
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    char perms[5] = "?";
    perms_of(BASE + 306 * GIB, perms);
    const unsigned s0 = hos_ps5_cold_state(CU(306, 0), NULL), s1 = hos_ps5_cold_state(CU(306, 1), NULL), s2 = hos_ps5_cold_state(CU(306, 2), NULL);
    CHECK(c.spilled == 12 && c.raced == 4 && s0 == 0 && s1 == 0 && s2 == 0 && *(volatile uint64_t *)CU(306, 0) == 0xabcd &&
          !strncmp(perms, "rw-", 3) && cold_right(CU(306, 1), 8) && cold_right(CU(306, 3), 8) && !hos_ps5_guest_committed(CU(306, 2), NULL) &&
          host_direct_allocations() == a0 + 16 - 12 - 1,
          "a batch of 16 with a unit written, one read, one freed and one committed while it was written: 12 on the SSD, those 4 as the game "
          "left them (%llu on the SSD, %llu raced, states %u %u %u, %#llx; allocations %+lld: the reserve, the 12, the freed one given back by "
          "the writer)", (unsigned long long)c.spilled, (unsigned long long)c.raced, s0, s1, s2, (unsigned long long)*(volatile uint64_t *)CU(306, 0),
          host_direct_allocations() - a0);
    int right = 1;
    for (unsigned i = 4; i < 16; i++) right &= cold_right(CU(306, i), 8);
    CHECK(right, "the 12 read back right");
    return failures;
}
/* (2026-11-13, the review) an address of the cold thread's own whose reservation the kernel does not lay again: the model's
   lay_reservation then unmaps it and leaves it free, where another mapping could be given. Never mapped over again */
static int staging_hooked;
static void staging_lost_hook(int step, const uint64_t *units, unsigned n) {
    (void)units, (void)n;
    if (step == 1) host_fail(HOST_CALL_RESERVE, -1), staging_hooked = 1;   /* (the writer's reservation over its staging address, and its fallback) */
}
static int cold_staging_lost_child(void) {   /* the writer's: no unit of the batch goes, no park nor write after, nothing lost */
    if (cold_start_child("1", "staging") != HOS_PS5_COLD_ON) return 100;
    for (unsigned i = 0; i < 16; i++) cold_fill(CU(304, i), 10), hos_ps5_cold_park_now(CU(304, i));
    const long long a0 = host_direct_allocations();
    hos_ps5_cold_writer_hook = staging_lost_hook;
    for (int k = 0; k < 100 && !staging_hooked; k++) hos_ps5_cold_tick(1, 0), usleep(2000);   /* (the reserve's 16 units, then the batch of 16) */
    hos_ps5_cold_writer_hook = NULL;
    host_fail(HOST_CALL_NONE, 0);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    int parked = 1;
    for (unsigned i = 0; i < 16; i++) parked &= hos_ps5_cold_state(CU(304, i), NULL) == CS_PARKED;
    const long long a1 = host_direct_allocations();
    cold_fill(CU(304, 40), 10);
    for (int k = 0; k < 5; k++) hos_ps5_cold_tick(1, 0);
    const int later = hos_ps5_cold_park_now(CU(304, 40));
    struct hos_ps5_cold d;
    hos_ps5_cold_stats(&d);
    int right = 1;
    for (unsigned i = 0; i < 16; i++) right &= cold_right(CU(304, i), 10);
    char line[1700];
    hos_ps5_cold_line(line, sizeof line);
    CHECK(c.space_lost == 1 && c.spilled == 0 && parked && a1 == a0 + 16 && c.stop_why == 6 && c.parks_stopped && c.writes_stopped &&
          d.spills == 0 && right && strstr(line, "LOST"),
          "the writer's staging address lost (its reservation refused): the 16 units stay parked with their memory (%lld allocations "
          "since: the reserve), nothing goes to the SSD after, every word right (lost %llu, why %d; the park asked by hand after: %d)",
          a1 - a0, (unsigned long long)c.space_lost, c.stop_why, later);
    return failures;
}
static int cold_scratch_lost_child(void) {   /* the fault handler's: the unit it brought back is right; the next of that GiB ends the title */
    if (cold_start_child("1", "scratch") != HOS_PS5_COLD_ON) return 100;
    for (unsigned i = 0; i < 8; i++) cold_fill(CU(305, i), 11), hos_ps5_cold_park_now(CU(305, i));
    for (unsigned i = 0; i < 8; i++) cold_fill(CU(306, i), 11), hos_ps5_cold_park_now(CU(306, i));
    COLD_TICKS(cold_now(c_spilled) >= 16, 300);
    if (cold_now(c_spilled) != 16) return 101;
    host_fail(HOST_CALL_RESERVE, -1);
    const int first = cold_right(CU(305, 0), 11);
    host_fail(HOST_CALL_NONE, 0);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    hos_ps5_cold_tick(1, 0);
    struct hos_ps5_cold d;
    hos_ps5_cold_stats(&d);
    const int other = cold_right(CU(306, 0), 11);   /* (another GiB: its scratch is its own) */
    printf("  [scratch] the unit brought back while its scratch was lost: right %d; lost %llu, why %d; another GiB's unit: right %d\n",
           first, (unsigned long long)c.space_lost, d.stop_why, other);
    fflush(stdout);
    if (!first || c.space_lost != 1 || d.stop_why != 6 || !other) return 102;
    (void)*(volatile uint64_t *)CU(305, 1);   /* (should end the process: nowhere to check its bytes) */
    return 103;
}

/* (2026-11-13, the review) units on the SSD, and one parked with its copy, freed by the game and used again: a park and the
   SSD then have their new bytes, never the old copy (a decommit forgets the copy along with the state) */
static int cold_reuse_child(void) {
    if (cold_start_child("1", "reuse") != HOS_PS5_COLD_ON) return 100;
    if (!cold_spill_some(307, 16, 12)) return 101;
    if (!cold_right(CU(307, 0), 12) || hos_ps5_cold_state(CU(307, 0), NULL) != CS_CLEAN) return 102;   /* (read: clean, its copy kept) */
    if (hos_ps5_cold_park_now(CU(307, 0)) != 1) return 103;                                            /* (parked again with it) */
    uint32_t slots = 0, slot = 0;
    hos_ps5_guest_decommit(CU(307, 0), 4 * 65536);
    unsigned states = 0;
    for (unsigned i = 0; i < 4; i++) states |= hos_ps5_cold_state(CU(307, i), &slot), slots |= slot;
    const uint64_t w0 = cold_now(c_written);
    hos_ps5_guest_commit(CU(307, 0), 4 * 65536);
    int parked = 0;
    for (unsigned i = 0; i < 4; i++) cold_fill(CU(307, i), 13), parked += hos_ps5_cold_park_now(CU(307, i));
    COLD_TICKS((hos_ps5_cold_state(CU(307, 0), NULL) & CS_SPILLED) && (hos_ps5_cold_state(CU(307, 3), NULL) & CS_SPILLED), 300);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    int right = 1;
    for (unsigned i = 0; i < 4; i++) right &= cold_right(CU(307, i), 13);
    for (unsigned i = 4; i < 16; i++) right &= cold_right(CU(307, i), 12);
    CHECK(states == 0 && slots == 0 && parked == 4 && c.written == w0 + 4 * 65536ull && right,
          "4 units freed while away (3 on the SSD, 1 parked with its copy) and used again: their copies forgotten (state %#x, slot %u), "
          "the new bytes written (%llu KiB) and read back right, never the old copies", states, slots,
          (unsigned long long)((c.written - w0) >> 10));
    return failures;
}

static int cold_measure_child(void) {   /* the measurement: nothing written, nothing given back, the reads timed */
    if (cold_start_child("measure", "measure") != HOS_PS5_COLD_MEASURE) return 100;
    const long long a0 = host_direct_allocations();
    for (unsigned i = 0; i < 32; i++) cold_fill(CU(307, i), 9), hos_ps5_cold_park_now(CU(307, i));
    const long long a1 = host_direct_allocations();
    COLD_TICKS(cold_now(c_spilled) >= 32, 200);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    char path[560];
    snprintf(path, sizeof path, "%s/hookshot-cold-00.bin", getenv("HOOKSHOT_PS5_COLD_DIR"));
    CHECK(c.spilled == 32 && c.written == 32 * 65536ull && host_direct_allocations() == a1 && access(path, F_OK) != 0,
          "measure: 32 units \"on the SSD\" (%llu MiB would be written), no file, no memory given back", (unsigned long long)(c.written >> 20));
    char pr[1300];
    CHECK(hos_ps5_cold_probe(pr, sizeof pr) == 1 && strstr(pr, "done") && strstr(pr, "64 KiB writes"), "the SSD probe ran");
    printf("  [measure] %s\n", pr);
    int right = 1;
    for (unsigned i = 0; i < 32; i++) right &= cold_right(CU(307, i), 9);
    hos_ps5_cold_stats(&c);
    CHECK(right && c.reloads == 32 && c.sum_mismatch == 0 && c.probe_reads == 32 && c.probe_read_errors == 0 && c.clean == 32,
          "all 32 read back right from their own memory, the sums agree (%llu differ), 32 timed reads of the probe's file in the handler",
          (unsigned long long)c.sum_mismatch);
    char line[1600];
    hos_ps5_cold_line(line, sizeof line);
    printf("  [measure] %s\n", line);
    CHECK(strstr(line, "measure: ") && strstr(line, "would have written"), "the line says what would have been");
    (void)a0;
    return failures;
}

/* the load: writers and readers checking every word, a thread making kernel copies into units the sweep takes, pins, while the
   cold thread parks, writes and gives back as fast as it can (cold after 0 s) */
static volatile int cs_stop;
static volatile unsigned long cs_bad, cs_bad_reader, cs_rounds, cs_copies, cs_copy_bad, cs_pins, cs_pin_bad;
static volatile uint64_t cs_first_bad[4];   /* (the first wrong word a writer met: where, which word, what it read, what it wanted) */
static uint32_t cs_gen[8][32], cs_round[8];
static void *cs_writer(void *p) {
    const unsigned id = (unsigned)(uintptr_t)p;
    for (uint32_t gen = 2; !cs_stop; gen++) {
        __atomic_store_n(&cs_round[id], gen, __ATOMIC_RELEASE);   /* (before any word of the round: the readers' bound) */
        for (unsigned i = 0; i < 32 && !cs_stop; i++) {
            uint8_t *const unit = CU(300 + id / 4, (id % 4) * 64 + i);
            const uint32_t want = __atomic_load_n(&cs_gen[id][i], __ATOMIC_ACQUIRE);
            if (!cold_right(unit, want)) {
                if (__atomic_add_fetch(&cs_bad, 1, __ATOMIC_RELAXED) == 1) {
                    unsigned w = 0;
                    while (w < 8192 && ((volatile uint64_t *)unit)[w] == cw((uint64_t)(uintptr_t)unit, w, want)) w++;
                    cs_first_bad[0] = (uint64_t)(uintptr_t)unit, cs_first_bad[1] = w, cs_first_bad[2] = w < 8192 ? ((volatile uint64_t *)unit)[w] : 0;
                    cs_first_bad[3] = w < 8192 ? cw((uint64_t)(uintptr_t)unit, w, want) : 0;
                }
            }
            if ((gen + i) % 3 == 0) continue;   /* (a third left as it is a round: cold, clean, read back) */
            cold_fill(unit, gen);
            __atomic_store_n(&cs_gen[id][i], gen, __ATOMIC_RELEASE);
        }
        __atomic_add_fetch(&cs_rounds, 1, __ATOMIC_RELAXED);
        usleep(id >= 6 ? 30000 : 1000);   /* (two writers slow, and no reader of theirs: their units go cold, clean, between rounds) */
    }
    return NULL;
}
static void *cs_reader(void *p) {   /* every word says where it is, and a round no later than its writer's */
    (void)p;
    while (!cs_stop)
        for (unsigned id = 0; id < 6; id++)
            for (unsigned i = 0; i < 32; i += 5) {
                const uint8_t *const unit = CU(300 + id / 4, (id % 4) * 64 + i);
                for (unsigned w = 0; w < 8192; w += 97) {
                    const uint64_t v = ((const volatile uint64_t *)unit)[w], g = v ^ cw((uint64_t)(uintptr_t)unit, w, 0);
                    /* (the writer says its round before it writes a word of it: no word is of a later round) */
                    const uint32_t top = __atomic_load_n(&cs_round[id], __ATOMIC_ACQUIRE);
                    if ((g == 0 || g > (uint64_t)(top > 1 ? top : 1)) && __atomic_add_fetch(&cs_bad_reader, 1, __ATOMIC_RELAXED) == 1)
                        printf("  [stress] reader: unit %p word %u: %#llx, round %llu, its writer's %u\n", (const void *)unit, w,
                               (unsigned long long)v, (unsigned long long)g, top);
                }
            }
    return NULL;
}
static void *cs_copy(void *p) {   /* ReadFile's way: commit, then a system call writes the buffer (a refusal: EFAULT) */
    (void)p;
    static uint8_t src[65536];
    for (unsigned k = 0; !cs_stop; k++) {
        uint8_t *const unit = CU(302, k % 16);
        memset(src, (int)(k & 0xff), sizeof src);
        int fds[2];
        if (pipe(fds)) break;
        hos_ps5_guest_commit(unit, 65536);
        size_t done = 0;
        int bad = 0;
        while (done < 65536 && !bad) {
            const ssize_t w = write(fds[1], src + done, 65536 - done < 32768 ? 65536 - done : 32768);
            const ssize_t r = w > 0 ? read(fds[0], unit + done, (size_t)w) : -1;
            if (r != w || w <= 0) bad = 1;
            else done += (size_t)r;
        }
        close(fds[0]), close(fds[1]);
        for (unsigned i = 0; i < 65536 && !bad; i += 512) bad = unit[i] != (uint8_t)k;
        if (bad) __atomic_add_fetch(&cs_copy_bad, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&cs_copies, 1, __ATOMIC_RELAXED);
        usleep(150000);   /* (a unit's turn comes back past its hold, 2.4 s: the sweep takes it between its copies) */
    }
    return NULL;
}
static void *cs_pin(void *p) {   /* units pinned one by one: never parked once pinned */
    (void)p;
    for (unsigned k = 0; k < 256 && !cs_stop; k++) {
        hos_ps5_guest_pin(CU(303, k), 65536);
        __atomic_add_fetch(&cs_pins, 1, __ATOMIC_RELAXED);
        for (unsigned j = 0; j <= k; j += 7)
            if (hos_ps5_cold_state(CU(303, j), NULL) & (CS_PARKED | CS_SPILLED | CS_CLEAN)) __atomic_add_fetch(&cs_pin_bad, 1, __ATOMIC_RELAXED);
        usleep(2000);
    }
    return NULL;
}
static void *cs_cold(void *p) {
    (void)p;
    while (!cs_stop) hos_ps5_cold_tick(1, 0), usleep(500);
    return NULL;
}
static int cold_stress_child(void) {
    setenv("HOOKSHOT_PS5_COLD_HOLD_S", "1", 1);   /* (1 to 2 s: a copy's units are taken between its turns, 2.4 s apart) */
    setenv("HOOKSHOT_PS5_COLD_PARK_RATE", "1000000", 1);
    if (cold_start_child("1", "stress") != HOS_PS5_COLD_ON) return 100;
    for (unsigned id = 0; id < 8; id++)
        for (unsigned i = 0; i < 32; i++) cold_fill(CU(300 + id / 4, (id % 4) * 64 + i), 1), cs_gen[id][i] = 1;
    for (unsigned k = 0; k < 256; k++) cold_fill(CU(303, k), 1);
    usleep(1100000);   /* (the notes' hold) */
    pthread_t t[13];
    unsigned n = 0;
    for (unsigned i = 0; i < 8; i++) pthread_create(&t[n++], NULL, cs_writer, (void *)(uintptr_t)i);
    pthread_create(&t[n++], NULL, cs_reader, NULL);
    pthread_create(&t[n++], NULL, cs_reader, NULL);
    pthread_create(&t[n++], NULL, cs_copy, NULL);
    pthread_create(&t[n++], NULL, cs_pin, NULL);
    pthread_create(&t[n++], NULL, cs_cold, NULL);
    sleep(6);
    cs_stop = 1;
    for (unsigned i = 0; i < n; i++) pthread_join(t[i], NULL);
    int right = 1;
    for (unsigned id = 0; id < 8; id++)
        for (unsigned i = 0; i < 32; i++) right &= cold_right(CU(300 + id / 4, (id % 4) * 64 + i), cs_gen[id][i]);
    for (unsigned k = 0; k < 256; k++)
        if (hos_ps5_guest_pinned(CU(303, k), 65536)) right &= cold_right(CU(303, k), 1);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    char line[1600];
    hos_ps5_cold_line(line, sizeof line);
    printf("  [stress] %s\n", line);
    CHECK(right && !cs_bad && !cs_bad_reader && !cs_copy_bad && !cs_pin_bad && c.spills > 100 && c.reloads > 100 && c.dirtied > 0 && c.spills_again > 0,
          "8 writers and 2 readers checking every word, kernel copies after a commit, pins, beside the cold thread for 6 s: %lu rounds, %lu "
          "copies, %lu pins; %llu units to the SSD (%llu again with no write), %llu read back, %llu made writable; %lu units wrong for their "
          "writer (the first at %#llx word %llu: %#llx for %#llx), %lu wrong words for the readers, %lu bad copies, %lu pinned units met away, "
          "at the end %s",
          cs_rounds, cs_copies, cs_pins, (unsigned long long)c.spills, (unsigned long long)c.spills_again, (unsigned long long)c.reloads,
          (unsigned long long)c.dirtied, cs_bad, (unsigned long long)cs_first_bad[0], (unsigned long long)cs_first_bad[1],
          (unsigned long long)cs_first_bad[2], (unsigned long long)cs_first_bad[3], cs_bad_reader, cs_copy_bad, cs_pin_bad, right ? "right" : "WRONG");
    return failures;
}

/* (2026-11-13, the review) the lots run the reclaim of zero units (HOOKSHOT_PS5_ZERO_RECLAIM=1) beside the cold units: its passes
   as fast as they go, beside the cold thread, the writers and readers of the load, and units turned all zeros and back that both
   want */
static volatile unsigned long cr_flip_bad, cr_flips, cr_passes, cr_put_back;
static uint32_t cr_want[64];
static void *cr_flipper(void *p) {
    (void)p;
    for (uint32_t gen = 2; !cs_stop; gen++) {
        for (unsigned i = 0; i < 64 && !cs_stop; i++) {
            uint8_t *const unit = CU(304, i);
            int ok = 1;
            if (cr_want[i]) ok = cold_right(unit, cr_want[i]);
            else
                for (unsigned w = 0; w < 8192 && ok; w++) ok = ((const volatile uint64_t *)unit)[w] == 0;
            if (!ok) __atomic_add_fetch(&cr_flip_bad, 1, __ATOMIC_RELAXED);
            if ((gen + i) % 4 == 0) memset(unit, 0, 65536), cr_want[i] = 0;
            else if ((gen + i) % 4 == 2) cold_fill(unit, gen), cr_want[i] = gen;
        }
        __atomic_add_fetch(&cr_flips, 1, __ATOMIC_RELAXED);
        usleep(20000);
    }
    return NULL;
}
static void *cr_reclaim(void *p) {
    (void)p;
    struct hos_ps5_guest_reclaim r;
    while (!cs_stop) {
        hos_ps5_guest_reclaim(&r);
        __atomic_add_fetch(&cr_passes, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&cr_put_back, r.reclaimed, __ATOMIC_RELAXED);
        usleep(1000);
    }
    return NULL;
}
static int cold_reclaim_child(void) {
    setenv("HOOKSHOT_PS5_COLD_HOLD_S", "1", 1);
    setenv("HOOKSHOT_PS5_COLD_PARK_RATE", "1000000", 1);
    setenv("HOOKSHOT_PS5_ZERO_RECLAIM", "1", 1);
    if (cold_start_child("1", "reclaim") != HOS_PS5_COLD_ON) return 100;
    for (unsigned id = 0; id < 8; id++)
        for (unsigned i = 0; i < 32; i++) cold_fill(CU(300 + id / 4, (id % 4) * 64 + i), 1), cs_gen[id][i] = 1;
    for (unsigned i = 0; i < 64; i++) cold_fill(CU(304, i), 1), cr_want[i] = 1;
    usleep(1100000);   /* (the notes' hold) */
    struct hos_ps5_guest_stats s0, s1;
    hos_ps5_guest_stats(&s0);
    pthread_t t[13];
    unsigned n = 0;
    for (unsigned i = 0; i < 8; i++) pthread_create(&t[n++], NULL, cs_writer, (void *)(uintptr_t)i);
    pthread_create(&t[n++], NULL, cs_reader, NULL);
    pthread_create(&t[n++], NULL, cs_reader, NULL);
    pthread_create(&t[n++], NULL, cr_flipper, NULL);
    pthread_create(&t[n++], NULL, cr_reclaim, NULL);
    pthread_create(&t[n++], NULL, cs_cold, NULL);
    sleep(6);
    cs_stop = 1;
    for (unsigned i = 0; i < n; i++) pthread_join(t[i], NULL);
    hos_ps5_guest_stats(&s1);
    int right = 1;
    for (unsigned id = 0; id < 8; id++)
        for (unsigned i = 0; i < 32; i++) right &= cold_right(CU(300 + id / 4, (id % 4) * 64 + i), cs_gen[id][i]);
    for (unsigned i = 0; i < 64; i++) {
        if (cr_want[i]) right &= cold_right(CU(304, i), cr_want[i]);
        else
            for (unsigned w = 0; w < 8192; w++) right &= ((const volatile uint64_t *)CU(304, i))[w] == 0;
    }
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    CHECK(right && !cs_bad && !cs_bad_reader && !cr_flip_bad && c.spills > 100 && c.reloads > 100 && cr_put_back > 0 &&
          s1.fault_refused == s0.fault_refused && s1.fault_failures == s0.fault_failures,
          "the reclaim of zero units beside the cold thread, 8 writers, 2 readers and units turned all zeros and back, 6 s: %lu passes put "
          "%lu units back behind the shared zeros, %llu units to the SSD, %llu read back; %lu units wrong for their writer, %lu wrong words "
          "for the readers, %lu for the units turned zeros (%lu rounds); no fault refused (%llu); at the end %s",
          cr_passes, cr_put_back, (unsigned long long)c.spills, (unsigned long long)c.reloads, cs_bad, cs_bad_reader, cr_flip_bad, cr_flips,
          (unsigned long long)(s1.fault_refused - s0.fault_refused), right ? "right" : "WRONG");
    return failures;
}

/* (2026-11-14, the inquiry into a save written with a block of zeros after lot 28's run: 535 units "decommitted while away" as
   a save loaded after 170 s of title screen, 7,578 units back from parking and the writer at full speed, all in the same
   seconds) the game's side of such a load: units freed whole while parked or on the SSD, freed and committed again at once
   (FreeMemoryPages then AllocateMemoryPages), parts of units freed (one unit, or across two), units committed, written all
   zeros, written, or left to go cold, by four threads that each own their units and check every page against what they last
   left in it (their bytes, or zeros where they freed), and a fifth that only reads units it does not own (back read-only,
   clean, then freed by their owner); beside the cold thread parking, writing and giving back as fast as it can and the reclaim
   of zero units as fast as it goes */
#define CH_GIB 305
#define CH_THREADS 4
#define CH_UNITS 64   /* (a thread's) */
static volatile unsigned long ch_bad, ch_rounds, ch_frees, ch_refills, ch_partials, ch_straddles, ch_commits, ch_zeroed, ch_multi, ch_reads;
static volatile uint64_t ch_first_bad[5];   /* (the first wrong page: unit, page, word, what it read, what it wanted) */
static uint32_t ch_gen[CH_THREADS][CH_UNITS];     /* (0: the unit all zeros) */
static uint16_t ch_zmask[CH_THREADS][CH_UNITS];   /* (pages of 4 KiB freed since the unit was last written: zeros) */
static uint8_t *ch_unit(unsigned id, unsigned i) { return CU(CH_GIB, id * CH_UNITS + i); }
static int ch_right(unsigned id, unsigned i) {
    const uint8_t *const unit = ch_unit(id, i);
    const uint32_t gen = __atomic_load_n(&ch_gen[id][i], __ATOMIC_ACQUIRE);
    const unsigned zmask = __atomic_load_n(&ch_zmask[id][i], __ATOMIC_ACQUIRE);
    for (unsigned p = 0; p < 16; p++) {
        const int zero = !gen || (zmask >> p & 1);
        const volatile uint64_t *const w = (const volatile uint64_t *)(unit + p * 4096);
        for (unsigned k = 0; k < 512; k++) {
            const uint64_t want = zero ? 0 : cw((uint64_t)(uintptr_t)unit, p * 512 + k, gen), got = w[k];
            if (got != want) {
                if (__atomic_add_fetch(&ch_bad, 1, __ATOMIC_RELAXED) == 1)
                    ch_first_bad[0] = (uint64_t)(uintptr_t)unit, ch_first_bad[1] = p, ch_first_bad[2] = k, ch_first_bad[3] = got,
                    ch_first_bad[4] = want;
                return 0;
            }
        }
    }
    return 1;
}
static void ch_freed(unsigned id, unsigned i, unsigned pages) {   /* (the model: pages of the unit now zeros) */
    if (pages == 0xffff) __atomic_store_n(&ch_gen[id][i], 0, __ATOMIC_RELEASE), __atomic_store_n(&ch_zmask[id][i], 0, __ATOMIC_RELEASE);
    else if (ch_gen[id][i]) __atomic_store_n(&ch_zmask[id][i], (uint16_t)(ch_zmask[id][i] | pages), __ATOMIC_RELEASE);
}
static void *ch_heap(void *p) {
    const unsigned id = (unsigned)(uintptr_t)p;
    uint64_t rnd = 0x9e3779b97f4a7c15ull * (id + 1);
    for (uint32_t gen = 2; !cs_stop; gen++) {
        for (unsigned i = 0; i < CH_UNITS && !cs_stop; i++) {
            uint8_t *const unit = ch_unit(id, i);
            const unsigned what = (unsigned)(next_random(&rnd) % 40);
            /* (a unit freed whole is not read first: the game gives back what it no longer touches, away more often than not) */
            if (what != 4 && what != 5 && what != 10) ch_right(id, i);
            if (what <= 3 || (what >= 20 && what < 28)) {   /* written */
                cold_fill(unit, gen);
                __atomic_store_n(&ch_zmask[id][i], 0, __ATOMIC_RELEASE);
                __atomic_store_n(&ch_gen[id][i], gen, __ATOMIC_RELEASE);
            } else if (what == 4) {   /* freed whole (a decommit: what is away is forgotten) */
                hos_ps5_guest_decommit(unit, 65536);
                ch_freed(id, i, 0xffff);
                __atomic_add_fetch(&ch_frees, 1, __ATOMIC_RELAXED);
            } else if (what == 5) {   /* freed and taken again at once: fresh zeros */
                hos_ps5_guest_decommit(unit, 65536);
                ch_freed(id, i, 0xffff);
                hos_ps5_guest_commit(unit, 65536);
                __atomic_add_fetch(&ch_refills, 1, __ATOMIC_RELAXED);
            } else if (what == 6) {   /* a part freed: those pages zeros, the rest kept */
                const unsigned p0 = (unsigned)(next_random(&rnd) % 15), k = 1 + (unsigned)(next_random(&rnd) % (15 - p0));
                hos_ps5_guest_decommit(unit + p0 * 4096, k * 4096);
                ch_freed(id, i, (unsigned)(((1u << k) - 1) << p0));
                __atomic_add_fetch(&ch_partials, 1, __ATOMIC_RELAXED);
            } else if (what == 7 && i + 1 < CH_UNITS) {   /* the end of this unit and the start of the next freed */
                ch_right(id, i + 1);
                hos_ps5_guest_decommit(unit + 12 * 4096, 8 * 4096);
                ch_freed(id, i, 0xf000);
                ch_freed(id, i + 1, 0x000f);
                __atomic_add_fetch(&ch_straddles, 1, __ATOMIC_RELAXED);
            } else if (what == 8) {   /* a commit: nothing changes */
                hos_ps5_guest_commit(unit, 65536);
                __atomic_add_fetch(&ch_commits, 1, __ATOMIC_RELAXED);
            } else if (what == 9) {   /* all zeros written (the reclaim's and the writer's units of zeros) */
                memset(unit, 0, 65536);
                ch_freed(id, i, 0xffff);
                __atomic_add_fetch(&ch_zeroed, 1, __ATOMIC_RELAXED);
            } else if (what == 10 && i + 4 <= CH_UNITS) {   /* four units freed in one call, as a heap gives back a block */
                hos_ps5_guest_decommit(unit, 4 * 65536);
                for (unsigned j = 0; j < 4; j++) ch_freed(id, i + j, 0xffff);
                __atomic_add_fetch(&ch_multi, 1, __ATOMIC_RELAXED);
            }   /* (else left as it is: parked, on the SSD, read back) */
        }
        __atomic_add_fetch(&ch_rounds, 1, __ATOMIC_RELAXED);
        usleep(id == 0 ? 5000 : id == 1 ? 20000 : id == 2 ? 60000 : 120000);   /* (the slower, the more of its units go cold and away) */
    }
    return NULL;
}
static void ch_writer_hook(int step, const uint64_t *units, unsigned n) {   /* (the writer slowed at its two steps: what the game does meets it) */
    (void)step, (void)units, (void)n;
    usleep(1500);
}
static void *ch_reader(void *p) {   /* (units of the slow threads read, never checked: back read-only, clean, before their owner moves them) */
    (void)p;
    uint64_t rnd = 12345;
    while (!cs_stop) {
        const unsigned id = 2 + (unsigned)(next_random(&rnd) % 2), i = (unsigned)(next_random(&rnd) % CH_UNITS);
        (void)*(const volatile uint64_t *)(ch_unit(id, i) + (next_random(&rnd) % 16) * 4096);
        __atomic_add_fetch(&ch_reads, 1, __ATOMIC_RELAXED);
        usleep(200);
    }
    return NULL;
}
static int cold_churn_child(void) {
    setenv("HOOKSHOT_PS5_COLD_HOLD_S", "1", 1);
    setenv("HOOKSHOT_PS5_COLD_PARK_RATE", "1000000", 1);
    setenv("HOOKSHOT_PS5_ZERO_RECLAIM", "1", 1);
    if (cold_start_child("1", "churn") != HOS_PS5_COLD_ON) return 100;
    for (unsigned id = 0; id < CH_THREADS; id++)
        for (unsigned i = 0; i < CH_UNITS; i++) cold_fill(ch_unit(id, i), 1), ch_gen[id][i] = 1;
    usleep(1100000);   /* (the notes' hold) */
    struct hos_ps5_guest_stats s0, s1;
    hos_ps5_guest_stats(&s0);
    pthread_t t[8];
    unsigned n = 0;
    hos_ps5_cold_writer_hook = ch_writer_hook;
    for (unsigned i = 0; i < CH_THREADS; i++) pthread_create(&t[n++], NULL, ch_heap, (void *)(uintptr_t)i);
    pthread_create(&t[n++], NULL, ch_reader, NULL);
    pthread_create(&t[n++], NULL, cr_reclaim, NULL);
    pthread_create(&t[n++], NULL, cs_cold, NULL);
    sleep(6);
    cs_stop = 1;
    for (unsigned i = 0; i < n; i++) pthread_join(t[i], NULL);
    hos_ps5_cold_writer_hook = NULL;
    hos_ps5_guest_stats(&s1);
    const unsigned long during = ch_bad;
    int right = 1;
    for (unsigned id = 0; id < CH_THREADS; id++)
        for (unsigned i = 0; i < CH_UNITS; i++) right &= ch_right(id, i);
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    char line[1700];
    hos_ps5_cold_line(line, sizeof line);
    printf("  [churn] %s\n", line);
    CHECK(right && !during && c.released > 0 && c.spills_zero > 0 && c.spills > 100 && c.reloads > 100 && cr_put_back > 0 &&
          s1.fault_refused == s0.fault_refused && s1.fault_failures == s0.fault_failures,
          "units freed while away, freed and taken again, freed in part, committed, written zeros, beside the cold thread and the reclaim "
          "of zero units, 6 s: %lu rounds, %lu freed, %lu freed and taken again, %lu freed in part, %lu across two units, %lu four at once, "
          "%lu commits, %lu written zeros, %lu reads; %llu decommitted while away, %llu units to the SSD (%llu of zeros), %llu moved while "
          "the writer had them, %llu read back, "
          "%lu put back behind the shared zeros; %lu pages wrong (the first: unit %#llx page %llu word %llu: %#llx for %#llx); at the end %s",
          ch_rounds, ch_frees, ch_refills, ch_partials, ch_straddles, ch_multi, ch_commits, ch_zeroed, ch_reads,
          (unsigned long long)c.released, (unsigned long long)c.spills, (unsigned long long)c.spills_zero, (unsigned long long)c.raced,
          (unsigned long long)c.reloads,
          cr_put_back, ch_bad, (unsigned long long)ch_first_bad[0], (unsigned long long)ch_first_bad[1], (unsigned long long)ch_first_bad[2],
          (unsigned long long)ch_first_bad[3], (unsigned long long)ch_first_bad[4], right ? "right" : "WRONG");
    return failures;
}

static void cold_tests(void) {
    const char *d = getenv("HOOKSHOT_TEST_COLD_DIR");
    if (!d || !*d) {
        printf("SKIP cold units: HOOKSHOT_TEST_COLD_DIR not given (a folder on disk for the SSD's files)\n");
        return;
    }
    snprintf(cold_dir, sizeof cold_dir, "%s", d);
    const char *only = getenv("HOOKSHOT_TEST_COLD_CASE");   /* (one case alone, to look at it: nofile, writer, stress) */
    if (only && !strcmp(only, "nofile")) { CHECK(child(cold_open_fails_child) == 0, "nofile"); return; }
    if (only && !strcmp(only, "writer")) { const int w = child(cold_writer_child); CHECK(w == 0, "writer %d", w); return; }
    if (only && !strcmp(only, "stress")) { const int w = child(cold_stress_child); CHECK(w == 0, "stress %d", w); return; }
    if (only && !strcmp(only, "reclaim")) { const int w = child(cold_reclaim_child); CHECK(w == 0, "reclaim %d", w); return; }
    if (only && !strcmp(only, "churn")) { const int w = child(cold_churn_child); CHECK(w == 0, "churn %d", w); return; }
    CHECK(child(cold_off_child) == 0, "cold units: off when unset (a child)");
    CHECK(child(cold_typo_child) == 0, "cold units: off for a value not understood (a child)");
    CHECK(child(cold_rules_child) == 0, "cold units: the rules, one by one (a child)");
    CHECK(child(cold_open_fails_child) == 0, "cold units: the SSD's file cannot be made (a child)");
    CHECK(child(cold_enospc_child) == 0, "cold units: the SSD full (a child)");
    int e = child(cold_park_refused_child);
    CHECK(e == 0, "cold units: a park refused (a child: %d)", e);
    CHECK(child(cold_hold_child) == 0, "cold units: the holds (a child)");
    CHECK(child(cold_writer_child) == 0, "cold units: units that move while the writer has them (a child)");
    CHECK(child(cold_measure_child) == 0, "cold units: the measurement (a child)");
    CHECK(child(cold_reuse_child) == 0, "cold units: units freed while away and used again (a child)");
    CHECK(child(cold_staging_lost_child) == 0, "cold units: the writer's address lost (a child)");
    e = child(cold_scratch_lost_child);
    CHECK(e == 77, "cold units: a GiB's scratch address lost: the next unit of it from the SSD ends the title with its line (exit %d)", e);
    static const char *const how[] = {"a copy the SSD gives back short", "a copy with a byte changed", "no memory, the reserve spent",
                                      "a copy with its last byte changed"};
    static int (*const fatal_cases[])(void) = {cold_fatal_short, cold_fatal_sum, cold_fatal_nomem, cold_fatal_sum_end};
    for (int i = 0; i < 4; i++) {
        e = child(fatal_cases[i]);
        CHECK(e == 77, "cold units: %s ends the title with its line, never runs on (exit %d)", how[i], e);
    }
    e = child(cold_stress_child);
    CHECK(e == 0, "cold units: the load (a child: %d)", e);
    e = child(cold_reclaim_child);
    CHECK(e == 0, "cold units: the load with the reclaim of zero units beside it (a child: %d)", e);
    e = child(cold_churn_child);
    CHECK(e == 0, "cold units: units freed while away, in part and taken again, beside the cold thread and the reclaim (a child: %d)", e);
}

int main(void) {
    printf("host/posix/ps5_window.c on the platform's kernel model\n");
    CHECK(fiber_test(), "ps5_fiber.c: two fibers and the thread switched 3,000 times, stacks, registers and each fiber's MXCSR kept (%u rounds, %u bad)",
          rounds, bad_fiber);
    CHECK(child(way_pieces) == 0, "the window as 32 adjacent pieces of 16 GiB (HOOKSHOT_PS5_WINDOW=pieces)");
    CHECK(child(way_prefix) == 0, "the window's top 16 GiB taken: 496 GiB from the base kept, hos_space_end 496 GiB, a fault past it refused");
    CHECK(child(way_too_short) == 0, "a window cut at 304 GiB: refused (the heap and the VAMM region need 336), every piece given back");

    install();
    struct hos_ps5_guest_stats s0, s1;
    CHECK(hos_ps5_guest_reserve(BASE, 512 * GIB), "512 GiB at %#llx reserved", BASE);
    if (getenv("HOOKSHOT_TEST_COLD_ONLY")) {   /* (tests/test_cold.py's quick runs: the cold units' cases alone) */
        hos_ps5_guest_settings();
        cold_tests();
        printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? 1 : 0;
    }
    hos_ps5_guest_stats(&s0);
    CHECK(s0.mode == HOS_PS5_WINDOW_WHOLE && hos_space_end == 512 * GIB, "in one reservation, the guest's space 512 GiB");
    CHECK(hos_ps5_guest_reserve(BASE, 512 * GIB), "a second call for the same window finds it made");
    /* (the reclaim's switches, in children, before this process has direct memory of its own: the model's is a shared memfd,
       and a child's pass over GiBs this process had backed would write their tables. Off unless asked for since 2026-11-12:
       asked for here, for this process and the children, which take it away where they check the switches) */
    setenv("HOOKSHOT_PS5_ZERO_RECLAIM", "1", 1);
    reclaim_switches();

    /* backing at once: zeroed (the model's direct memory comes with 0xa5) */
    const long long a0 = host_direct_allocations();
    CHECK(hos_ps5_guest_commit(G(0x1000000000ull), 256 * MIB) == 0, "256 MiB committed");
    int zero = 1;
    for (uint64_t off = 0; off < 256 * MIB; off += 4096) zero &= *(volatile uint64_t *)(G(0x1000000000ull) + off) == 0;
    size_t run_bytes = 0;
    CHECK(zero && hos_ps5_guest_committed(G(0x1000000000ull), &run_bytes) && run_bytes == 256 * MIB, "every page zero; VirtualQuery's run %zu MiB", run_bytes >> 20);
    CHECK(host_direct_allocations() - a0 == 4096 + 1, "4096 units and the GiB's table allocated (%lld)", host_direct_allocations() - a0);

    /* a unit already backed keeps its protection (an import's GPU access): a commit over it does not touch it */
    sceKernelMprotect(G(0x1000000000ull), 65536, PS5_KERNEL_PROT_CPU_READ);
    hos_ps5_guest_commit(G(0x1000000000ull), 2 * MIB);
    char perms[5] = "?";
    perms_of(BASE + 0x1000000000ull, perms);
    CHECK(!strncmp(perms, "r--", 3), "a commit over a backed unit left its protection (%s)", perms);
    sceKernelMprotect(G(0x1000000000ull), 65536, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE);

    /* decommit, touched again: zero, from a fault */
    memset(G(0x1000000000ull), 0x5c, 256 * MIB);
    CHECK(hos_ps5_guest_decommit(G(0x1000000000ull), 256 * MIB) == 0 && !hos_ps5_guest_committed(G(0x1000000000ull), NULL), "decommitted");
    CHECK(host_direct_allocations() - a0 == 1, "its units given back (%lld left: the GiB's table, kept)", host_direct_allocations() - a0);
    hos_ps5_guest_stats(&s0);
    zero = 1;
    for (uint64_t off = 0; off < 64 * MIB; off += 4096) zero &= *(volatile uint64_t *)(G(0x1000000000ull) + off) == 0;
    hos_ps5_guest_stats(&s1);
    CHECK(zero && s1.faults - s0.faults == 64, "64 MiB read again: zero, through %llu faults of 1 MiB", (unsigned long long)(s1.faults - s0.faults));
    hos_ps5_guest_decommit(G(0x1000000000ull), 64 * MIB);

    /* a partial decommit zeroes and keeps */
    hos_ps5_guest_commit(G(0x1100000000ull), 65536);
    memset(G(0x1100000000ull), 0x77, 65536);
    hos_ps5_guest_decommit(G(0x1100000000ull + 4096), 8192);
    CHECK(G(0x1100000000ull)[0] == 0x77 && G(0x1100000000ull)[4096] == 0 && G(0x1100000000ull)[12288] == 0x77 &&
          hos_ps5_guest_committed(G(0x1100000000ull), NULL), "a decommit of 8 KiB inside a unit zeroed them and kept the unit");

    /* threads, each its own GiB, then interleaved over one region */
    static struct job jobs[16];
    memset(jobs, 0, sizeof jobs);
    for (unsigned i = 0; i < 8; i++) jobs[i] = (struct job){.at = 0x3000000000ull + i * GIB, .bytes = 64 * MIB, .stride = 1, .id = i + 1};
    hos_ps5_guest_stats(&s0);
    int bad = run(jobs, 8);
    hos_ps5_guest_stats(&s1);
    CHECK(!bad, "8 threads x 64 MiB, each its own GiB: %llu faults, every page right", (unsigned long long)(s1.faults - s0.faults));
    memset(jobs, 0, sizeof jobs);
    for (unsigned i = 0; i < 16; i++) jobs[i] = (struct job){.at = 0x3800000000ull, .bytes = 256 * MIB, .stride = 16, .first = i, .id = 0x40 + i};
    hos_ps5_guest_stats(&s0);
    bad = run(jobs, 16);
    hos_ps5_guest_stats(&s1);
    CHECK(!bad && s1.fault_failures == s0.fault_failures, "16 threads interleaved over 256 MiB: %llu faults, %llu retried", (unsigned long long)(s1.faults - s0.faults),
          (unsigned long long)(s1.fault_retries - s0.fault_retries));

    /* a fault with a lock held */
    pthread_t t;
    void *ret = NULL;
    pthread_create(&t, NULL, fault_holding, NULL);
    pthread_join(t, &ret);
    CHECK(ret != NULL, "a fault taken with a mutex held");

    /* a refusal part way: what was backed stays consistent, decommit gives it all back */
    for (unsigned i = 0; i < 8; i++) hos_ps5_guest_decommit(G(0x3000000000ull + i * GIB), 64 * MIB);
    hos_ps5_guest_decommit(G(0x3800000000ull), 256 * MIB);
    hos_ps5_guest_decommit(G(0x2800000000ull), 65536);
    hos_ps5_guest_decommit(G(0x1100000000ull), 65536);
    const long long a1 = host_direct_allocations();
    host_fail(HOST_CALL_ALLOCATE, 1000);
    const int r = hos_ps5_guest_commit(G(0x5000000000ull), 128 * MIB);
    hos_ps5_guest_stats(&s1);
    CHECK(r != 0 && s1.commit_failures > 0, "the 1000th allocation refused: the commit answers %#x", (unsigned)r);
    size_t backed_run = 0;
    hos_ps5_guest_committed(G(0x5000000000ull), &backed_run);
    CHECK(backed_run == 998 * 65536ull, "the 998 units before it backed (the GiB's table took the first allocation; %zu KiB)", backed_run >> 10);
    hos_ps5_guest_decommit(G(0x5000000000ull), 128 * MIB);
    CHECK(host_direct_allocations() - a1 == 1, "all given back but the GiB's table (%lld left over)", host_direct_allocations() - a1);
    host_fail(HOST_CALL_NONE, 0);

    pin_tests();
    untouched_tests();
    backed_tests();
    reclaim_tests();
    reclaim_fences();
    reclaim_valve();
    reclaim_stress();
    cold_tests();

    hos_ps5_guest_stats(&s1);
    printf("stats: %llu commits, %llu faults (%llu units, %llu retried, %llu refused, %llu failed), mean %.0f us, the longest %.0f us; "
           "%llu decommitted units, %llu decommit fallbacks, tables %llu KiB, peak %llu MiB\n",
           (unsigned long long)s1.commits, (unsigned long long)s1.faults, (unsigned long long)s1.fault_units,
           (unsigned long long)s1.fault_retries, (unsigned long long)s1.fault_refused, (unsigned long long)s1.fault_failures,
           s1.faults ? (double)s1.fault_ns / s1.faults / 1e3 : 0.0, s1.fault_ns_max / 1e3, (unsigned long long)s1.decommit_units,
           (unsigned long long)s1.rereserve_fallbacks, (unsigned long long)(s1.meta_bytes >> 10), (unsigned long long)(s1.peak_backed_bytes >> 20));
    printf("the reclaim: %llu passes, %llu units put back, %llu given memory again (%llu soon after), %llu found written and kept, %llu refused\n",
           (unsigned long long)s1.reclaim_passes, (unsigned long long)s1.reclaim_units, (unsigned long long)s1.reclaim_rebacked,
           (unsigned long long)s1.reclaim_flaps, (unsigned long long)s1.reclaim_written, (unsigned long long)s1.reclaim_failures);
    printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
