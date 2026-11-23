/* (ps5, 2026-11-16, lot 40) The per-thread CPU report: HOOKSHOT_PS5_THREAD_TIMES=<seconds>. host/hos_thread_times.h says why.

   Only with the setting, on a thread of its own ("hookshot cpu"):
   1. The calibration. A witness thread ("hookshot cpuw") burns 200 ms of CPU, then sleeps 300 ms, twice. It counts the CPU it
      really got: the steps of its monotonic clock under 50 us (a preemption is a longer step and is not counted). Each reading
      is taken before and after each half and held against that:
        own-clock      the witness's own CLOCK_THREAD_CPUTIME_ID (Savage Wind reads its threads so on the console:
                       ext/savagewind-recomp/ps5/FINDINGS.md, 2026-10-28)
        own-rusage     the witness's own getrusage(RUSAGE_THREAD) (FreeBSD 8.1 on; RUSAGE_SELF works on the console)
        pthread-clock  pthread_getcpuclockid(witness) + clock_gettime, from this thread: the scheduling report's reading,
                       expected wrong there (it gives the caller's clock)
        tid-clock      clock_gettime(0x80000000 | the witness's tid), from this thread: FreeBSD's per-thread CPU clock id
                       (kern_time.c MAKE_THREAD_CPUCLOCK, what clock_getcpuclockid2 returns and get_cputime reads: that
                       thread's td_runtime). libkernel exports no clock_getcpuclockid2: the id is made here
        kern-proc      sysctl {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD, pid}, from this thread: a struct
                       kinfo_proc a thread, records of ki_structsize bytes, ki_pid at 72, ki_runtime (us) at 328, ki_tdname
                       at 394, ki_tid at 600 (the SDK's FreeBSD 11 sys/user.h; its samples read ki_pid at 72 and ki_comm at
                       447 on the console). Used only when every record is ki_structsize long and has this pid, the witness's
                       tid is in exactly one at 600, and its ki_runtime moves as the witness burned; its names only when the
                       witness's ki_tdname reads "hookshot cpuw". Refused, the line says where the tid, the name and the
                       fields that grew by the burn are in the witness's record, for a layout to be fixed from one run
      A reading is kept when in both rounds it moved by the witness's CPU (85 to 115 %, 2 ms either way) while it burned and
      by 5 % of it (2 ms) at most while it slept. A line a reading. A reading refused, or that failed once, is not called
      again. Before the first call of each reading made from this thread a stage-trail line names it: should one stop the
      title, HOOKSHOT_PS5_THREAD_TIMES_SKIP=<its name> leaves it out.
   2. The report, every <seconds>, by the first kept of kern-proc (every thread of the process, RADV's too), tid-clock,
      pthread-clock (win32_posix.c's threads), own-clock, own-rusage (those threads as they last published). Two lines:
        cpu: report <n> at <s> s over <p> s by <reading>: <frames> frames (<f>/s); the process <c> CPUs (getrusage);
             <k> threads seen, <c> CPUs (<c> not seen)[; <k> new, from the next report]; <k> at 85% or more[: <name> <pct>%, ...]
             [; <k> running unseen: <name> (<s> s), ...][; check on the registered threads: <reading> <c>, <reading> <c>]
        cpu: busiest <n>: <name> [<tid>] <pct>%[!]; ...[; <k> more at <pct>% or less]
      Percentages of one CPU over the period, "!" at 85 % and more. "Not seen": getrusage's total less what the threads seen
      add up to. "Running unseen" (a thread's own reading only): a thread that neither waited nor published since the report
      before, its CPU unknown. "Check": the same threads by the other kept readings, which should agree. tools/thread-times.py
      reads the lines. Only this thread writes them: no measured thread logs, waits on this one, or is signalled or suspended.
   Allocations, only with the setting: the rows and the previous values (~200 KiB of the heap) and kern-proc's buffer (twice
   its first size, 2 MiB at most). Off, nothing here runs: hos_thread_times_start reads the setting and returns. */
#if defined(__PROSPERO__) || defined(HOOKSHOT_THREAD_TIMES_TEST)
#include "hos_thread_times.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>
#ifdef __PROSPERO__
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int sysctl(const int *name, unsigned int namelen, void *old, size_t *oldlen, const void *newp, size_t newlen);
#endif
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif

void hos_log(const char *fmt, ...);
void hos_ps5_trail(const char *fmt, ...);
int64_t hos_now_ns(void);
int hos_ps5_tid(void);
double hos_ps5_since_start(void);
extern unsigned long rx_ps5_presents;   /* (host/rx_backend.c: the frames shown) */

enum { M_OWN_CLOCK, M_OWN_RUSAGE, M_PTHREAD_CLOCK, M_TID_CLOCK, M_KPROC, M_N };
static const char *const tt_name[M_N] = {"own-clock", "own-rusage", "pthread-clock", "tid-clock", "kern-proc"};
static const char *const tt_what[M_N] = {
    "the witness's own CLOCK_THREAD_CPUTIME_ID",
    "the witness's own getrusage(RUSAGE_THREAD)",
    "pthread_getcpuclockid(witness) + clock_gettime from the report's thread",
    "clock_gettime of the kernel's clock id of the witness's tid from the report's thread",
    "sysctl kern.proc.pid with its threads (ki_runtime) from the report's thread"};
static const int tt_order[M_N] = {M_KPROC, M_TID_CLOCK, M_PTHREAD_CLOCK, M_OWN_CLOCK, M_OWN_RUSAGE};   /* (the report's preference) */

enum { TT_BURN_NS = 200000000, TT_SLEEP_MS = 300, TT_SETTLE_MS = 30, TT_ROUNDS = 2, TT_TRIES = 4, TT_STEP_NS = 50000,
       TT_LEAST_NS = 50000000, TT_MARK_MS = 50, TT_ROWS = 1024, TT_REG = 512, TT_TOP_CHARS = 800, TT_WAIT_S = 10 };
/* struct kinfo_proc, FreeBSD 11 amd64 (the SDK's sys/user.h, KINFO_PROC_SIZE 1088) */
enum { KP_PID = 72, KP_RUNTIME = 328, KP_TDNAME = 394, KP_TDNAME_LEN = 17, KP_TID = 600, KP_NEED = 604, KP_MAX_RECORD = 8192,
       KP_MAX_BUF = 2 << 20 };
#define TT_WITNESS_NAME "hookshot cpuw"

/* ---- the readings ---- */
static int64_t ts_ns(const struct timespec *ts) { return (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec; }
static int64_t ru_ns(const struct rusage *u) {
    return ((int64_t)u->ru_utime.tv_sec + u->ru_stime.tv_sec) * 1000000000LL + ((int64_t)u->ru_utime.tv_usec + u->ru_stime.tv_usec) * 1000;
}
static int64_t neg_errno(void) { return -(int64_t)(errno ? errno : EINVAL); }
static int64_t own_clock_real(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) ? neg_errno() : ts_ns(&ts);
}
static int64_t own_rusage_real(void) {
    struct rusage u;
    return getrusage(RUSAGE_THREAD, &u) ? neg_errno() : ru_ns(&u);
}
static int64_t pthread_clock_real(void *t) {
    clockid_t id;
    const int r = pthread_getcpuclockid((pthread_t)t, &id);
    if (r) return -(int64_t)r;
    struct timespec ts;
    return clock_gettime(id, &ts) ? neg_errno() : ts_ns(&ts);
}
static clockid_t tid_clock_id(int tid) {
#ifdef __PROSPERO__
    return (clockid_t)(0x80000000u | (uint32_t)tid);   /* (FreeBSD: CPUCLOCK_BIT | tid) */
#else
    return (clockid_t)((~(uint32_t)tid << 3) | 6);     /* (Linux: MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)) */
#endif
}
static int64_t tid_clock_real(int tid) {
    struct timespec ts;
    return clock_gettime(tid_clock_id(tid), &ts) ? neg_errno() : ts_ns(&ts);
}
static int kproc_real(void *buf, size_t *len) {
#ifdef __PROSPERO__
    int mib[4] = {1 /* CTL_KERN */, 14 /* KERN_PROC */, 1 /* KERN_PROC_PID */ | 0x10 /* KERN_PROC_INC_THREAD */, (int)getpid()};
    return sysctl(mib, 4, buf, len, NULL, 0) ? (errno ? errno : EINVAL) : 0;
#else
    (void)buf, (void)len;
    return ENOSYS;
#endif
}
static int64_t process_real(void) {
    struct rusage u;
    return getrusage(RUSAGE_SELF, &u) ? neg_errno() : ru_ns(&u);
}
#ifdef HOOKSHOT_THREAD_TIMES_TEST   /* (tests/thread_times_test.c puts wrong readings in, to see them refused) */
hos_thread_times_ops_t hos_thread_times_test_ops = {own_clock_real, own_rusage_real, pthread_clock_real, tid_clock_real, kproc_real,
                                                    process_real};
#define OPS hos_thread_times_test_ops
#else
static const hos_thread_times_ops_t tt_ops = {own_clock_real, own_rusage_real, pthread_clock_real, tid_clock_real, kproc_real, process_real};
#define OPS tt_ops
#endif

/* ---- the marks: a thread's own reading into its own slot, at most every TT_MARK_MS ---- */
volatile int hos_cpu_marks;
static int tt_mark_how = M_OWN_CLOCK;
static uint64_t tt_mark_every = (uint64_t)TT_MARK_MS * 1000000;
static int tt_tick_tsc;
static double tt_tick_per_ns = 1.0;
static inline uint64_t tt_tick(void) {
#if defined(__x86_64__)
    if (tt_tick_tsc) return __builtin_ia32_rdtsc();
#endif
    return (uint64_t)hos_now_ns();
}
void hos_cpu_slot_mark(struct hos_cpu_slot *s, int blocking) {
    const uint64_t now = tt_tick();
    const uint32_t q = __atomic_load_n(&s->seq, __ATOMIC_RELAXED);
    if (s->reads && now - s->read_tick < tt_mark_every) {
        if (s->blocked == blocking) return;
        __atomic_store_n(&s->seq, q + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&s->blocked, blocking, __ATOMIC_RELAXED);
        __atomic_store_n(&s->seq, q + 2, __ATOMIC_RELEASE);
        return;
    }
    const int64_t cpu = tt_mark_how == M_OWN_RUSAGE ? OPS.own_rusage() : OPS.own_clock();
    if (cpu < 0) return;
    __atomic_store_n(&s->seq, q + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s->cpu_ns, cpu, __ATOMIC_RELAXED);
    __atomic_store_n(&s->read_tick, now, __ATOMIC_RELAXED);
    __atomic_store_n(&s->reads, s->reads + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s->blocked, blocking, __ATOMIC_RELAXED);
    __atomic_store_n(&s->seq, q + 2, __ATOMIC_RELEASE);
}
void hos_cpu_slot_read(const struct hos_cpu_slot *s, struct hos_cpu_sample *o) {
    o->torn = 1;
    for (int i = 0; i < 64; i++) {
        const uint32_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        o->cpu_ns = __atomic_load_n(&s->cpu_ns, __ATOMIC_RELAXED);
        o->read_tick = __atomic_load_n(&s->read_tick, __ATOMIC_RELAXED);
        o->reads = __atomic_load_n(&s->reads, __ATOMIC_RELAXED);
        o->blocked = __atomic_load_n(&s->blocked, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!(a & 1) && __atomic_load_n(&s->seq, __ATOMIC_RELAXED) == a) {
            o->torn = 0;
            return;
        }
    }
}

/* ---- the state (allocated by the report's thread) ---- */
typedef struct { int tid; uint32_t reads; int64_t cpu; } tt_prev;
typedef struct {
    int tid, reg, blocked, state;   /* state: 0 counted, 1 new (no value before), 2 running unseen */
    uint32_t reads;
    int64_t cpu, delta;
    uint64_t tick;
    char name[40];
} tt_row;
static struct {
    int period_s, skip[M_N], kept[M_N], failed[M_N], kp_names, kp_size;
    unsigned char *kbuf;
    size_t kcap;
    hos_thread_time_row *reg;
    tt_row *cur;
    tt_prev *prev[M_N];
    int nprev[M_N];
    int *order;
} tt;

/* ---- kern-proc's records ---- */
static int32_t rd32(const unsigned char *p) { int32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int kp_fetch(size_t *len_out) {
    if (!tt.kbuf) return ENOMEM;
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t len = tt.kcap;
        const int e = OPS.kproc(tt.kbuf, &len);
        if (!e) {
            *len_out = len;
            return 0;
        }
        if (e != ENOMEM || attempt) return e;
        size_t need = 0;
        if (OPS.kproc(NULL, &need) || !need) return ENOMEM;
        size_t cap = need * 2 > KP_MAX_BUF ? KP_MAX_BUF : need * 2;
        if (cap <= tt.kcap) return ENOMEM;
        unsigned char *b = (unsigned char *)realloc(tt.kbuf, cap);
        if (!b) return ENOMEM;
        tt.kbuf = b, tt.kcap = cap;
    }
    return ENOMEM;
}
/* 0: every record is ki_structsize long and has this pid; else why not */
static const char *kp_layout(const unsigned char *b, size_t len, int *size_out) {
    *size_out = 0;
    if (len < 8) return "fewer than 8 bytes";
    const int size = rd32(b);
    *size_out = size;
    if (size < KP_NEED || size > KP_MAX_RECORD) return "a ki_structsize too small for the fields read, or too large";
    if (len % (size_t)size) return "not a whole number of records";
    const int pid = (int)getpid();
    for (size_t off = 0; off < len; off += (size_t)size) {
        if (rd32(b + off) != size) return "records of different sizes";
        if (rd32(b + off + KP_PID) != pid) return "a ki_pid that is not this process's";
    }
    return NULL;
}
static const unsigned char *kp_find(const unsigned char *b, size_t len, int size, int tid, int *count) {
    const unsigned char *found = NULL;
    *count = 0;
    for (size_t off = 0; off + (size_t)size <= len; off += (size_t)size)
        if (rd32(b + off + KP_TID) == tid) found = b + off, ++*count;
    return *count == 1 ? found : NULL;
}
/* where a 32-bit value sits in records (4-aligned offsets), as " +off" up to four */
static void kp_scan32(const unsigned char *b, size_t len, int size, int32_t v, char *out, size_t cap) {
    int n = 0;
    size_t used = 0;
    out[0] = 0;
    for (size_t off = 0; off + (size_t)size <= len && n < 4; off += (size_t)size)
        for (int k = 0; k + 4 <= size && n < 4; k += 4)
            if (rd32(b + off + k) == v) {
                int dup = 0;
                char tag[24];
                snprintf(tag, sizeof tag, " +%d", k);
                dup = strstr(out, tag) != NULL;
                if (!dup && used + strlen(tag) + 1 < cap) {
                    memcpy(out + used, tag, strlen(tag) + 1);
                    used += strlen(tag);
                    n++;
                }
            }
    if (!n) snprintf(out, cap, " nowhere");
}
static int kp_find_text(const unsigned char *r, int size, const char *text) {
    const size_t n = strlen(text);
    for (int k = 0; k + (int)n <= size; k++)
        if (!memcmp(r + k, text, n)) return k;
    return -1;
}
/* the 8-aligned fields of the witness's record that grew by about its burn (us, ns, or TSC ticks), as " +off (us)" */
static void kp_growth(const unsigned char *r0, const unsigned char *r1, int size, int64_t x_ns, char *out, size_t cap) {
    size_t used = 0;
    int n = 0;
    out[0] = 0;
    const double units[3] = {1e-3, 1.0, tt_tick_tsc ? tt_tick_per_ns : 0.0};
    static const char *const unit_name[3] = {"us", "ns", "TSC"};
    for (int k = 0; k + 8 <= size && n < 4; k += 8) {
        const uint64_t a = rd64(r0 + k), c = rd64(r1 + k);
        if (c <= a) continue;
        const double d = (double)(c - a);
        for (int u = 0; u < 3; u++) {
            const double want = (double)x_ns * units[u];
            if (want > 0 && d >= want * 0.85 && d <= want * 1.15) {
                char tag[32];
                snprintf(tag, sizeof tag, " +%d (%s)", k, unit_name[u]);
                if (used + strlen(tag) + 1 < cap) {
                    memcpy(out + used, tag, strlen(tag) + 1);
                    used += strlen(tag);
                    n++;
                }
            }
        }
    }
    if (!n) snprintf(out, cap, " none");
}
static void printable(char *dst, const unsigned char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i] >= 0x20 && src[i] < 0x7f ? (char)src[i] : '?';
    dst[i] = 0;
}

/* ---- the witness ---- */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int ready, go, step, quit, tid;
    pthread_t th;
    int64_t own[2][3];
    int64_t ran_ns, wall_ns;
    long samples;
} tw;   /* (mu and cv made by tt_main before the witness starts) */
static void tw_own(int i) {   /* (a reading that failed once is not called again) */
    static int bad[2];
    for (int j = 0; j < 2; j++) {
        if (tt.skip[j] || bad[j]) {
            tw.own[j][i] = -ENOSYS;
            continue;
        }
        tw.own[j][i] = j ? OPS.own_rusage() : OPS.own_clock();
        if (tw.own[j][i] < 0) bad[j] = 1;
    }
}
static void tw_body(void) {
    pthread_mutex_lock(&tw.mu);
    tw.tid = hos_ps5_tid(), tw.th = pthread_self(), tw.ready = 1;
    pthread_cond_broadcast(&tw.cv);
    for (int k = 1;; k++) {
        while (tw.go < k && !tw.quit) pthread_cond_wait(&tw.cv, &tw.mu);
        if (tw.go < k) break;
        pthread_mutex_unlock(&tw.mu);
        tw_own(0);
        const int64_t t0 = hos_now_ns();
        int64_t prev = t0, ran = 0, t = t0;
        long n = 0;
        do {
            t = hos_now_ns();
            if (t - prev < TT_STEP_NS) ran += t - prev;
            prev = t, n++;
        } while (t - t0 < TT_BURN_NS);
        tw_own(1);
        pthread_mutex_lock(&tw.mu);
        tw.ran_ns = ran, tw.wall_ns = t - t0, tw.samples = n, tw.step = 2 * k - 1;
        pthread_cond_broadcast(&tw.cv);
        pthread_mutex_unlock(&tw.mu);
        struct timespec ts = {0, TT_SLEEP_MS * 1000000L};
        while (nanosleep(&ts, &ts) && errno == EINTR) {}
        tw_own(2);
        pthread_mutex_lock(&tw.mu);
        tw.step = 2 * k;
        pthread_cond_broadcast(&tw.cv);
    }
    tw.ready = 2;
    pthread_cond_broadcast(&tw.cv);
    pthread_mutex_unlock(&tw.mu);
}
/* until *field >= value, TT_WAIT_S at most: 0, or ETIMEDOUT */
static int tw_wait(const int *field, int value) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += TT_WAIT_S;
    int r = 0;
    pthread_mutex_lock(&tw.mu);
    while (*field < value && r != ETIMEDOUT) r = pthread_cond_timedwait(&tw.cv, &tw.mu, &until);
    r = *field >= value ? 0 : ETIMEDOUT;
    pthread_mutex_unlock(&tw.mu);
    return r;
}
static void ms_sleep(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

#ifdef __PROSPERO__
static void tt_main(void);
static DWORD WINAPI tt_main_entry(void *p) {
    (void)p;
    tt_main();
    return 0;
}
static DWORD WINAPI tw_entry(void *p) {
    (void)p;
    SetThreadDescription(GetCurrentThread(), L"hookshot cpuw");
    tw_body();
    return 0;
}
static int tt_spawn(int witness) {
    HANDLE h = CreateThread(NULL, 1u << 18, witness ? tw_entry : tt_main_entry, NULL, 0, NULL);
    if (h && !witness) SetThreadDescription(h, L"hookshot cpu");
    return h != NULL;
}
#else
static void tt_main(void);
static void *tt_main_entry(void *p) {
    (void)p;
    pthread_setname_np(pthread_self(), "hookshot cpu");
    tt_main();
    return NULL;
}
static void *tw_entry(void *p) {
    (void)p;
    pthread_setname_np(pthread_self(), TT_WITNESS_NAME);
    tw_body();
    return NULL;
}
static int tt_spawn(int witness) {
    pthread_t t;
    if (pthread_create(&t, NULL, witness ? tw_entry : tt_main_entry, NULL)) return 0;
    pthread_detach(t);
    return 1;
}
#endif

/* ---- the calibration ---- */
typedef struct {
    int64_t v[TT_TRIES][3];
    int64_t x[TT_TRIES];
    int rounds, passes, err, tried;
    char kp[400];   /* (kern-proc: what its records said) */
} tt_cal;
static int kp_keep_have[2];   /* (the witness's record kept before and after the first burn, for the line's search) */
enum { KP_BAD_LAYOUT = 1000, KP_NO_WITNESS = 1001 };
static int64_t kp_witness(int phase, int first, unsigned char *keep, tt_cal *c) {
    size_t len = 0;
    const int e = kp_fetch(&len);
    if (e) {
        if (first) snprintf(c->kp, sizeof c->kp, "; sysctl refused: errno %d", e);
        return -(int64_t)e;
    }
    int size = 0;
    const char *bad = kp_layout(tt.kbuf, len, &size);
    if (bad) {
        if (first) {
            char at[64];
            kp_scan32(tt.kbuf, len, len >= 8 && rd32(tt.kbuf) > 0 && rd32(tt.kbuf) <= KP_MAX_RECORD ? rd32(tt.kbuf) : (int)(len < 4096 ? len : 4096),
                      (int32_t)getpid(), at, sizeof at);
            snprintf(c->kp, sizeof c->kp, "; %zu bytes, ki_structsize %d: %s (this pid at%s of the first records)", len, size, bad, at);
        }
        return -KP_BAD_LAYOUT;
    }
    tt.kp_size = size;
    int count = 0;
    const unsigned char *r = kp_find(tt.kbuf, len, size, tw.tid, &count), *any = r;
    for (size_t off = 0; !any && off + (size_t)size <= len; off += (size_t)size)   /* (not at +600: the record that has it anywhere) */
        for (int k = 0; k + 4 <= size; k += 4)
            if (rd32(tt.kbuf + off + k) == tw.tid) {
                any = tt.kbuf + off;
                break;
            }
    if (keep && any && phase < 2) {
        memcpy(keep + (size_t)phase * KP_MAX_RECORD, any, (size_t)size);
        kp_keep_have[phase] = 1;
    }
    if (!r) {
        if (first) {
            char at[64];
            kp_scan32(tt.kbuf, len, size, tw.tid, at, sizeof at);
            snprintf(c->kp, sizeof c->kp, "; %zu records of %d bytes; the witness's tid %d at +%d in %d of them (expected 1); found at%s",
                     len / (size_t)size, size, tw.tid, KP_TID, count, at);
            if (any) {
                const int at_name = kp_find_text(any, size, TT_WITNESS_NAME);
                const size_t u = strlen(c->kp);
                if (at_name >= 0) snprintf(c->kp + u, sizeof c->kp - u, "; its name at +%d", at_name);
            }
        }
        return -KP_NO_WITNESS;
    }
    if (first) {
        char name[KP_TDNAME_LEN + 1];
        printable(name, r + KP_TDNAME, KP_TDNAME_LEN);
        tt.kp_names = !strncmp(name, TT_WITNESS_NAME, strlen(TT_WITNESS_NAME));
        snprintf(c->kp, sizeof c->kp, "; %zu records of %d bytes, the witness's tid at +%d, ki_tdname at +%d \"%s\"%s", len / (size_t)size,
                 size, KP_TID, KP_TDNAME, name, tt.kp_names ? "" : " (not its name: the names are not used)");
        if (!tt.kp_names) {
            const int at = kp_find_text(r, size, TT_WITNESS_NAME);
            const size_t u = strlen(c->kp);
            if (at >= 0) snprintf(c->kp + u, sizeof c->kp - u, "; its name found at +%d", at);
        }
    }
    return (int64_t)rd64(r + KP_RUNTIME) * 1000;
}
static void tt_reader_reads(int64_t v[M_N], int phase, int first, unsigned char *keep, tt_cal *cal) {
    for (int m = M_PTHREAD_CLOCK; m < M_N; m++) {
        v[m] = -ENOSYS;
        if (tt.skip[m] || tt.failed[m]) continue;
        if (first && phase == 0 && !cal[m].tried) {
            cal[m].tried = 1;
            hos_ps5_trail("cpu: calibration: trying %s (%s); should the title stop here, HOOKSHOT_PS5_THREAD_TIMES_SKIP=%s leaves it out",
                          tt_name[m], tt_what[m], tt_name[m]);
        }
        v[m] = m == M_PTHREAD_CLOCK ? OPS.pthread_clock((void *)tw.th) : m == M_TID_CLOCK ? OPS.tid_clock(tw.tid)
             : kp_witness(phase, first && phase == 0, keep, &cal[m]);
        /* (kern-proc's records not as read: asked again for the first round's other two halves, the call itself answering, for
           the line's search of the witness's fields; never after) */
        if (v[m] < 0 && !(m == M_KPROC && first && v[m] <= -KP_BAD_LAYOUT)) tt.failed[m] = 1;
    }
}
static int tt_judge(int64_t db, int64_t ds, int64_t x) {
    return db >= x * 85 / 100 - 2000000 && db <= x * 115 / 100 + 2000000 && ds >= 0 && ds <= x * 5 / 100 + 2000000;
}
/* 1 when the witness answered; the kept readings in tt.kept */
static int tt_calibrate(void) {
    tt_cal cal[M_N];
    memset(cal, 0, sizeof cal);
    unsigned char *keep = (unsigned char *)calloc(2, KP_MAX_RECORD);   /* (the witness's kern-proc record before and after a burn) */
    int64_t grew[TT_TRIES], ran[TT_TRIES], wall[TT_TRIES];
    long steps[TT_TRIES];
    int conclusive = 0, tries = 0;
    if (!tt.skip[M_KPROC]) {   /* (kern-proc's buffer: twice what the first size asks, 64 KiB at least, 2 MiB at most) */
        size_t need = 0;
        hos_ps5_trail("cpu: calibration: trying kern-proc's size (sysctl kern.proc.pid with its threads); should the title stop here, "
                      "HOOKSHOT_PS5_THREAD_TIMES_SKIP=kern-proc leaves it out");
        cal[M_KPROC].tried = 1;
        const int e = OPS.kproc(NULL, &need);
        if (e || !need) {
            tt.failed[M_KPROC] = 1;
            cal[M_KPROC].err = e ? e : EINVAL;
            snprintf(cal[M_KPROC].kp, sizeof cal[M_KPROC].kp, "; sysctl refused: errno %d", e ? e : EINVAL);
        } else {
            tt.kcap = need * 2 < 65536 ? 65536 : need * 2 > KP_MAX_BUF ? KP_MAX_BUF : need * 2;
            if (!(tt.kbuf = (unsigned char *)malloc(tt.kcap))) tt.failed[M_KPROC] = 1, tt.kcap = 0;
        }
    }
    if (!tt_spawn(1)) {
        hos_log("cpu: calibration: no witness thread could be made: no report");
        free(keep);
        return 0;
    }
    if (tw_wait(&tw.ready, 1)) {
        hos_log("cpu: calibration: the witness did not start in %d s: no report", TT_WAIT_S);
        free(keep);
        return 0;
    }
    hos_ps5_trail("cpu: calibration: the witness (tid %d) tries own-clock and own-rusage on itself%s%s", tw.tid,
                  tt.skip[M_OWN_CLOCK] ? " (own-clock left out)" : "", tt.skip[M_OWN_RUSAGE] ? " (own-rusage left out)" : "");
    for (int k = 1; k <= TT_TRIES && conclusive < TT_ROUNDS; k++) {
        int64_t v[3][M_N];
        tt_reader_reads(v[0], 0, k == 1, keep, cal);
        const int64_t p0 = OPS.process();
        pthread_mutex_lock(&tw.mu);
        tw.go = k;
        pthread_cond_broadcast(&tw.cv);
        pthread_mutex_unlock(&tw.mu);
        if (tw_wait(&tw.step, 2 * k - 1)) break;
        ms_sleep(TT_SETTLE_MS);   /* (the witness in its sleep by now: what it ran is all counted) */
        tt_reader_reads(v[1], 1, k == 1, keep, cal);
        const int64_t p1 = OPS.process();
        if (tw_wait(&tw.step, 2 * k)) break;
        ms_sleep(TT_SETTLE_MS);
        tt_reader_reads(v[2], 2, k == 1, keep, cal);
        tries = k;
        const int i = k - 1;
        ran[i] = tw.ran_ns, wall[i] = tw.wall_ns, steps[i] = tw.samples, grew[i] = p0 >= 0 && p1 >= 0 ? p1 - p0 : -1;
        const int ok = tw.ran_ns >= TT_LEAST_NS;
        conclusive += ok;
        for (int m = 0; m < M_N; m++) {
            for (int ph = 0; ph < 3; ph++) cal[m].v[i][ph] = m < M_PTHREAD_CLOCK ? tw.own[m][ph] : v[ph][m];
            cal[m].x[i] = tw.ran_ns;
            if (cal[m].err) continue;
            for (int ph = 0; ph < 3; ph++)
                if (cal[m].v[i][ph] < 0 && !cal[m].err) cal[m].err = (int)-cal[m].v[i][ph];
            if (cal[m].err || !ok) continue;
            cal[m].rounds++;
            cal[m].passes += tt_judge(cal[m].v[i][1] - cal[m].v[i][0], cal[m].v[i][2] - cal[m].v[i][1], tw.ran_ns);
        }
        if (k == 1 && keep && kp_keep_have[0] && kp_keep_have[1] &&
            (cal[M_KPROC].err || !tt_judge(cal[M_KPROC].v[0][1] - cal[M_KPROC].v[0][0], cal[M_KPROC].v[0][2] - cal[M_KPROC].v[0][1], tw.ran_ns))) {
            char g[160];
            kp_growth(keep, keep + KP_MAX_RECORD, tt.kp_size, tw.ran_ns, g, sizeof g);
            const size_t u = strlen(cal[M_KPROC].kp);
            snprintf(cal[M_KPROC].kp + u, sizeof cal[M_KPROC].kp - u, "; the witness's fields that grew by its burn:%s", g);
        }
        if (cal[M_KPROC].err) tt.failed[M_KPROC] = 1;
    }
    pthread_mutex_lock(&tw.mu);
    tw.quit = 1;
    pthread_cond_broadcast(&tw.cv);
    pthread_mutex_unlock(&tw.mu);
    free(keep);
    if (!tries) {
        hos_log("cpu: calibration: the witness did not answer in %d s: no report", TT_WAIT_S);
        return 0;
    }
    hos_log("cpu: calibration: the witness (tid %d) burned %.1f and %.1f ms of CPU (counted by its clock's steps under %d us: %ld and %ld "
            "steps, %.1f and %.1f ms of wall time), then slept %d ms, %d times; the process's getrusage grew %.1f and %.1f ms meanwhile; "
            "%d conclusive (%.0f ms of CPU at least); the TSC %.3f ticks a ns%s",
            tw.tid, ran[0] / 1e6, tries > 1 ? ran[1] / 1e6 : 0.0, TT_STEP_NS / 1000, steps[0], tries > 1 ? steps[1] : 0L, wall[0] / 1e6,
            tries > 1 ? wall[1] / 1e6 : 0.0, TT_SLEEP_MS, tries, grew[0] / 1e6, tries > 1 ? grew[1] / 1e6 : 0.0, conclusive,
            TT_LEAST_NS / 1e6, tt_tick_per_ns, tt_tick_tsc ? "" : " (not used: the marks time themselves by the monotonic clock)");
    for (int m = 0; m < M_N; m++) {
        tt_cal *c = &cal[m];
        tt.kept[m] = !tt.skip[m] && !c->err && c->rounds >= TT_ROUNDS && c->passes == c->rounds;
        char vals[200] = "";
        if (!tt.skip[m] && !c->err) {
            int u = 0;
            for (int i = 0; i < tries && u < (int)sizeof vals - 60; i++)
                u += snprintf(vals + u, sizeof vals - (size_t)u, "%sburning %.1f, asleep %.1f ms (the witness ran %.1f)", i ? "; " : ": ",
                              (c->v[i][1] - c->v[i][0]) / 1e6, (c->v[i][2] - c->v[i][1]) / 1e6, c->x[i] / 1e6);
        }
        const char *verdict = tt.skip[m] ? "not tried (HOOKSHOT_PS5_THREAD_TIMES_SKIP)"
                            : c->err >= KP_BAD_LAYOUT ? "refused: the records are not laid out as read"
                            : c->err ? "refused: it failed" : tt.kept[m] ? "kept"
                            : c->rounds < TT_ROUNDS ? "refused: too few conclusive rounds" : "refused: not the witness's CPU";
        char err[40] = "";
        if (c->err && c->err < KP_BAD_LAYOUT && !tt.skip[m]) snprintf(err, sizeof err, " (error %d)", c->err);
        hos_log("cpu: reading %s (%s)%s: %s%s%s", tt_name[m], tt_what[m], vals, verdict, err, m == M_KPROC ? c->kp : "");
    }
    return 1;
}

/* ---- the report ---- */
static int tt_active(int m) {
    if (!tt.kept[m] || tt.failed[m]) return 0;
    if (m == M_OWN_CLOCK || m == M_OWN_RUSAGE) return hos_cpu_marks && tt_mark_how == m;
    return 1;
}
static void name_copy(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   /* (not in the middle of a UTF-8 character) */
    }
    for (size_t i = 0; i < n; i++) dst[i] = src[i] == ';' ? ',' : src[i];
    dst[n] = 0;
}
/* the rows of reading m now (tt.cur); -errno or the count */
static int tt_fill(int m, int nreg) {
    tt_row *cur = tt.cur;
    int n = 0;
    if (m == M_KPROC) {
        size_t len = 0;
        const int e = kp_fetch(&len);
        if (e) return -e;
        int size = 0;
        if (kp_layout(tt.kbuf, len, &size) || size != tt.kp_size) return -EPROTO;
        for (size_t off = 0; off + (size_t)size <= len && n < TT_ROWS; off += (size_t)size) {
            const unsigned char *r = tt.kbuf + off;
            tt_row *w = &cur[n++];
            memset(w, 0, sizeof *w);
            w->tid = rd32(r + KP_TID), w->cpu = (int64_t)rd64(r + KP_RUNTIME) * 1000;
            for (int i = 0; i < nreg; i++)
                if (tt.reg[i].tid == w->tid) {
                    w->reg = 1;
                    name_copy(w->name, sizeof w->name, tt.reg[i].name);
                    break;
                }
            if (!w->reg && tt.kp_names) {
                char nm[KP_TDNAME_LEN + 1];
                printable(nm, r + KP_TDNAME, KP_TDNAME_LEN);
                name_copy(w->name, sizeof w->name, nm);
            }
        }
        return n;
    }
    for (int i = 0; i < nreg && n < TT_ROWS; i++) {
        const hos_thread_time_row *g = &tt.reg[i];
        int64_t cpu = -1;
        if (m == M_TID_CLOCK) cpu = OPS.tid_clock(g->tid);
        else if (m == M_PTHREAD_CLOCK) cpu = OPS.pthread_clock(g->pthread);
        else if (g->cpu.reads) cpu = g->cpu.cpu_ns;
        if (cpu < 0) continue;
        tt_row *w = &cur[n++];
        memset(w, 0, sizeof *w);
        w->tid = g->tid, w->reg = 1, w->cpu = cpu, w->reads = g->cpu.reads, w->blocked = g->cpu.blocked, w->tick = g->cpu.read_tick;
        name_copy(w->name, sizeof w->name, g->name);
    }
    return n;
}
/* each row's CPU since the last report of reading m, and the previous values made these */
static void tt_deltas(int m, int n) {
    tt_prev *p = tt.prev[m];
    const int own = m == M_OWN_CLOCK || m == M_OWN_RUSAGE;
    for (int i = 0; i < n; i++) {
        tt_row *w = &tt.cur[i];
        const tt_prev *was = NULL;
        for (int k = 0; k < tt.nprev[m]; k++)
            if (p[k].tid == w->tid) { was = &p[k]; break; }
        if (!was || w->cpu < was->cpu) w->state = 1, w->delta = 0;
        else if (own && w->reads == was->reads && !w->blocked) w->state = 2, w->delta = 0;
        else w->state = 0, w->delta = w->cpu - was->cpu;
    }
    for (int i = 0; i < n && i < TT_ROWS; i++) p[i].tid = tt.cur[i].tid, p[i].cpu = tt.cur[i].cpu, p[i].reads = tt.cur[i].reads;
    tt.nprev[m] = n;
}
static int by_delta(const void *a, const void *b) {
    const tt_row *x = (const tt_row *)a, *y = (const tt_row *)b;
    return x->delta < y->delta ? 1 : x->delta > y->delta ? -1 : x->tid - y->tid;
}
static double reg_sum(int n) {
    double s = 0;
    for (int i = 0; i < n; i++)
        if (tt.cur[i].reg && !tt.cur[i].state) s += (double)tt.cur[i].delta;
    return s;
}
static void tt_loop(void) {
    if (hos_cpu_marks) ms_sleep(1000);   /* (the threads' first marks, for a baseline that has them) */
    int nreg = hos_posix_thread_times(tt.reg, TT_REG);
    for (int m = 0; m < M_N; m++)   /* (the baseline) */
        if (tt_active(m)) {
            const int n = tt_fill(m, nreg);
            if (n >= 0) tt_deltas(m, n);
        }
    int64_t last = hos_now_ns(), ru_last = OPS.process();
    unsigned long frames_last = __atomic_load_n(&rx_ps5_presents, __ATOMIC_RELAXED);
    for (int number = 1;; number++) {
        const int64_t due = last + (int64_t)tt.period_s * 1000000000LL;
        for (int64_t now = hos_now_ns(); now < due; now = hos_now_ns()) ms_sleep((int)((due - now + 999999) / 1000000));
        nreg = hos_posix_thread_times(tt.reg, TT_REG);
        const int64_t now = hos_now_ns(), ru = OPS.process();
        const unsigned long frames = __atomic_load_n(&rx_ps5_presents, __ATOMIC_RELAXED);
        const uint64_t tick_now = tt_tick();
        const double period_ns = (double)(now - last);
        /* the primary: the first active reading that answers; the rows sorted, the seen and the lines */
        int primary = -1, n = 0;
        char failed[120] = "";
        for (int i = 0; i < M_N && primary < 0; i++) {
            const int m = tt_order[i];
            if (!tt_active(m)) continue;
            n = tt_fill(m, nreg);
            if (n < 0) {
                const size_t u = strlen(failed);
                snprintf(failed + u, sizeof failed - u, "; %s failed (error %d) this time", tt_name[m], -n);
                continue;
            }
            primary = m;
        }
        if (primary < 0) {
            hos_log("cpu: report %d: no reading answered%s", number, failed);
            last = now, ru_last = ru, frames_last = frames;
            continue;
        }
        tt_deltas(primary, n);
        double seen = 0;
        int counted = 0, fresh = 0, unseen = 0, full = 0;
        for (int i = 0; i < n; i++) {
            const tt_row *w = &tt.cur[i];
            if (w->state == 0) seen += (double)w->delta, counted++;
            else if (w->state == 1) fresh++;
            else unseen++;
        }
        const double reg_primary = reg_sum(n);
        qsort(tt.cur, (size_t)n, sizeof *tt.cur, by_delta);
        char sat[200] = "", run[200] = "";
        int su = 0, ru_n = 0;
        for (int i = 0; i < n; i++) {
            const tt_row *w = &tt.cur[i];
            const double pct = w->delta * 100.0 / period_ns;
            if (w->state == 0 && pct >= 85.0) {
                if (full++ < 4 && su < 150) su += snprintf(sat + su, sizeof sat - (size_t)su, "%s%s %.0f%%", full > 1 ? ", " : ": ", w->name, pct);
            }
            if (w->state == 2 && ru_n < 150) {
                const double silent = tt_tick_per_ns > 0 ? (double)(tick_now - w->tick) / tt_tick_per_ns / 1e9 : 0;
                ru_n += snprintf(run + ru_n, sizeof run - (size_t)ru_n, "%s%s (%.0f s)", ru_n ? ", " : ": ", w->name, silent);
            }
        }
        char line2[TT_TOP_CHARS + 160];
        int l2 = snprintf(line2, sizeof line2, "cpu: busiest %d:", number), listed = 0;
        double next_pct = 0;   /* (the first one not listed: the most any of the rest used) */
        for (int i = 0; i < n; i++) {
            const tt_row *w = &tt.cur[i];
            if (w->state) continue;
            const double pct = w->delta * 100.0 / period_ns;
            if ((pct < 1.0 && listed >= 3) || l2 > TT_TOP_CHARS) {
                next_pct = pct;
                break;
            }
            l2 += snprintf(line2 + l2, sizeof line2 - (size_t)l2, "%s %s [%d] %.1f%%%s", listed ? ";" : "", w->name[0] ? w->name : "?", w->tid,
                           pct, pct >= 85.0 ? "!" : "");
            listed++;
        }
        if (counted > listed) snprintf(line2 + l2, sizeof line2 - (size_t)l2, "; %d more at %.1f%% or less", counted - listed, next_pct);
        /* the others kept, on the registered threads */
        char check[200] = "";
        int cu = 0;
        for (int m = 0; m < M_N; m++) {
            if (m == primary || !tt_active(m)) continue;
            const int k = tt_fill(m, nreg);
            if (k < 0) continue;
            tt_deltas(m, k);
            cu += snprintf(check + cu, sizeof check - (size_t)cu, "%s %s %.2f", cu ? "," : "; check on the registered threads:", tt_name[m],
                           reg_sum(k) / period_ns);
        }
        if (cu) snprintf(check + cu, sizeof check - (size_t)cu, " (%s %.2f) CPUs", tt_name[primary], reg_primary / period_ns);
        const double process = ru >= 0 && ru_last >= 0 ? (double)(ru - ru_last) / period_ns : -1;
        double not_seen = process >= 0 ? process - seen / period_ns : 0.0;
        if (not_seen > -0.005 && not_seen < 0.005) not_seen = 0.0;   /* (no "-0.00") */
        char news[48] = "";
        if (fresh) snprintf(news, sizeof news, "; %d new, from the next report", fresh);
        char unseen_s[240] = "";
        if (unseen) snprintf(unseen_s, sizeof unseen_s, "; %d running unseen%s", unseen, run);
        hos_log("cpu: report %d at %.0f s over %.1f s by %s: %lu frames (%.1f/s); the process %.2f CPUs (getrusage); %d threads seen, %.2f CPUs "
                "(%.2f not seen)%s; %d at 85%% or more%s%s%s%s",
                number, hos_ps5_since_start(), period_ns / 1e9, tt_name[primary], frames - frames_last, (frames - frames_last) / (period_ns / 1e9),
                process, counted, seen / period_ns, not_seen, news, full, sat, unseen_s, check, failed);
        hos_log("%s", line2);
        if (number == 1) hos_ps5_trail("cpu: the first report: by %s, the process %.2f CPUs, %d threads seen %.2f, %d at 85%% or more%s", tt_name[primary],
                                       process, counted, seen / period_ns, full, sat);
        last = now, ru_last = ru, frames_last = frames;
    }
}

static void tt_main(void) {
    hos_log("cpu: per-thread CPU times every %d s (HOOKSHOT_PS5_THREAD_TIMES): the readings first, on a witness thread", tt.period_s);
    tt.reg = (hos_thread_time_row *)calloc(TT_REG, sizeof *tt.reg);
    tt.cur = (tt_row *)calloc(TT_ROWS, sizeof *tt.cur);
    for (int m = 0; m < M_N; m++) tt.prev[m] = (tt_prev *)calloc(TT_ROWS, sizeof *tt.prev[m]);
    int ok = tt.reg && tt.cur;
    for (int m = 0; m < M_N; m++) ok = ok && tt.prev[m];
    if (!ok) {
        hos_log("cpu: no memory for the report's rows: no report");
        return;
    }
    pthread_mutex_init(&tw.mu, NULL);
    pthread_cond_init(&tw.cv, NULL);
    /* (the marks time themselves by the TSC, which the renderer's timers read on the console already: gpu/vk/vk_internal.h
       prof_now. Its rate over 50 ms first, for the calibration's search of a kern-proc field in TSC ticks; over the whole
       calibration after) */
    const int64_t n0 = hos_now_ns();
#if defined(__x86_64__)
    const uint64_t c0 = __builtin_ia32_rdtsc();
    for (int pass = 0; pass < 2; pass++) {
        if (!pass) ms_sleep(50);
        else if (!tt_calibrate()) return;
        const uint64_t c1 = __builtin_ia32_rdtsc();
        const int64_t n1 = hos_now_ns();
        const double r = n1 > n0 && c1 > c0 ? (double)(c1 - c0) / (double)(n1 - n0) : 0;
        tt_tick_tsc = r > 0.1 && r < 10.0;
        tt_tick_per_ns = tt_tick_tsc ? r : 1.0;
    }
#else
    (void)n0;
    if (!tt_calibrate()) return;
#endif
    tt_mark_every = (uint64_t)((double)TT_MARK_MS * 1e6 * tt_tick_per_ns);
    const int how = tt.kept[M_OWN_CLOCK] ? M_OWN_CLOCK : tt.kept[M_OWN_RUSAGE] ? M_OWN_RUSAGE : -1;
    if (how >= 0) {
        tt_mark_how = how;
        __atomic_store_n(&hos_cpu_marks, 1, __ATOMIC_RELEASE);
    }
    int primary = -1;
    char kept[120] = "";
    int ku = 0;
    for (int i = 0; i < M_N; i++) {
        const int m = tt_order[i];
        if (!tt_active(m)) continue;
        if (primary < 0) primary = m;
        ku += snprintf(kept + ku, sizeof kept - (size_t)ku, "%s%s", ku ? ", " : "", tt_name[m]);
    }
    if (primary < 0) {
        hos_log("cpu: no reading read the witness right: no report");
        hos_ps5_trail("cpu: calibration: no reading read the witness right: no report");
        return;
    }
    hos_log("cpu: the report every %d s by %s%s; the threads' marks: %s; in use: %s", tt.period_s, tt_name[primary],
            primary == M_KPROC ? (tt.kp_names ? " (every thread of the process, the kernel's names for those the host did not start)"
                                              : " (every thread of the process; the kernel's names not used)")
                               : " (the threads the host started or adopted)",
            how >= 0 ? tt_name[how] : "off", kept);
    hos_ps5_trail("cpu: calibration: the report by %s; in use: %s; own-clock %s, own-rusage %s, pthread-clock %s, tid-clock %s, kern-proc %s",
                  tt_name[primary], kept, tt.kept[0] ? "kept" : "no", tt.kept[1] ? "kept" : "no", tt.kept[2] ? "kept" : "no",
                  tt.kept[3] ? "kept" : "no", tt.kept[4] ? "kept" : "no");
    tt_loop();
}

void hos_thread_times_start(void) {
    static int started;
    const char *e = getenv("HOOKSHOT_PS5_THREAD_TIMES");
    const int p = e && *e ? atoi(e) : 0;
    if (p <= 0 || started) return;
    started = 1;
    tt.period_s = p < 2 ? 2 : p > 600 ? 600 : p;
    const char *s = getenv("HOOKSHOT_PS5_THREAD_TIMES_SKIP");
    for (int m = 0; s && m < M_N; m++) {
        const size_t len = strlen(tt_name[m]);
        for (const char *q = strstr(s, tt_name[m]); q; q = strstr(q + 1, tt_name[m]))
            if ((q == s || q[-1] == ',' || q[-1] == ' ') && (!q[len] || q[len] == ',' || q[len] == ' ')) tt.skip[m] = 1;
    }
    if (!tt_spawn(0)) hos_log("cpu: no thread could be made for the per-thread CPU report (HOOKSHOT_PS5_THREAD_TIMES)");
}
#else
typedef int hos_thread_times_off;   /* (the console's alone: an empty unit elsewhere) */
#endif
