/* (ps5, lot 62) tests/test_tsc_clock.py's program: the console's QueryPerformanceCounter as host/posix/windows.h has it (cut
   from the source into qpc_cut.inc) and host/posix/tsc_clock.h's start, on this PC's TSC and CLOCK_MONOTONIC. Every
   clock_gettime of the program is counted (its own definition is the one its calls bind to). Prints PASS or exits 1. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#define HOS_TSC_CLOCK_START
#include "tsc_clock.h"

typedef int BOOL;
typedef union { struct { uint32_t LowPart; int32_t HighPart; } u; int64_t QuadPart; } LARGE_INTEGER;
#define TRUE 1

static volatile long system_reads;
int clock_gettime(clockid_t id, struct timespec *ts) {
    __atomic_add_fetch(&system_reads, 1, __ATOMIC_RELAXED);
    return (int)syscall(SYS_clock_gettime, id, ts);
}
static int64_t mono(void) {   /* (CLOCK_MONOTONIC, not counted) */
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#include "qpc_cut.inc"
volatile int hos_tsc_qpc;
HosTscClock hos_tsc_clk;

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int64_t qpc(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts)) {}
}

enum { THREADS = 4, READS = 200000 };
static long back_steps[THREADS];
static void *reader(void *arg) {
    const long k = (long)(intptr_t)arg;
    int64_t last = qpc();
    for (int i = 0; i < READS; i++) {
        const int64_t now = qpc();
        if (now < last) back_steps[k]++;
        last = now;
    }
    return NULL;
}

int main(void) {
    char line[400];
    HosTscClock clk;
    HosTscPair early;

    /* the pure parts: the clamp, the rate's refusals and its error bound */
    {
        const HosTscClock c = {1000, 5000, (uint64_t)1 << 32};   /* (1 ns a tick from 1000 ns at tick 5000) */
        CHECK(hos_tsc_clock_ns(&c, 6000) == 2000, "a tick past the start: %lld", (long long)hos_tsc_clock_ns(&c, 6000));
        CHECK(hos_tsc_clock_ns(&c, 4000) == 1000, "a TSC behind the start reads %lld, not the start (1000)",
              (long long)hos_tsc_clock_ns(&c, 4000));
        CHECK(hos_tsc_clock_ns(&c, 0) == 1000, "a TSC far behind the start reads %lld", (long long)hos_tsc_clock_ns(&c, 0));
        const HosTscPair a = {1000, 1100, 1000000}, b = {1000001000, 1000001100, 501000000};   /* (1e9 ticks in 0.5 s) */
        double ppm = -1;
        CHECK(hos_tsc_rate(&a, &b, &ppm) == 2000000000ull, "2e9 ticks a second read as %llu", (unsigned long long)hos_tsc_rate(&a, &b, NULL));
        CHECK(ppm > 0.099 && ppm < 0.101, "the error bound of 100-tick pairs over 1e9 ticks: %g ppm, not 0.1", ppm);
        CHECK(!hos_tsc_rate(&b, &a, NULL), "a rate from a pair to an earlier one");
        const HosTscPair slow = {1000001000, 1000001100, 1000000 + 100000000000LL};   /* (0.01 GHz) */
        CHECK(!hos_tsc_rate(&a, &slow, NULL), "a rate of 10 MHz taken");
    }

    /* off: the system clock, one read each, as before; the start says so and touches nothing */
    hos_tsc_pair(&early, 8);
    memset(&clk, 0x5a, sizeof clk);
    const HosTscClock untouched = clk;
    CHECK(!hos_tsc_clock_start(0, &early, 100000000, &clk, line, sizeof line), "the start with the switch off says on");
    CHECK(strstr(line, "reads CLOCK_MONOTONIC") != NULL, "the line off: %s", line);
    CHECK(!memcmp(&clk, &untouched, sizeof clk), "the start with the switch off wrote the clock");
    {
        const long r0 = system_reads;
        int outside = 0;
        for (int i = 0; i < 1000; i++) {
            const int64_t lo = mono(), v = qpc(), hi = mono();
            outside += v < lo || v > hi;
        }
        CHECK(system_reads - r0 == 1000, "off: %ld system clock reads for 1000 QueryPerformanceCounter", system_reads - r0);
        CHECK(!outside, "off: %d of 1000 reads outside the system clock's around them", outside);
    }

    /* on: measured over 100 ms at least (slept up to it from a pair taken just before), started at the system clock */
    hos_tsc_pair(&early, 8);
    const int64_t s0 = mono();
    const int on = hos_tsc_clock_start(1, &early, 100000000, &clk, line, sizeof line);
    const int64_t took = mono() - s0;
    printf("%s\n", line);
    CHECK(on, "the start with the switch on: %s", line);
    if (!on) return 1;
    CHECK(took >= 100000000, "the start measured over %.1f ms, not 100 at least", took / 1e6);
    const char *over = strstr(line, "over ");
    CHECK(over && atof(over + 5) >= 100.0, "the line's measure: %s", line);

    /* the switch: no step back, no jump */
    const int64_t before = qpc();
    hos_tsc_clk = clk;
    __atomic_store_n(&hos_tsc_qpc, 1, __ATOMIC_RELEASE);
    const int64_t after = qpc();
    CHECK(after >= before, "the first read on the TSC (%lld) is before the last on the system clock (%lld)", (long long)after, (long long)before);
    CHECK(after - before < 1000000, "the switch jumped %lld ns", (long long)(after - before));

    /* on: no system clock read; the rate is the system clock's over 300 ms; the two still together after */
    {
        const long r0 = system_reads;
        int64_t sink = 0;
        const uint64_t t0 = __builtin_ia32_rdtsc();
        for (int i = 0; i < 100000; i++) sink += qpc();
        const uint64_t t1 = __builtin_ia32_rdtsc();
        CHECK(system_reads == r0, "on: %ld system clock reads for 100000 QueryPerformanceCounter", system_reads - r0);
        printf("on: %.1f TSC ticks a read (%lld)\n", (double)(t1 - t0) / 100000.0, (long long)(sink & 1));
        const int64_t m1 = mono(), q1 = qpc();
        sleep_ms(300);
        const int64_t m2 = mono(), q2 = qpc();
        const double ratio = (double)(q2 - q1) / (double)(m2 - m1);
        printf("on: %.6f ns of QueryPerformanceCounter a ns of CLOCK_MONOTONIC over %.1f ms; the two %lld ns apart after\n",
               ratio, (m2 - m1) / 1e6, (long long)(q2 - m2));
        CHECK(ratio > 1 - 5e-4 && ratio < 1 + 5e-4, "on: the rate is %.6f of the system clock's", ratio);
        CHECK(q2 - m2 > -1000000 && q2 - m2 < 1000000, "on: %lld ns from the system clock 0.3 s after the start", (long long)(q2 - m2));
    }

    /* on: each thread's reads never go back */
    {
        pthread_t th[THREADS];
        for (long k = 0; k < THREADS; k++) pthread_create(&th[k], NULL, reader, (void *)(intptr_t)k);
        long back = 0;
        for (long k = 0; k < THREADS; k++) pthread_join(th[k], NULL), back += back_steps[k];
        CHECK(!back, "on: %ld reads before the same thread's last, in %d threads of %d reads", back, THREADS, READS);
    }

    if (failures) return 1;
    printf("PASS\n");
    return 0;
}
