/* The platform probe's scheduling check (scripts/ps5/plat-probe, tests=sched): how the console schedules a title's threads,
   and the game's virtual cores (host/hos_thread.c) on it with no game. Written 2026-11-09 after the game's first run
   crawled (logs/game-run2: frames 20 to 100 s apart, a thread waiting 26 s for a virtual core whose limit is 250 ms).

   1. sched.policy: the policy, priority and CPUs a thread starts with, the policies' ranges, the process's CPU sets, and
      whether a thread's CPU clock counts (the game's scheduling report rests on it).
   2. sched.slice.<policy>: two threads of one priority on one CPU. A spins up to 300 ms; B moves onto A's CPU and says
      when it first ran there. "after 300 ms, when A stopped": threads of one priority are not time-sliced (a real-time
      FIFO); "while A spun": a slice, its length. Once on the policy a thread starts with, then on rr, then on other.
   3. sched.vcores.<round>: the virtual cores' pattern: sched_threads threads (24) share sched_cores cores (6) the way
      hos_thread.c's do (a core taken for a piece of work and given back to block, a waiter that waits in 1 ms steps,
      asks an equal holder to yield after 2 ms, takes a granted core not taken up after 2 ms, runs without one after
      250 ms), alone, then beside sched_spinners threads (the title's CPUs less two) that spin without yielding for the
      round (guest loops with no checkpoint, escaped threads), on the console's policy, then rr, then other.
      Each says the takes, the waits for a core, the 1 ms steps' real length, how long granted cores waited to be taken
      up, the escapes, the work done against what six cores could do, and the longest any thread went without progress.
   Every thread spins at most 300 ms (slice) or the round's sched_seconds (3): nothing here can hang the title.
   SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <windows.h>   /* (host/posix's: SRWLOCK, CONDITION_VARIABLE and WaitOnAddress on the futexes, as the runtime's) */

#include "plat_probe.h"
#include "ps5.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int32_t scePthreadSetaffinity(pthread_t thread, uint64_t mask);

static int64_t ns_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
/* 0: the policy a thread starts with; else that policy (its priority at the same place in the range: ps5_platform.c) */
static int set_policy(int policy) { return policy ? hos_ps5_sched_set(policy) : 0; }
static const char *round_policy(int policy, char *buf, size_t cap) {
    int pol = -1, prio = 0;
    hos_ps5_sched_of((void *)pthread_self(), &pol, &prio);
    snprintf(buf, cap, "%s %d", hos_ps5_policy_name(pol), prio);
    (void)policy;
    return buf;
}

/* ---- 2. one CPU, two threads of one priority ---- */
static struct {
    int policy, cpu;
    volatile int a_spinning, b_ran;
    int64_t a_start, a_end, b_ran_at;
    int a_set, b_set, b_pinned;
    char a_pol[32], b_pol[32];
} sl;
static void *slice_a(void *p) {
    (void)p;
    sl.a_set = set_policy(sl.policy);
    round_policy(sl.policy, sl.a_pol, sizeof sl.a_pol);
    scePthreadSetaffinity(pthread_self(), 1ull << sl.cpu);
    sl.a_start = ns_now();
    __atomic_store_n(&sl.a_spinning, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&sl.b_ran, __ATOMIC_ACQUIRE) && ns_now() - sl.a_start < 300000000) {}
    sl.a_end = ns_now();
    return NULL;
}
static void *slice_b(void *p) {
    (void)p;
    sl.b_set = set_policy(sl.policy);
    round_policy(sl.policy, sl.b_pol, sizeof sl.b_pol);
    const int64_t t0 = ns_now();
    while (!__atomic_load_n(&sl.a_spinning, __ATOMIC_ACQUIRE) && ns_now() - t0 < 2000000000) {
        const struct timespec s = {0, 100000};
        nanosleep(&s, NULL);
    }
    {   /* (a moment so that A is spinning on its CPU, then onto it: the call comes back once B runs there) */
        const struct timespec s = {0, 2000000};
        nanosleep(&s, NULL);
    }
    sl.b_pinned = scePthreadSetaffinity(pthread_self(), 1ull << sl.cpu);
    sl.b_ran_at = ns_now();
    __atomic_store_n(&sl.b_ran, 1, __ATOMIC_RELEASE);
    return NULL;
}
static double slice_round(int policy, const char *name, int cpu) {
    memset(&sl, 0, sizeof sl);
    sl.policy = policy, sl.cpu = cpu;
    pthread_t a, b;
    pthread_create(&a, NULL, slice_a, NULL);
    pthread_create(&b, NULL, slice_b, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    const double after = (sl.b_ran_at - sl.a_start) / 1e6, a_spun = (sl.a_end - sl.a_start) / 1e6;
    const int sliced = after < 290 && a_spun < 290;
    char id[48];
    snprintf(id, sizeof id, "sched.slice.%s", name);
    probe_info(id, "A (%s, set %d) spun on CPU %d; B (%s, set %d) moved onto it (%d) and ran there %.1f ms after A began; A stopped "
                   "spinning at %.1f ms: %s",
               sl.a_pol, sl.a_set, cpu, sl.b_pol, sl.b_set, sl.b_pinned, after, a_spun,
               sliced ? "B ran while A spun (a time slice)" : "B ran only once A stopped (no time slice between them)");
    return sliced ? after : -1;
}

/* ---- 3. the virtual cores' pattern (host/hos_thread.c vc_acquire, vc_release, vc_yield, as of this build) ---- */
typedef struct {
    CONDITION_VARIABLE cv;
    int held, queued;
    volatile int kick;
    int64_t since, granted;
    int64_t work_ns, gap_max;
    unsigned seed;
} Vt;
static SRWLOCK mv_lock = SRWLOCK_INIT;
static Vt *mv_owner[8];
static Vt *mv_queue[64];
static int mv_nq, mv_cores;
static struct {
    int64_t takes, waits, wait_ns, wait_max, steps, step_max, claim_max, stolen, escapes, kicks;
} mv;
static int64_t mv_end;
static int mv_policy;
static void mv_dequeue(Vt *t) {
    for (int i = 0; i < mv_nq; i++)
        if (mv_queue[i] == t) { mv_queue[i] = mv_queue[--mv_nq]; break; }
    t->queued = 0;
}
static int mv_free(void) {
    for (int c = 0; c < mv_cores; c++)
        if (!mv_owner[c]) return c;
    return -1;
}
static void mv_take(Vt *t, int c) {
    mv_owner[c] = t;
    t->held = c + 1;
    t->kick = 0;
    t->granted = 0;
}
static void mv_acquire(Vt *t) {
    AcquireSRWLockExclusive(&mv_lock);
    mv.takes++;
    int c = mv_free();
    if (c >= 0) {
        mv_take(t, c);
        ReleaseSRWLockExclusive(&mv_lock);
        return;
    }
    const int64_t t0 = ns_now();
    t->since = t0;
    t->queued = 1;
    if (mv_nq < 64) mv_queue[mv_nq++] = t;
    mv.waits++;
    for (;;) {
        if (ns_now() - t0 > 2000000)   /* (an equal holder asked to yield after 2 ms: HOS_VCORE_SLICE_US) */
            for (int k = 0; k < mv_cores; k++)
                if (mv_owner[k] && !mv_owner[k]->kick && !mv_owner[k]->granted) {
                    mv_owner[k]->kick = 1;
                    mv.kicks++;
                    break;
                }
        const int64_t s0 = ns_now();
        SleepConditionVariableSRW(&t->cv, &mv_lock, 1, 0);
        const int64_t s1 = ns_now();
        mv.steps++;
        if (s1 - s0 > mv.step_max) mv.step_max = s1 - s0;
        if (t->held) {
            if (t->granted && s1 - t->granted > mv.claim_max) mv.claim_max = s1 - t->granted;
            t->granted = 0;
            break;
        }
        if ((c = mv_free()) >= 0) {
            mv_dequeue(t);
            mv_take(t, c);
            break;
        }
        int u = -1;   /* (HOS_VCORE_CLAIM_US: a core granted 2 ms ago and not taken up) */
        for (int k = 0; k < mv_cores; k++)
            if (mv_owner[k] && mv_owner[k]->granted && s1 - mv_owner[k]->granted > 2000000) { u = k; break; }
        if (u >= 0) {
            Vt *g = mv_owner[u];
            g->held = 0, g->granted = 0, g->queued = 1;
            if (mv_nq < 64) mv_queue[mv_nq++] = g;
            mv_dequeue(t);
            mv_take(t, u);
            mv.stolen++;
            break;
        }
        if (s1 - t0 > 250000000) {   /* (HOS_VCORE_ESCAPE_EQ_US: runs without a core) */
            mv_dequeue(t);
            mv.escapes++;
            break;
        }
    }
    const int64_t w = ns_now() - t0;
    mv.wait_ns += w;
    if (w > mv.wait_max) mv.wait_max = w;
    ReleaseSRWLockExclusive(&mv_lock);
}
static void mv_release(Vt *t) {
    if (!t->held) return;
    AcquireSRWLockExclusive(&mv_lock);
    const int c = t->held - 1;
    mv_owner[c] = NULL;
    t->held = 0;
    t->kick = 0;
    Vt *best = NULL;
    for (int i = 0; i < mv_nq; i++)
        if (!best || mv_queue[i]->since < best->since) best = mv_queue[i];
    if (best) {
        mv_dequeue(best);
        mv_take(best, c);
        best->granted = ns_now();
        WakeConditionVariable(&best->cv);
    }
    ReleaseSRWLockExclusive(&mv_lock);
}
static unsigned rnd(unsigned *s) { return *s = *s * 1103515245u + 12345u, *s >> 8; }
static void *mv_worker(void *p) {
    Vt *t = p;
    set_policy(mv_policy);
    int64_t last = ns_now();
    while (ns_now() < mv_end) {
        mv_acquire(t);
        /* a piece of work, 0.2 to 3 ms, in 50 us steps; a holder asked to yield gives its core up between them */
        const int64_t w0 = ns_now(), want = 200000 + (int64_t)(rnd(&t->seed) % 2800000);
        while (ns_now() - w0 < want && ns_now() < mv_end) {
            const int64_t c0 = ns_now();
            while (ns_now() - c0 < 50000) {}
            t->work_ns += 50000;
            const int64_t now = ns_now();
            if (now - last > t->gap_max) t->gap_max = now - last;
            last = now;
            if (t->kick && t->held) {
                mv_release(t);
                mv_acquire(t);
            }
        }
        mv_release(t);
        /* blocked a moment, as a guest thread waiting for its next job */
        const DWORD ms = rnd(&t->seed) % 3;
        if (ms) {
            uint32_t word = 0, same = 0;
            WaitOnAddress(&word, &same, sizeof word, ms);
        }
        const int64_t now = ns_now();
        if (now - last > t->gap_max) t->gap_max = now - last;
        last = now;
    }
    return NULL;
}
static void *mv_spinner(void *p) {
    (void)p;
    set_policy(mv_policy);
    while (ns_now() < mv_end) {}
    return NULL;
}
static void vcores_round(const char *name, int policy, unsigned threads, unsigned cores, unsigned spinners, unsigned seconds) {
    if (threads > 64) threads = 64;
    if (cores > 8) cores = 8;
    if (spinners > 32) spinners = 32;
    static Vt vt[64];
    memset(vt, 0, sizeof vt);
    memset(&mv, 0, sizeof mv);
    memset(mv_owner, 0, sizeof mv_owner);
    mv_nq = 0;
    mv_cores = (int)cores;
    mv_policy = policy;
    pthread_t th[96];
    unsigned n = 0;
    char id[48];
    snprintf(id, sizeof id, "sched.vcores.%s", name);
    probe_progress("%s", id);
    mv_end = ns_now() + (int64_t)seconds * 1000000000LL;
    for (unsigned i = 0; i < spinners; i++)
        if (!pthread_create(&th[n], NULL, mv_spinner, NULL)) n++;
    for (unsigned i = 0; i < threads; i++) {
        vt[i].seed = 0x9e3779b9u * (i + 1);
        if (!pthread_create(&th[n], NULL, mv_worker, &vt[i])) n++;
    }
    for (unsigned i = 0; i < n; i++) pthread_join(th[i], NULL);
    int64_t work = 0, gap = 0;
    for (unsigned i = 0; i < threads; i++) {
        work += vt[i].work_ns;
        if (vt[i].gap_max > gap) gap = vt[i].gap_max;
    }
    char pol[32];
    {   /* (the policy the round's threads were on, as one of them would read it) */
        int p0 = -1, pr = 0;
        hos_ps5_sched_of((void *)pthread_self(), &p0, &pr);
        if (policy) snprintf(pol, sizeof pol, "%s", hos_ps5_policy_name(policy));
        else snprintf(pol, sizeof pol, "%s %d (the console's)", hos_ps5_policy_name(p0), pr);
    }
    const double could = (double)cores * seconds;
    probe_info(id, "%u threads on %u cores beside %u spinners, %s, %u s: %lld takes (%.0f a second), %lld waits for a core (mean %.2f ms, "
                   "the longest %.1f ms), 1 ms steps %lld (the longest %.1f ms), granted cores taken up after up to %.1f ms, %lld stolen, "
                   "%lld kicks, %lld escapes; work %.2f s of the cores' %.0f (%.0f%%); the longest a thread went without progress %.1f ms",
               threads, cores, spinners, pol, seconds, (long long)mv.takes, mv.takes / (double)seconds, (long long)mv.waits,
               mv.waits ? mv.wait_ns / 1e6 / mv.waits : 0.0, mv.wait_max / 1e6, (long long)mv.steps, mv.step_max / 1e6, mv.claim_max / 1e6,
               (long long)mv.stolen, (long long)mv.kicks, (long long)mv.escapes, work / 1e9, could, 100.0 * work / 1e9 / could, gap / 1e6);
}

void check_sched(unsigned threads, unsigned cores, unsigned spinners, unsigned seconds) {
    probe_check_begin("sched", 30 + 7 * seconds);
    /* 1. what a thread starts with */
    {
        int pol = -1, prio = 0;
        const int r = hos_ps5_sched_of((void *)pthread_self(), &pol, &prio);
        uint64_t which = 0, root = 0;
        int we = 0, re = 0;
        hos_ps5_process_cpuset(&which, &we, &root, &re);
        uint64_t title = 0;
        const int ncpu = hos_ps5_cpus(&title);
        char note[400];
        hos_ps5_sched_note(note, sizeof note);
        probe_info("sched.policy", "the main thread: %s %d (%d), CPUs %#llx, on CPU %d; the title's CPUs %#llx (%d); the process's set %#llx (%d), "
                                   "root %#llx (%d); %s",
                   hos_ps5_policy_name(pol), prio, r, (unsigned long long)hos_ps5_affinity_of((void *)pthread_self()), hos_ps5_current_cpu(),
                   (unsigned long long)title, ncpu, (unsigned long long)which, we, (unsigned long long)root, re, note);
        const int64_t c0 = hos_ps5_thread_cpu_ns((void *)pthread_self()), p0 = hos_ps5_process_cpu_ns(), t0 = ns_now();
        while (ns_now() - t0 < 20000000) {}
        const int64_t c1 = hos_ps5_thread_cpu_ns((void *)pthread_self()), p1 = hos_ps5_process_cpu_ns();
        const double counted = c0 >= 0 && c1 >= 0 ? (c1 - c0) / 1e6 : -1, process = p0 >= 0 && p1 >= 0 ? (p1 - p0) / 1e6 : -1;
        probe_result(counted > 15 && counted < 30, "sched.clock", "20.0 ms spun: the thread's CPU clock counted %.1f ms (%lld), the process's %.1f ms "
                                                                  "(%lld): the game's scheduling report %s",
                     counted, (long long)(c0 < 0 ? c0 : c1 < 0 ? c1 : 0), process, (long long)(p0 < 0 ? p0 : p1 < 0 ? p1 : 0),
                     counted > 15 && counted < 30 ? "can say which threads use the CPUs" : "cannot count a thread's CPU time");
        uint64_t m = title;
        int cpu = 63;
        while (cpu > 0 && !(m >> cpu & 1)) cpu--;
        /* 2. one CPU, two threads of one priority: the console's policy, rr, other */
        const double s0 = slice_round(0, "console", cpu), s1 = slice_round(SCHED_RR, "rr", cpu), s2 = slice_round(SCHED_OTHER, "other", cpu);
        char a[64], b[64], c[64];
        snprintf(a, sizeof a, s0 < 0 ? "none" : "%.1f ms", s0);
        snprintf(b, sizeof b, s1 < 0 ? "none" : "%.1f ms", s1);
        snprintf(c, sizeof c, s2 < 0 ? "none" : "%.1f ms", s2);
        probe_verdict("sched.slice", "a second thread of one priority on a busy CPU ran after: %s on the console's policy (%s), %s on rr, %s on "
                                     "other: %s",
                      a, hos_ps5_policy_name(pol), b, c,
                      s0 < 0 && s1 >= 0 ? "threads of one priority are not time-sliced as they start, and rr slices them: HOOKSHOT_PS5_SCHED=auto "
                                          "(FIFO made RR) is the fix" :
                      s0 < 0 && s2 >= 0 ? "not time-sliced as they start, rr does not help, other does: HOOKSHOT_PS5_SCHED=other" :
                      s0 >= 0 ? "threads are time-sliced as they start: the game's crawl is not the missing slice" :
                                "no policy slices them here: the virtual cores must not count on the kernel");
        if (cores > 8) cores = 8;
        const unsigned s = spinners ? spinners : (unsigned)(ncpu > 2 ? ncpu - 2 : 1);
        /* 3. the virtual cores' pattern: alone, then beside threads that never yield, on each policy */
        vcores_round("alone", 0, threads, cores, 0, seconds);
        vcores_round("busy", 0, threads, cores, s, seconds);
        vcores_round("busy-rr", SCHED_RR, threads, cores, s, seconds);
        vcores_round("busy-other", SCHED_OTHER, threads, cores, s, seconds);
    }
    probe_check_end();
}
