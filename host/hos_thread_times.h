/* (ps5, 2026-11-16, lot 40) The per-thread CPU report, HOOKSHOT_PS5_THREAD_TIMES=<seconds> (host/hos_thread_times.c).

   The scheduling report's per-thread figures are wrong on the console: pthread_getcpuclockid gives every thread the calling
   thread's clock (logs/udp/essai-lot34-part47-1.txt: every thread "1.13 s in all", the report thread's own), and
   CLOCK_PROCESS_CPUTIME_ID reads 1.00 CPU always. This report tries five readings against a witness thread that burns a
   known time and sleeps a known time, keeps those that read it right, and says which one each report used.

   Two of the readings are a thread's own (its CLOCK_THREAD_CPUTIME_ID, its getrusage(RUSAGE_THREAD)): the threads publish
   them into a slot of their own at their waits (HOS_CPU_MARK: win32_posix.c's futex waits, Sleep and monitor waits, the
   virtual cores' away and back, each present, the renderer's thread loops), at most every 50 ms each, with no lock, no
   signal and no log line. Off (the setting unset or 0), hos_cpu_marks stays 0 and a mark is one load and a branch. */
#ifndef HOS_THREAD_TIMES_H
#define HOS_THREAD_TIMES_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* A thread's own CPU time as it last read it. Written by that thread only (a sequence count: odd while it writes), read by
   the report's thread with hos_cpu_slot_read. Zero (calloc) is "never read". */
struct hos_cpu_slot {
    volatile uint32_t seq;
    int32_t blocked;     /* 1: its last mark was on its way into a wait */
    int64_t cpu_ns;      /* its CPU time at its last read */
    uint64_t read_tick;  /* when it read it (the TSC, or hos_now_ns where there is none) */
    uint32_t reads;      /* how many times it read it */
    uint32_t spare;
};
struct hos_cpu_sample {
    int64_t cpu_ns;
    uint64_t read_tick;
    uint32_t reads;
    int blocked, torn;   /* torn: the writer was busy on every try (the values may mix two reads) */
};
/* 0 (the default): no mark reads anything. Set once, by the report's thread, when the calibration kept a thread's own reading */
extern volatile int hos_cpu_marks;
void hos_cpu_slot_mark(struct hos_cpu_slot *slot, int blocking);   /* the calling thread's own slot only */
void hos_cpu_slot_read(const struct hos_cpu_slot *slot, struct hos_cpu_sample *out);

/* win32_posix.c's threads (CreateThread's and the adopted ones) with their slots, for the report: the live ones, up to max */
typedef struct {
    int tid;
    void *pthread;
    char name[64];
    struct hos_cpu_sample cpu;
} hos_thread_time_row;
int hos_posix_thread_times(hos_thread_time_row *out, int max);

/* the calling thread's mark (win32_posix.c: its HThread's slot; nothing for a thread win32_posix.c does not know). errno kept */
void hos_ps5_cpu_mark(int blocking);
/* HOOKSHOT_PS5_THREAD_TIMES=<seconds> (2 to 600): the calibration, then a report every that many seconds, on a thread of its
   own. Unset, empty or 0: returns at once, nothing made, nothing read. HOOKSHOT_PS5_THREAD_TIMES_SKIP=<names>: the readings
   never tried (own-clock, own-rusage, pthread-clock, tid-clock, kern-proc) */
void hos_thread_times_start(void);

/* the readings (host/hos_thread_times.c's; tests/thread_times_test.c gives it wrong ones under HOOKSHOT_THREAD_TIMES_TEST, to
   see them refused). Each answers nanoseconds, or -errno; kproc 0 or an errno, the size alone when buf is NULL */
typedef struct {
    int64_t (*own_clock)(void);
    int64_t (*own_rusage)(void);
    int64_t (*pthread_clock)(void *pthread);
    int64_t (*tid_clock)(int tid);
    int (*kproc)(void *buf, size_t *len);
    int64_t (*process)(void);
} hos_thread_times_ops_t;
#ifdef HOOKSHOT_THREAD_TIMES_TEST
extern hos_thread_times_ops_t hos_thread_times_test_ops;
#endif

#ifdef __PROSPERO__
#define HOS_CPU_MARK(blocking) do { if (__builtin_expect(hos_cpu_marks != 0, 0)) hos_ps5_cpu_mark(blocking); } while (0)
#else
#define HOS_CPU_MARK(blocking) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
#endif
