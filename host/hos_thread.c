/* Guest threads: one host thread each, with its own Ctx, TLS block and thread-local region.

   TPIDR_EL0 (TLS): a 4 KB block per thread, TP in its middle. TOTK's main reads its one thread-local at TP+0x10
   (local-exec, after AArch64's 16-byte TCB); the area below TP is zeroed room for whatever the SDK's musl keeps
   there. TPIDRRO_EL0 (thread-local region): 0x200 bytes; the SDK's nn::os keeps the current ThreadType* at
   +0x1F8, so lifted SDK code that reads it inline finds the same thread the HLE knows. */
#include "hos.h"
#include "hos_crash_record.h"
#include "hos_report_prefixes.h"
#ifndef LONGSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/longshot_env.h"
#endif
#include "hos_thread_times.h"   /* (ps5: HOS_CPU_MARK and the per-thread CPU report; nothing elsewhere) */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION   /* (older Windows SDKs lack it: Windows 10 1803 and later take it) */
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#include <emmintrin.h>
#ifdef __PROSPERO__
#include "ps5.h"   /* (host/posix/ps5.h: the console's scheduling calls, the stage trail) */
#include <sys/resource.h>
#include <strings.h>
#endif
#ifdef __EMSCRIPTEN__
#include <sched.h>
#include <stdatomic.h>
#include <unistd.h>
#endif

struct HosThread {
    Ctx ctx;
    uint64_t guest_tt, entry, arg, stack, stack_size, id;
    uint64_t tls_block, tlr;
    int priority, ideal_core;
    uint64_t core_mask;
    uint64_t tls_slots[HOS_TLS_SLOTS];
    char name[48];
    HANDLE handle;
    int started;
    int serial;   /* one of the frame's serial threads (hos_thread_serial), set with the name */
    /* the kernel side (hos_svc.c): this thread's Horizon handle and the event it blocks on */
    uint32_t kernel_handle;
    HANDLE wake;
    /* virtual cores (HOS_VCORES): the core held + 1 (0: none), the last one, a better waiter's request, queued, and
       whether it has run without one since an escape (vc_acquire; until its next block, hos_vcore_away, or the next
       hos_vcore_back); vc_starved: waiting longer than HOS_VCORE_STARVE_US (it goes first at the next release) */
    int vc_held, vc_last, vc_queued, vc_escaped, vc_starved;
    volatile int vc_kick;
    int64_t vc_since;
    CONDITION_VARIABLE vc_cv;
    /* a core vc_grant gave it that it has not taken up yet: when (0: none); and for the scheduling report (ps5): takes,
       the time spent waiting for a core, the CPU it ran on at its last sampled take */
    int64_t vc_granted;
    unsigned vc_ntakes;
    int vc_cpu;
    int64_t vc_wait_ns;
    /* (longshot) fibers: the context running now (NULL: ctx, the thread's own), the thread's host fiber once it
       switched to a guest fiber */
    Ctx *volatile run_ctx;
    void *host_fiber;
};

/* The SDK's own mutexes (InternalCriticalSection) tag a lock word with the owner's handle, read from
   ThreadType+0x1B0 through TLR+0x1F8 (sdk+0x13186c). Every thread gets a unique handle there. */
#define TT_HANDLE 0x1B0
static volatile LONG next_handle = 0x10000;
uint32_t hos_thread_handle(HosThread *t) { return t->kernel_handle; }

/* nnosInitialize gives the main thread the SDK's own ThreadType (it writes TLR+0x1F8): adopt it, so the HLE and the
   SDK agree on which object is the current thread. */
void hos_thread_adopt_guest(HosThread *t) {
    uint64_t tt = G64(t->tlr + 0x1F8);
    if (tt && tt != t->guest_tt) {
        t->guest_tt = tt;
        G32(tt + TT_HANDLE) = t->kernel_handle;
    }
}
int hos_thread_prio(HosThread *t) { return t->priority; }

int hos_trace;
extern const uint64_t hos_thread_start_hook, hos_thread_exit_hook;   /* gen/hos_image.c */
static _Thread_local HosThread *cur;
static SRWLOCK threads_lock = SRWLOCK_INIT;
static HosThread *threads[1024];
static int nthreads;
static volatile LONG64 next_id = 0x50;

HosThread *hos_self(void) { return cur; }
Ctx *hos_ctx(void) { return cur ? (cur->run_ctx ? cur->run_ctx : &cur->ctx) : NULL; }
uint64_t hos_thread_guest(HosThread *t) { return t->guest_tt; }
uint64_t hos_thread_id(HosThread *t) { return t->id; }
const char *hos_thread_name(HosThread *t) { return t->name; }
uint64_t *hos_tls_slots(HosThread *t) { return t->tls_slots; }
/* The core a thread runs on. Most of TOTK's threads have no ideal core (-2, "use default") and a one-core affinity
   mask, so the mask decides; the engine keys per-core resources (its command memory arenas, for one) on
   GetCurrentCoreNumber, so threads pinned to different cores must report different ones. */
/* HOS_PIN_BY_CORE=1 (experiment): every game thread runs on the host's logical core 2 x its core number (one per
   physical core on SMT machines), so threads the game puts on the same core never run at the same time, as on the
   Switch, where TOTK keeps per-core data without locks (both MacNativeGames and totk-pc-recomp found it relies on
   that); they are still time-sliced at any instruction, not scheduled by priority as Horizon does. */
static void pin_by_core(HosThread *t) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    static int on = -1;
    if (on < 0) on = env_on("HOS_PIN_BY_CORE");
    if (on && t->handle) SetThreadAffinityMask(t->handle, (DWORD_PTR)1 << (2 * hos_thread_core(t)));
#else
    (void)t;
#endif
}
void hos_thread_affinity(HosThread *t, int ideal, uint64_t mask) {
    if (ideal >= 0) t->ideal_core = ideal;
    if (mask) t->core_mask = mask;
    pin_by_core(t);
}
uint64_t hos_thread_mask(HosThread *t) { return t->core_mask; }
void hos_thread_boost(int level) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    static int off = -1;
    if (off < 0) off = env_on("HOS_NO_PRIORITIES");
    if (!off) SetThreadPriority(GetCurrentThread(), level >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
#else
    (void)level;
#endif
}

int hos_cores(void) {
    static int n;
    if (!n) {
        const char *e = getenv("HOS_CORES");
        n = e && atoi(e) >= 1 && atoi(e) <= 8 ? atoi(e) : 6;   /* (mkw-recomp: a Switch 2 application's six) */
        if (!e && hos_game_cores()) n = hos_game_cores();   /* (totk_cores.c: the game set up for more cores) */
    }
    return n;
}
int hos_thread_core(HosThread *t) {
    if (!t) return 0;
    if (t->vc_held) return t->vc_held - 1;   /* (virtual cores: the one it holds) */
    const int n = hos_cores();
    if (t->ideal_core >= 0) return t->ideal_core % n;
    /* the mask's lowest core, wrapped (a thread pinned past the cores there are shares a core number: the extra
       BaseProc executors, totk_cores.c) */
    return t->core_mask ? (int)(__builtin_ctzll(t->core_mask) % (unsigned)n) : 0;
}
void hos_thread_stack(HosThread *t, uint64_t *addr, uint64_t *size) { *addr = t->stack; *size = t->stack_size; }

int hos_thread_priority(HosThread *t, int set, int value) {
    int old = t->priority;
    if (set) t->priority = value;
    return old;
}

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
static SRWLOCK log_lock = SRWLOCK_INIT, log_write_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE log_cv = CONDITION_VARIABLE_INIT;
static char *log_q[2048];
static int log_head, log_count;
static volatile LONG log_async = -1;
static volatile DWORD log_writer;   /* log_main's thread */

static int log_take_locked(char **out) {
    int n = 0;
    while (log_count) {
        out[n++] = log_q[log_head];
        log_head = (log_head + 1) % 2048;
        log_count--;
    }
    return n;
}

/* (longshot) A batch goes out in one write. stderr is unbuffered (hos_main.c), so each line was a write of its own: 30-90 a
   second in play, every one extending the log file. With the game on a slow disk (a SATA SSD, 90% full) those writes kept
   the disk busy: its 4 KB reads took 10-200 ms where they take 0.4 ms, and what the game read from it meanwhile (a page of
   its own code, a file of the game's) waited as long: the long frames of a session. */
static void log_write(char **lines, int n) {
    size_t total = 0;
    for (int i = 0; i < n; i++) total += strlen(lines[i]);
    char *all = n > 1 ? (char *)malloc(total + 1) : NULL;
    if (all) {
        size_t at = 0;
        for (int i = 0; i < n; i++) {
            const size_t len = strlen(lines[i]);
            memcpy(all + at, lines[i], len);
            at += len;
            free(lines[i]);
        }
        fwrite(all, 1, total, stderr);
        free(all);
    } else {   /* (one line, or no memory for the batch: line by line) */
        for (int i = 0; i < n; i++) {
            fputs(lines[i], stderr);
            free(lines[i]);
        }
    }
    fflush(stderr);
}

/* (longshot) and the writer waits a little after the line it is woken for, so the lines of that time go out together: a few
   writes a second however many lines there are. A queue half full goes out at once, before a burst can fill it (a full
   queue has the thread that logs write the line itself). HOS_LOG_BATCH_MS=<ms> (default 250, at most 2000; 0: the lines
   there are at each wake, as before). Nothing waiting is lost: every way out writes the queue first (log_flush,
   log_crash_filter), and the writer holds no lock while it waits */
enum { LOG_BATCH_FULL = 1024 };
static DWORD log_batch_ms(void) {
    static int ms = -1;
    if (ms < 0) {
        const char *e = getenv("HOS_LOG_BATCH_MS");
        const int v = e && *e ? atoi(e) : 250;
        ms = v < 0 ? 0 : v > 2000 ? 2000 : v;
    }
    return (DWORD)ms;
}

static DWORD WINAPI log_main(void *p) {
    (void)p;
    static char *batch[2048];
    log_writer = GetCurrentThreadId();
    for (;;) {
        AcquireSRWLockExclusive(&log_lock);
        while (!log_count) SleepConditionVariableSRW(&log_cv, &log_lock, INFINITE, 0);
        /* (log_queue wakes it at each line: until the time is up or the queue half full) */
        const ULONGLONG until = GetTickCount64() + log_batch_ms();
        for (ULONGLONG now; log_count < LOG_BATCH_FULL && (now = GetTickCount64()) < until;)
            SleepConditionVariableSRW(&log_cv, &log_lock, (DWORD)(until - now), 0);
        const int n = log_take_locked(batch);
        AcquireSRWLockExclusive(&log_write_lock);
        ReleaseSRWLockExclusive(&log_lock);
        log_write(batch, n);
        ReleaseSRWLockExclusive(&log_write_lock);
    }
}

/* the queued lines straight to the handle (crash paths: no CRT stream, no write lock) */
static void log_drain_raw(void) {
    if (!TryAcquireSRWLockExclusive(&log_lock)) return;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    while (log_count) {
        const char *line = log_q[log_head];
        DWORD written;
        WriteFile(err, line, (DWORD)strlen(line), &written, NULL);
        log_head = (log_head + 1) % 2048;
        log_count--;
    }
    ReleaseSRWLockExclusive(&log_lock);
}

static void log_flush(void) {
    if (log_async <= 0) return;
    /* (a crash report on the writer itself, in its own write: it holds log_write_lock) */
    if (GetCurrentThreadId() == log_writer) { log_drain_raw(); return; }
    static char *batch[2048];
    AcquireSRWLockExclusive(&log_write_lock);
    AcquireSRWLockExclusive(&log_lock);
    const int n = log_take_locked(batch);
    ReleaseSRWLockExclusive(&log_lock);
    log_write(batch, n);
    ReleaseSRWLockExclusive(&log_write_lock);
}

static LPTOP_LEVEL_EXCEPTION_FILTER log_prev_filter;

static LONG WINAPI log_crash_filter(EXCEPTION_POINTERS *e) {
    if (GetCurrentThreadId() == log_writer) log_drain_raw();
    else {
        /* the writer may be in the middle of a batch: it finishes first (at most 2 s: a write stuck on the disk), then the
           rest of the queue goes out (before: the queue was left unwritten whenever the writer was busy) */
        int got = 0;
        for (int i = 0; i < 2000 && !(got = TryAcquireSRWLockExclusive(&log_write_lock)); i++) Sleep(1);
        log_drain_raw();
        if (got) ReleaseSRWLockExclusive(&log_write_lock);
    }
    return log_prev_filter ? log_prev_filter(e) : EXCEPTION_CONTINUE_SEARCH;
}

/* Lines are written by log_main, not by the thread that logs: a write that blocks (a console; the log file while the
   disk is busy: the render thread was seen held 140-310 ms in one line's write) holds no game thread. Nothing queued is
   lost when the game ends or crashes: hos_fatal, report_crash/report_fatal (mkw_glue.c), log_crash_filter,
   svcExitProcess, the window's close and the process's exit (atexit) write the queue first. HOS_LOG_SYNC=1: each line
   written by the thread that logs it, as files and pipes were before */
static int log_queue(const char *line) {
    if (log_async < 0) {
        AcquireSRWLockExclusive(&log_lock);
        if (log_async < 0) {
            int on = !env_on("HOS_LOG_SYNC");
            if (on && !CreateThread(NULL, 0, log_main, NULL, 0, NULL)) on = 0;
            if (on) log_prev_filter = SetUnhandledExceptionFilter(log_crash_filter);
            if (on) atexit(log_flush);
            log_async = on;
        }
        ReleaseSRWLockExclusive(&log_lock);
    }
    if (!log_async) return 0;
    char *s = _strdup(line);
    if (!s) return 0;
    AcquireSRWLockExclusive(&log_lock);
    if (log_count < 2048) {
        log_q[(log_head + log_count) % 2048] = s;
        log_count++;
        ReleaseSRWLockExclusive(&log_lock);
        WakeConditionVariable(&log_cv);
        return 1;
    }
    ReleaseSRWLockExclusive(&log_lock);
    free(s);
    log_flush();
    return 0;
}
#elif defined(__PROSPERO__)
#include <pthread.h>
/* (ps5, 2026-11-10, after the game's fourth run: the process sat idle while threads waited seconds for the virtual cores'
   lock, and lines were written to the log with that lock held. A line's write is two blocking calls: hookshot_log.txt
   (unbuffered, on /data) and standard error, a pipe one thread drains into klog a line at a time (the payload SDK's
   klog.c). So here a line is queued and a thread of its own ("hookshot log") writes it; a full queue drops the line and
   counts it instead of making its thread wait. HOS_LOG_SYNC=1: every line written by its own thread, as before (after a
   crash the queue's last lines are lost; the stage trail is not queued). The longest write of each kind is kept for the
   scheduling report's "log:" line, which says whether the writes were what stalled. */
enum { PS5_LOGQ = 1024, PS5_LOGLINE = 1200, PS5_LOGBATCH = 64 };
FILE *hos_log_tee;   /* hookshot_log.txt, a copy of every line (standard error is klog's: host/hos_main.c game_main) */
static pthread_mutex_t ps5_log_mu = PTHREAD_MUTEX_INITIALIZER;   /* (never held across a write) */
static char (*ps5_lq)[PS5_LOGLINE];
static unsigned ps5_lq_head, ps5_lq_count;
static volatile uint32_t ps5_lq_word;
static volatile int ps5_log_mode = -1;   /* 0 written by the caller, 1 queued */
static int ps5_log_writing;
static struct { int64_t lines, dropped, drop_pending, high, file_max_ns, klog_max_ns; } ps5_log_st;

/* (2026-11-10, the fifth run: the game ran at 60 frames a second, and the file's writes took 131, 362, 820 ms and then
   seconds each: the file stopped at 32.7 s while the game ran on. A title's own writes under /data are held back by the
   console, which is what the storage broker is for (host/posix/ps5_broker.h). So the log thread keeps the file's lines in
   a block and has the broker write it at the file's end, whatever its size, when it is 32 KiB or 2 s old; without a
   broker the block is stdio's, as before. klog's line goes first and never waits for the file.) */
#include "ps5_broker.h"
char hos_log_tee_path[HB_MAX_PATH + 1];   /* (host/hos_main.c game_main: the file's path, for the broker) */
static char ps5_tee_block[256 << 10];
static size_t ps5_tee_n;
static int64_t ps5_tee_since, ps5_tee_at = -1;
static void ps5_tee_flush(void) {
    if (!ps5_tee_n || !hos_log_tee) { ps5_tee_n = 0; return; }
    const int64_t a = hos_now_ns();
    size_t done = 0;
    if (hos_log_tee_path[0] && hb_ready()) {
        if (ps5_tee_at < 0) fflush(hos_log_tee), ps5_tee_at = (int64_t)ftello(hos_log_tee);
        if (ps5_tee_at >= 0) {
            const int64_t r = hb_write(hos_log_tee_path, (uint64_t)ps5_tee_at, ps5_tee_block, (uint32_t)ps5_tee_n);
            if (r > 0) done = (size_t)r, ps5_tee_at += r;
        }
    }
    if (done < ps5_tee_n) {
        if (ps5_tee_at >= 0) fseeko(hos_log_tee, (off_t)ps5_tee_at, SEEK_SET);
        const size_t w = fwrite(ps5_tee_block + done, 1, ps5_tee_n - done, hos_log_tee);
        if (ps5_tee_at >= 0) ps5_tee_at += (int64_t)w;
    }
    ps5_tee_n = 0;
    const int64_t d = hos_now_ns() - a;
    if (d > ps5_log_st.file_max_ns) ps5_log_st.file_max_ns = d;
}
/* the log thread's: the line into the file's block */
static void ps5_tee_add(const char *line) {
    const size_t n = strlen(line);
    if (ps5_tee_n + n > sizeof ps5_tee_block) ps5_tee_flush();
    if (!ps5_tee_n) ps5_tee_since = hos_now_ns();
    memcpy(ps5_tee_block + ps5_tee_n, line, n);
    ps5_tee_n += n;
}
static void ps5_log_emit_queued(const char *line) {
    const int64_t b = hos_now_ns();
    fputs(line, stderr);
    fflush(stderr);
    const int64_t c = hos_now_ns();
    if (c - b > ps5_log_st.klog_max_ns) ps5_log_st.klog_max_ns = c - b;
    ps5_log_st.lines++;
    ps5_tee_add(line);
}
static void ps5_log_emit(const char *line) {
    const int64_t a = hos_now_ns();
    if (hos_log_tee && ps5_tee_at < 0) fputs(line, hos_log_tee);   /* (HOS_LOG_SYNC, or before the thread: the caller's own write) */
    const int64_t b = hos_now_ns();
    fputs(line, stderr);
    fflush(stderr);
    const int64_t c = hos_now_ns();
    if (b - a > ps5_log_st.file_max_ns) ps5_log_st.file_max_ns = b - a;
    if (c - b > ps5_log_st.klog_max_ns) ps5_log_st.klog_max_ns = c - b;
    ps5_log_st.lines++;
}

static DWORD WINAPI ps5_log_main(void *p) {
    (void)p;
    static char batch[PS5_LOGBATCH][PS5_LOGLINE];
    for (;;) {
        uint32_t seen = __atomic_load_n(&ps5_lq_word, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&ps5_log_mu);
        int n = 0;
        while (ps5_lq_count && n < PS5_LOGBATCH) {
            strcpy(batch[n++], ps5_lq[ps5_lq_head]);
            ps5_lq_head = (ps5_lq_head + 1) % PS5_LOGQ;
            ps5_lq_count--;
        }
        const int64_t dropped = ps5_log_st.drop_pending;
        ps5_log_st.drop_pending = 0;
        ps5_log_writing = n || dropped;
        pthread_mutex_unlock(&ps5_log_mu);
        if (!n && !dropped) {
            if (ps5_tee_n) {   /* (nothing waits: the file's block, then asleep until a line comes) */
                pthread_mutex_lock(&ps5_log_mu);
                ps5_log_writing = 1;
                pthread_mutex_unlock(&ps5_log_mu);
                ps5_tee_flush();
                pthread_mutex_lock(&ps5_log_mu);
                ps5_log_writing = 0;
                pthread_mutex_unlock(&ps5_log_mu);
                continue;
            }
            WaitOnAddress(&ps5_lq_word, &seen, sizeof seen, 1000);
            continue;
        }
        for (int i = 0; i < n; i++) ps5_log_emit_queued(batch[i]);
        if (dropped) {
            char l[200];
            snprintf(l, sizeof l, "[%8.3f] [hos log] %lld lines dropped here: the queue was full (%d lines)\n", hos_ps5_since_start(),
                     (long long)dropped, PS5_LOGQ);
            ps5_log_emit_queued(l);
        }
        if (ps5_tee_n >= (32u << 10) || (ps5_tee_n && hos_now_ns() - ps5_tee_since > 2000000000LL)) ps5_tee_flush();
        pthread_mutex_lock(&ps5_log_mu);
        ps5_log_writing = ps5_tee_n != 0;
        pthread_mutex_unlock(&ps5_log_mu);
    }
}

static int log_queue(const char *line) {
    if (ps5_log_mode < 0) {
        pthread_mutex_lock(&ps5_log_mu);
        if (ps5_log_mode < 0) {
            ps5_log_mode = 0;   /* (a line from the thread's own start, or from another thread meanwhile: written by its caller) */
            pthread_mutex_unlock(&ps5_log_mu);
            int on = !env_on("HOS_LOG_SYNC");
            if (on && !(ps5_lq = malloc((size_t)PS5_LOGQ * PS5_LOGLINE))) on = 0;
            if (on) {
                HANDLE h = CreateThread(NULL, 1u << 18, ps5_log_main, NULL, 0, NULL);
                if (h) SetThreadDescription(h, L"hookshot log");
                else on = 0;
            }
            ps5_log_mode = on;
        } else {
            pthread_mutex_unlock(&ps5_log_mu);
        }
    }
    if (ps5_log_mode <= 0) return 0;
    pthread_mutex_lock(&ps5_log_mu);
    if (ps5_lq_count < PS5_LOGQ) {
        snprintf(ps5_lq[(ps5_lq_head + ps5_lq_count) % PS5_LOGQ], PS5_LOGLINE, "%s", line);
        ps5_lq_count++;
        if (ps5_lq_count > ps5_log_st.high) ps5_log_st.high = ps5_lq_count;
    } else {
        ps5_log_st.dropped++;
        ps5_log_st.drop_pending++;
    }
    pthread_mutex_unlock(&ps5_log_mu);
    __atomic_add_fetch(&ps5_lq_word, 1, __ATOMIC_SEQ_CST);
    WakeByAddressSingle((void *)&ps5_lq_word);
    return 1;
}
/* the queue written out, for at most 2 s (before a fatal line, at the end) */
static void log_flush(void) {
    if (ps5_log_mode <= 0) return;
    for (int i = 0; i < 2000; i++) {
        pthread_mutex_lock(&ps5_log_mu);
        const int busy = ps5_lq_count || ps5_log_writing;
        pthread_mutex_unlock(&ps5_log_mu);
        if (!busy) return;
        Sleep(1);
    }
}
/* for the scheduling report: the lines and the longest writes since the last call */
static void ps5_log_stats(char *buf, size_t cap) {
    pthread_mutex_lock(&ps5_log_mu);
    const int64_t lines = ps5_log_st.lines, dropped = ps5_log_st.dropped, high = ps5_log_st.high, f = ps5_log_st.file_max_ns,
                  k = ps5_log_st.klog_max_ns;
    ps5_log_st.lines = ps5_log_st.dropped = ps5_log_st.high = ps5_log_st.file_max_ns = ps5_log_st.klog_max_ns = 0;
    pthread_mutex_unlock(&ps5_log_mu);
    snprintf(buf, cap, "%s, %lld lines written, %lld dropped, at most %lld queued of %d; the longest write: the file %.1f ms, klog's pipe %.1f ms",
             ps5_log_mode > 0 ? "queued (a thread of its own writes)" : "written by each line's thread (HOS_LOG_SYNC)", (long long)lines,
             (long long)dropped, (long long)high, PS5_LOGQ, f / 1e6, k / 1e6);
}
#else
static int log_queue(const char *line) { (void)line; return 0; }
static void log_flush(void) {}
#endif

void hos_log_flush(void) { log_flush(); }

/* (longshot) A line's level as the log writes it: none for info, so the lines most of the log is stay as they were; "[trace] "
   after the thread's tag for the rest, which the launcher's log reads to show info and above unless asked for more */
static const char *log_level_tag(int level) {
    switch (level) {
    case HOS_LOG_ERROR: return "[error] ";
    case HOS_LOG_WARN: return "[warn] ";
    case HOS_LOG_DEBUG: return "[debug] ";
    case HOS_LOG_TRACE: return "[trace] ";
    default: return "";
    }
}

static void hos_vlog(int level, const char *fmt, va_list ap) {
    char buf[1024], line[1100];
    vsnprintf(buf, sizeof buf, fmt, ap);
    if (!strncmp(buf, HOS_REPORT_DEVICE_LOST_PREFIX, sizeof HOS_REPORT_DEVICE_LOST_PREFIX - 1))
        hos_crash_record_fatal(NULL, "device lost");
    else if (!strncmp(buf, HOS_REPORT_FATAL_PREFIX, sizeof HOS_REPORT_FATAL_PREFIX - 1))
        hos_crash_record_fatal(NULL, strstr(buf, "VK_ERROR_DEVICE_LOST") ? "device lost" : "Vulkan fatal");
#ifdef __PROSPERO__
    /* (the time since the eboot's first code, the stage trail's clock: what a line waited for can be read from the log).
       (ps5) No level tag: the console's lines stay as they were, for the PC's tools that read the UDP log */
    (void)level, (void)log_level_tag;
    snprintf(line, sizeof line, "[%8.3f] [hos %s] %s\n", hos_ps5_since_start(), cur ? cur->name : "host", buf);
    hos_ps5_udp_line(line, strlen(line));   /* (HOOKSHOT_PS5_LOG_UDP: out of the console at once, before the queue) */
    if (!log_queue(line)) ps5_log_emit(line);
    return;
#else
    snprintf(line, sizeof line, "[hos %s] %s%s\n", cur ? cur->name : "host", log_level_tag(level), buf);
    extern volatile int hos_control_on;   /* (control.c: log lines for its subscribers) */
    if (hos_control_on) {
        void hos_control_log(const char *line);
        hos_control_log(line);
    }
#endif
    if (log_queue(line)) return;
    fputs(line, stderr);
    fflush(stderr);
}

void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    hos_vlog(HOS_LOG_INFO, fmt, ap);
    va_end(ap);
}

void hos_log_at(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    hos_vlog(level, fmt, ap);
    va_end(ap);
}

void hos_fatal(Ctx *c, const char *fmt, ...) {
    if (!c) c = hos_ctx();
    hos_crash_record_fatal(c ? c->x : NULL, "hos_fatal");
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_flush();
#ifdef __PROSPERO__
    hos_ps5_trail("fatal: [%s] %s", cur ? cur->name : "host", buf);   /* (synced at once: the log's own lines may not get out) */
    if (hos_log_tee && ps5_tee_at >= 0) fseeko(hos_log_tee, (off_t)ps5_tee_at, SEEK_SET);   /* (past the broker's blocks) */
    if (hos_log_tee) fprintf(hos_log_tee, "[hos %s] FATAL: %s\n", cur ? cur->name : "host", buf);
#endif
    fprintf(stderr, "[hos %s] FATAL: %s", cur ? cur->name : "host", buf);
    if (c) fprintf(stderr, " (lr %#llx sp %#llx)", (unsigned long long)c->x[30], (unsigned long long)c->x[31]);
    fprintf(stderr, "\n");
    fflush(stderr);
    void hos_backtrace(void), hos_hostcall_stats(void);
    hos_backtrace();
    hos_hostcall_stats();
    ExitProcess(3);
}

/* Traps from the lifted code (rt/a64_rt.c): name the thread, then stop. */
void a64_on_trap(Ctx *c, uint64_t pc, uint32_t insn, const char *why) {
    hos_fatal(c, "%s at %#llx (insn %08x)", why, (unsigned long long)pc, insn);
}

static void setup_guest(HosThread *t) {
    t->tls_block = hos_sys_alloc(0x1000, 0x1000);
    t->tlr = hos_sys_alloc(0x200, 0x200);
    G64(t->tlr + 0x1F8) = t->guest_tt;
    t->kernel_handle = (uint32_t)InterlockedIncrement(&next_handle);
    G32(t->guest_tt + TT_HANDLE) = t->kernel_handle;
    /* (longshot) SDK 22's InternalCriticalSection::IsLockedByCurrentThread reads the owner handle from TLR+0x110
       directly (SDK 21: ThreadType+0x1B0 through TLR+0x1F8). Left 0, every free mutex looked held by the caller
       and the first heap lock aborted in pthread_mutex_lock. */
    G32(t->tlr + 0x110) = t->kernel_handle;
    t->wake = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset: one signal, one wake-up */
    Ctx *c = &t->ctx;
    memset(c, 0, sizeof *c);
    c->tpidr_el0 = t->tls_block + 0x800;
    c->tpidrro_el0 = t->tlr;
    c->thread = t;
}

static void track(HosThread *t) {
    AcquireSRWLockExclusive(&threads_lock);
    if (nthreads < 1024) threads[nthreads++] = t;
    ReleaseSRWLockExclusive(&threads_lock);
}

/* Host handles of every running guest thread, for the profiler (hos_prof.c). */
int hos_thread_handles(HANDLE *out, const char **names, int max) {
    int n = 0;
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads && n < max; i++)
        if (threads[i]->handle) { out[n] = threads[i]->handle; names[n] = threads[i]->name; n++; }
    ReleaseSRWLockShared(&threads_lock);
    return n;
}

/* (mkw-recomp) Every guest thread's name and guest registers as of its last call out of lifted code, with its
   frame chain from x29: what each thread of a stalled game waits in (mkw_glue.c, HOS_STATS_AFTER). */
void hos_thread_dump(void) {
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads; i++) {
        const Ctx *c = &threads[i]->ctx;
        char bt[400];
        int n = 0;
        uint64_t fp = c->x[29];
        for (int k = 0; k < 12 && fp && !(fp & 15) && fp < HOS_GUEST_SPACE && n < (int)sizeof bt - 24; k++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(GP(fp), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)G64(fp + 8));
            const uint64_t next = G64(fp);
            if (next <= fp) break;
            fp = next;
        }
        bt[n] = 0;
        hos_log("thread %-24s lr %#llx sp %#llx x0 %#llx; frames%s", threads[i]->name, (unsigned long long)c->x[30],
                (unsigned long long)c->x[31], (unsigned long long)c->x[0], bt);
        if (hos_game_is_mkw() && c->x[30] == 0x825c6844ULL) continue;   /* (MKW's idle thread loop) */
        /* registers (current at the call out), with the text a register (or the word it points to) leads to */
        for (int r = 0; r < 29; r++) {
            uint64_t v = c->x[r], s = 0;
            MEMORY_BASIC_INFORMATION mbi;
            for (int hop = 0; hop < 2 && v > 0x100000 && v < HOS_GUEST_SPACE - 64; hop++) {
                if (!VirtualQuery(GP(v), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                const uint8_t *p = (const uint8_t *)GP(v);
                int k = 0;
                while (k < 48 && p[k] >= 0x20 && p[k] < 0x7f) k++;
                if (k >= 6 && (p[k] == 0 || k == 48)) { s = v; break; }
                v = G64(v);
            }
            if (s) hos_log("thread %s:   x%d %#llx -> \"%.48s\"", threads[i]->name, r, (unsigned long long)c->x[r],
                           (const char *)GP(s));
        }
    }
    ReleaseSRWLockShared(&threads_lock);
}

/* HOS_FP_REPORT=1 (debugging): the FPCR / FPSR of the busiest game threads, with the thread stats */
int hos_thread_fp_report(char *out, int cap) {
    int len = 0;
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads && len < cap - 64; i++) {
        const char *nm = threads[i]->name;
        if (strstr(nm, "Main") || strstr(nm, "Worker") || strstr(nm, "Executor"))
            len += snprintf(out + len, cap - len, " %s fpcr %llx fpsr %llx;", nm,
                            (unsigned long long)threads[i]->ctx.fpcr, (unsigned long long)threads[i]->ctx.fpsr);
    }
    ReleaseSRWLockShared(&threads_lock);
    return len;
}

HosThread *hos_thread_by_handle(uint32_t h) {
    HosThread *r = NULL;
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->kernel_handle == h) { r = threads[i]; break; }
    ReleaseSRWLockShared(&threads_lock);
    return r;
}

/* Block on the thread's wake event (the kernel layer's lock must not be held). */
/* (HOS_CHAIN: the kernel's waits, svc mutexes, condition variables and the address arbiter, end in a wake of the
   thread itself: one key per thread, bit 62: never a guest address) */
static uint64_t wake_chain_key(HosThread *t) { return 1ULL << 62 | (uint64_t)(uintptr_t)t; }
void hos_thread_wait(HosThread *t, int64_t timeout_ns, int *timed_out) {
    DWORD ms = timeout_ns < 0 ? INFINITE : (DWORD)((timeout_ns + 999999) / 1000000);
    HOS_CHAIN(HOS_CHAIN_WAIT, wake_chain_key(t));
    const int away = hos_vcore_away();
    *timed_out = WaitForSingleObject(t->wake, ms) == WAIT_TIMEOUT;
    if (away) hos_vcore_back();
    HOS_CHAIN(HOS_CHAIN_WOKE, wake_chain_key(t));
}
void hos_thread_wake(HosThread *t) {
    HOS_CHAIN(HOS_CHAIN_SIGNAL, wake_chain_key(t));
    SetEvent(t->wake);
}

HosThread *hos_thread_of(uint64_t guest_tt) {
    HosThread *r = NULL;
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->guest_tt == guest_tt) { r = threads[i]; break; }
    ReleaseSRWLockShared(&threads_lock);
    return r;
}

Ctx *hos_boot_main_thread(uint64_t stack_size) {
    HosThread *t = (HosThread *)calloc(1, sizeof *t);
    t->guest_tt = hos_sys_alloc(0x400, 0x10);      /* a ThreadType for the main thread; the SDK never made one */
    t->stack_size = stack_size;
    t->stack = hos_sys_alloc(stack_size, 0x1000);
    t->id = InterlockedIncrement64(&next_id);
    t->priority = 44;                              /* main thread priority in TOTK's NPDM range */
    t->ideal_core = 0;
    t->started = 1;
    snprintf(t->name, sizeof t->name, "MainThread");
    __atomic_store_n(&t->serial, 1, __ATOMIC_RELAXED);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);   /* a real handle, for the profiler */
    SetThreadDescription(GetCurrentThread(), L"MainThread");   /* (HOS_SAMPLE finds threads by it) */
    hos_thread_pin(t->handle, t->name);
    setup_guest(t);
    t->ctx.x[31] = (t->stack + stack_size) & ~15ULL;
    cur = t;
    track(t);
    hos_vcore_back();   /* (virtual cores: before any guest code) */
    return &t->ctx;
}

static DWORD WINAPI thread_main(void *p) {
    HosThread *t = (HosThread *)p;
    cur = t;
    Ctx *c = &t->ctx;
    c->x[0] = t->arg;
    c->x[30] = 0;
    c->x[31] = (t->stack + t->stack_size) & ~15ULL;
    A64Fn f = a64_lookup(t->entry);
    if (!f) hos_fatal(c, "thread %s: entry %#llx is not a function", t->name, (unsigned long long)t->entry);
    hos_vcore_back();   /* (virtual cores: before any guest code) */
    if (hos_trace) hos_log("thread start: entry %#llx arg %#llx", (unsigned long long)t->entry, (unsigned long long)t->arg);
    /* What the SDK's own thread entry does before the thread function (see build.py thread_hooks): nnmusl's start
       hook gives the thread its TLS (thread pointer with the DTV at TP+8, every module's TLS block) and leaves the
       thread pointer at TLR+0x1F0. Without it every thread would share one DTV, and so every thread_local. */
    uint64_t hook = hos_thread_start_hook ? G64(hos_thread_start_hook) : 0;
    if (hook) {
        c->x[0] = t->guest_tt;
        a64_icall(c, hook);
        if (G64(t->tlr + 0x1F0)) c->tpidr_el0 = G64(t->tlr + 0x1F0);
        if (hos_trace || env_on("HOS_TLS_TRACE"))
            hos_log("thread %s: TLS hook gave TP %#llx, DTV %#llx; ideal core %d mask %#llx priority %d", t->name,
                    (unsigned long long)c->tpidr_el0, (unsigned long long)G64(c->tpidr_el0 + 8), t->ideal_core,
                    (unsigned long long)t->core_mask, t->priority);
        c->x[31] = (t->stack + t->stack_size) & ~15ULL;
    }
    c->x[0] = t->arg;
    c->x[30] = 0;
    f(c);
    hook = hos_thread_exit_hook ? G64(hos_thread_exit_hook) : 0;
    if (hook) {
        c->x[0] = t->guest_tt;
        a64_icall(c, hook);
    }
    if (hos_trace) hos_log("thread exit");
    hos_vcore_away();
    return 0;
}

HosThread *hos_thread_create(uint64_t guest_tt, uint64_t entry, uint64_t arg, uint64_t stack, uint64_t stack_size,
                             int priority, int core) {
    HosThread *t = (HosThread *)calloc(1, sizeof *t);
    t->guest_tt = guest_tt;
    t->entry = entry;
    t->arg = arg;
    t->stack = stack;
    t->stack_size = stack_size;
    t->priority = priority;
    t->ideal_core = core;
    t->id = InterlockedIncrement64(&next_id);
    snprintf(t->name, sizeof t->name, "Thread_%llx", (unsigned long long)entry);
    setup_guest(t);   /* FPCR starts at 0, as the kernel does it; TOTK sets FZ itself (nnMain+0x474, thread entries) */
    track(t);
    return t;
}

void hos_thread_start(HosThread *t) {
    if (t->started) return;
    t->started = 1;
    /* Host stack: lifted code nests a C call per guest call, so give it plenty. */
    t->handle = CreateThread(NULL, 64 << 20, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->handle) hos_fatal(NULL, "CreateThread failed for %s", t->name);
    if (t->name[0]) {   /* (named before it started: the OS name too, for profilers and HOS_SAMPLE) */
        wchar_t w[48];
        MultiByteToWideChar(CP_UTF8, 0, t->name, -1, w, 48);
        SetThreadDescription(t->handle, w);
    }
    hos_thread_pin(t->handle, t->name);
    pin_by_core(t);
#ifdef __EMSCRIPTEN__
    { void web_thread_named(HANDLE t, const char *name); web_thread_named(t->handle, t->name); }
#endif
}

void hos_thread_join(HosThread *t) {
    if (!t->handle) return;
    const int away = hos_vcore_away();
    /* (EvilOOT, 11-07: a freeze after Load, one thread joining another for good: a join of over 10 s says who joins whom, once) */
    if (WaitForSingleObject(t->handle, 10000) == WAIT_TIMEOUT) {
        HosThread *self = hos_self();
        hos_log("thread %s: joining %s for 10 s", self ? hos_thread_name(self) : "?", t->name);
        WaitForSingleObject(t->handle, INFINITE);
    }
    if (away) hos_vcore_back();
}

void hos_thread_destroy(HosThread *t) {
    AcquireSRWLockExclusive(&threads_lock);
    for (int i = 0; i < nthreads; i++)
        if (threads[i] == t) { threads[i] = threads[--nthreads]; break; }
    ReleaseSRWLockExclusive(&threads_lock);
    if (t->handle) CloseHandle(t->handle);
    if (t->wake) CloseHandle(t->wake);
    free(t);
}

/* The frame's serial threads: OoT's game thread (sead::MainThread, the boot thread's MainThread before the game names
   it) and its Presentation Thread. A frame waits on each in turn, so a wait of theirs that ends late ends the frame late;
   the host keeps their short waits short (sleep_ns's pollers, the spin before a wait sleeps: hos_spin_ns). By name, as
   the game names them: kept in the thread's record when it is named, not read from the name while it may change. */
static int serial_name(const char *name) { return strstr(name, "MainThread") || !strcmp(name, "Presentation Thread"); }
int hos_thread_serial(const HosThread *t) { return t && __atomic_load_n(&t->serial, __ATOMIC_RELAXED); }

/* The processors this process may run on (its affinity mask, as the game starts with it): Windows with one processor
   group; 0 = not known (more than one group, or elsewhere). Up to SHORT_SLEEP_TIMER_THREADS of them, the host keeps the
   behaviour measured best there (sleep_ns's timer, hos_spin_ns's one spin). */
enum { SHORT_SLEEP_TIMER_THREADS = 6 };
static int process_threads(void) {
    int threads = 0;
#if defined(_WIN32)   /* (measured on Windows only) */
    /* (with more than one processor group the mask is one group's, not the process's: not known then) */
    DWORD_PTR proc = 0, sys = 0;
    if (GetActiveProcessorGroupCount() == 1 && GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
        threads = __builtin_popcountll((uint64_t)proc);
#endif
    return threads;
}

/* How long a wait spins before it sleeps (nn_os_sync.c's events, queues and mutexes; hos_svc.c's mutexes), in ns, for the
   calling thread: HOS_SPIN_US (20 us) on the frame's serial threads, HOS_SPIN_OTHER_US (5 us, never more than HOS_SPIN_US)
   on the others, where there are more than SHORT_SLEEP_TIMER_THREADS processors. With 6 or fewer (or not known), every
   thread keeps HOS_SPIN_US unless HOS_SPIN_OTHER_US is set: there the short spin saved no CPU and cost 1.6 % fps (6 pairs,
   Hyrule, 6 of 16 processors), as a 24-thread run with only 2 pollers did (1.7 %). A sleeper runs again 6.6-8.4 us after its signal (HOS_WAKE_LAT, Hyrule), so a spin much longer than that
   mostly burns its thread's time: 90 % of the spin's 15 % of a core in Hyrule was on the job workers and the actor
   executors. 5 us for every thread cut the process's CPU a frame 3.3 % at the same fps (6 pairs, Hyrule, 16 threads); the
   serial threads keep 20 us, as their wait ends are on the frame's path. HOS_SPIN_OTHER_US=20: as before, one spin for all;
   HOS_SPIN_US=0: no spin. */
int64_t hos_spin_ns(void) {
    static int64_t both = -1;   /* (the serial threads' ns << 32 | the others': one atomic word, chosen once) */
    int64_t b = __atomic_load_n(&both, __ATOMIC_RELAXED);
    if (b < 0) {
        const char *e = getenv("HOS_SPIN_US"), *o = getenv("HOS_SPIN_OTHER_US");
        int64_t s = e ? atoll(e) : 20, other = o ? atoll(o) : process_threads() > SHORT_SLEEP_TIMER_THREADS ? 5 : s;   /* (us, clamped before they become ns) */
        s = (s < 0 ? 0 : s > 1000000 ? 1000000 : s) * 1000;
        other = (other < 0 ? 0 : other > 1000000 ? 1000000 : other) * 1000;
        other = other > s ? s : other;
        b = s << 32 | other;
        __atomic_store_n(&both, b, __ATOMIC_RELAXED);
    }
    return hos_thread_serial(hos_self()) ? b >> 32 : b & 0xFFFFFFFF;
}

void hos_thread_set_name(HosThread *t, const char *name) {
    snprintf(t->name, sizeof t->name, "%s", name);
    __atomic_store_n(&t->serial, serial_name(t->name), __ATOMIC_RELAXED);   /* (atomic: SetThreadName may name another thread) */
    if (t->handle) {
        wchar_t w[48];
        MultiByteToWideChar(CP_UTF8, 0, t->name, -1, w, 48);
        SetThreadDescription(t->handle, w);
        hos_thread_pin(t->handle, t->name);
    }
}

/* HOS_PIN=<name part>=<hex mask>,...[,*=<hex mask>] (experiment): host CPU affinity by thread name (game threads as
   they are named or started, host threads as they register); "*" for every other named thread. The first matching
   entry wins. (2026-10-20: the castle's MainThread and ModuleSystem workers each on a physical core of their own, the
   rest elsewhere, was slower than Windows' own scheduling: 63-68 against 69-71 fps.) */
/* HOS_PRIO=<name part>=<level>,... (experiment, same matching): host thread priority, -2 (lowest) to 2 (highest).
   (10-20: MainThread, ModuleSystem workers and BaseProc executors above normal: no change at the castle) */
typedef struct { char pat[40]; long long value; } NameSpec;
static int parse_name_specs(const char *e, NameSpec *out, int max, int base) {
    int n = 0;
    while (e && *e && n < max) {
        const char *eq = strchr(e, '='), *end = strchr(e, ',');
        if (!end) end = e + strlen(e);
        if (!eq || eq > end) break;
        snprintf(out[n].pat, sizeof out[n].pat, "%.*s", (int)(eq - e), e);
        out[n++].value = strtoll(eq + 1, NULL, base);
        e = *end ? end + 1 : end;
    }
    return n;
}
static const NameSpec *match_name_spec(const NameSpec *s, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(s[i].pat, "*") ? strstr(name, s[i].pat) != NULL : 1) return &s[i];
    return NULL;
}
/* The last physical core (its logical processors' mask), kept for the Vulkan back end's replay thread (gpu/vk
   vk_replay.cpp pins it there): the game's threads run on the others. The replay is on the frame's critical path
   (MKW waits for the last frame's GPU work before it submits the next), and on a core shared with game threads
   it ran ~1.7x slower than alone. Only on machines with 8+ logical processors; HOS_RESERVE_CORE=0: none.
   (longshot) On a CPU with cores of more than one kind, the last core of the fastest kind: the last core of all is an
   efficiency core on Intel's hybrid CPUs (an i7-12700K's logical processors 16-19), the slowest place for the replay
   (an i7-13700K's Kokiri Forest: 48.7 fps with it there, 60 on a performance core). EfficiencyClass is higher the faster
   the core, and the same for every core of a CPU of one kind (the last core, as before). Only with 4+ cores of the
   fastest kind (8+ logical processors always have them on a CPU of one kind): a laptop's 2 performance cores and 8
   efficiency ones (i5-1235U) would keep one of the two from the game; then none is kept. The calling thread's
   processor group's cores: the masks the affinity calls take are its group's. On Linux the same choice from
   /sys/devices/cpu_core/cpus (Intel's hybrid CPUs; last_fastest_core below); without that file, the last core, as before. */
#ifdef __PROSPERO__
static int ps5_affinity_mode(void);   /* 0 all, 1 reserve, 2 pinned (HOOKSHOT_PS5_AFFINITY, below) */
#endif
/* (longshot) The cores that share the largest L3, on a CPU whose cores do not all share one of the same size: AMD's X3D CPUs
   with two CCDs (7900X3D, 7950X3D, 9900X3D, 9950X3D) have the 3D V-Cache on one CCD only (96 MB against 32 MB). Windows
   moves a game there only when its Game Bar knows the program as a game; this one it does not know, so its threads ran on
   both, and the replay's reserved core (the last one) is on the CCD without the cache. On a 9950X3D (SMT off), Castle Town's
   market (2,300 draws): the replay 11.5-12.3 ms a frame and 57-58 fps as it was (the slowest two seconds 54), 10.1-10.5 ms and
   59.9 fps (58.7) with the game's threads kept to the first CCD. So they are kept to those cores. 0 for none: one L3 for all, L3s
   of one size (a 7950X, both CCDs alike), L3s that do not all share as many logical processors (a hybrid CPU's two kinds of
   core: a Ryzen AI 9 HX 370 has 4 Zen 5 cores on a 16 MB L3 and 8 Zen 5c on an 8 MB one, and the game is not squeezed onto the 4;
   an X3D's CCDs are alike but for the cache), or fewer than 8 cores on the largest (the 12-core 7900X3D and 9900X3D have 6 there:
   not measured, left as they were). Measured with SMT off only: with it the 8 cores are 16 logical processors.
   HOS_CACHE_CORES=0: none. Windows only, as the reserved core's choice above: Linux keeps the last core, which on these CPUs is
   on the CCD without the cache.
   (the choice: each L3's logical processor mask and size, n of them, and each core's mask, ncores of them.
   tests/test_reserved_core.py) */
static uint64_t pick_cache_cores(const uint64_t *masks, const uint64_t *sizes, int n, const uint64_t *cores, int ncores) {
    uint64_t best = 0, most = 0, least = ~0ull;
    int width = 0, alike = 1;   /* (how many logical processors an L3 is shared by, and whether all are shared by as many) */
    for (int i = 0; i < n; i++) {
        if (!masks[i] || !sizes[i]) continue;
        if (!width) width = __builtin_popcountll(masks[i]);
        else if (__builtin_popcountll(masks[i]) != width) alike = 0;
        if (sizes[i] > most) most = sizes[i], best = masks[i];
        else if (sizes[i] == most) best |= masks[i];   /* (several of the largest size: all of them) */
        if (sizes[i] < least) least = sizes[i];
    }
    if (!best || least == most || !alike) return 0;
    int physical = 0;
    for (int i = 0; i < ncores; i++) physical += (cores[i] & best) != 0;
    if (physical < 8) return 0;
    return best;
}
/* (the choice: the cores' logical processor masks and EfficiencyClass, n of them; 0 for none. tests/test_reserved_core.py) */
static uint64_t pick_reserved_core(const uint64_t *masks, const unsigned char *classes, int n) {
    uint64_t last = 0;
    int best = -1, fast = 0, logical = 0;
    for (int i = 0; i < n; i++) {
        if (!masks[i]) continue;
        logical += __builtin_popcountll(masks[i]);
        if ((int)classes[i] > best) {
            best = classes[i], last = masks[i], fast = 1;
        } else if ((int)classes[i] == best) {
            fast++;
            if (masks[i] > last) last = masks[i];
        }
    }
    if (logical < 8 || fast < 4) return 0;

    // One-time debug logging
    static bool logged = false;
    if (!logged) {
        logged = true;
        hos_log("vk: reserved core selection: %d cores, %d logical, best class=%d, fast cores=%d, selected=%#llx",
                n, logical, best, fast, (unsigned long long)last);
        for (int i = 0; i < n; i++) {
            if (masks[i]) {
                hos_log("vk:   core %d: mask=%#llx, class=%d, popcount=%d",
                        i, (unsigned long long)masks[i], classes[i], __builtin_popcountll(masks[i]));
            }
        }
    }
    return last;
}
#if defined(_WIN32)
/* (pick_cache_cores over the calling thread's group's L3 caches; 0: no such split, or HOS_CACHE_CORES=0) */
uint64_t hos_cache_cpus(void) {
    static uint64_t mask = ~0ull;
    if (mask != ~0ull) return mask;
    mask = 0;
    if (getenv("HOS_CACHE_CORES") && !atoi(getenv("HOS_CACHE_CORES"))) return mask;
    static uint64_t masks[64], sizes[64], cores[256];
    DWORD len = 0;
    int n = 0, ncores = 0;
    GROUP_AFFINITY ga;
    const WORD group = GetThreadGroupAffinity(GetCurrentThread(), &ga) ? ga.Group : 0;
    if (GetLogicalProcessorInformationEx(RelationCache, NULL, &len) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return mask;
    char *buf = (char *)malloc(len);
    if (buf && GetLogicalProcessorInformationEx(RelationCache, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len))
        for (DWORD at = 0; at < len && n < 64;) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + at);
            if (!e->Size) break;
            if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == group) {
                masks[n] = (uint64_t)e->Cache.GroupMask.Mask;
                sizes[n] = e->Cache.CacheSize;
                n++;
            }
            at += e->Size;
        }
    free(buf);
    len = 0;   /* (and the group's cores: how many of them the largest L3 has) */
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return mask;
    buf = (char *)malloc(len);
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len))
        for (DWORD at = 0; at < len && ncores < 256;) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + at);
            if (!e->Size) break;
            if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount && e->Processor.GroupMask[0].Group == group)
                cores[ncores++] = (uint64_t)e->Processor.GroupMask[0].Mask;
            at += e->Size;
        }
    free(buf);
    mask = pick_cache_cores(masks, sizes, n, cores, ncores);
    if (mask) {
        uint64_t most = 0, least = ~0ull;
        for (int i = 0; i < n; i++) {
            if (sizes[i] > most) most = sizes[i];
            if (sizes[i] && sizes[i] < least) least = sizes[i];
        }
        hos_log("threads: the game's on logical processors %#llx, the cores that share the largest L3 (%llu MB; the others' is "
                "%llu MB). HOS_CACHE_CORES=0: every core", (unsigned long long)mask, (unsigned long long)(most >> 20),
                (unsigned long long)(least >> 20));
    }
    return mask;
}
/* (pick_reserved_core over the calling thread's group's cores; *cores: how many, 0 when they could not be read) */
static uint64_t last_fastest_core_legacy(int *cores) {
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[256];
    DWORD len = sizeof info;
    if (!GetLogicalProcessorInformation(info, &len)) {
        hos_log("vk: GetLogicalProcessorInformation failed: %lu", GetLastError());
        return 0;
    }

    int logical = 0;
    int current_core = 0;
    uint64_t masks[256];
    unsigned char classes[256];

    int pcore_candidates = 0;
    int ecore_candidates = 0;

    for (DWORD i = 0; i < len / sizeof info[0] && current_core < 256; i++) {
        if (info[i].Relationship == RelationProcessorCore) {
            uint64_t mask = (uint64_t)info[i].ProcessorMask;
            int popcount = __builtin_popcountll(mask);
            logical += popcount;

            // Heuristic: cores with 2+ logical processors = P-cores (with HT)
            // cores with 1 logical processor = E-cores (no HT)
            if (popcount >= 2) {
                classes[current_core] = 1;  // P-core
                pcore_candidates++;
            } else {
                classes[current_core] = 0;  // E-core
                ecore_candidates++;
            }
            masks[current_core] = mask;
            current_core++;
        }
    }

    *cores = current_core;

    hos_log("vk: legacy API fallback: %d cores, %d logical, P-candidates=%d, E-candidates=%d",
            current_core, logical, pcore_candidates, ecore_candidates);

    // If mixed core types detected, use enhanced picker
    if (pcore_candidates > 0 && ecore_candidates > 0) {
        hos_log("vk: Intel hybrid CPU detected via legacy API heuristic");
        return pick_reserved_core(masks, classes, current_core);
    }

    // Single CPU type - use original logic (last core)
    uint64_t last = 0;
    for (int i = 0; i < current_core; i++) {
        if ((uint64_t)masks[i] > last) last = (uint64_t)masks[i];
    }
    if (logical >= 8) return last;
    return 0;
}

/* (pick_reserved_core over the calling thread's group's cores; *cores: how many, 0 when they could not be read) */
static uint64_t last_fastest_core(int *cores) {
    static uint64_t masks[256];
    static unsigned char classes[256];
    DWORD len = 0;
    int n = 0;
    *cores = 0;
    GROUP_AFFINITY ga;
    const WORD group = GetThreadGroupAffinity(GetCurrentThread(), &ga) ? ga.Group : 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        hos_log("vk: GetLogicalProcessorInformationEx failed: %lu", GetLastError());
        return last_fastest_core_legacy(cores);
    }
    char *buf = (char *)malloc(len);
    if (!buf) {
        hos_log("vk: failed to allocate buffer for processor info");
        return last_fastest_core_legacy(cores);
    }

    bool success = false;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
        for (DWORD at = 0; at < len && n < 256;) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + at);
            if (!e->Size) break;
            if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount && e->Processor.GroupMask[0].Group == group) {
                masks[n] = (uint64_t)e->Processor.GroupMask[0].Mask;
                classes[n] = e->Processor.EfficiencyClass;
                n++;
            }
            at += e->Size;
        }
        success = true;
    }

    free(buf);

    if (!success || n == 0) {
        hos_log("vk: extended API failed or no cores found, trying legacy API with Intel hybrid detection");
        return last_fastest_core_legacy(cores);
    }

    *cores = n;
    return pick_reserved_core(masks, classes, n);
}
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
/* (longshot) A Linux cpulist ("0-7", "0-11,16-19") as a mask of logical processors 0-63; 0 when it does not parse.
   (tests/test_reserved_core.py) */
static uint64_t parse_cpu_list(const char *s) {
    uint64_t mask = 0;
    while (*s && *s != '\n') {
        char *end;
        long a = strtol(s, &end, 10), b = a;
        if (end == s || a < 0) return 0;
        s = end;
        if (*s == '-') {
            b = strtol(++s, &end, 10);
            if (end == s || b < a) return 0;
            s = end;
        }
        for (long i = a; i <= b && i < 64; i++) mask |= 1ull << i;
        if (*s == ',') s++;
        else if (*s && *s != '\n') return 0;
    }
    return mask;
}
/* (longshot) Linux: Windows' EfficiencyClass has no file of its own, but on Intel's hybrid CPUs the kernel lists the
   performance cores' logical processors in /sys/devices/cpu_core/cpus (the hybrid PMU; the efficiency cores are in
   cpu_atom). Without it the last core was kept, and on those CPUs that is an efficiency core: a Core Ultra 7 265KF's
   logical processor 19 (P-cores 0-7, E-cores 8-19). Those cores are class 1, the others 0, and pick_reserved_core
   chooses as on Windows. *cores: 0 when the file is not there (a CPU of one kind, AMD, an older kernel): as before. */
static uint64_t last_fastest_core(int *cores) {
    *cores = 0;
    char line[256] = {0};
    FILE *f = fopen("/sys/devices/cpu_core/cpus", "r");
    if (!f) return 0;
    const bool read = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    const uint64_t fast = read ? parse_cpu_list(line) : 0;
    if (!fast) return 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[64];
    DWORD len = sizeof info;
    if (!GetLogicalProcessorInformation(info, &len)) return 0;
    uint64_t masks[64];
    unsigned char classes[64];
    int n = 0;
    for (DWORD i = 0; i < len / sizeof info[0] && n < 64; i++)
        if (info[i].Relationship == RelationProcessorCore) {
            masks[n] = (uint64_t)info[i].ProcessorMask;
            classes[n] = (masks[n] & fast) != 0;
            n++;
        }
    *cores = n;
    return pick_reserved_core(masks, classes, n);
}
#endif
uint64_t hos_reserved_cpus(void) {
    static uint64_t mask = ~0ull;
    if (mask != ~0ull) return mask;
    mask = 0;

    // Allow explicit hex mask override for testing
    const char* override = getenv("HOS_RESERVE_CORE_MASK");
    if (override) {
        char* endptr;
        mask = strtoull(override, &endptr, 16);
        if (*endptr != '\0') {
            hos_log("vk: HOS_RESERVE_CORE_MASK '%s' is not a valid hex value, ignoring", override);
            mask = 0;
        } else {
            hos_log("vk: HOS_RESERVE_CORE_MASK override: %#llx", (unsigned long long)mask);
        }
        return mask;
    }

    if (getenv("HOS_RESERVE_CORE") && !atoi(getenv("HOS_RESERVE_CORE"))) {
        hos_log("vk: HOS_RESERVE_CORE=0, no core reserved");
        return mask;
    }
#ifdef __PROSPERO__
    /* (ps5: the replay thread runs only with RX_VK_ASYNC=1, and the title has 13 CPUs: the last one (12, a core's half the
       title may use) is kept for it only then, or with HOOKSHOT_PS5_AFFINITY=reserve. The 63c498e game kept it from the
       game's threads with no replay thread to run on it) */
    if (ps5_affinity_mode() == 2) return mask;
    {
        const char *async = getenv("RX_VK_ASYNC");   /* (the render thread's switch, read as vk_replay.cpp render_thread_mode reads it) */
        if (ps5_affinity_mode() != 1 && !(async && atoi(async))) return mask;
    }
#endif
#if defined(_WIN32)
    /* (longshot) none where the game is kept to the cores of the largest L3 (hos_cache_cpus): they are 8 cores on the CPUs
       measured (a 9950X3D, SMT off), and with one of them kept for the replay the game's threads had 7. In the market the replay
       then took 9.8-10.0 ms a frame but the frame rate stayed at 56.9-57.4; with the 8 shared, 10.1-10.5 ms and 59.9 fps.
       HOS_RESERVE_CORE=1: the last core of the fastest kind is kept all the same, as on other CPUs. That is normally a core of
       the other CCD, not the cache core of the 9.8-10.0 ms above, and the game's threads then have all of the cache cores */
    if (hos_cache_cpus() && !(getenv("HOS_RESERVE_CORE") && atoi(getenv("HOS_RESERVE_CORE")))) return mask;
    {
        int cores = 0;
        const uint64_t fast = last_fastest_core(&cores);
        if (cores) {
            mask = fast;
            hos_log("vk: reserved core mask (enhanced): %#llx (%d cores)", (unsigned long long)mask, cores);
            return mask;
        }
    }
#elif !defined(__EMSCRIPTEN__)
    {
        int cores = 0;
        const uint64_t fast = last_fastest_core(&cores);
        if (cores) {
            mask = fast;
            hos_log("vk: reserved core mask (hybrid, /sys/devices/cpu_core): %#llx (%d cores)", (unsigned long long)mask, cores);
            return mask;
        }
    }
#endif
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[256];
    DWORD len = sizeof info;
    if (!GetLogicalProcessorInformation(info, &len)) return mask;
    /* (longshot) of the processors this process may run on. A process kept to some of them (taskset, a launcher's
       affinity) had the PC's last core kept all the same, outside its set, and a thread may leave the set: the replay ran
       there, beside whatever else did. A timed session kept to one CCD of a 7900X (CPUs 0-5,12-17) replayed on CPUs 11 and
       23 of the other, with the builds: frames of 2-8 s whenever one ran, and a 13th core when none did. A core counts
       when all its logical processors are allowed; with every processor allowed, the last core as before */
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc) proc = ~(DWORD_PTR)0;
    int logical = 0;
    uint64_t last = 0;
    for (DWORD i = 0; i < len / sizeof info[0]; i++)
        if (info[i].Relationship == RelationProcessorCore && !((uint64_t)info[i].ProcessorMask & ~(uint64_t)proc)) {
            logical += __builtin_popcountll((uint64_t)info[i].ProcessorMask);
            if ((uint64_t)info[i].ProcessorMask > last) last = (uint64_t)info[i].ProcessorMask;
        }
    if (logical >= 8) mask = last;
    hos_log("vk: reserved core mask (fallback): %#llx", (unsigned long long)mask);
    return mask;
}

void hos_thread_pin(void *h, const char *name) {
    const uint64_t reserved = hos_reserved_cpus();
#if defined(_WIN32)
    const uint64_t cache = hos_cache_cpus();   /* (longshot: the cores of the largest L3, when the CPU's differ) */
#else
    const uint64_t cache = 0;
#endif
    if (reserved || cache) {
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        uint64_t may = (uint64_t)proc;
        if (cache && (may & cache)) may &= cache;
        if (may & ~reserved) SetThreadAffinityMask(h, (DWORD_PTR)(may & ~reserved));
    }
    static NameSpec pins[24], prios[24];
    static int npins = -1, nprios;
    static SRWLOCK lk = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lk);
    if (npins < 0) {
        npins = parse_name_specs(getenv("HOS_PIN"), pins, 24, 16);
        nprios = parse_name_specs(getenv("HOS_PRIO"), prios, 24, 10);
    }
    ReleaseSRWLockExclusive(&lk);
    const NameSpec *p = match_name_spec(pins, npins, name);
    if (p && p->value) SetThreadAffinityMask(h, (DWORD_PTR)p->value);
    static const int levels[5] = {THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL,
                                  THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST};
    if ((p = match_name_spec(prios, nprios, name)) && p->value >= -2 && p->value <= 2)
        SetThreadPriority(h, levels[p->value + 2]);
    else if (strstr(name, "OptimizeChange") && !env_on("HOS_OCB_NORMAL")) {
        /* (OoT's OptimizeChangeBank (guest priority 27, a background thread) walks every placed object of the
           area by name, again and again (Kokiri: ~344 names, ~250k lookups a second). On the console it only gets the
           time nothing more important wants; at the host's normal priority it took the renderer's and the driver's
           cores and the frame waited (Kokiri, RX 6600 + 5600G: 17-20 fps; lowest: 21-28). HOS_OCB_NORMAL=1: as before) */
        SetThreadPriority(h, THREAD_PRIORITY_LOWEST);
    }
}

/* ---- time ------------------------------------------------------------------------------------------------ */
uint64_t hos_ticks(void) { return recomp_cntpct(NULL); }

int64_t hos_now_ns(void) {
    static LARGE_INTEGER freq;
    static int64_t ns_per_tick;
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        ns_per_tick = 1000000000LL % f.QuadPart == 0 ? 1000000000LL / f.QuadPart : 0;
        freq = f;
    }
    QueryPerformanceCounter(&now);
    if (ns_per_tick) return (int64_t)now.QuadPart * ns_per_tick;
    return (int64_t)((now.QuadPart / freq.QuadPart) * 1000000000LL + (now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
}

static void sleep_ns(int64_t ns, int guest);
/* How sleeps under a millisecond wait: 0, all yield until their time; 1, the game's own (SleepThread) wait on the timer,
   which wakes about 0.5 ms late, and the host's (the frame limiter, the audio renderer) yield; 2, all wait on the timer.
   asked: the value of HOS_SHORT_SLEEP_TIMER, NULL when it is not set; set, it is read by env_on's rule (on: the timer for
   all, 2; off: yielding for all, 0). threads: the logical processors this process may run on, 0 when not known. Not set:
   1 with SHORT_SLEEP_TIMER_THREADS of them or fewer, which is no more than the game's six cores, else 0.
   OoT's idle job workers sleep 20 us and look again, about 7,600 times a second, and a yielding loop keeps a processor
   busy for each: with so few processors that is time the frame's threads need. Measured with the game held to part of a
   5700X3D: on 4 threads Hyrule Field went from 47 to 51 fps and lost its long frames (over 250 ms in 2 of 4 runs
   yielding, in none with the timer), Kokiri's worst frame went from 53-422 ms to 30 ms; on 6 the frame rate stayed and a
   frame's CPU time fell 9 %. On 8 threads and more yielding stays: with the timer for all sleeps there were no long
   frames either way and Kokiri lost 0.2 fps under its cap, and the timer made TOTK's 20 us waits for its job workers 25x
   longer, ~2000 a second on the frame's critical path. */
static int hos_short_sleep_timer(const char *asked, int threads) {
    if (asked) return env_value_off(asked) ? 0 : 2;
    return threads > 0 && threads <= SHORT_SLEEP_TIMER_THREADS ? 1 : 0;
}
/* Where the game's short sleeps yield (more than SHORT_SLEEP_TIMER_THREADS processors), at most this many of its
   threads yield through one at the same time; the game's next short sleep then waits on the timer until one is back.
   OoT's idle job workers all poll their queues in 20 us steps: the first few notice new work within ~20 us, the rest
   stop keeping a processor busy each (as Go's scheduler keeps at most half of its idle threads looking for work, and
   RPCS3 yields below its host quantum only on CPUs with few threads). The frame's serial threads (hos_thread_serial:
   the game thread's wait for its own batch of jobs) and the host's sleeps always yield and are not counted.
   asked: HOS_SHORT_SLEEP_POLLERS, NULL when not set: a number, 0 = no limit (every short sleep yields, as before); not
   a number = not set. Not set: a quarter of the processors (4 of 16), none where they are not known. */
static volatile LONG short_pollers;   /* (the game's threads in the yield loop now) */
static int hos_short_sleep_pollers(const char *asked, int threads) {
    char *end = NULL;
    const long n = asked && *asked ? strtol(asked, &end, 10) : 0;
    if (end && end != asked && *end == 0) return n > 0 ? (int)(n < 1024 ? n : 1024) : 0;
    return threads > SHORT_SLEEP_TIMER_THREADS ? threads / 4 : 0;
}
/* (virtual cores: a sleep gives the core up, SleepThread(0) offers it to a waiter as good) guest: the game's own sleep */
static void sleep_away(int64_t ns, int guest) {
    if (ns <= 0) {
        hos_vcore_yield();
        SwitchToThread();
        return;
    }
    HOS_CHAIN(HOS_CHAIN_SLEEP, (uint64_t)ns);
    const int away = hos_vcore_away();
    sleep_ns(ns, guest);
    if (away) hos_vcore_back();
    HOS_CHAIN(HOS_CHAIN_AWAKE, (uint64_t)ns);
}
void hos_sleep_ns(int64_t ns) { sleep_away(ns, 0); }
static void sleep_ns(int64_t ns, int guest) {
    /* A high-resolution waitable timer (one per thread) sleeps to within ~0.5 ms without spinning; Sleep() rounds to
       the timer resolution, so without one the last millisecond is spun (threads that sleep in short steps, the
       vibration thread for one, used 15% of a core spinning). */
    static _Thread_local HANDLE timer;
    static _Thread_local int tried;
    int64_t until = hos_now_ns() + ns;
#ifdef __EMSCRIPTEN__
    /* The web has no high-resolution timer, and its clock is a call into JavaScript: spinning to the deadline below
       burned ~0.8 of a core across TOTK's sleeping threads (cores its game, render and WebGPU threads needed). A
       timed wait (a futex: Atomics.wait) to `slack` before the time, then the rest spun; slack follows how late the
       waits wake (HOS_WEB_SPIN_SLEEP=1: spun, as before). */
    static int web_spin = -1;
    if (web_spin < 0) web_spin = env_on("HOS_WEB_SPIN_SLEEP");
    static _Atomic int64_t slack = 300000;
    const int64_t s = atomic_load_explicit(&slack, memory_order_relaxed);
    if (!web_spin && ns > s + 50000) {
        const int64_t want = until - s;
        usleep((useconds_t)((want - hos_now_ns()) / 1000));
        const int64_t late = hos_now_ns() - want;
        int64_t next = s + (late + 25000 - s) / 8;
        next = next < 50000 ? 50000 : next > 2000000 ? 2000000 : next;
        atomic_store_explicit(&slack, next, memory_order_relaxed);
    }
    while (hos_now_ns() < until) sched_yield();
    return;
#endif
    /* Under a millisecond: yielded away until the time, or the timer for the game's own sleeps where the processors
       are few (hos_short_sleep_timer). HOS_SHORT_SLEEP_TIMER=1: the timer always, 0: never, not set: by the processors */
    /* (the choice, how | the cap << 2: one atomic word, so a thread sees both or neither) */
    static int choice = -1;
    int chosen = __atomic_load_n(&choice, __ATOMIC_RELAXED);
    if (chosen < 0) {
        const int threads = process_threads();   /* (0 elsewhere than Windows: the timer when asked for) */
        int how = hos_short_sleep_timer(getenv("HOS_SHORT_SLEEP_TIMER"), threads);
#if !defined(_WIN32)
        /* (longshot a1eb4604) Linux: the game's own sleeps wait on the timer whatever the processors, the host's still
           yield (1). With #2602, OoT's five idle job workers and sead::MainThread yielded 3.3-3.6 ms a frame each in
           Hyrule Field (RTX 3090, 7900X, 24 threads), ~9 ms of CPU a frame in all, while one of them waited on a GPU
           readback. With the timer for all sleeps the frame's CPU fell 44.8 -> 35.4 ms at the same median frame
           (11.30 / 11.32 ms). The job workers are not on the frame's critical path there (HOS_CHAIN), so their later
           wake costs nothing measured; on 4-8 thread CPUs (the Steam Deck: 8) those yields took processors the frame
           needs. HOS_SHORT_SLEEP_TIMER=0: all yield, as before; =1: all on the timer.
           (ps5) The console too, as every build that is not Windows: the timer there is win32_posix.c's, a
           clock_nanosleep to the absolute time, and its lateness at 20 us is not measured (HOS_SLEEP_LOG=1 gives the
           asked and slept time of the game's sleeps by caller every 5 s). Its 13 CPUs hold the game's job workers
           yielding 20 us at a time beside the replay, the driver's threads and the host's; a yielding thread there
           shares its core with its SMT sibling the whole time */
        if (!getenv("HOS_SHORT_SLEEP_TIMER")) how = 1;
#endif
        /* (the line says what was chosen, before any thread has made its timer: where the high-resolution timer cannot
           be made, Windows before 10 1803, a short sleep falls through to the yield loop at the end and nothing changes) */
        static volatile LONG told;
        if (how && !InterlockedExchange(&told, 1))
            hos_log("sleep: %s sleeps under a millisecond wait on the timer where the high-resolution one can be made "
                    "(%d logical processors, 0: not counted; HOS_SHORT_SLEEP_TIMER=0: all yield)",
                    how == 2 ? "all" : "the game's", threads);
        /* (HOS_SHORT_SLEEP_TIMER set says how all short sleeps wait: no limit then) */
        const int cap = how || getenv("HOS_SHORT_SLEEP_TIMER") ? 0 : hos_short_sleep_pollers(getenv("HOS_SHORT_SLEEP_POLLERS"), threads);
        if (cap && !InterlockedExchange(&told, 1))
            hos_log("sleep: at most %d of the game's threads yield through a short sleep at a time, the others wait on the "
                    "timer (%d logical processors; HOS_SHORT_SLEEP_POLLERS=0: all yield)", cap, threads);
        chosen = how | cap << 2;
        __atomic_store_n(&choice, chosen, __ATOMIC_RELAXED);
    }
    const int short_timer = chosen & 3, poll_cap = chosen >> 2;
    if (ns < 1000000 && short_timer < (guest ? 1 : 2)) {
        const int counted = guest && poll_cap && !hos_thread_serial(hos_self());
        if (!counted || InterlockedIncrement(&short_pollers) <= poll_cap) {
            while (hos_now_ns() < until)
                if (!SwitchToThread()) YieldProcessor();
            if (counted) InterlockedDecrement(&short_pollers);
            return;
        }
        InterlockedDecrement(&short_pollers);   /* (enough of them poll already: the timer, below) */
    }
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100 > 0 ? ns / 100 : 1);   /* relative, 100 ns units */
        if (SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    if (ns > 2000000) Sleep((DWORD)((ns - 1000000) / 1000000));
    while (hos_now_ns() < until) SwitchToThread();
}

/* ---- nn::os threads -------------------------------------------------------------------------------------- */
static HosThread *thread_arg(Ctx *c, uint64_t tt) {
    /* (the calling thread's own ThreadType, the usual case: GetThreadPriority(GetCurrentThread()) ~2,400 times a frame in
       Hyrule from the navigation workers: its record, without the scan of every thread under threads_lock; the SDK reads
       the priority from the ThreadType itself) */
    if (cur && cur->guest_tt == tt) return cur;
    HosThread *t = hos_thread_of(tt);
    if (!t) hos_fatal(c, "unknown nn::os::ThreadType %#llx", (unsigned long long)tt);
    return t;
}

/* nn::os::CreateThread(ThreadType*, void (*)(void*), void* arg, void* stack, unsigned long size, int prio) */
HLE(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi) {
    HOS_TRACE_CALL("CreateThread");
    hos_thread_create(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (int)ARG(5), -2);
    RET(0);
}

/* nn::os::CreateThread(ThreadType*, void (*)(void*), void* arg, void* stack, unsigned long size, int prio,
   int idealCore) (longshot: OoT's Porter thread pool uses this overload; left to the guest SDK it reached
   ThreadManagerImplByHorizon::MapAliasStack, which needs svcMapMemory, and aborted) */
HLE(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii) {
    HOS_TRACE_CALL("CreateThread");
    hos_thread_create(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (int)ARG(5), (int)ARG(6));
    RET(0);
}

HLE(_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE) {
    HOS_TRACE_CALL("StartThread");
    hos_thread_start(thread_arg(c, ARG(0)));
}

HLE(_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE) {
    HOS_TRACE_CALL("WaitThread");
    hos_thread_join(thread_arg(c, ARG(0)));
}

HLE(_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE) {
    HOS_TRACE_CALL("DestroyThread");
    HosThread *t = thread_arg(c, ARG(0));
    hos_thread_join(t);
    hos_thread_destroy(t);
}

HLE(_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc) {
    HosThread *t = thread_arg(c, ARG(0));
    hos_thread_set_name(t, ARG(1) ? (const char *)GP(ARG(1)) : "");
    if (hos_trace) hos_log("SetThreadName %s", t->name);
}

HLE(_ZN2nn2os16GetCurrentThreadEv) { RET(cur->guest_tt); }
HLE(_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE) { RET(thread_arg(c, ARG(0))->id); }
HLE(_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE) { RET((uint32_t)hos_thread_priority(thread_arg(c, ARG(0)), 0, 0)); }
HLE(_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi) {
    RET((uint32_t)hos_thread_priority(thread_arg(c, ARG(0)), 1, (int)ARG(1)));
}

/* nn::os::SetThreadCoreMask(ThreadType*, int idealCore, unsigned long mask) */
HLE(_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim) {
    HosThread *t = thread_arg(c, ARG(0));
    hos_thread_affinity(t, (int)ARG(1), ARG(2));
}

HLE(_ZN2nn2os20GetCurrentCoreNumberEv) { RET((uint32_t)hos_thread_core(cur)); }
HLE(_ZN2nn2os26GetThreadAvailableCoreMaskEv) { RET((1u << hos_cores()) - 1); }   /* cores 0-2: what an application gets */

HLE(_ZN2nn2os19GetCurrentStackInfoEPmS1_) {
    if (ARG(0)) G64(ARG(0)) = cur->stack;
    if (ARG(1)) G64(ARG(1)) = cur->stack_size;
}

HLE(_ZN2nn2os11YieldThreadEv) {
    hos_vcore_yield();
    SwitchToThread();
}
/* HOS_SLEEP_LOG=1 (diagnostic): guest sleeps by caller (LR) every 5 s: count, asked, slept */
void hos_sleep_logged(int64_t ns, uint64_t lr) {
    /* HOS_POLL_SLEEP_NS=<ns> (experiment): a guest sleep of up to 100 us lasts at most that long. TOTK's job system
       waits for its workers by sleeping 20 us and looking again (~90 times a frame at the castle, on MainThread and
       the workers): a shorter step notices the job's end sooner (the loop looks again; nothing else changes).
       Measured 2026-10-21 (castle, 2000 ns, 3 rounds): no change. */
    static int64_t poll_cap = -1;
    if (poll_cap < 0) {
        const char *e = getenv("HOS_POLL_SLEEP_NS");
        poll_cap = e ? atoll(e) : 0;
    }
    if (poll_cap && ns > poll_cap && ns <= 100000) ns = poll_cap;
    static int on = -1;
    if (on < 0) on = env_on("HOS_SLEEP_LOG");
    if (!on) { sleep_away(ns, 1); return; }
    const int64_t t0 = hos_now_ns();
    sleep_away(ns, 1);
    const int64_t took = hos_now_ns() - t0;
    static SRWLOCK lk = SRWLOCK_INIT;
    static struct { uint64_t lr; int64_t n, asked, slept; } tab[64];
    static int64_t last;
    AcquireSRWLockExclusive(&lk);
    int i = 0;
    while (i < 64 && tab[i].lr && tab[i].lr != lr) i++;
    if (i < 64) {
        tab[i].lr = lr;
        tab[i].n++;
        tab[i].asked += ns > 0 ? ns : 0;
        tab[i].slept += took;
    }
    const int64_t now = hos_now_ns();
    if (now - last > 5000000000LL) {
        last = now;
        for (int k = 0; k < 64 && tab[k].lr; k++)
            if (tab[k].n)
                hos_log("sleep: lr %#llx: %lld calls, asked %.2f ms, slept %.2f ms (%.1f us each)",
                        (unsigned long long)tab[k].lr, (long long)tab[k].n, tab[k].asked / 1e6, tab[k].slept / 1e6,
                        tab[k].slept / 1e3 / tab[k].n);
        memset(tab, 0, sizeof tab);
    }
    ReleaseSRWLockExclusive(&lk);
}

/* (longshot) OoT's present waits for its frame's GPU work by polling an NVN event, sleeping ~1.6 ms between looks
   (0x80590500: `while (nvnEventGetValue(e) != 1) SleepThread(...)`, ~3.5 sleeps a frame, 2 ms each on Windows): the
   frame noticed the GPU's signal late, ~7 ms of the 25 ms frame asleep on its critical path. The GPU writes the event
   into guest memory itself, so the sleep looks at it between yields (its virtual core given up) and ends once it is
   1, at the latest when the asked time is up. Only where the poll is nvnEventGetValue. HOS_PRESENT_POLL=1 (an
   experiment: no fps change measured, and it keeps a core busy while it looks) */
static int present_poll(Ctx *c, int64_t ns) {
    static int on = -1;
    static uint64_t fn_ok, fn_bad;
    if (on < 0) on = getenv("HOS_PRESENT_POLL") && atoi(getenv("HOS_PRESENT_POLL")) != 0;
    if (!on || c->x[30] != 0x80590520 || ns <= 0 || ns > 20000000) return 0;
    const uint64_t fp = G64(c->x[24]);
    if (fp != fn_ok) {
        if (fp == fn_bad) return 0;
        const char *hos_hostcall_name(uint64_t addr);
        const char *n = hos_hostcall_name(fp);
        if (!n || strcmp(n, "nvnEventGetValue")) { fn_bad = fp; return 0; }
        fn_ok = fp;
        hos_log("sleep: OoT's present poll looks at its event between yields (HOS_PRESENT_POLL)");
    }
    int hos_nvn_event_value(uint64_t guest_event, uint32_t *v);
    const uint64_t ev = c->x[23] + (uint64_t)G32(c->x[22] + 0x164) * 64;
    uint32_t v = 0;
    if (!hos_nvn_event_value(ev, &v)) return 0;
    const int away = hos_vcore_away();
    const int64_t end = hos_now_ns() + ns;
    while (v != 1 && hos_now_ns() < end) {
        SwitchToThread();
        for (int i = 0; i < 64; i++) _mm_pause();
        hos_nvn_event_value(ev, &v);
    }
    if (away) hos_vcore_back();
    return 1;
}
/* (longshot) The FPS counter's Breakdown (gpu/vk/vk_fps.cpp, through MkwVkHost slept_ns): the time slept in OoT's present poll
   (0x80590520 above: waiting for its frame's GPU work) and in the window's acquire (host/nn_vi.c: the frame pacing), whichever
   thread they are on (OoT simulates on its main thread and records on its presentation thread, which waits for it), and each
   thread's other guest sleeps. A clock read before and after a sleep, only while the Breakdown shows (hos_fps_timing, set by the
   back end through MkwVkHost fps_timing): off, a load of that flag */
volatile int hos_fps_timing;
volatile int64_t hos_poll_slept_ns, hos_acquire_slept_ns;
_Thread_local int64_t hos_slept_ns[2];
static void slept_since(uint64_t lr, int64_t t0) {
    if (!t0) return;
    const int64_t d = hos_now_ns() - t0;
    if ((lr & ~1ull) == 0x80590520) InterlockedAdd64(&hos_poll_slept_ns, d);
    else hos_slept_ns[1] += d;
}
HLE(_ZN2nn2os11SleepThreadENS_8TimeSpanE) {
    const int64_t t0 = hos_fps_timing ? hos_now_ns() : 0;
    if (present_poll(c, (int64_t)ARG(0))) { slept_since(c->x[30], t0); return; }
    if (c->x[30] == 0x80590520 && env_on("HOS_SLEEP_LOG")) {   /* (longshot, diagnostic: OoT's present poll) */
        static int logged;
        if (logged++ < 3) {
            const char *hos_hostcall_name(uint64_t addr);
            const uint64_t fp = G64(c->x[24]), sync = c->x[23] + (uint64_t)G32(c->x[22] + 0x164) * 64;
            const char *n = hos_hostcall_name(fp);
            hos_log("sleep: OoT's present poll: fn %#llx (%s), arg %#llx, sleep %lld ns", (unsigned long long)fp,
                    n ? n : "guest code", (unsigned long long)sync, (long long)ARG(0));
        }
    }
    hos_sleep_logged((int64_t)ARG(0), c->x[30]);
    slept_since(c->x[30], t0);
}

/* ---- TLS slots --------------------------------------------------------------------------------------------- */
static volatile LONG slot_used[HOS_TLS_SLOTS];

/* nn::os::AllocateTlsSlot(TlsSlot*, void (*)(unsigned long)) -> Result */
HLE(_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE) {
    for (int i = 0; i < HOS_TLS_SLOTS; i++) {
        if (!InterlockedCompareExchange(&slot_used[i], 1, 0)) {
            G32(ARG(0)) = (uint32_t)i;
            RET(0);
            return;
        }
    }
    RET(0x2C01);   /* os::ResultOutOfResource */
}

HLE(_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE) { if ((uint32_t)ARG(0) < HOS_TLS_SLOTS) slot_used[(uint32_t)ARG(0)] = 0; }
HLE(_ZN2nn2os11GetTlsValueENS0_7TlsSlotE) { RET((uint32_t)ARG(0) < HOS_TLS_SLOTS ? cur->tls_slots[(uint32_t)ARG(0)] : 0); }
HLE(_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm) { if ((uint32_t)ARG(0) < HOS_TLS_SLOTS) cur->tls_slots[(uint32_t)ARG(0)] = ARG(1); }

/* ---- ticks ------------------------------------------------------------------------------------------------- */
volatile uint64_t hos_gfx_module;
/* HOS_TICK_LOG=1 (diagnostic): who reads the clock, by caller (LR), every 5 s */
static void tick_log(uint64_t lr) {
    static CRITICAL_SECTION cs; static volatile LONG once; static uint64_t lrs[512]; static uint32_t counts[512]; static int64_t next;
    if (!InterlockedExchange(&once, 1)) InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    for (int i = (int)((lr >> 2) * 2654435761u >> 8) & 511, n = 0; n < 512; n++, i = (i + 1) & 511)
        if (!lrs[i] || lrs[i] == lr) { lrs[i] = lr; counts[i]++; break; }
    const int64_t now = hos_now_ns();
    if (now > next) {
        next = now + 5000000000LL;
        char line[1500]; int n = 0;
        for (int k = 0; k < 40 && n < 1400; k++) {
            int best = -1;
            for (int i = 0; i < 512; i++) if (counts[i] && (best < 0 || counts[i] > counts[best])) best = i;
            if (best < 0) break;
            n += snprintf(line + n, sizeof line - n, " %llx:%u", (unsigned long long)lrs[best], counts[best]);
            counts[best] = 0;
        }
        memset(counts, 0, sizeof counts);
        hos_log("tick readers in 5 s:%s", line);
    }
    LeaveCriticalSection(&cs);
}
HLE(_ZN2nn2os13GetSystemTickEv) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_TICK_LOG");
    if (on) tick_log(c->x[30]);
    /* (longshot) The graphics module: x19 at its frame-end clock read (main+0x346c770 is after the bl), for nn_vi.c's
       real-time step */
    if (c->x[30] == 0x8348c770) hos_gfx_module = c->x[19];
    RET(hos_ticks());
}
HLE(_ZN2nn2os22GetSystemTickFrequencyEv) { RET(HOS_TICK_HZ); }
/* (longshot) At the counter's own rate, HOS_TICK_HZ (31.25 MHz: 32 ns a tick). These converted at the Switch 1's
   19.2 MHz (ticks = ns * 12 / 625) while GetSystemTick counts 31.25 MHz, so a tick span came out 1.63x too long: OoT's
   present slept 1.63 ms for its 1 ms poll step (0x80590500: SleepThread(ConvertToTimeSpan(frequency / 1000))), and
   any of its timing through these was off by as much. HOS_TICK_CONVERT_192=1: as before */
static int tick_convert_192(void) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_TICK_CONVERT_192");
    return on;
}
HLE(_ZN2nn2os13ConvertToTickENS_8TimeSpanE) {
    int64_t ns = (int64_t)ARG(0);
    if (tick_convert_192()) { RET((ns / 625) * 12 + (ns % 625) * 12 / 625); return; }
    RET(ns / (int64_t)(1000000000ULL / HOS_TICK_HZ));
}
HLE(_ZN2nn2os17ConvertToTimeSpanENS0_4TickE) {
    int64_t t = (int64_t)ARG(0);
    if (tick_convert_192()) { RET((t / 12) * 625 + (t % 12) * 625 / 12); return; }
    RET(t * (int64_t)(1000000000ULL / HOS_TICK_HZ));
}

/* ---- fibers (longshot: nn::async's FiberTaskCoordinator, on OoT's nn.npln threads) ----------------------------
   A guest fiber is a Win32 fiber with its own guest context: the lifted frames on its host stack keep using that
   context wherever it runs, so a fiber may resume on another thread. Switching in, it takes the running thread's
   identity (TLS registers, host thread object); the thread's own context stays in HosThread.ctx. The fiber function
   returns the FiberType to switch to next (nn::os semantics). */
typedef struct HosFiber {
    Ctx ctx;   /* (first: a context's fiber is found from its address) */
    uint64_t ft, fn, arg, stack, size;
    void *host;
    int started, done;
    struct HosFiber *next;
} HosFiber;
static SRWLOCK fibers_lock = SRWLOCK_INIT;
static HosFiber *fibers;

static HosFiber *fiber_of_ft(uint64_t ft) {
    AcquireSRWLockShared(&fibers_lock);
    HosFiber *f = fibers;
    while (f && f->ft != ft) f = f->next;
    ReleaseSRWLockShared(&fibers_lock);
    return f;
}
static HosFiber *fiber_of_ctx(Ctx *c) {
    AcquireSRWLockShared(&fibers_lock);
    HosFiber *f = fibers;
    while (f && &f->ctx != c) f = f->next;
    ReleaseSRWLockShared(&fibers_lock);
    return f;
}
static void fiber_switch(Ctx *from, HosFiber *to);

static void WINAPI fiber_main(void *p) {
    HosFiber *f = (HosFiber *)p;
    Ctx *c = &f->ctx;
    c->x[0] = f->arg;
    c->x[30] = 0;
    c->x[31] = (f->stack + f->size) & ~15ULL;
    a64_icall(c, f->fn);   /* FiberType* (*)(void*) */
    const uint64_t next = c->x[0];
    f->done = 1;
    HosFiber *to = next ? fiber_of_ft(next) : NULL;   /* (NULL: back to the thread's own context, as nn::os does) */
    if (next && !to) hos_fatal(c, "fiber %#llx returned %#llx: not an initialized fiber", (unsigned long long)f->ft,
                               (unsigned long long)next);
    fiber_switch(c, to);
    hos_fatal(c, "fiber %#llx: switched to after it returned", (unsigned long long)f->ft);
}

/* from: the context calling (the thread's own or a fiber's); to: NULL for the thread's own context */
static void fiber_switch(Ctx *from, HosFiber *to) {
    HosThread *t = cur;
    if (!t->host_fiber) {
        t->host_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
        if (!t->host_fiber) hos_fatal(from, "ConvertThreadToFiber failed (%lu)", GetLastError());
    }
    Ctx *dst = to ? &to->ctx : &t->ctx;
    if (dst == from) return;
    if (to) {
        if (to->done) hos_fatal(from, "SwitchToFiber(%#llx): that fiber has returned", (unsigned long long)to->ft);
        if (!to->started) {   /* the thread's state it inherits: FPCR, then the identity below */
            to->started = 1;
            to->ctx.fpcr = from->fpcr;
            to->ctx.fpsr = from->fpsr;
        }
    }
    /* the running thread's identity, whoever ran this context last */
    dst->tpidr_el0 = t->ctx.tpidr_el0;
    dst->tpidrro_el0 = t->ctx.tpidrro_el0;
    dst->thread = t->ctx.thread;
    if (from->poll) dst->poll = 1;   /* (a pending virtual-core request follows the thread) */
    t->run_ctx = to ? &to->ctx : NULL;
    SwitchToFiber(to ? to->host : t->host_fiber);
    /* back in `from`, maybe on another thread: take that one's identity (fiber_switch's caller set it already) */
}

/* InitializeFiber(FiberType*, FiberType* (*)(void*), void* arg, void* stack, unsigned long size, int flags) */
HLE(_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi) {
    HosFiber *f = fiber_of_ft(ARG(0));
    if (!f) {
        f = (HosFiber *)calloc(1, sizeof *f);
        f->ft = ARG(0);
        AcquireSRWLockExclusive(&fibers_lock);
        f->next = fibers, fibers = f;
        ReleaseSRWLockExclusive(&fibers_lock);
    } else if (f->host) {
        DeleteFiber(f->host);
    }
    memset(&f->ctx, 0, sizeof f->ctx);
    f->fn = ARG(1), f->arg = ARG(2), f->stack = ARG(3), f->size = ARG(4);
    f->started = f->done = 0;
    f->host = CreateFiberEx(64 << 10, 64 << 20, FIBER_FLAG_FLOAT_SWITCH, fiber_main, f);
    if (!f->host) hos_fatal(c, "CreateFiberEx failed (%lu)", GetLastError());
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 8 || hos_trace)
        hos_log("fiber %#llx: function %#llx arg %#llx stack %#llx+%#llx flags %d", (unsigned long long)f->ft,
                (unsigned long long)f->fn, (unsigned long long)f->arg, (unsigned long long)f->stack,
                (unsigned long long)f->size, (int)ARG(5));
}

HLE(_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE) {
    HosFiber *f = fiber_of_ft(ARG(0));
    if (f && f->host && &f->ctx != c) { DeleteFiber(f->host); f->host = NULL; f->done = 1; }
}

HLE(_ZN2nn2os15GetCurrentFiberEv) {
    HosFiber *f = c == &cur->ctx ? NULL : fiber_of_ctx(c);
    RET(f ? f->ft : 0);
}

/* SwitchToFiber(FiberType*): NULL goes back to the thread's own context */
HLE(_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE) {
    HosFiber *to = NULL;
    if (ARG(0) && !(to = fiber_of_ft(ARG(0))))
        hos_fatal(c, "SwitchToFiber(%#llx): not an initialized fiber", (unsigned long long)ARG(0));
    fiber_switch(c, to);
}

/* Every thread's kernel handle (the tag its SDK mutexes hold), for hos_kernel_dump. */
void hos_thread_log_handles(void) {
    AcquireSRWLockShared(&threads_lock);
    /* (lr: the return address of the thread's last call, x0 its first argument: in a wait, which game code waits on
       what; the web has no stack walk) */
    for (int i = 0; i < nthreads; i++)
        hos_log("kernel:   handle %#x = %s: lr %#llx x0 %#llx x19 %#llx x20 %#llx x21 %#llx x22 %#llx",
                threads[i]->kernel_handle, threads[i]->name, (unsigned long long)threads[i]->ctx.x[30],
                (unsigned long long)threads[i]->ctx.x[0], (unsigned long long)threads[i]->ctx.x[19],
                (unsigned long long)threads[i]->ctx.x[20], (unsigned long long)threads[i]->ctx.x[21],
                (unsigned long long)threads[i]->ctx.x[22]);
    ReleaseSRWLockShared(&threads_lock);
}

/* ---- virtual cores (on unless HOS_VCORES=0) -------------------------------------------------------------------
   Horizon runs at most one thread per core, and TOTK keys per-core data (allocators, job queues) by the current core
   number without locks; both MacNativeGames (threads.c) and totk-pc-recomp (runtime/kernel.c, MIT) schedule the game's
   threads on three virtual cores for it. Here a guest thread holds one of hos_cores() cores while it runs guest code
   and gives it up where Horizon would switch threads: in every block (hos_cv_sleep, hos_thread_wait, hos_sleep_ns,
   the NVN waits), at a yield, at exit. A free core goes to the best waiter allowed on it (lower priority value first,
   then the one waiting longest). A waiter better than a holder asks it to give the core up (vc_kick), which it does at
   its next checkpoint (every nn::os call and SVC: hos_vcore_checkpoint), as Horizon would run the better thread at
   once. Guest code with no checkpoint (a spin loop) keeps its core, so a waiter that has waited too long runs anyway
   without one (an "escape", logged with the holders): HOS_VCORE_ESCAPE_US for a better waiter, HOS_VCORE_ESCAPE_EQ_US
   otherwise (both 2000000: 2 s; the console keeps 250000, 250 ms, the default it played with: vc_acquire). An escape runs a
   thread with no core while the holder still runs on it, so two threads share per-core state (#1929); it is kept only as a
   deadlock breaker, and the escaped thread takes a core again at its next block (hos_vcore_away). 250 ms was the default
   before: on the 25-minute route it escaped 6-8 times a run, at waits that ended by themselves within 1.2 s (0 escapes at
   2 s, frame times the same or better). A thread that waits only because better threads keep winning its core (they take
   it in turns at every block, or one of them runs on) is starved, not deadlocked, and an escape runs it beside the holder
   for nothing: once it has waited HOS_VCORE_STARVE_US (1000000: 1 s; 0: never) it goes before every other waiter at the
   next release of a core it may use, and asks the worst holder of those cores to give the core up, whatever its priority.
   It runs a turn, the holders ask for the core back as for any worse thread, and a wait that long ends in a grant, not an
   escape (#262; the 5 s line counts these grants). (ps5: the console escapes at 250 ms, so upstream's 1 s would never be
   reached there; with HOOKSHOT_PS5_VCORE_ESCAPE=block its default is half the shorter escape wait, at most 1 s, as
   upstream's 1 s is half its 2 s: 125 ms with the console's 250 ms. HOS_VCORE_STARVE_US=0: the rule before, escapes only,
   and the console's default: there an escape lasts until the thread ends, as before the merge, vc_escape_for_life.)
   Every 5 s the log has a "vcores:" line.
   GetCurrentCoreNumber answers the core a thread holds.
   A core vc_grant hands to a waiter stays its own until it wakes and takes it up; a waiter that cannot get CPU time then
   holds a core nobody runs on (the console: real-time threads of one priority are not time-sliced, host/posix/ps5_platform.c).
   Past HOS_VCORE_CLAIM_US (the console: 2000; elsewhere 0, never) another waiter that is running takes it, and the one it was granted to waits
   again in its place. The 5 s line also says how long the 1 ms waiting steps really took, how long a granted core waited
   to be taken up, those taken over ("stolen") and the longest wait for the scheduler's own lock. */
#if !defined(__EMSCRIPTEN__)
static SRWLOCK vc_lock = SRWLOCK_INIT;
/* (ps5, 2026-11-09, the game's third run: with every thread on SCHED_RR, waits for vc_lock still reached 9.7 s and a 1 ms
   waiting step 4.9 s. The console's pthread_rwlock lets a releasing thread take the lock straight back while the waiter it
   woke has not run yet, and every waiter below comes back for it each millisecond: one can lose that race for seconds.
   HOOKSHOT_PS5_VCLOCK=ticket (the default): a ticket lock, served in arrival order, each waiter asleep on one of 32 futex
   words (its ticket's), so a release wakes the next in line only; =rwlock: the lock as before. Elsewhere vc_lock, as upstream.
   And after the first 4 ms of a wait the steps are HOS_VCORE_STEP_US long (the console: 10000; elsewhere 1 ms, as upstream):
   a grant wakes its waiter at once, the steps only check the slices, take-overs and escapes) */
#ifdef __PROSPERO__
static struct { uint32_t next, serving; uint32_t gate[32]; } vc_tk;
static int64_t vc_handoff_t0;   /* (when the lock was let go with a ticket waiting; the lock's) */
static int vc_ticket = -1;
static int vc_ticket_on(void) {
    if (vc_ticket < 0) {
        const char *e = getenv("HOOKSHOT_PS5_VCLOCK");
        vc_ticket = !(e && !strcmp(e, "rwlock"));
    }
    return vc_ticket;
}
#endif
/* (2026-11-10: how long the lock is held and where it is let go, and how long a waiting ticket takes to take it once it is
   let go: the 5 s line and the report say them, to tell a holder that blocks from a waiter that is slow to wake) */
static void vc_hold_begin(int waited);
static void vc_hold_end(int line);
static void vc_lk_acquire(void) {
#ifdef __PROSPERO__
    if (vc_ticket_on()) {
        const uint32_t t = __atomic_fetch_add(&vc_tk.next, 1, __ATOMIC_SEQ_CST);
        uint32_t *g = &vc_tk.gate[t & 31];
        for (int spin = 0;; spin++) {
            uint32_t seen = __atomic_load_n(g, __ATOMIC_SEQ_CST);   /* (before serving: a release moves serving, then the gate) */
            if (__atomic_load_n(&vc_tk.serving, __ATOMIC_SEQ_CST) == t) {
                vc_hold_begin(spin > 0);
                return;
            }
            if (spin < 64) {
                __builtin_ia32_pause();
                continue;
            }
            WaitOnAddress(g, &seen, sizeof seen, INFINITE);
        }
    }
#endif
    AcquireSRWLockExclusive(&vc_lock);
    vc_hold_begin(0);
}
#define vc_lk_release() vc_lk_release_at(__LINE__)
static void vc_lk_release_at(int line) {
    vc_hold_end(line);
#ifdef __PROSPERO__
    if (vc_ticket_on()) {
        const uint32_t n = __atomic_load_n(&vc_tk.serving, __ATOMIC_RELAXED) + 1;
        vc_handoff_t0 = __atomic_load_n(&vc_tk.next, __ATOMIC_SEQ_CST) != n ? hos_now_ns() : 0;
        __atomic_store_n(&vc_tk.serving, n, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&vc_tk.gate[n & 31], 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&vc_tk.next, __ATOMIC_SEQ_CST) != n) WakeByAddressAll(&vc_tk.gate[n & 31]);   /* (ticket n waits) */
        return;
    }
#endif
    ReleaseSRWLockExclusive(&vc_lock);
}
static int vc_lk_try(void) {
#ifdef __PROSPERO__
    if (vc_ticket_on()) {
        uint32_t s = __atomic_load_n(&vc_tk.serving, __ATOMIC_SEQ_CST), n = s;   /* (free: no ticket out past serving) */
        if (!__atomic_compare_exchange_n(&vc_tk.next, &n, s + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return 0;
        vc_hold_begin(0);
        return 1;
    }
#endif
    if (!TryAcquireSRWLockExclusive(&vc_lock)) return 0;
    vc_hold_begin(0);
    return 1;
}
/* SleepConditionVariableSRW on the lock above: woken by WakeConditionVariable (cv->seq), or after ms */
static void vc_lk_wait(CONDITION_VARIABLE *cv, DWORD ms) {
#ifdef __PROSPERO__
    if (vc_ticket_on()) {
        uint32_t seq = __atomic_load_n(&cv->seq, __ATOMIC_SEQ_CST);
        vc_lk_release();
        WaitOnAddress(&cv->seq, &seq, sizeof seq, ms);
        vc_lk_acquire();
        return;
    }
#endif
    vc_hold_end(__LINE__);
    SleepConditionVariableSRW(cv, &vc_lock, ms, 0);
    vc_hold_begin(0);
}
static HosThread *vc_owner[8];
static HosThread *vc_queue[1024];
static int vc_nq;
/* (the 5 s line's; vc_rep: the same since the scheduling report's last, hos_sched_*) */
typedef struct {
    int64_t takes, waits, wait_ns, max_ns, escapes, kicks, yields, boosts, since;
    int64_t steps, step_max_ns, steps_over10, claims, claim_max_ns, claims_over10, stolen, lock_waits, lock_max_ns;
    int64_t hold_max_ns, handoff_max_ns;
    int hold_line;
} VcStats;
static VcStats vc_st, vc_rep;
static int64_t vc_hold_t0;
static void vc_hold_begin(int waited) {
    const int64_t now = hos_now_ns();
#ifdef __PROSPERO__
    if (waited && vc_handoff_t0) {
        const int64_t h = now - vc_handoff_t0;
        if (h > vc_st.handoff_max_ns) vc_st.handoff_max_ns = h;
        if (h > vc_rep.handoff_max_ns) vc_rep.handoff_max_ns = h;
    }
#else
    (void)waited;
#endif
    vc_hold_t0 = now;
}
static void vc_hold_end(int line) {
    const int64_t d = hos_now_ns() - vc_hold_t0;
    if (d > vc_st.hold_max_ns) vc_st.hold_max_ns = d, vc_st.hold_line = line;
    if (d > vc_rep.hold_max_ns) vc_rep.hold_max_ns = d, vc_rep.hold_line = line;
}
static int hos_cpu_now(void) {
#ifdef __PROSPERO__
    return hos_ps5_current_cpu();
#else
    return -1;
#endif
}
/* (ps5, HOOKSHOT_PS5_AFFINITY=pinned: the thread on its core's CPU while it holds one, else on the shared CPUs; done by the
   thread itself after it took or gave up a core) */
#ifdef __PROSPERO__
static void vc_place(HosThread *t);
#else
#define vc_place(t) ((void)0)
#endif

int hos_vcores_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HOS_VCORES");
        on = e ? atoi(e) != 0 : 1;   /* (mkw-recomp: on unless HOS_VCORES=0; the game's job queues need it) */
    }
    return on;
}
static int vc_cores(void) { return hos_cores() > 8 ? 8 : hos_cores(); }
static unsigned vc_allowed(HosThread *t) {
    const unsigned all = (1u << vc_cores()) - 1;
    unsigned m = (unsigned)(t->core_mask & all);
    if (!m) m = 1u << hos_thread_core(t);
    return m;
}
/* a runs before b: a starved waiter first (the one waiting longest first among them), then by priority */
static int vc_better(HosThread *a, HosThread *b) {
    if (a->vc_starved != b->vc_starved) return a->vc_starved;
    if (!a->vc_starved && a->priority != b->priority) return a->priority < b->priority;
    return a->vc_since < b->vc_since;
}
/* a free core t may run on (its last one, its ideal one, then the lowest), or -1; holding vc_lock */
static int vc_free_for(HosThread *t) {
    const unsigned m = vc_allowed(t);
    if (t->vc_last > 0 && (m >> (t->vc_last - 1) & 1) && !vc_owner[t->vc_last - 1]) return t->vc_last - 1;
    const int ideal = hos_thread_core(t);
    if ((m >> ideal & 1) && !vc_owner[ideal]) return ideal;
    for (int c = 0; c < vc_cores(); c++)
        if ((m >> c & 1) && !vc_owner[c]) return c;
    return -1;
}
/* a core t may run on that was granted more than claim_ns ago and not taken up yet, or -1; holding vc_lock */
static int vc_unclaimed_for(HosThread *t, int64_t now, int64_t claim_ns) {
    const unsigned m = vc_allowed(t);
    int best = -1;
    for (int c = 0; c < vc_cores(); c++) {
        HosThread *o = vc_owner[c];
        if ((m >> c & 1) && o && o != t && o->vc_granted && now - o->vc_granted > claim_ns &&
            (best < 0 || o->vc_granted < vc_owner[best]->vc_granted))
            best = c;
    }
    return best;
}
static void vc_take(HosThread *t, int c) {
    vc_owner[c] = t;
    t->vc_held = c + 1;
    t->vc_last = c + 1;
    t->vc_kick = 0;
    t->vc_granted = 0;
    t->vc_starved = 0;
}
static void vc_dequeue(HosThread *t) {
    for (int i = 0; i < vc_nq; i++)
        if (vc_queue[i] == t) {
            vc_queue[i] = vc_queue[--vc_nq];
            break;
        }
    t->vc_queued = 0;
}
/* (HOS_CHAIN: a thread's wait for a core, and the grant that ends it; bit 63: never a guest address) */
static uint64_t vc_chain_key(HosThread *t) { return 1ULL << 63 | (uint64_t)(uintptr_t)t; }
/* core c is free: to the best waiter allowed on it; holding vc_lock */
static void vc_grant(int c) {
    HosThread *best = NULL;
    for (int i = 0; i < vc_nq; i++)
        if ((vc_allowed(vc_queue[i]) >> c & 1) && (!best || vc_better(vc_queue[i], best))) best = vc_queue[i];
    if (!best) return;
    vc_st.boosts += best->vc_starved;   /* (and the report's: VC_COUNT is defined below) */
    vc_rep.boosts += best->vc_starved;
    vc_dequeue(best);
    vc_take(best, c);
    best->vc_granted = hos_now_ns();
    HOS_CHAIN(HOS_CHAIN_SIGNAL, vc_chain_key(best));
    WakeConditionVariable(&best->vc_cv);
}
/* (the line is made here and written by the caller once the lock is let go: a write can block) */
static void vc_stats_locked(int64_t now, char *out, size_t cap) {
    if (!vc_st.since) vc_st.since = now;
    if (now - vc_st.since < 5000000000LL) return;
    snprintf(out, cap, "vcores: %lld takes, %lld waits (mean %.0f us, longest %.1f ms), %lld kicks, %lld yields, %lld starved grants, %lld escapes in %.0f s; "
            "1 ms steps %lld (longest %.1f ms, %lld over 10 ms), granted cores taken up %lld (longest after %.1f ms, %lld over "
            "10 ms), %lld stolen, lock waits %lld (longest %.1f ms); the lock held at most %.1f ms (let go at line %d), a waiting "
            "ticket took it after at most %.1f ms",
            (long long)vc_st.takes, (long long)vc_st.waits, vc_st.waits ? vc_st.wait_ns / 1e3 / vc_st.waits : 0.0,
            vc_st.max_ns / 1e6, (long long)vc_st.kicks, (long long)vc_st.yields, (long long)vc_st.boosts,
            (long long)vc_st.escapes, (now - vc_st.since) / 1e9, (long long)vc_st.steps, vc_st.step_max_ns / 1e6, (long long)vc_st.steps_over10,
            (long long)vc_st.claims, vc_st.claim_max_ns / 1e6, (long long)vc_st.claims_over10, (long long)vc_st.stolen,
            (long long)vc_st.lock_waits, vc_st.lock_max_ns / 1e6, vc_st.hold_max_ns / 1e6, vc_st.hold_line, vc_st.handoff_max_ns / 1e6);
    memset(&vc_st, 0, sizeof vc_st);
    vc_st.since = now;
}
/* (both the 5 s line's counters and the report's) */
#define VC_COUNT(field, v) (vc_st.field += (v), vc_rep.field += (v))
#define VC_MAX(field, v) do { const int64_t v_ = (v); if (v_ > vc_st.field) vc_st.field = v_; if (v_ > vc_rep.field) vc_rep.field = v_; } while (0)
static int64_t vc_env_ns(const char *name, int64_t us) {
    const char *e = getenv(name);
    return (e ? atoll(e) : us) * 1000;
}
static void vc_lock_timed(void) {
    if (vc_lk_try()) return;
    const int64_t l0 = hos_now_ns();
    vc_lk_acquire();
    VC_COUNT(lock_waits, 1);
    VC_MAX(lock_max_ns, hos_now_ns() - l0);
}
/* (ps5, 2026-11-17) How long an escape lasts on the console, HOOKSHOT_PS5_VCORE_ESCAPE:
     thread (the default)  until the thread ends, as the console played before the merge (every lot up to 42): an escaped
                           thread holds no core, hos_vcore_away answers 0 for it, and it never waits for one again. And
                           no starved grants unless HOS_VCORE_STARVE_US is set: a wait that long ends in an escape at
                           250 ms, as before.
     block                 upstream's rules, as the merged eboot ran them: the escape ends at the thread's next block
                           (#1929: c8b9a9f0, bce1476c) and a starved thread is granted a core at 125 ms (#594, adapted).
   Why: under the old rule the escapes of the game's start (dozens a run; lot 42's 33 in its first 25 s took the game's
   main thread and 8 of its 11 job workers) took those threads off the six virtual cores for the rest of the game, and the
   others shared the six with little contention: lot 42 in Kakariko, 530 waits for a core per 5 s. Under upstream's, every
   thread waits for one of the six after each of its blocks: 22,662 waits, 6.7 s of waiting per 5 s, and 27.0 fps against
   35.8 at the same places with the same GPU time (logs/udp/20261116-131147.log, 20261117-000427.log;
   notes-agents/coeurs-virtuels-fusion.md). Which threads escape depends on the start: the fewer of the busy ones, the more
   waits (a run that lost only one: 13,220 waits per 5 s). The price is #1929's: a thread with no core reports its ideal
   core's number while another thread may hold that core, and the two can share per-core state; the console ran so in
   every session before the merge.
   Elsewhere upstream's rules. (tests/test_ps5_vcore_escape_for_good.py) */
static int vc_escape_for_life(void) {
#ifdef __PROSPERO__
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HOOKSHOT_PS5_VCORE_ESCAPE");
        on = !(e && !strcmp(e, "block"));
    }
    return on;
#else
    return 0;
#endif
}
static void vc_acquire(HosThread *t) {
    static int64_t esc_better = -1, esc_eq, slice, claim, starve;
    if (esc_better < 0) {
#ifdef __PROSPERO__   /* (ps5: 250 ms, as the console has played; upstream's 2 s not tried there yet) */
        esc_eq = vc_env_ns("HOS_VCORE_ESCAPE_EQ_US", 250000);
        esc_better = vc_env_ns("HOS_VCORE_ESCAPE_US", 250000);
#else
        esc_eq = vc_env_ns("HOS_VCORE_ESCAPE_EQ_US", 2000000);
        esc_better = vc_env_ns("HOS_VCORE_ESCAPE_US", 2000000);
#endif
        slice = vc_env_ns("HOS_VCORE_SLICE_US", 2000);
#ifdef __PROSPERO__
        claim = vc_env_ns("HOS_VCORE_CLAIM_US", 2000);
        {   /* (#262 on the console: below its escape wait, or a starved thread escapes first and the rule never plays;
               half the shorter escape wait as upstream's 1 s is half its 2 s, at most 1 s: 125 ms with 250 ms. None under
               the console's own escape rule, vc_escape_for_life: the waits end in escapes, as before the merge) */
            const int64_t esc_min_us = (esc_eq < esc_better ? esc_eq : esc_better) / 1000;
            const int64_t half_us = esc_min_us / 2;
            starve = vc_env_ns("HOS_VCORE_STARVE_US", vc_escape_for_life() ? 0 : half_us < 1000000 ? half_us : 1000000);
        }
        {   /* (said once, before the first wait: which rule the run plays) */
            static int said;
            if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
                hos_log("vcores: an escape lasts %s, starved grants after %.0f ms (0: none), escapes after %.0f ms "
                        "(HOOKSHOT_PS5_VCORE_ESCAPE=%s; the other rule: =%s)",
                        vc_escape_for_life() ? "until the thread ends, as before the merge" : "until the thread's next block (upstream's #1929)",
                        starve / 1e6, (esc_eq < esc_better ? esc_eq : esc_better) / 1e6,
                        vc_escape_for_life() ? "thread" : "block", vc_escape_for_life() ? "block" : "thread");
        }
#else
        claim = vc_env_ns("HOS_VCORE_CLAIM_US", 0);   /* (a PC's scheduler runs a woken thread: as upstream) */
        starve = vc_env_ns("HOS_VCORE_STARVE_US", 1000000);
#endif
    }
    vc_lock_timed();
    VC_COUNT(takes, 1);
    int c = vc_free_for(t);
    if (c >= 0) {
        vc_take(t, c);
        vc_lk_release();
        vc_place(t);
        if (!(++t->vc_ntakes & 63)) t->vc_cpu = hos_cpu_now() + 1;
        return;
    }
    const int64_t t0 = hos_now_ns();
    char stats[800], escaped[440];   /* (lines made with the lock held, written after it) */
    stats[0] = escaped[0] = 0;
    HOS_CHAIN(HOS_CHAIN_WAIT, vc_chain_key(t));
    t->vc_since = t0;
    t->vc_queued = 1;
    if (vc_nq < 1024) vc_queue[vc_nq++] = t;
    VC_COUNT(waits, 1);
    for (;;) {
        /* the worst holder worse than t on a core t may use gives it up at its next checkpoint; after
           HOS_VCORE_SLICE_US (2000) of waiting, a holder as good as t does too (a time slice: Horizon would leave
           it running, but its threads never run two at once on one core, which the game's per-core data needs); once
           t is starved (HOS_VCORE_STARVE_US), the worst holder there is, whatever its priority. The holder sees the
           request at its next nn::os call or SVC, or at a loop header (the interrupt polls) */
        const unsigned m = vc_allowed(t);
        HosThread *worst = NULL;
        for (int k = 0; k < vc_cores(); k++) {
            HosThread *h = vc_owner[k];
            if ((m >> k & 1) && h && h->priority > t->priority && (!worst || h->priority > worst->priority)) worst = h;
        }
        if (!worst && hos_now_ns() - t0 > slice)
            for (int k = 0; k < vc_cores() && !worst; k++)
                if ((m >> k & 1) && vc_owner[k] && vc_owner[k]->priority == t->priority) worst = vc_owner[k];
        if (!worst && t->vc_starved)
            for (int k = 0; k < vc_cores(); k++) {
                HosThread *h = vc_owner[k];
                if ((m >> k & 1) && h && (!worst || h->priority > worst->priority)) worst = h;
            }
        if (worst && !worst->vc_kick && !worst->vc_granted) {
            worst->vc_kick = worst->priority > t->priority ? 1 : 2;
            Ctx *rc = worst->run_ctx;   /* (a fiber's context while it runs one) */
            __atomic_store_n(rc ? &rc->poll : &worst->ctx.poll, 1, __ATOMIC_RELAXED);
            VC_COUNT(kicks, 1);
        }
        const int64_t s0 = hos_now_ns();
        DWORD step_ms = 1;
#ifdef __PROSPERO__
        {
            static int64_t step_ns = -1;
            if (step_ns < 0) step_ns = vc_env_ns("HOS_VCORE_STEP_US", 10000);
            if (s0 - t0 > 4000000 && step_ns >= 2000000) step_ms = (DWORD)(step_ns / 1000000);
        }
#endif
        vc_lk_wait(&t->vc_cv, step_ms);
        const int64_t s1 = hos_now_ns();
        VC_COUNT(steps, 1);
        VC_MAX(step_max_ns, s1 - s0);
        if (s1 - s0 > 10000000) VC_COUNT(steps_over10, 1);
        if (t->vc_held) {   /* granted by a release */
            if (t->vc_granted) {
                VC_COUNT(claims, 1);
                VC_MAX(claim_max_ns, s1 - t->vc_granted);
                if (s1 - t->vc_granted > 10000000) VC_COUNT(claims_over10, 1);
                t->vc_granted = 0;
            }
            break;
        }
        if ((c = vc_free_for(t)) >= 0) {
            vc_dequeue(t);
            vc_take(t, c);
            break;
        }
        if (claim > 0 && (c = vc_unclaimed_for(t, s1, claim)) >= 0) {
            /* (granted to a thread that has not run since: t runs now, so t takes it, and that one waits again, first in
               line as it was: its priority and its vc_since are kept) */
            HosThread *g = vc_owner[c];
            g->vc_held = 0;
            g->vc_granted = 0;
            g->vc_queued = 1;
            if (vc_nq < 1024) vc_queue[vc_nq++] = g;
            vc_dequeue(t);
            vc_take(t, c);
            VC_COUNT(stolen, 1);
            break;
        }
        const int64_t waited = hos_now_ns() - t0;
        if (starve > 0 && waited > starve) t->vc_starved = 1;
        if (waited > (worst ? esc_better : esc_eq)) {
            vc_dequeue(t);
            t->vc_starved = 0;
            VC_COUNT(escapes, 1);
            t->vc_escaped = 1;   /* (#1929: it takes a core again at its next block, hos_vcore_away) */
            static int logged;
            if (logged < 40) {
                logged++;
                char who[256] = "";
                int len = 0;
                for (int k = 0; k < vc_cores() && len < 200; k++)
                    if (m >> k & 1)
                        len += snprintf(who + len, sizeof who - len, " core %d: %s (%d)%s;", k,
                                        vc_owner[k] ? vc_owner[k]->name : "-", vc_owner[k] ? vc_owner[k]->priority : 0,
                                        vc_owner[k] && vc_owner[k]->vc_kick ? " asked" :
                                        vc_owner[k] && vc_owner[k]->vc_granted ? " granted, not taken up" : "");
                snprintf(escaped, sizeof escaped, "vcores: %s (priority %d) ran without a core after %.1f ms:%s", t->name, t->priority,
                         waited / 1e6, who);
            }
            break;
        }
    }
    const int64_t waited = hos_now_ns() - t0;
    HOS_CHAIN(HOS_CHAIN_WOKE, vc_chain_key(t));
    VC_COUNT(wait_ns, waited);
    VC_MAX(max_ns, waited);
    t->vc_wait_ns += waited;
    vc_stats_locked(hos_now_ns(), stats, sizeof stats);
    vc_lk_release();
    if (escaped[0]) hos_log("%s", escaped);
    if (stats[0]) hos_log("%s", stats);
    vc_place(t);
    if (!(++t->vc_ntakes & 63)) t->vc_cpu = hos_cpu_now() + 1;
}
static void vc_release(HosThread *t) {
    if (!t->vc_held) return;
    vc_lk_acquire();
    const int c = t->vc_held - 1;
    vc_owner[c] = NULL;
    t->vc_held = 0;
    t->vc_kick = 0;
    vc_grant(c);
    vc_lk_release();
}
/* gives the core to a waiter allowed on it that is better than t (strict) or as good (a yield), or starved */
static void vc_yield(HosThread *t, int strict) {
    if (!t->vc_held) return;
    vc_lk_acquire();
    const int c = t->vc_held - 1;
    int any = 0;
    for (int i = 0; i < vc_nq && !any; i++) {
        HosThread *w = vc_queue[i];
        any = (vc_allowed(w) >> c & 1) && (w->vc_starved || (strict ? w->priority < t->priority : w->priority <= t->priority));
    }
    t->vc_kick = 0;
    if (any) VC_COUNT(yields, 1);
    vc_lk_release();
    if (!any) return;
    vc_release(t);
    vc_acquire(t);
}
/* 1 when the caller is to take a core again after its block (hos_vcore_back): it gave its core up, or it has run without
   one since an escape. An escape lasts until the thread's next block, as every other thread waits for a core after one;
   it took none again before, so an escaped thread ran with no core, and the core number of a core another thread held,
   for the rest of its life (issue 1929). (ps5: that is the console's rule, as before the merge, unless
   HOOKSHOT_PS5_VCORE_ESCAPE=block: vc_escape_for_life) */
int hos_vcore_away(void) {
    HosThread *t = cur;
    if (!hos_vcores_on() || !t) return 0;
    if (!t->vc_held) {
        /* (only the outermost block of nested ones takes the core again; none under the console's rule) */
        const int escaped = t->vc_escaped && !vc_escape_for_life();
        t->vc_escaped = 0;
        return escaped;
    }
    vc_release(t);
    vc_place(t);
    HOS_CPU_MARK(1);   /* (ps5, lot 40: on its way into a block, maybe one in the driver or a file read: HOOKSHOT_PS5_THREAD_TIMES) */
    return 1;
}
void hos_vcore_back(void) {
    HosThread *t = cur;
    HOS_CPU_MARK(0);
    if (!hos_vcores_on() || !t) return;
    /* after it the thread holds a core or has escaped again (vc_acquire sets the flag again): the escape the flag stood
       for has ended, also at a back without its away (a thread's start). A flag left up while a core is held would make
       a later inner block's away answer 1 and take a core in the middle of the outer block */
    t->vc_escaped = 0;
    if (!t->vc_held) vc_acquire(t);
}
/* Lifted code calls this from its interrupt polls with the loop's guest registers still in host registers (a64_poll_cur,
   mkw_glue.c: the poll hands over no context, so the compiler need not spill them). It, and everything it calls, must never
   read or write the thread's guest context (Ctx) or run guest code: what it would see there may be older than the loop. A
   stall dump of a thread spinning in a polled loop shows such older registers for the same reason. */
void hos_vcore_checkpoint(void) {
    HosThread *t = cur;
    if (t && t->vc_kick) vc_yield(t, t->vc_kick == 1);
}
void hos_vcore_yield(void) {
    HosThread *t = cur;
    if (hos_vcores_on() && t) vc_yield(t, 0);
}
/* SleepConditionVariableSRW for guest threads: the core given up for the wait; after it the lock is let go while a
   core is taken again (waiting for a core while holding it could stall the thread that will wake the next waiter),
   so callers see a spurious wake-up and look at their condition again, as they do anyway */
int hos_cv_sleep(void *cv, void *lk, unsigned ms) {
    const int away = hos_vcore_away();
    const BOOL r = SleepConditionVariableSRW((PCONDITION_VARIABLE)cv, (PSRWLOCK)lk, ms, 0);
    if (away) {
        ReleaseSRWLockExclusive((PSRWLOCK)lk);
        hos_vcore_back();
        AcquireSRWLockExclusive((PSRWLOCK)lk);
    }
    return r;
}
#else
int hos_vcores_on(void) { return 0; }
int hos_vcore_away(void) { return 0; }
void hos_vcore_back(void) {}
void hos_vcore_checkpoint(void) {}
void hos_vcore_yield(void) {}
int hos_cv_sleep(void *cv, void *lk, unsigned ms) {
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)cv, (PSRWLOCK)lk, ms, 0);
}
#endif

#ifdef __PROSPERO__
#include <pthread.h>
#include <sched.h>
/* ---- (ps5) the threads on the console's CPUs, and the scheduling report (2026-11-09: the game's first run crawled, a
   thread waiting 26 s for a virtual core whose limit is 250 ms: logs/game-run2) ----------------------------------------

   HOOKSHOT_PS5_AFFINITY:
     all (default)  every thread on the title's CPUs (measured at start: 0x1fff, 0-12); CPU 12 is kept for the renderer's
                    replay thread only when it runs (RX_VK_ASYNC=1). The 63c498e game kept it from the game's threads always
     reserve        as on a PC: the last core (12) for the replay thread, the game's threads on the rest
     pinned         (an experiment) a thread holding virtual core k runs on physical core k's first CPU (the title's pairs
                    are (0,1) (2,3) ... (10,11), 12 alone: PS5_Lichii's measure, ProsperoEden-cores.md), and on the other
                    CPUs (1,3,5,7,9,11,12) while it waits, is blocked or runs without a core, as are the threads
                    win32_posix.c starts. Two affinity calls a block (one to leave the core's CPU, one to come back);
                    RADV's own threads keep the CPUs of the thread that made them.
   HOOKSHOT_PS5_SCHED (host/posix/ps5_platform.c): the policy every thread is put on (auto: FIFO made RR).

   The report (HOOKSHOT_PS5_SCHED_REPORT_S, every 10 s; 0: none) is a thread of its own, on the same policy as the others:
   - how many CPUs the process used (its CPU time), the title's CPUs, the process's CPU sets;
   - each thread's CPU time over the period (busiest first), its policy, priority and CPU mask as the console reports
     them, and for the game's threads the Horizon priority, the virtual core held, the CPU it last ran on, its takes and
     how long it waited for a core;
   - how long its own 10 ms timed waits really took (a futex, as every condition variable here), and the virtual cores'
     1 ms waiting steps, the time a granted core waited to be taken up, the cores taken over and the escapes.
   Lines start with "sched:" in hookshot_log.txt; one in three (and the first) also go to the stage trail. */
static int ps5_affinity_value = -1;
static uint64_t ps5_title_cpus, ps5_shared_cpus;
static int ps5_core_cpu[8];   /* (pinned) virtual core k's CPU + 1, 0: none */
static int ps5_affinity_mode(void) {
    if (ps5_affinity_value >= 0) return ps5_affinity_value;
    const char *e = getenv("HOOKSHOT_PS5_AFFINITY");
    int m = 0;
    if (e && !strcasecmp(e, "reserve")) m = 1;
    else if (e && !strcasecmp(e, "pinned")) m = 2;
    hos_ps5_cpus(&ps5_title_cpus);   /* (the first caller's: main's or the game's thread, before any of ours is pinned) */
    if (m == 2) {
        int k = 0;
        for (int cpu = 0; cpu < 62 && k < 8; cpu += 2)
            if ((ps5_title_cpus >> cpu & 3) == 3) ps5_core_cpu[k++] = cpu + 1;
        uint64_t pinned = 0;
        for (int i = 0; i < k && i < vc_cores(); i++) pinned |= 1ull << (ps5_core_cpu[i] - 1);
        ps5_shared_cpus = ps5_title_cpus & ~pinned;
        if (!ps5_shared_cpus) ps5_shared_cpus = ps5_title_cpus;
        hos_ps5_new_thread_cpus = ps5_shared_cpus;   /* (win32_posix.c's new threads: not on the core of the thread that made them) */
    }
    ps5_affinity_value = m;
    return m;
}
/* (ps5, lot 66) The title's CPUs as the first caller of ps5_affinity_mode saw them (main's thread by stage 6 at the latest,
   before any thread of ours is pinned), for host/posix/win32_posix.c's HOOKSHOT_PS5_HELPERS_SHARED: a thread's own mask is
   its maker's on the console, so asking hos_ps5_cpus from a thread the render thread made answers the render thread's CPU */
uint64_t hos_ps5_title_cpus(void) {
    (void)ps5_affinity_mode();
    return ps5_title_cpus;
}
static const char *ps5_affinity_name(void) {
    static const char *const n[] = {"all", "reserve", "pinned"};
    return n[ps5_affinity_mode()];
}
static volatile int64_t ps5_place_calls, ps5_place_failures;
static void vc_place(HosThread *t) {
    if (ps5_affinity_mode() != 2 || !t->handle) return;
    static __thread uint64_t now_mask;
    const int c = t->vc_held - 1;
    const uint64_t want = c >= 0 && c < 8 && ps5_core_cpu[c] ? 1ull << (ps5_core_cpu[c] - 1) : ps5_shared_cpus;
    if (want == now_mask) return;
    now_mask = want;
    __atomic_add_fetch(&ps5_place_calls, 1, __ATOMIC_RELAXED);
    if (!SetThreadAffinityMask(t->handle, (DWORD_PTR)want)) __atomic_add_fetch(&ps5_place_failures, 1, __ATOMIC_RELAXED);
}

/* stage 6's line (hos_main.c): the policy, the CPUs, where the calling thread runs */
void hos_sched_describe(char *buf, size_t cap) {
    char note[400];
    hos_ps5_sched_note(note, sizeof note);
    uint64_t which = 0, root = 0;
    int which_error = 0, root_error = 0;
    hos_ps5_process_cpuset(&which, &which_error, &root, &root_error);
    const int mode = ps5_affinity_mode();
    int pol = -1, prio = 0;
    hos_ps5_sched_of((void *)pthread_self(), &pol, &prio);
    snprintf(buf, cap, "%s; CPUs: the title's %#llx (%d), the process's set %#llx (%d), root %#llx (%d); HOOKSHOT_PS5_AFFINITY=%s%s; "
                       "this thread %s %d on CPU %d",
             note, (unsigned long long)ps5_title_cpus, __builtin_popcountll(ps5_title_cpus), (unsigned long long)which, which_error,
             (unsigned long long)root, root_error, ps5_affinity_name(),
             mode == 2 ? " (cores on their first CPUs, the rest shared)" : hos_reserved_cpus() ? " (CPU 12 kept for the replay)" : "",
             hos_ps5_policy_name(pol), prio, hos_ps5_current_cpu());
}

typedef struct {
    void *handle;
    int64_t cpu_ns;
    unsigned takes;
    int64_t wait_ns;
} SchedPrev;
typedef struct {
    hos_posix_thread_info info;
    int64_t cpu_ns, d_cpu;
    int policy, prio;
    uint64_t mask;
    int guest, gprio, core, last_cpu;
    unsigned d_takes;
    int64_t d_wait;
} SchedRow;
static SchedPrev sched_prev[512];
static int sched_nprev;
static struct { int64_t n, max_ns, over5, over50, over1000; } sched_waits;
extern unsigned long rx_ps5_presents;

static void sched_report(int number, double period_s, int64_t process_d, int64_t process_all, int64_t uptime_ns) {
    static hos_posix_thread_info list[400];
    static SchedRow rows[400];
    const int64_t read0 = hos_now_ns();
    const int n = hos_posix_threads(list, 400);
    static SchedPrev next[512];
    int nnext = 0;
    double listed = 0, guest_cpus = 0;
    int pols[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; i++) {
        SchedRow *r = &rows[i];
        memset(r, 0, sizeof *r);
        r->info = list[i];
        r->cpu_ns = hos_ps5_thread_cpu_ns(list[i].pthread);
        hos_ps5_sched_of(list[i].pthread, &r->policy, &r->prio);
        pols[r->policy == SCHED_FIFO ? 0 : r->policy == SCHED_RR ? 1 : r->policy == SCHED_OTHER ? 2 : 3]++;
        r->mask = hos_ps5_affinity_of(list[i].pthread);
        r->core = -1;
        r->last_cpu = -1;
        r->d_cpu = -1;
        const SchedPrev *p = NULL;
        for (int k = 0; k < sched_nprev; k++)
            if (sched_prev[k].handle == list[i].handle) { p = &sched_prev[k]; break; }
        unsigned takes = 0;
        int64_t wait_ns = 0;
        AcquireSRWLockShared(&threads_lock);
        for (int k = 0; k < nthreads; k++)
            if (threads[k]->handle == list[i].handle) {
                HosThread *t = threads[k];
                r->guest = 1;
                r->gprio = t->priority;
                r->core = t->vc_held - 1;
                r->last_cpu = t->vc_cpu - 1;
                takes = t->vc_ntakes;
                wait_ns = t->vc_wait_ns;
                break;
            }
        ReleaseSRWLockShared(&threads_lock);
        if (r->cpu_ns >= 0) r->d_cpu = p && p->cpu_ns >= 0 ? r->cpu_ns - p->cpu_ns : r->cpu_ns;
        r->d_takes = p ? takes - p->takes : takes;
        r->d_wait = p ? wait_ns - p->wait_ns : wait_ns;
        if (r->d_cpu > 0) {
            listed += r->d_cpu / 1e9;
            if (r->guest) guest_cpus += r->d_cpu / 1e9;
        }
        if (nnext < 512) next[nnext++] = (SchedPrev){list[i].handle, r->cpu_ns, takes, wait_ns};
    }
    memcpy(sched_prev, next, sizeof(SchedPrev) * (size_t)nnext);
    sched_nprev = nnext;
    /* busiest first */
    for (int i = 1; i < n; i++) {
        SchedRow x = rows[i];
        int j = i;
        while (j > 0 && rows[j - 1].d_cpu < x.d_cpu) { rows[j] = rows[j - 1]; j--; }
        rows[j] = x;
    }
    const int64_t read_ns = hos_now_ns() - read0;
    VcStats v;
    vc_lk_acquire();
    v = vc_rep;
    memset(&vc_rep, 0, sizeof vc_rep);
    vc_lk_release();
    const int ncpus = __builtin_popcountll(ps5_title_cpus);
    const double used = process_d >= 0 && period_s > 0 ? process_d / 1e9 / period_s : -1;
    /* (the game's third run read exactly 1.00 every time from CLOCK_PROCESS_CPUTIME_ID while the threads listed used 0.15 to
       0.57: getrusage beside it, to tell which one counts) */
    static int64_t ru_last = -1;
    int64_t ru = -1;
    struct rusage u;
    if (!getrusage(RUSAGE_SELF, &u))
        ru = ((int64_t)u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000000000LL + ((int64_t)u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1000;
    const double ru_used = ru >= 0 && ru_last >= 0 && period_s > 0 ? (ru - ru_last) / 1e9 / period_s : -1;
    ru_last = ru;
    hos_log("sched: report %d at %.0f s: the process used %.2f CPUs over %.1f s by its CPU clock, %.2f by getrusage (%.1f s of CPU "
            "in all; the title has %d: %#llx); "
            "%d threads listed used %.2f (the game's %.2f); their policies: fifo %d, rr %d, other %d, unknown %d; %lu frames",
            number, uptime_ns / 1e9, used, period_s, ru_used, process_all / 1e9, ncpus, (unsigned long long)ps5_title_cpus, n,
            period_s > 0 ? listed / period_s : 0, period_s > 0 ? guest_cpus / period_s : 0, pols[0], pols[1], pols[2], pols[3],
            rx_ps5_presents);
    char busy[700];
    int len = 0;
    busy[0] = 0;
    for (int i = 0; i < n && i < 10 && len < 600; i++) {
        const SchedRow *r = &rows[i];
        char core[48] = "";
        if (r->guest) snprintf(core, sizeof core, " (core %d, cpu %d)", r->core, r->last_cpu);
        len += snprintf(busy + len, sizeof busy - len, "%s%s %.0f%%%s", i ? "; " : "", r->info.name,
                        r->d_cpu >= 0 && period_s > 0 ? r->d_cpu / 1e7 / period_s : -1.0, core);
    }
    hos_log("sched: busiest: %s", busy);
    {   /* (2026-11-10, the eleventh run: the presentation thread asleep for good at the first 3D scene, nothing said. Two
           reports with no new frame after frames were shown: the threads that make them are asked where they are) */
        static unsigned long frames_before;
        static int still, asked;
        int hos_posix_ask_where(const char *prefix);
        void rx_ps5_vk_gpu_trail_dump(const char *why) __attribute__((weak));   /* (gpu/vk/vk_gputrail.cpp; none: no Vulkan renderer) */
        if (rx_ps5_presents > 100 && rx_ps5_presents == frames_before) still++;
        else still = 0;
        frames_before = rx_ps5_presents;
        if (still >= 2 && asked < 2) {
            asked++;
            hos_ps5_trail("sched: no frame for %d reports (%lu shown): asking the presentation thread and the game's main thread where they are",
                          still, rx_ps5_presents);
            hos_posix_ask_where("Presentation Thread");
            hos_posix_ask_where("sead::MainThread");
            /* (the renderer's own threads are not asked: asking them ended the game in the fourth scene attempt, with no line;
               the queue's lock says who holds it instead: gpu/vk/vk_internal.h PresentMutex) */
            /* (2026-11-10) and what the GPU was given lately, from this thread: the renderer's GPU trail (RX_VK_GPU_TRAIL),
               which says in one line when it wrote it already for this stall */
            if (rx_ps5_vk_gpu_trail_dump) rx_ps5_vk_gpu_trail_dump("no frame for 20 s");
            still = 0;
        }
    }
    hos_log("sched: timed waits of 10 ms (the report's thread: a futex, as every condition variable): %lld, the longest %.1f ms, late by "
            "over 5 ms %lld, over 50 ms %lld, over 1 s %lld; virtual cores: 1 ms steps %lld (the longest %.1f ms, %lld over 10 ms), granted "
            "cores taken up %lld (the longest after %.1f ms, %lld over 10 ms), %lld stolen, %lld escapes, %lld takes, the scheduler's lock "
            "waited for %lld times (the longest %.1f ms), held at most %.1f ms (let go at host/hos_thread.c:%d), taken by a waiting "
            "ticket after at most %.1f ms%s",
            (long long)sched_waits.n, sched_waits.max_ns / 1e6, (long long)sched_waits.over5, (long long)sched_waits.over50,
            (long long)sched_waits.over1000, (long long)v.steps, v.step_max_ns / 1e6, (long long)v.steps_over10, (long long)v.claims,
            v.claim_max_ns / 1e6, (long long)v.claims_over10, (long long)v.stolen, (long long)v.escapes, (long long)v.takes,
            (long long)v.lock_waits, v.lock_max_ns / 1e6, v.hold_max_ns / 1e6, v.hold_line, v.handoff_max_ns / 1e6,
            ps5_affinity_mode() == 2 ? "; pinned: affinity calls made and refused below" : "");
    if (ps5_affinity_mode() == 2)
        hos_log("sched: pinned: %lld affinity calls, %lld refused; cores on CPUs %d %d %d %d %d %d, the shared %#llx", (long long)ps5_place_calls,
                (long long)ps5_place_failures, ps5_core_cpu[0] - 1, ps5_core_cpu[1] - 1, ps5_core_cpu[2] - 1, ps5_core_cpu[3] - 1,
                ps5_core_cpu[4] - 1, ps5_core_cpu[5] - 1, (unsigned long long)ps5_shared_cpus);
    {
        char lg[320];
        ps5_log_stats(lg, sizeof lg);
        hos_log("sched: the log: %s; this report's threads read in %.1f ms", lg, read_ns / 1e6);
        char po[900];
        hos_ps5_pthread_counts(po, sizeof po);
        hos_log("sched: the thread library's objects: %s", po);
        if (number == 1 || number % 6 == 0) hos_ps5_trail("sched: the thread library's objects at %.0f s: %s", uptime_ns / 1e9, po);
    }
    /* every thread: the first time, then every HOOKSHOT_PS5_SCHED_THREADS reports (30: 5 minutes; 0: never again) */
    static int threads_every = -1;
    if (threads_every < 0) {
        const char *te = getenv("HOOKSHOT_PS5_SCHED_THREADS");
        threads_every = te ? atoi(te) : 30;
    }
    if (number == 1 || (threads_every > 0 && number % threads_every == 0))
        for (int i = 0; i < n; i++) {
            const SchedRow *r = &rows[i];
            char g[160] = "";
            if (r->guest)
                snprintf(g, sizeof g, "; the game's: priority %d, core %d, last on CPU %d, %u takes, %.2f s waiting for a core", r->gprio,
                         r->core, r->last_cpu, r->d_takes, r->d_wait / 1e9);
            char cpu[48];
            if (r->d_cpu >= 0) snprintf(cpu, sizeof cpu, "%.1f%% (%.2f s in all)", period_s > 0 ? r->d_cpu / 1e7 / period_s : 0.0, r->cpu_ns / 1e9);
            else snprintf(cpu, sizeof cpu, "unknown (%lld)", (long long)r->cpu_ns);
            hos_log("sched:   %s (tid %d): CPU %s, %s %d, CPUs %#llx%s", r->info.name, r->info.tid, cpu, hos_ps5_policy_name(r->policy),
                    r->prio, (unsigned long long)r->mask, g);
        }
    if (number == 1 || number % 3 == 0) {
        char top[240];
        int tl = 0;
        top[0] = 0;
        for (int i = 0; i < n && i < 4 && tl < 200; i++)
            tl += snprintf(top + tl, sizeof top - tl, "%s%s %.0f%%", i ? ", " : "", rows[i].info.name,
                           rows[i].d_cpu >= 0 && period_s > 0 ? rows[i].d_cpu / 1e7 / period_s : -1.0);
        hos_ps5_trail("sched: %.0f s: %lu frames; the process used %.2f of %d CPUs (busiest: %s); 10 ms waits: the longest %.1f ms, %lld over "
                      "50 ms; virtual cores: steps up to %.1f ms, granted cores taken up after up to %.1f ms, %lld stolen, %lld escapes",
                      uptime_ns / 1e9, rx_ps5_presents, used, ncpus, top, sched_waits.max_ns / 1e6, (long long)sched_waits.over50,
                      v.step_max_ns / 1e6, v.claim_max_ns / 1e6, (long long)v.stolen, (long long)v.escapes);
    }
    memset(&sched_waits, 0, sizeof sched_waits);
}
static volatile uint32_t sched_watch_word;
static DWORD WINAPI sched_watch_main(void *p) {
    (void)p;
    const char *e = getenv("HOOKSHOT_PS5_SCHED_REPORT_S");
    const int every = e ? atoi(e) : 10;
    const int64_t start = hos_now_ns();
    int64_t last = start, process_last = hos_ps5_process_cpu_ns();
    int number = 0;
    for (;;) {
        const int64_t a = hos_now_ns();
        uint32_t same = 0;
        WaitOnAddress(&sched_watch_word, &same, sizeof same, 10);
        const int64_t late = hos_now_ns() - a - 10000000;
        sched_waits.n++;
        if (late + 10000000 > sched_waits.max_ns) sched_waits.max_ns = late + 10000000;
        if (late > 5000000) sched_waits.over5++;
        if (late > 50000000) sched_waits.over50++;
        if (late > 1000000000) sched_waits.over1000++;
        const int64_t now = hos_now_ns();
        {   /* (2026-11-10) the GPU trail's watcher (gpu/vk/vk_gputrail.cpp gt_poll): a tick a second. A wait for a submission
               that has lasted 5 s has the trail written from here; the waits themselves are not touched */
            void rx_ps5_vk_gpu_trail_poll(void) __attribute__((weak));
            static int64_t trail_ticked;
            if (rx_ps5_vk_gpu_trail_poll && now - trail_ticked >= 1000000000LL) {
                trail_ticked = now;
                rx_ps5_vk_gpu_trail_poll();
            }
        }
        if (now - last >= (int64_t)every * 1000000000LL) {
            const int64_t process = hos_ps5_process_cpu_ns();
            sched_report(++number, (now - last) / 1e9, process >= 0 && process_last >= 0 ? process - process_last : -1, process,
                         now - start);
            last = now;
            process_last = process;
        }
    }
    return 0;
}
/* (2026-11-10, after runs 13 and 14: the console itself went down at the first 3D scene, the second time with its storage.
   The best-supported cause is direct memory running out: 2.2 GiB were free during the intro, before the scene's own
   textures and pools, and the first launch's power-off was a memory overrun too. What the console does at zero is not
   known, so the title does not go there: a thread of its own reads the free direct memory every 50 ms, says it every 2 s
   in the log (which goes to the PC: HOOKSHOT_PS5_LOG_UDP), and under HOOKSHOT_PS5_MEMORY_FLOOR_MB (768; 0: no floor)
   writes why in the stage trail and ends the process at once, as an abort does (three aborts at that scene left the
   console up). HOOKSHOT_PS5_MEMORY_FLOOR_EXIT=shell: the shell is asked to close the title instead (slower: the other
   threads go on meanwhile). Flexible memory under 48 MiB ends it the same way. */
void rx_ps5_vk_memory_note(char *buf, size_t cap) __attribute__((weak));   /* (gpu/vk/vk_vma.cpp: VMA's heaps) */
static DWORD WINAPI memory_watch_main(void *p) {
    (void)p;
    const char *e = getenv("HOOKSHOT_PS5_MEMORY_FLOOR_MB");
    const uint64_t floor_mb = e ? strtoull(e, NULL, 0) : 768;
    const char *how = getenv("HOOKSHOT_PS5_MEMORY_FLOOR_EXIT");
    uint64_t lowest = ~0ULL, last_said = 0, direct = 0, flexible = 0, largest = 0, before = 0;
    int64_t said_at = 0, before_at = hos_now_ns();
    /* (2026-11-11) HOOKSHOT_PS5_HEAP_TRIM_S=<s>: every s seconds, dlmalloc's trim of the heap's arenas (host/posix/ps5_heap_pieces.c:
       an arena's segments that are all free, and the free top of its last one, go back to the direct memory); 0 or unset: never */
    const unsigned trim_every = hos_heap_trim_every();
    int64_t trimmed_at = hos_now_ns();
    uint64_t trimmed = 0, trims = 0;
    hos_log("memory: watching the free direct memory every 50 ms; the title ends itself under %llu MiB (HOOKSHOT_PS5_MEMORY_FLOOR_MB); "
            "the heap's blocks of pieces of %llu KiB and more (HOOKSHOT_PS5_HEAP_PIECES_KB, 0: none); the heap trimmed every %u s "
            "(HOOKSHOT_PS5_HEAP_TRIM_S, 0: never)",
            (unsigned long long)floor_mb, (unsigned long long)(hos_heap_pieces_min() >> 10), trim_every);
    for (;;) {
        Sleep(50);
        if (trim_every && hos_now_ns() - trimmed_at >= (int64_t)trim_every * 1000000000LL) {
            unsigned arenas = 0;
            uint64_t after = 0;
            const int64_t t0 = hos_now_ns();
            const uint64_t gave = hos_ps5_heap_trim_now(&arenas, &after);
            trimmed_at = hos_now_ns(), trims++, trimmed += gave;
            if (gave || trims == 1)
                hos_log("memory: the heap's trim gave back %llu MiB (%u arenas; the heap now %llu MiB; %llu MiB in %llu trims) in %.1f ms "
                        "(HOOKSHOT_PS5_HEAP_TRIM_S)",
                        (unsigned long long)(gave >> 20), arenas, (unsigned long long)(after >> 20), (unsigned long long)(trimmed >> 20),
                        (unsigned long long)trims, (trimmed_at - t0) / 1e6);
        }
        if (!hos_ps5_memory_free(&direct, &flexible, &largest)) continue;
        if (direct < lowest) lowest = direct;
        const int64_t now = hos_now_ns();
        if (now - said_at >= 2000000000LL || (last_said > direct && last_said - direct >= 256)) {
            /* (every 2 s, and at once when 256 MiB went since the last line) */
            uint64_t no_walk = 0;
            const uint64_t pieces = hos_ps5_memory_pieces(&no_walk);
            hos_log("memory: direct %llu MiB free (largest piece %llu, lowest so far %llu), flexible %llu MiB free; %+lld MiB in %.1f s; %lu frames; "
                    "%llu free pieces (%llu measures without their walk)",
                    (unsigned long long)direct, (unsigned long long)largest, (unsigned long long)lowest, (unsigned long long)flexible,
                    (long long)direct - (long long)before, (now - before_at) / 1e9, rx_ps5_presents, (unsigned long long)pieces,
                    (unsigned long long)no_walk);
            said_at = now, last_said = direct, before = direct, before_at = now;
            static int64_t where_at;
            if (now - where_at >= 10000000000LL) {   /* (every 10 s: by what took it, of the title's direct memory) */
                char wh[600];
                uint64_t total = hos_ps5_memory_total_mb();
                hos_ps5_memory_where(wh, sizeof wh, total > direct ? total - direct : 0);
                hos_log("memory: %llu MiB in use: %s", (unsigned long long)(total - direct), wh);
                {   /* (2026-11-11) the heap's blocks made of pieces and its refusals, once there is something to say */
                    char hp[400];
                    hos_ps5_heap_pieces_line(hp, sizeof hp);
                    if (hp[0]) hos_log("memory: the host heap's blocks of pieces: %s", hp);
                }
                if (rx_ps5_vk_memory_note) {
                    char vk[700];
                    rx_ps5_vk_memory_note(vk, sizeof vk);
                    if (vk[0]) hos_log("memory: the renderer's:%s", vk);
                }
                where_at = now;
            }
            static int64_t sites_at;
            if (now - sites_at >= 30000000000LL) {   /* (every 30 s: the direct memory by the site that allocated it, the heap's growth) */
                char st[1500];
                hos_ps5_direct_sites(st, sizeof st, 0, 6);
                hos_log("memory: direct by site (image: acc_words at %#lx): %s", (unsigned long)hos_ps5_sites_image(), st);
                hos_ps5_direct_sites(st, sizeof st, 6, 6);
                if (st[0]) hos_log("memory: direct by site, the next: %s", st);
                hos_ps5_direct_sites(st, sizeof st, 12, 6);
                if (st[0]) hos_log("memory: direct by site, the next: %s", st);
                hos_ps5_direct_sites(st, sizeof st, 18, 6);
                if (st[0]) hos_log("memory: direct by site, the next: %s", st);
                hos_ps5_direct_layout(st, sizeof st);
                hos_log("memory: direct by GiB, small/large MiB: %s", st);
                hos_ps5_heap_sites(st, sizeof st, 0, 6);
                hos_log("memory: heap growth by site: %s", st);
                hos_ps5_heap_sites(st, sizeof st, 6, 6);
                if (st[0]) hos_log("memory: heap growth by site, the next: %s", st);
                hos_ps5_heap_sites(st, sizeof st, 12, 6);
                if (st[0]) hos_log("memory: heap growth by site, the next: %s", st);
                hos_ps5_heap_sites(st, sizeof st, 18, 6);
                if (st[0]) hos_log("memory: heap growth by site, the next: %s", st);
                sites_at = now;
            }
        }
        if ((floor_mb && direct < floor_mb) || flexible < 48) {
            hos_ps5_trail("memory: STOPPING THE TITLE: direct %llu MiB free (floor %llu, largest piece %llu), flexible %llu MiB free, %lu frames "
                          "shown: the title ends itself before the console runs out (HOOKSHOT_PS5_MEMORY_FLOOR_MB)",
                          (unsigned long long)direct, (unsigned long long)floor_mb, (unsigned long long)largest, (unsigned long long)flexible,
                          rx_ps5_presents);
            {   /* (by what took it, and the renderer's heaps: what the last seconds added) */
                char wh[600], vk[700];
                hos_ps5_memory_where(wh, sizeof wh, hos_ps5_memory_total_mb() > direct ? hos_ps5_memory_total_mb() - direct : 0);
                hos_ps5_trail("memory: at the stop, %llu MiB in use: %s", (unsigned long long)(hos_ps5_memory_total_mb() - direct), wh);
                vk[0] = 0;
                if (rx_ps5_vk_memory_note) rx_ps5_vk_memory_note(vk, sizeof vk);
                if (vk[0]) hos_ps5_trail("memory: at the stop, the renderer's:%s", vk);
                if (hos_ps5_cold_mode()) {   /* (2026-11-12: the cold units, HOOKSHOT_PS5_COLD) */
                    char cl[1500];
                    hos_ps5_cold_line(cl, sizeof cl);
                    hos_ps5_trail("memory: at the stop, cold units: %s", cl);
                }
            }
            if (how && !strcmp(how, "shell")) hos_ps5_exit(3);
            abort();
        }
    }
    return 0;
}
/* (2026-11-10) what the window's backed units hold, every 30 s from the first minute, on a thread of its own: the read of a
   unit that is all zeros takes its 64 KiB, and the memory watch is not to wait behind gigabytes of them.
   Then the units that are all zeros go back behind the shared zeros (host/posix/ps5_window.c hos_ps5_guest_reclaim: about
   400 MiB of them in play, which the count above found the same through a whole session). One pass after each count, so
   30 s apart at least: the window counts a commit's hold in passes. A line a pass: what it looked at, put back, left, what
   was given memory again since the pass before, how long it took and the longest it held a GiB's lock for one unit (a fault
   in that GiB waits that long), and the totals. No pass unless HOOKSHOT_PS5_ZERO_RECLAIM=1 (2026-11-12); HOOKSHOT_PS5_MEMORY_CONTENTS=0: no
   count (the passes go on) */
static void zero_reclaim_pass(void) {
    static int off_said;
    struct hos_ps5_guest_reclaim r;
    if (!hos_ps5_guest_reclaim(&r)) {
        if (!off_said++) {
            char why[96];
            snprintf(why, sizeof why, "the kernel refused step %d of it (%#x)", r.error_step, (unsigned)r.error);
            hos_log("memory: zero units: no reclaim: %s; %llu MiB put back before (HOOKSHOT_PS5_ZERO_RECLAIM)",
                    r.state == HOS_PS5_RECLAIM_OFF ? "off (HOOKSHOT_PS5_ZERO_RECLAIM=1 turns it on)"
                    : r.state == HOS_PS5_RECLAIM_NO_ZEROS ? "the shared zeros are off (HOOKSHOT_PS5_ZERO_READS)"
                    : r.state == HOS_PS5_RECLAIM_NO_PINS ? "no pins (HOOKSHOT_PS5_GPU_PIN=0): the window does not know what the GPU reaches"
                    : r.state == HOS_PS5_RECLAIM_REFUSED ? why : "another pass is running",
                    (unsigned long long)(r.total_reclaimed >> 4));
            if (r.state == HOS_PS5_RECLAIM_REFUSED)
                hos_ps5_trail("memory: zero units: the reclaim turned itself off: the kernel refused step %d of it (%#x: 1 read-only, 2 read-write "
                              "again, 3 the zeros' mapping, 4 still writable after the read-only protection)", r.error_step, (unsigned)r.error);
        }
        return;
    }
    hos_log("memory: zero units: pass %llu looked at %llu backed units (%llu pinned, %llu left alone after a commit or a write) and found %llu "
            "all zeros: %llu MiB put back behind the shared zeros (%llu units; %llu written meanwhile and kept, %llu changed meanwhile, %llu "
            "not the CPU's alone, %llu of them unanswered); %llu MiB given memory again since the last pass (%llu units); %.1f ms, the longest "
            "lock %.0f us; %llu free pieces of direct memory%s; so far %llu MiB put back, %llu MiB given memory again (%llu units, %llu of them "
            "soon after), %llu MiB behind the shared zeros now",
            (unsigned long long)r.pass, (unsigned long long)r.looked, (unsigned long long)r.pinned, (unsigned long long)r.held,
            (unsigned long long)r.zeros, (unsigned long long)(r.reclaimed >> 4), (unsigned long long)r.reclaimed,
            (unsigned long long)r.written, (unsigned long long)r.changed, (unsigned long long)r.foreign, (unsigned long long)r.unanswered,
            (unsigned long long)(r.rebacked >> 4), (unsigned long long)r.rebacked, r.ns / 1e6, r.lock_ns_max / 1e3,
            (unsigned long long)r.pieces, r.crowded ? " (STOPPED THERE: too many, or a walk of them failed)" : "",
            (unsigned long long)(r.total_reclaimed >> 4), (unsigned long long)(r.total_rebacked >> 4), (unsigned long long)r.total_rebacked,
            (unsigned long long)r.total_flaps, (unsigned long long)(r.zero_units >> 4));
    if (r.pass <= 3) {   /* (2026-11-12: the first passes in the stage trail too, which is synced: what the first ever said stays if the run ends) */
        char mem[200];
        hos_ps5_trail("memory: zero units: pass %llu: %llu MiB put back of %llu found all zeros (%llu pinned met, %llu not the CPU's alone, %llu "
                      "unanswered), %llu units given memory again since; %.1f ms, the longest lock %.0f us; %llu free pieces%s%s",
                      (unsigned long long)r.pass, (unsigned long long)(r.reclaimed >> 4), (unsigned long long)r.zeros, (unsigned long long)r.pinned,
                      (unsigned long long)r.foreign, (unsigned long long)r.unanswered, (unsigned long long)r.rebacked, r.ns / 1e6,
                      r.lock_ns_max / 1e3, (unsigned long long)r.pieces, r.crowded ? " (stopped there)" : "", hos_ps5_trail_memory(mem, sizeof mem));
    }
    if (r.error_step && !off_said++) {   /* (this pass met the refusal: said at once, the passes stop here) */
        hos_log("memory: zero units: the kernel refused step %d of the reclaim (%#x): no more passes", r.error_step, (unsigned)r.error);
        hos_ps5_trail("memory: zero units: the reclaim turned itself off: the kernel refused step %d of it (%#x: 1 read-only, 2 read-write "
                      "again, 3 the zeros' mapping, 4 still writable after the read-only protection)", r.error_step, (unsigned)r.error);
    }
}
static DWORD WINAPI memory_contents_main(void *p) {
    (void)p;
    const char *c = getenv("HOOKSHOT_PS5_MEMORY_CONTENTS");
    const int count = !c || atoi(c);
    Sleep(60000);
    for (;;) {
        if (count) {
            char by[1600];
            uint64_t backed = 0, zeros = 0, same = 0;
            const int64_t t0 = hos_now_ns();
            hos_ps5_guest_contents(by, sizeof by, &backed, &zeros, &same);
            hos_log("memory: the window's backed units: %llu MiB, all zeros %llu MiB, one word repeated %llu MiB (read in %.0f ms); by GiB "
                    "of the window, backed/zeros/repeated: %s", (unsigned long long)backed, (unsigned long long)zeros,
                    (unsigned long long)same, (hos_now_ns() - t0) / 1e6, by);
        }
        zero_reclaim_pass();
        Sleep(30000);
    }
    return 0;
}
/* (2026-11-12) Cold units on the SSD (HOOKSHOT_PS5_COLD=measure or 1: host/posix/ps5_window.c's header). A thread of its own,
   as its writes may wait on the SSD: every 100 ms, whether the game is calm (HOOKSHOT_PS5_COLD_MIN_FPS (20) frames a second
   or more over the last 2 s, and the window grew HOOKSHOT_PS5_COLD_GROWTH_MB (32) or less in them: a load shows a frame a
   second and grows the window by hundreds of MiB) and the free direct memory as last measured go to hos_ps5_cold_tick; a line
   every 30 s (the first three in the stage trail too); measure: the SSD probe once, two minutes in, while calm */
static DWORD WINAPI memory_cold_main(void *p) {
    (void)p;
    Sleep(5000);   /* (after the first frames: the renderer's first pin reads HOOKSHOT_PS5_GPU_PIN) */
    char said[600];
    const int mode = hos_ps5_cold_start(said, sizeof said);
    hos_log("memory: cold units: %s (HOOKSHOT_PS5_COLD)", said);
    /* (in the trail when off too: the thread runs only when the setting asked for it, so off is a value not understood, no pins, no
       window or no address space, which the person who set it wants to see) */
    hos_ps5_trail("memory: cold units: %s (HOOKSHOT_PS5_COLD)", said);
    if (!mode) return 0;
    const char *f = getenv("HOOKSHOT_PS5_COLD_MIN_FPS"), *g = getenv("HOOKSHOT_PS5_COLD_GROWTH_MB");
    const unsigned long min_fps = f && *f ? strtoul(f, NULL, 0) : 20;
    const uint64_t growth_mb = g && *g ? strtoull(g, NULL, 0) : 32;
    enum { RING = 20 };   /* (2 s of 100 ms steps) */
    unsigned long frames[RING] = {0};
    uint64_t window[RING] = {0};
    unsigned at = 0, filled = 0, lines = 0, calm_now = 0;
    int probed = mode != HOS_PS5_COLD_MEASURE;
    const int64_t t0 = hos_now_ns();
    int64_t said_at = t0;
    for (;;) {
        Sleep(100);
        struct hos_ps5_guest_stats w;
        hos_ps5_guest_stats(&w);
        const unsigned long fr = __atomic_load_n(&rx_ps5_presents, __ATOMIC_RELAXED);
        const unsigned old = (at + 1) % RING;   /* (the sample 2 s back, once the ring is full) */
        frames[at] = fr, window[at] = w.backed_bytes, at = old;
        if (filled < RING) filled++;
        const int full = filled == RING;
        const unsigned long fps = full ? (fr - frames[old]) / 2 : 0;
        const uint64_t grew = full && w.backed_bytes > window[old] ? (w.backed_bytes - window[old]) >> 20 : 0;
        const int calm = full && fps >= min_fps && grew <= growth_mb;
        calm_now = (unsigned)calm;
        const int64_t free_est = __atomic_load_n(&hos_ps5_free_estimate, __ATOMIC_RELAXED);
        hos_ps5_cold_tick(calm, free_est > 0 ? (uint64_t)free_est >> 20 : 0);
        const int64_t now = hos_now_ns();
        if (!probed && calm && now - t0 >= 120000000000LL) {
            char pr[1200];
            probed = 1;
            if (hos_ps5_cold_probe(pr, sizeof pr)) {
                hos_log("memory: cold units: %s", pr);
                hos_ps5_trail("memory: cold units: %s", pr);
            }
        }
        if (now - said_at >= 30000000000LL) {
            char cl[1500];
            hos_ps5_cold_line(cl, sizeof cl);
            hos_log("memory: cold units: %s; now %s (%lu frames a second, the window grew %llu MiB in 2 s)", cl, calm_now ? "calm" : "busy",
                    fps, (unsigned long long)grew);
            if (++lines <= 3) hos_ps5_trail("memory: cold units: %s", cl);
            said_at = now;
        }
    }
    return 0;
}
void hos_sched_watch_start(void) {
    /* (2026-11-16, lot 40) the per-thread CPU report, only with HOOKSHOT_PS5_THREAD_TIMES=<seconds>: host/hos_thread_times.c. The
       "sched:" report's per-thread figures stay as they were (wrong on the console: pthread_getcpuclockid's caller's clock) */
    hos_thread_times_start();
    {
        static int started;
        if (!started++) {
            HANDLE m = CreateThread(NULL, 1u << 18, memory_watch_main, NULL, 0, NULL);
            if (m) SetThreadDescription(m, L"hookshot memory");
            {   /* (2026-11-12: the cold units' thread, only when HOOKSHOT_PS5_COLD asks for them) */
                const char *c = getenv("HOOKSHOT_PS5_COLD");
                if (c && c[0] && strcmp(c, "0") && strcmp(c, "off")) {
                    HANDLE k = CreateThread(NULL, 1u << 18, memory_cold_main, NULL, 0, NULL);
                    if (k) SetThreadDescription(k, L"hookshot cold");
                }
            }
            /* (the count of what the window holds, and the reclaim of its zero units after each: the thread unless both are off) */
            const char *c = getenv("HOOKSHOT_PS5_MEMORY_CONTENTS"), *z = getenv("HOOKSHOT_PS5_ZERO_RECLAIM");
            if (!c || atoi(c) || (z && z[0] && z[0] != '0')) {
                HANDLE k = CreateThread(NULL, 1u << 18, memory_contents_main, NULL, 0, NULL);
                if (k) SetThreadDescription(k, L"hookshot contents");
            }
        }
    }
    const char *e = getenv("HOOKSHOT_PS5_SCHED_REPORT_S");
    if (e && atoi(e) <= 0) return;
    HANDLE h = CreateThread(NULL, 1u << 20, sched_watch_main, NULL, 0, NULL);
    if (h) SetThreadDescription(h, L"hookshot sched");
}
#endif
