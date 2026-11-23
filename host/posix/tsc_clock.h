/* (ps5, lot 62) HOS_TSC_CLOCK=1: QueryPerformanceCounter (host/posix/windows.h) on the processor's time stamp counter, not
   clock_gettime. Ported from Mihawk's 6572e0c0 in heydemoura/PS5_Longshot (GPL-3.0), with the differences said below.

   On the console a clock_gettime is a system call, 0.886 us a read (the witness of HOOKSHOT_PS5_THREAD_TIMES, "cpu:
   calibration", logs/udp/essai-lot57-3.txt; gpu/vk/cheap_clock.h). QueryPerformanceCounter is the clock of hos_now_ns (the
   host's waits and spins before them, the sleeps, the frame pacing, the audio mixer's period, the FPS counter) and of the
   game's own: nn::os::GetSystemTick and every CNTPCT_EL0 read of the lifted code (host/mkw_glue.c a64_cntpct_host), about
   1,200 reads a frame from the game's job threads in the crowded market (lots 54 and 55s's samples: half as many as
   RX_VK_SKIP_FILE's one a draw). The renderer's own reads are lot 59's (gpu/vk/cheap_clock.h).

   Not Mihawk's: the rate is not sceKernelGetTscFrequency's (an import more) but measured against CLOCK_MONOTONIC from the
   eboot's first code to the settings (~0.2 s), each reading of that clock between two of the TSC: the rate the system
   clock keeps, which the display and the audio output keep too, to a few parts per million. The switch is read once,
   when the settings are loaded (host/hos_main.c, stage 4): before that, and off, QueryPerformanceCounter is
   clock_gettime(CLOCK_MONOTONIC) as before. A TSC read behind the start's (another core's, by a few cycles) is the start,
   not 2^64 ticks later.

   What stays on CLOCK_MONOTONIC: the waits' own deadlines (host/posix/win32_posix.c: the futexes and condition variables
   are handed a relative time, the timer and the monitor waits a deadline taken on CLOCK_MONOTONIC there; none is handed a
   QueryPerformanceCounter value: the host's deadlines on hos_now_ns are turned into what is left before each wait),
   GetTickCount64, the trail's and the log's clock (hos_ps5_since_start), the renderer's std::chrono::steady_clock and
   RADV's os_time_get_nano. */
#ifndef LONGSHOT_TSC_CLOCK_H
#define LONGSHOT_TSC_CLOCK_H
#include <stdint.h>

/* the clock: ns0 at the TSC's tsc0, mult / 2^32 ns a tick */
typedef struct { int64_t ns0; uint64_t tsc0, mult; } HosTscClock;
/* a reading of CLOCK_MONOTONIC (ns) between two of the TSC */
typedef struct { uint64_t tsc_before, tsc_after; int64_t ns; } HosTscPair;

/* the clock's ns at the TSC reading t: never before the start (a TSC behind tsc0 reads as tsc0) */
static inline int64_t hos_tsc_clock_ns(const HosTscClock *c, uint64_t t) {
    const int64_t d = (int64_t)(t - c->tsc0);
    return c->ns0 + (int64_t)(((unsigned __int128)(uint64_t)(d > 0 ? d : 0) * c->mult) >> 32);
}

#endif

#if defined(HOS_TSC_CLOCK_START) && !defined(LONGSHOT_TSC_CLOCK_START_H)   /* (the start: host/posix/ps5_platform.c, the test) */
#define LONGSHOT_TSC_CLOCK_START_H
#include <errno.h>
#include <stdio.h>
#include <time.h>

/* a pair, the one of tries readings with the least time between its two TSC reads (a reading preempted in its system call
   says nothing of when the clock was read) */
static inline void hos_tsc_pair(HosTscPair *p, int tries) {
    for (int i = 0; i < tries || i == 0; i++) {
        struct timespec ts;
        const uint64_t before = __builtin_ia32_rdtsc();
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t after = __builtin_ia32_rdtsc();
        if (i == 0 || after - before < p->tsc_after - p->tsc_before)
            p->tsc_before = before, p->tsc_after = after, p->ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }
}

/* the TSC's rate in Hz from a to b, each pair's TSC the middle of its two reads, and how far off it can be (*ppm: the two
   pairs' half widths over the ticks between them); 0 when they say none: b not after a, or a rate outside 0.1-10 GHz */
static inline uint64_t hos_tsc_rate(const HosTscPair *a, const HosTscPair *b, double *ppm) {
    const uint64_t ma = a->tsc_before + (a->tsc_after - a->tsc_before) / 2, mb = b->tsc_before + (b->tsc_after - b->tsc_before) / 2;
    const int64_t dns = b->ns - a->ns;
    if (dns <= 0 || mb <= ma) return 0;
    const double ticks = (double)(mb - ma), hz = ticks * 1e9 / (double)dns;
    if (ppm) *ppm = ((double)(a->tsc_after - a->tsc_before) + (double)(b->tsc_after - b->tsc_before)) / 2.0 / ticks * 1e6;
    return hz >= 1e8 && hz <= 1e10 ? (uint64_t)(hz + 0.5) : 0;
}

/* The start. on: HOS_TSC_CLOCK by env_on's rule. early: the pair taken at the eboot's first code (ns 0: none, taken here).
   The rate is measured over min_ns at least (slept up to it), then the clock starts at a pair taken now, at its first TSC
   read: it never reads before CLOCK_MONOTONIC did. 1 with *clk set when QueryPerformanceCounter is to read it; line: what
   was done, for the log */
static inline int hos_tsc_clock_start(int on, const HosTscPair *early, int64_t min_ns, HosTscClock *clk, char *line, size_t size) {
    if (!on) {
        snprintf(line, size, "clock: QueryPerformanceCounter reads CLOCK_MONOTONIC, a system call (HOS_TSC_CLOCK=1: the time "
                             "stamp counter)");
        return 0;
    }
    HosTscPair a = *early, b;
    if (!a.ns) hos_tsc_pair(&a, 8);
    hos_tsc_pair(&b, 8);
    if (b.ns - a.ns < min_ns) {
        const int64_t rest = min_ns - (b.ns - a.ns);
        struct timespec ts = {(time_t)(rest / 1000000000LL), (long)(rest % 1000000000LL)};
        while (nanosleep(&ts, &ts) && errno == EINTR) {}
        hos_tsc_pair(&b, 8);
    }
    double ppm = 0;
    const uint64_t hz = hos_tsc_rate(&a, &b, &ppm);
    if (!hz) {
        snprintf(line, size, "clock: HOS_TSC_CLOCK=1 but the time stamp counter's rate could not be measured (%llu ticks in "
                             "%lld ns): QueryPerformanceCounter reads CLOCK_MONOTONIC, a system call",
                 (unsigned long long)(b.tsc_before - a.tsc_before), (long long)(b.ns - a.ns));
        return 0;
    }
    HosTscPair s;
    hos_tsc_pair(&s, 8);
    clk->ns0 = s.ns, clk->tsc0 = s.tsc_before, clk->mult = (uint64_t)(((unsigned __int128)1000000000u << 32) / hz);
    snprintf(line, size, "clock: HOS_TSC_CLOCK=1: QueryPerformanceCounter (hos_now_ns, the game's GetSystemTick and CNTPCT) reads "
                         "the time stamp counter from here: %.6f GHz, measured against CLOCK_MONOTONIC over %.1f ms (to %.2f ppm; "
                         "the pairs' widths %llu and %llu ticks), started at its %lld ns",
             hz / 1e9, (b.ns - a.ns) / 1e6, ppm, (unsigned long long)(a.tsc_after - a.tsc_before),
             (unsigned long long)(b.tsc_after - b.tsc_before), (long long)s.ns);
    return 1;
}
#endif
