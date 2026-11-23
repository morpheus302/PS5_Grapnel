/* (linux) The Win32 calls of host/posix/windows.h, on POSIX. Built into the executable and, on its own, into
   hookshot_vk.so (each its own copy, -Bsymbolic: the executable exports nothing of it).
   (ps5) The same file on the console (RX_TARGET=ps5, __PROSPERO__): one copy in eboot.bin for the runtime and the renderer.
   Where Linux has a system call, a /proc file or dlopen the console lacks, the __PROSPERO__ branches call host/posix/ps5.h
   (ps5_platform.c). */
#define _GNU_SOURCE
#include <windows.h>
#include <mmsystem.h>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include "../hos_thread_times.h"   /* (HOS_CPU_MARK: the threads' own CPU times on the console, host/hos_thread_times.c; nothing elsewhere) */
#ifdef __PROSPERO__
#include "ps5.h"
#include <ps5platform/context.h>   /* (the console's ucontext_t: 48 bytes more before uc_mcontext than FreeBSD's) */
#include <ps5platform/libc.h>      /* (ps5_pthread_[gs]etaffinity_np: libkernel answers ERANGE to a 32-byte cpuset_t) */
#include <pthread_np.h>
#include <sys/cpuset.h>
#else
#include <linux/futex.h>
#endif
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#ifdef __PROSPERO__   /* (the file calls below through ps5platform/fdv.h: more than the console's 249 files open, ps5_files.c) */
#include "ps5_fdv_hooks.h"
#include "ps5_helpers.h"   /* (lot 66: HOOKSHOT_PS5_HELPERS_SHARED, the render thread's helpers off its CPU) */
#endif

/* the guest window (third_party/a64-kit/rt/a64.h A64_HOST_BASE, host/hos.h HOS_GUEST_SPACE): one mapping, all of it
   readable and writable, which VirtualQuery reports as committed without asking the kernel */
#ifdef A64_HOST_BASE   /* (ps5: 0x80_0000_0000, given by nx2/build.py: the console grants nothing at or above 1 TiB) */
#define GUEST_LO ((uint64_t)A64_HOST_BASE)
#else
#define GUEST_LO 0x100000000000ULL
#endif
#define GUEST_HI (GUEST_LO + (1ULL << 39))

/* ---- errors ---- */
static __thread DWORD last_error;
DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD e) { last_error = e; }
static DWORD error_of(int e) {
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case EACCES: case EPERM: case EISDIR: case EROFS: return ERROR_ACCESS_DENIED;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ETIMEDOUT: return ERROR_TIMEOUT;
    default: return ERROR_INVALID_PARAMETER;
    }
}
static BOOL fail_errno(void) { last_error = error_of(errno); return FALSE; }

/* ---- handles ---- */
enum { H_THREAD = 0x48540001, H_EVENT, H_SEMA, H_TIMER, H_FILE, H_FIND };
typedef struct { int type; } HObj;
static int is(HANDLE h, int type) { return h && h != INVALID_HANDLE_VALUE && (uintptr_t)h > 0x10000 && ((HObj *)h)->type == type; }

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static struct timespec abs_after_ms(DWORD ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
    return ts;
}
static void monitor_init(pthread_mutex_t *mu, pthread_cond_t *cv) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &a);
    pthread_condattr_destroy(&a);
    pthread_mutex_init(mu, NULL);
}
/* waits for pred() under mu, at most ms: WAIT_OBJECT_0 or WAIT_TIMEOUT */
#define MONITOR_WAIT(mu, cv, pred, ms, result)                                                                        \
    do {                                                                                                              \
        struct timespec until_ = abs_after_ms(ms);                                                                    \
        (result) = WAIT_OBJECT_0;                                                                                     \
        while (!(pred)) {                                                                                             \
            HOS_CPU_MARK(1);                                                                                          \
            if ((ms) == INFINITE) pthread_cond_wait((cv), (mu));                                                      \
            else if (pthread_cond_timedwait((cv), (mu), &until_) == ETIMEDOUT && !(pred)) { HOS_CPU_MARK(0); (result) = WAIT_TIMEOUT; break; } \
            HOS_CPU_MARK(0);                                                                                          \
        }                                                                                                             \
    } while (0)

/* ---- futexes: condition variables, WaitOnAddress ---- */
#ifdef __PROSPERO__   /* (FreeBSD's _umtx_op in Linux's futex shape: ps5_platform.c) */
enum { FUTEX_WAIT_PRIVATE = 128, FUTEX_WAKE_PRIVATE = 129 };
static long futex(volatile uint32_t *a, int op, uint32_t v, const struct timespec *ts) {
    if (op != FUTEX_WAIT_PRIVATE) return hos_ps5_futex_wake(a, (int)(v > INT_MAX ? INT_MAX : v));
    /* (2026-11-16, lot 40: every wait here publishes the thread's own CPU time for HOOKSHOT_PS5_THREAD_TIMES; off, a load) */
    HOS_CPU_MARK(1);
    const long r = hos_ps5_futex_wait(a, v, ts);
    HOS_CPU_MARK(0);
    return r;
}
#else
static long futex(volatile uint32_t *a, int op, uint32_t v, const struct timespec *ts) {
    return syscall(SYS_futex, a, op, v, ts, NULL, 0);
}
#endif
#ifdef __PROSPERO__
#define isatty ps5_isatty   /* (libc's isatty is only libScePosixForWebKit's: the platform's, which says there are no terminals) */
#define posix_tid() ((pid_t)hos_ps5_tid())
#else
#define posix_tid() ((pid_t)syscall(SYS_gettid))
#endif
static struct timespec rel_ms(DWORD ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    return ts;
}
void WakeConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST);
    futex(&cv->seq, FUTEX_WAKE_PRIVATE, 1, NULL);
}
void WakeAllConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST);
    futex(&cv->seq, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);
}
static BOOL cv_wait(CONDITION_VARIABLE *cv, DWORD ms, void (*unlock)(void *), void (*relock)(void *, ULONG), void *l, ULONG flags) {
    const uint32_t seq = __atomic_load_n(&cv->seq, __ATOMIC_SEQ_CST);
    unlock(l);
    struct timespec ts = rel_ms(ms);
    const long r = futex(&cv->seq, FUTEX_WAIT_PRIVATE, seq, ms == INFINITE ? NULL : &ts);
    const int timed_out = r == -1 && errno == ETIMEDOUT;
    relock(l, flags);
    if (timed_out) { last_error = ERROR_TIMEOUT; return FALSE; }
    return TRUE;
}
#ifdef __PROSPERO__
/* (the SRW lock's waits: host/posix/windows.h says why it is a word here. A short spin first: the locks are held for
   microseconds; then the waiter marks the word and sleeps on it, and the release that finds the mark wakes everyone) */
void hos_srw_lock_slow(SRWLOCK *l, int shared) {
    /* (2026-11-10: the scene's first frame shown, then every thread asleep: a wait of 5 s and more for one of these locks
       says so once in the stage trail, with the lock, its word and who asked: a lock taken twice by its own thread, which
       pthread's read-write lock answered with an error nobody read, waits for good here) */
    int64_t waiting_since = 0;
    int told = 0;
    for (int spin = 0;; spin++) {
        if (shared ? TryAcquireSRWLockShared(l) : TryAcquireSRWLockExclusive(l)) return;
        if (spin < 100) {
            __builtin_ia32_pause();
            continue;
        }
        uint32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (!s || (shared && !(s & HOS_SRW_WRITER))) continue;   /* (free for this caller now) */
        if (!(s & HOS_SRW_WAIT) &&
            !__atomic_compare_exchange_n(&l->state, &s, s | HOS_SRW_WAIT, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            continue;
        if (!waiting_since) waiting_since = now_ns();
        const struct timespec second = {1, 0};
        futex(&l->state, FUTEX_WAIT_PRIVATE, s | HOS_SRW_WAIT, &second);
        if (!told && now_ns() - waiting_since > 5000000000LL) {
            static volatile int lines;
            told = 1;
            if (__atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) < 16)
                hos_ps5_trail("srw: thread %d has waited 5 s for lock %p (%s; its word %#x: bit 31 held exclusively, bit 30 waiters, "
                              "the rest readers); asked from %p (image: hos_srw_lock_slow at %p)",
                              hos_ps5_tid(), (void *)l, shared ? "shared" : "exclusive", __atomic_load_n(&l->state, __ATOMIC_RELAXED),
                              __builtin_return_address(0), (void *)&hos_srw_lock_slow);
        }
    }
}
void hos_srw_wake(SRWLOCK *l) { futex(&l->state, FUTEX_WAKE_PRIVATE, INT_MAX, NULL); }
static void srw_unlock(void *l) { hos_srw_unlock((SRWLOCK *)l); }
static void srw_relock(void *l, ULONG flags) {
    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) AcquireSRWLockShared((SRWLOCK *)l);
    else AcquireSRWLockExclusive((SRWLOCK *)l);
}
#else
static void srw_unlock(void *l) { pthread_rwlock_unlock((pthread_rwlock_t *)l); }
static void srw_relock(void *l, ULONG flags) {
    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) pthread_rwlock_rdlock((pthread_rwlock_t *)l);
    else pthread_rwlock_wrlock((pthread_rwlock_t *)l);
}
#endif
static void cs_unlock(void *l) { pthread_mutex_unlock((pthread_mutex_t *)l); }
static void cs_relock(void *l, ULONG flags) { (void)flags; pthread_mutex_lock((pthread_mutex_t *)l); }
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags) {
    return cv_wait(cv, ms, srw_unlock, srw_relock, l, flags);
}
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms) {
    return cv_wait(cv, ms, cs_unlock, cs_relock, cs, 0);
}
void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &a);
    pthread_mutexattr_destroy(&a);
}
/* (any size: the futex is the aligned 32-bit word holding the address, woken by any change near it) */
BOOL WaitOnAddress(volatile void *addr, void *compare, SIZE_T size, DWORD ms) {
    volatile uint32_t *w = (volatile uint32_t *)((uintptr_t)addr & ~(uintptr_t)3);
    const uint32_t seen = *w;
    if (memcmp((const void *)addr, compare, size)) return TRUE;
    struct timespec ts = rel_ms(ms);
    const long r = futex(w, FUTEX_WAIT_PRIVATE, seen, ms == INFINITE ? NULL : &ts);
    if (r == -1 && errno == ETIMEDOUT) { last_error = ERROR_TIMEOUT; return FALSE; }
    return TRUE;
}
void WakeByAddressSingle(void *addr) { futex((volatile uint32_t *)((uintptr_t)addr & ~(uintptr_t)3), FUTEX_WAKE_PRIVATE, INT_MAX, NULL); }
void WakeByAddressAll(void *addr) { futex((volatile uint32_t *)((uintptr_t)addr & ~(uintptr_t)3), FUTEX_WAKE_PRIVATE, INT_MAX, NULL); }

/* ---- time ---- */
void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec ts = rel_ms(ms);
    HOS_CPU_MARK(1);
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
    HOS_CPU_MARK(0);
}
void GetSystemTimeAsFileTime(FILETIME *ft) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t t = ((uint64_t)ts.tv_sec + 11644473600ULL) * 10000000ULL + (uint64_t)ts.tv_nsec / 100;
    ft->dwLowDateTime = (DWORD)t, ft->dwHighDateTime = (DWORD)(t >> 32);
}
typedef struct { HObj h; int64_t due; } HTimer;
HANDLE CreateWaitableTimerExW(void *attr, LPCWSTR name, DWORD flags, DWORD access) {
    (void)attr, (void)name, (void)flags, (void)access;
    HTimer *t = (HTimer *)calloc(1, sizeof *t);
    t->h.type = H_TIMER;
    return t;
}
BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, void *ctx, ULONG delay) {
    (void)period, (void)fn, (void)arg, (void)ctx, (void)delay;
    if (!is(h, H_TIMER)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    /* (relative times only: negative, in 100 ns units) */
    ((HTimer *)h)->due = now_ns() + (due->QuadPart < 0 ? -due->QuadPart * 100 : 0);
    return TRUE;
}

/* ---- threads ---- */
typedef struct {
    HObj h;
    pthread_t th;
    volatile pid_t tid;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done, priority;
    int pinned;   /* (SetThreadAffinityMask set its affinity: thread_entry leaves it; under mu) */
    DWORD code;
    volatile int refs;
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    char name[64];
#ifdef __PROSPERO__
    struct hos_cpu_slot cpu;   /* (its own CPU time as it last published it: HOOKSHOT_PS5_THREAD_TIMES, host/hos_thread_times.c) */
#endif
} HThread;
static __thread HThread *self_thread;
static void thread_unref(HThread *t) {
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        pthread_mutex_destroy(&t->mu);
        pthread_cond_destroy(&t->cv);
        free(t);
    }
}
static HThread *thread_new(void) {
    HThread *t = (HThread *)calloc(1, sizeof *t);
    t->h.type = H_THREAD;
    monitor_init(&t->mu, &t->cv);
    return t;
}
#ifdef __PROSPERO__
/* (ps5) Every thread this file started or adopted, for the scheduling report (host/hos_thread.c, hos_posix_threads): each
   entry holds a reference, dropped once the thread has ended */
#define REGISTRY_MAX 512
static HThread *registry[REGISTRY_MAX];
static int registry_n;
static pthread_mutex_t registry_mu = PTHREAD_MUTEX_INITIALIZER;
static void registry_add(HThread *t) {
    pthread_mutex_lock(&registry_mu);
    int i = 0;
    while (i < registry_n && registry[i] && !registry[i]->done) i++;
    if (i < REGISTRY_MAX) {
        if (i < registry_n && registry[i]) thread_unref(registry[i]);   /* (an ended thread's place) */
        __atomic_add_fetch(&t->refs, 1, __ATOMIC_ACQ_REL);
        registry[i] = t;
        if (i == registry_n) registry_n++;
    }
    pthread_mutex_unlock(&registry_mu);
}
#ifdef __PROSPERO__
/* (the scheduling report's watchdog, host/hos_thread.c: SIGUSR2 to each live thread whose name starts with `prefix`; its
   handler writes where the thread is to the stage trail) */
int hos_posix_ask_where(const char *prefix) {
    int n = 0;
    pthread_mutex_lock(&registry_mu);
    for (int i = 0; i < registry_n; i++) {
        HThread *t = registry[i];
        if (t && !t->done && !strncmp(t->name, prefix, strlen(prefix)) && !pthread_kill(t->th, SIGUSR2)) n++;
    }
    pthread_mutex_unlock(&registry_mu);
    return n;
}
#endif
/* (2026-11-16, lot 40) The per-thread CPU report's (host/hos_thread_times.c, HOOKSHOT_PS5_THREAD_TIMES): the live threads with
   the CPU time each last published in its slot. The slots are read without their threads' help: each writes its own, with a
   sequence count, and takes no lock for it */
int hos_posix_thread_times(hos_thread_time_row *out, int max) {
    int n = 0;
    pthread_mutex_lock(&registry_mu);
    for (int i = 0; i < registry_n; i++) {
        HThread *t = registry[i];
        if (!t) continue;
        if (t->done) {
            thread_unref(t);
            registry[i] = NULL;
            continue;
        }
        if (n < max) {
            out[n].tid = t->tid;
            out[n].pthread = (void *)t->th;
            memcpy(out[n].name, t->name, sizeof out[n].name);
            out[n].name[sizeof out[n].name - 1] = 0;
            hos_cpu_slot_read(&t->cpu, &out[n].cpu);
            n++;
        }
    }
    pthread_mutex_unlock(&registry_mu);
    return n;
}
/* the calling thread's own CPU time into its slot (HOS_CPU_MARK, only once the report turned the marks on): nothing for a
   thread this file did not start or adopt (RADV's), or that has ended; errno as it was (a wait's caller reads it after) */
void hos_ps5_cpu_mark(int blocking) {
    HThread *t = self_thread;
    if (!t || t->done) return;
    const int e = errno;
    hos_cpu_slot_mark(&t->cpu, blocking);
    errno = e;
}
int hos_posix_threads(hos_posix_thread_info *out, int max) {
    int n = 0;
    pthread_mutex_lock(&registry_mu);
    for (int i = 0; i < registry_n; i++) {
        HThread *t = registry[i];
        if (!t) continue;
        if (t->done) {
            thread_unref(t);
            registry[i] = NULL;
            continue;
        }
        if (n < max) {
            out[n].handle = t;
            out[n].pthread = (void *)t->th;
            out[n].tid = t->tid;
            memcpy(out[n].name, t->name, sizeof out[n].name);
            out[n].name[sizeof out[n].name - 1] = 0;
            n++;
        }
    }
    pthread_mutex_unlock(&registry_mu);
    return n;
}
/* (an adopted thread's end, which thread_finish does not see: a key's destructor marks it done, so the registry drops it
   before the report asks the kernel about a thread that is gone) */
static pthread_key_t adopted_key;
static pthread_once_t adopted_once = PTHREAD_ONCE_INIT;
static void adopted_end(void *p) {
    HThread *t = (HThread *)p;
    pthread_mutex_lock(&t->mu);
    t->done = 1;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
}
static void adopted_key_make(void) { pthread_key_create(&adopted_key, adopted_end); }
#endif
#ifndef __PROSPERO__   /* (ps5: the console's mask is hos_ps5_cpus', GetProcessAffinityMask below; its threads' are
                          set by HOOKSHOT_PS5_AFFINITY in thread_entry, as before) */
/* The process's affinity as the process started: Windows keeps a process mask apart from its threads' masks, Linux has
   only the threads'. Read from the asking thread each time, it was that thread's own mask, which shrank once hos_thread_pin
   pinned it off the reserved core: the Presentation Thread's pin, asked for from a game thread kept off that core, then
   saw no reserved core in the "process" mask and kept the Presentation Thread off it too, against render_thread_on_reserved
   (host/hos_thread.c; Ryzen 9 9950X3D, 2026-11-09). Taken once, before main, when no thread is pinned yet.
   Also applied when a thread first registers here (CreateThread's thread_entry, or a std::thread in thread_self on its
   first GetCurrentThread call in SetThreadDescription / SetThreadPriority), so a thread made inside a pinned replay
   (ReplayPin in gpu/vk/vk_replay.cpp) starts with the process's affinity as on Windows rather than inheriting the pin.
   (A thread that never registers here, a std::thread that calls none of these or a library's, keeps what it inherited) */
static DWORD_PTR process_affinity;
__attribute__((constructor)) static void init_process_affinity(void) {
    cpu_set_t s;
    CPU_ZERO(&s);
    if (sched_getaffinity(0, sizeof s, &s) == 0)
        for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &s)) process_affinity |= (DWORD_PTR)1 << i;
}
static void apply_process_affinity(pthread_t th) {
    if (!process_affinity) init_process_affinity();
    if (!process_affinity) return;
    cpu_set_t want;
    CPU_ZERO(&want);
    for (int i = 0; i < 64; i++) if (process_affinity >> i & 1) CPU_SET(i, &want);
    pthread_setaffinity_np(th, sizeof want, &want);
}
#endif
/* A thread this file did not start gives its record back when it ends: the key's destructor (a std::thread that called
   GetCurrentThread or GetCurrentThreadId, one per helper of a big texture). CreateThread's threads leave the key clear:
   thread_finish gives theirs back, and clears the key first for a registered thread that calls ExitThread. The main thread
   keeps its record */
static pthread_key_t self_key;
static pthread_once_t self_key_once = PTHREAD_ONCE_INIT;
static void thread_done(HThread *t, DWORD code) {
    pthread_mutex_lock(&t->mu);
    t->done = 1, t->code = code;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    self_thread = NULL;
    thread_unref(t);
}
#ifdef __PROSPERO__
/* (ps5, lot 66) HOOKSHOT_PS5_HELPERS_SHARED=1: a thread made on the render thread, which starts on its one CPU (ps5_helpers.h
   says why), goes to the title's other CPUs when it first registers here (CreateThread's thread_entry, or a std::thread's
   first GetCurrentThread in SetThreadDescription). hos_ps5_replay_cpus is the render thread's mask, written by it once it has
   pinned itself (gpu/vk/vk_replay.cpp), so the settings are loaded by then; 0 until then, and always with the switch unset.
   The title's CPUs are the ones hos_thread.c took from the main thread at stage 6 (hos_ps5_title_cpus), not this thread's.
   Unset: nothing read but that 0, as before */
volatile uint64_t hos_ps5_replay_cpus;
uint64_t hos_ps5_title_cpus(void);   /* host/hos_thread.c */
static void ps5_helper_leave_replay_cpu(pthread_t th, int tid) {
    const uint64_t replay = __atomic_load_n(&hos_ps5_replay_cpus, __ATOMIC_ACQUIRE);
    if (!replay) return;
    static int on = -1;   /* (read once the render thread runs: its settings are loaded) */
    if (on < 0) {
        const char *e = getenv("HOOKSHOT_PS5_HELPERS_SHARED");
        on = e && atoi(e) != 0;
    }
    if (!on) return;
    cpuset_t cur;
    CPU_ZERO(&cur);
    if (ps5_pthread_getaffinity_np(th, sizeof cur, &cur)) return;
    uint64_t inherited = 0;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &cur)) inherited |= 1ull << i;
    const uint64_t want = ps5_helper_mask(inherited, replay, hos_ps5_title_cpus());
    if (!want) return;
    cpuset_t w;
    CPU_ZERO(&w);
    for (int i = 0; i < 64; i++) if (want >> i & 1) CPU_SET(i, &w);
    const int failed = ps5_pthread_setaffinity_np(th, sizeof w, &w);
    static volatile int said;
    if (__atomic_add_fetch(&said, 1, __ATOMIC_RELAXED) <= 8)
        hos_ps5_trail("ps5: thread %d, made on the render thread's CPUs %#llx, %s %#llx (HOOKSHOT_PS5_HELPERS_SHARED=1)", tid,
                      (unsigned long long)replay, failed ? "NOT moved to" : "moved to", (unsigned long long)want);
}
#endif
static void self_key_end(void *p) { thread_done((HThread *)p, 0); }
static void self_key_make(void) { pthread_key_create(&self_key, self_key_end); }
static HThread *thread_self(void) {
    if (!self_thread) {   /* (a thread this file did not start: the main thread, a library's) */
        HThread *t = thread_new();
        t->th = pthread_self();
        t->tid = posix_tid();
        t->refs = 1;   /* (kept for the thread's life) */
        self_thread = t;
#ifdef __PROSPERO__
        snprintf(t->name, sizeof t->name, "thread %d", (int)t->tid);
        hos_ps5_sched_apply();   /* (HOOKSHOT_PS5_SCHED: ps5_platform.c) */
        pthread_once(&adopted_once, adopted_key_make);
        pthread_setspecific(adopted_key, t);
        registry_add(t);
        ps5_helper_leave_replay_cpu(t->th, (int)t->tid);   /* (lot 66: a std::thread the render thread made, off its CPU) */
#else
        pthread_once(&self_key_once, self_key_make);
        pthread_setspecific(self_key, t);
        apply_process_affinity(t->th);
#endif
    }
    return self_thread;
}
#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)
static HThread *thread_of(HANDLE h) {
    if (h == PSEUDO_THREAD) return thread_self();
    return is(h, H_THREAD) ? (HThread *)h : NULL;
}
static void thread_finish(DWORD code) {
    HThread *t = self_thread;
    if (!t) return;
    pthread_once(&self_key_once, self_key_make);
    pthread_setspecific(self_key, NULL);   /* (a registered thread's ExitThread: given back here, not again by the key) */
    thread_done(t, code);
}
static void *thread_entry(void *p) {
    HThread *t = (HThread *)p;
    self_thread = t;
    t->tid = posix_tid();
#ifdef __PROSPERO__
    hos_ps5_thread_started();   /* (the IEEE MXCSR: a console thread starts FTZ/DAZ) */
    hos_ps5_sched_apply();      /* (HOOKSHOT_PS5_SCHED: real-time threads of one priority are not time-sliced) */
    if (hos_ps5_new_thread_cpus) {   /* (HOOKSHOT_PS5_AFFINITY=pinned: host/hos_thread.c) */
        cpuset_t want;
        CPU_ZERO(&want);
        for (int i = 0; i < 64; i++) if (hos_ps5_new_thread_cpus >> i & 1) CPU_SET(i, &want);
        ps5_pthread_setaffinity_np(pthread_self(), sizeof want, &want);
    } else {
        ps5_helper_leave_replay_cpu(pthread_self(), (int)t->tid);   /* (lot 66: a thread the render thread made, off its CPU) */
    }
    registry_add(t);
#else
    /* (longshot: unless its maker has pinned it by its handle already, hos_thread_start's hos_thread_pin right after
       CreateThread: the reset coming second would have put a game thread back on the reserved core) */
    pthread_mutex_lock(&t->mu);
    if (!t->pinned) apply_process_affinity(t->th);
    pthread_mutex_unlock(&t->mu);
#endif
    thread_finish(t->fn(t->arg));
    return NULL;
}
HANDLE CreateThread(void *attr, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id) {
    (void)attr;
    HThread *t = thread_new();
    t->fn = fn, t->arg = arg, t->refs = 2;   /* (the handle's and the thread's own) */
    pthread_attr_t a;
    pthread_attr_init(&a);
    /* (Windows' default is the executable's: 8 MB here, as longshot.exe's /stack; guest threads ask 64 MB) */
#ifdef __PROSPERO__
    /* (ps5) The stack placed by us, in direct memory: the platform's pthread_create wrap gives its 2 MiB only to threads that
       ask for less, and libkernel takes a larger one from the title's 448 MiB of flexible memory (64 MiB a guest thread).
       Reserved at the size asked, backed at its top only (hos_ps5_stack: HOOKSHOT_PS5_STACK_MB, 8 by default; RADV's
       pipeline compiles need more than 1.5 MiB, PLAN.md's r8). Not freed when the thread ends: phase 4 (a reaper, as the
       platform's wrap has) */
    {
        const SIZE_T want = stack ? (stack + 0xFFFF) & ~(SIZE_T)0xFFFF : (8u << 20);
        void *base = hos_ps5_stack(want);
        if (!base) { free(t); last_error = ERROR_NOT_ENOUGH_MEMORY; pthread_attr_destroy(&a); return NULL; }
        pthread_attr_setstack(&a, base, want);
    }
#else
    pthread_attr_setstacksize(&a, stack ? (stack + 0xFFFF) & ~(SIZE_T)0xFFFF : (8u << 20));
#endif
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (flags & CREATE_SUSPENDED) fprintf(stderr, "[win32] CreateThread: CREATE_SUSPENDED is not supported: started at once\n");
    const int r = pthread_create(&t->th, &a, thread_entry, t);
    pthread_attr_destroy(&a);
    if (r) {
        free(t);
        last_error = error_of(r);
        return NULL;
    }
    if (id) {
        while (!t->tid) sched_yield();
        *id = (DWORD)t->tid;
    }
    return t;
}
HANDLE GetCurrentThread(void) { return PSEUDO_THREAD; }
DWORD GetCurrentThreadId(void) { return (DWORD)thread_self()->tid; }
DWORD GetThreadId(HANDLE h) {
    HThread *t = thread_of(h);
    if (!t) return 0;
    while (!t->tid && !t->done) sched_yield();
    return (DWORD)t->tid;
}
DWORD GetProcessId(HANDLE p) { (void)p; return (DWORD)getpid(); }
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD id) {
    (void)access, (void)inherit, (void)id;
    last_error = ERROR_NOT_SUPPORTED;
    return NULL;
}
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD options) {
    (void)sp, (void)tp, (void)access, (void)inherit, (void)options;
    HThread *t = thread_of(src);
    if (!t) { last_error = ERROR_NOT_SUPPORTED; return FALSE; }
    __atomic_add_fetch(&t->refs, 1, __ATOMIC_ACQ_REL);
    *out = t;
    return TRUE;
}
/* (thread priorities: only lowered, with nice values: an unprivileged process cannot raise them, nor take back a lowering) */
BOOL SetThreadPriority(HANDLE h, int p) {
    HThread *t = thread_of(h);
    if (!t) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    t->priority = p;
#ifdef __PROSPERO__
    /* (ps5: kept as a note only. A title's threads are real-time, so a priority is strict here, not a weight: a raised
       thread that spins (vk_framegen.cpp's pacer yields with SwitchToThread, which lets no lower thread run) would starve
       the rest, a lowered one (the compile workers, OoT's OptimizeChangeBank) would get nothing while the game is busy.
       All threads keep one priority; HOOKSHOT_PS5_SCHED (ps5_platform.c) gives them time slices instead) */
#else
    const int nice = p <= THREAD_PRIORITY_IDLE ? 19 : p <= THREAD_PRIORITY_LOWEST ? 10 : p == THREAD_PRIORITY_BELOW_NORMAL ? 5 : 0;
    if (!nice) return TRUE;
    /* (a thread just made has no id until it runs: hos_thread_start lowers it right after CreateThread) */
    while (!t->tid && !t->done) sched_yield();
    if (t->tid) setpriority(PRIO_PROCESS, (id_t)t->tid, nice);
#endif
    return TRUE;
}
int GetThreadPriority(HANDLE h) {
    HThread *t = thread_of(h);
    return t ? t->priority : 0;
}
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
    HThread *t = thread_of(h);
    if (!t || !mask) { last_error = ERROR_INVALID_PARAMETER; return 0; }
#ifdef __PROSPERO__   /* (FreeBSD's cpuset_t, through the platform's calls: libkernel's answer ERANGE to its 32-byte set) */
    cpuset_t old, want;
    CPU_ZERO(&old);
    ps5_pthread_getaffinity_np(t->th, sizeof old, &old);
    CPU_ZERO(&want);
    for (int i = 0; i < 64; i++) if (mask >> i & 1) CPU_SET(i, &want);
    if (ps5_pthread_setaffinity_np(t->th, sizeof want, &want)) { last_error = ERROR_INVALID_PARAMETER; return 0; }
#else
    cpu_set_t old, want;
    CPU_ZERO(&old);
    pthread_getaffinity_np(t->th, sizeof old, &old);
    CPU_ZERO(&want);
    for (int i = 0; i < 64; i++) if (mask >> i & 1) CPU_SET(i, &want);
    pthread_mutex_lock(&t->mu);   /* (with thread_entry's reset to the process's affinity: whichever comes first, this one stays) */
    const int failed = pthread_setaffinity_np(t->th, sizeof want, &want);
    if (!failed) t->pinned = 1;
    pthread_mutex_unlock(&t->mu);
    if (failed) { last_error = ERROR_INVALID_PARAMETER; return 0; }
#endif
    DWORD_PTR r = 0;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &old)) r |= (DWORD_PTR)1 << i;
    return r ? r : 1;
}
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys) {
    (void)p;
#ifdef __PROSPERO__   /* (the 13 CPUs a title's threads get, 0x1fff measured; sysconf says 16) */
    uint64_t cpus = 0;
    hos_ps5_cpus(&cpus);
    *proc = (DWORD_PTR)cpus;
    long n = 16;
#else
    if (!process_affinity) init_process_affinity();   /* (the constructor did not run, or the call failed then) */
    long n = sysconf(_SC_NPROCESSORS_CONF);
    *proc = process_affinity;
#endif
    *sys = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1);
    return TRUE;
}
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int n, LPSTR s, int cap, LPCSTR def, BOOL *used) {
    (void)cp, (void)flags, (void)def;
    if (used) *used = FALSE;
    int out = 0;
    for (int i = 0; n < 0 ? 1 : i < n; i++) {
        uint32_t c = (uint32_t)w[i];
        char b[4];
        int k = c < 0x80 ? (b[0] = (char)c, 1)
              : c < 0x800 ? (b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3F)), 2)
              : c < 0x10000 ? (b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 0x3F)), b[2] = (char)(0x80 | (c & 0x3F)), 3)
              : (b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 0x3F)), b[2] = (char)(0x80 | (c >> 6 & 0x3F)),
                 b[3] = (char)(0x80 | (c & 0x3F)), 4);
        if (cap) {
            if (out + k > cap) { last_error = ERROR_INVALID_PARAMETER; return 0; }
            memcpy(s + out, b, (size_t)k);
        }
        out += k;
        if (n < 0 && !c) break;
    }
    return out;
}
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR w, int cap) {
    (void)cp, (void)flags;
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = n < 0 ? NULL : p + n;
    int out = 0;
    for (;;) {
        if (end && p >= end) break;
        uint32_t c = *p++;
        if (c >= 0xF0 && (!end || p + 2 < end)) c = (c & 7) << 18 | (p[0] & 0x3Fu) << 12 | (p[1] & 0x3Fu) << 6 | (p[2] & 0x3Fu), p += 3;
        else if (c >= 0xE0 && (!end || p + 1 < end)) c = (c & 15) << 12 | (p[0] & 0x3Fu) << 6 | (p[1] & 0x3Fu), p += 2;
        else if (c >= 0xC0 && (!end || p < end)) c = (c & 31) << 6 | (p[0] & 0x3Fu), p += 1;
        if (cap) {
            if (out >= cap) { last_error = ERROR_INVALID_PARAMETER; return 0; }
            w[out] = (wchar_t)c;
        }
        out++;
        if (!end && !c) break;
    }
    return out;
}
HRESULT SetThreadDescription(HANDLE h, PCWSTR name) {
    HThread *t = thread_of(h);
    if (!t) return (HRESULT)0x80070006;
    WideCharToMultiByte(CP_UTF8, 0, name, -1, t->name, sizeof t->name - 1, NULL, NULL);
    t->name[sizeof t->name - 1] = 0;
    char shortname[16];
    snprintf(shortname, sizeof shortname, "%s", t->name);   /* (the kernel keeps 15 characters) */
#ifdef __PROSPERO__   /* (pthread_setname_np is only libScePosixForWebKit's: NULL in a title. FreeBSD's own name call) */
    hos_ps5_set_name((void *)t->th, shortname);
#else
    pthread_setname_np(t->th, shortname);
#endif
    return S_OK;
}
HRESULT GetThreadDescription(HANDLE h, PWSTR *name) {
    HThread *t = thread_of(h);
    if (!t) return (HRESULT)0x80070006;
    const int n = MultiByteToWideChar(CP_UTF8, 0, t->name, -1, NULL, 0);
    *name = (PWSTR)calloc((size_t)n + 1, sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, t->name, -1, *name, n);
    return S_OK;
}
void *LocalFree(void *p) { free(p); return NULL; }
static void filetime_ns(FILETIME *ft, int64_t ns) {
    const uint64_t t = (uint64_t)ns / 100;
    ft->dwLowDateTime = (DWORD)t, ft->dwHighDateTime = (DWORD)(t >> 32);
}
BOOL GetThreadTimes(HANDLE h, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user) {
    HThread *t = thread_of(h);
    clockid_t clk;
    struct timespec ts;
    if (!t || t->done || pthread_getcpuclockid(t->th, &clk) || clock_gettime(clk, &ts)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    filetime_ns(create, 0), filetime_ns(exit_, 0), filetime_ns(kernel, 0);
    filetime_ns(user, (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec);
    return TRUE;
}
/* (no suspending another thread: the samplers that do it find nothing to sample) */
DWORD SuspendThread(HANDLE h) { (void)h; last_error = ERROR_NOT_SUPPORTED; return (DWORD)-1; }
DWORD ResumeThread(HANDLE h) { (void)h; last_error = ERROR_NOT_SUPPORTED; return (DWORD)-1; }
BOOL GetThreadContext(HANDLE h, CONTEXT *c) { (void)h, (void)c; last_error = ERROR_NOT_SUPPORTED; return FALSE; }
BOOL SetThreadContext(HANDLE h, const CONTEXT *c) { (void)h, (void)c; last_error = ERROR_NOT_SUPPORTED; return FALSE; }
BOOL GetExitCodeThread(HANDLE h, DWORD *code) {
    HThread *t = thread_of(h);
    if (!t) return FALSE;
    pthread_mutex_lock(&t->mu);
    *code = t->done ? t->code : STILL_ACTIVE;
    pthread_mutex_unlock(&t->mu);
    return TRUE;
}
void ExitThread(DWORD code) {
    thread_finish(code);
#ifdef __PROSPERO__   /* (the platform's: a thread's thread_local destructors before its keys', which libkernel runs in any order) */
    ps5_pthread_exit(NULL);
#else
    pthread_exit(NULL);
#endif
}
void ExitProcess(UINT code) {
#ifdef __PROSPERO__   /* (a title never exits: it asks the shell to close it, and exit() is a crash report) */
    hos_ps5_exit((int)code);
#else
    /* (at once, as Windows ends the other threads first: exit() would run static destructors under running threads) */
    fflush(NULL);
    _exit((int)code);
#endif
}
BOOL TerminateProcess(HANDLE p, UINT code) { (void)p; ExitProcess(code); }

/* ---- events and semaphores ---- */
typedef struct {
    HObj h;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int manual, state;     /* (events) */
    LONG count, max;       /* (semaphores) */
} HEvent;
HANDLE CreateEventA(void *attr, BOOL manual, BOOL initial, LPCSTR name) {
    (void)attr, (void)name;
    HEvent *e = (HEvent *)calloc(1, sizeof *e);
    e->h.type = H_EVENT;
    e->manual = manual, e->state = initial;
    monitor_init(&e->mu, &e->cv);
    return e;
}
BOOL SetEvent(HANDLE h) {
    if (!is(h, H_EVENT)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    HEvent *e = (HEvent *)h;
    pthread_mutex_lock(&e->mu);
    e->state = 1;
    if (e->manual) pthread_cond_broadcast(&e->cv);
    else pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
    return TRUE;
}
BOOL ResetEvent(HANDLE h) {
    if (!is(h, H_EVENT)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    HEvent *e = (HEvent *)h;
    pthread_mutex_lock(&e->mu);
    e->state = 0;
    pthread_mutex_unlock(&e->mu);
    return TRUE;
}
HANDLE CreateSemaphoreA(void *attr, LONG initial, LONG max, LPCSTR name) {
    (void)attr, (void)name;
    HEvent *e = (HEvent *)calloc(1, sizeof *e);
    e->h.type = H_SEMA;
    e->count = initial, e->max = max;
    monitor_init(&e->mu, &e->cv);
    return e;
}
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *prev) {
    if (!is(h, H_SEMA)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    HEvent *e = (HEvent *)h;
    pthread_mutex_lock(&e->mu);
    if (prev) *prev = e->count;
    if (e->count + n > e->max) { pthread_mutex_unlock(&e->mu); last_error = ERROR_INVALID_PARAMETER; return FALSE; }
    e->count += n;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->mu);
    return TRUE;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    DWORD r = WAIT_FAILED;
    if (is(h, H_EVENT)) {
        HEvent *e = (HEvent *)h;
        pthread_mutex_lock(&e->mu);
        MONITOR_WAIT(&e->mu, &e->cv, e->state, ms, r);
        if (r == WAIT_OBJECT_0 && !e->manual) e->state = 0;
        pthread_mutex_unlock(&e->mu);
    } else if (is(h, H_SEMA)) {
        HEvent *e = (HEvent *)h;
        pthread_mutex_lock(&e->mu);
        MONITOR_WAIT(&e->mu, &e->cv, e->count > 0, ms, r);
        if (r == WAIT_OBJECT_0) e->count--;
        pthread_mutex_unlock(&e->mu);
    } else if (thread_of(h)) {
        HThread *t = thread_of(h);
        pthread_mutex_lock(&t->mu);
        MONITOR_WAIT(&t->mu, &t->cv, t->done, ms, r);
        pthread_mutex_unlock(&t->mu);
    } else if (is(h, H_TIMER)) {
        const int64_t due = ((HTimer *)h)->due;
        struct timespec ts = {(time_t)(due / 1000000000LL), (long)(due % 1000000000LL)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
        r = WAIT_OBJECT_0;
    } else {
        last_error = ERROR_INVALID_HANDLE;
    }
    return r;
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    const int64_t until = ms == INFINITE ? INT64_MAX : now_ns() + (int64_t)ms * 1000000LL;
    for (;;) {   /* (polled: nothing waits on several objects in a hot path) */
        DWORD ready = 0, first = WAIT_TIMEOUT;
        for (DWORD i = 0; i < n; i++)
            if (WaitForSingleObject(hs[i], 0) == WAIT_OBJECT_0) {
                ready++;
                if (first == WAIT_TIMEOUT) first = WAIT_OBJECT_0 + i;
                if (!all) return first;
            }
        if (all && ready == n) return WAIT_OBJECT_0;
        if (now_ns() >= until) return WAIT_TIMEOUT;
        Sleep(1);
    }
}

/* ---- fibers ---- */
typedef struct {
#ifdef __PROSPERO__
    void *rsp;   /* (ps5: where the fiber's switch left its stack: hos_ps5_fiber_switch, no ucontext_t) */
#else
    ucontext_t uc;
#endif
    void *param, *stack;
    size_t stack_size;
    LPFIBER_START_ROUTINE fn;
    int is_thread;
} Fiber;
static __thread Fiber *cur_fiber;
void *ConvertThreadToFiberEx(void *param, DWORD flags) {
    (void)flags;
    Fiber *f = (Fiber *)calloc(1, sizeof *f);
    f->param = param, f->is_thread = 1;
    cur_fiber = f;
    return f;
}
void *ConvertThreadToFiber(void *param) { return ConvertThreadToFiberEx(param, 0); }
BOOL ConvertFiberToThread(void) {
    if (!cur_fiber || !cur_fiber->is_thread) return FALSE;
    free(cur_fiber);
    cur_fiber = NULL;
    return TRUE;
}
#ifdef __PROSPERO__
static void fiber_start(void *p) {
    Fiber *f = (Fiber *)p;
#else
static void fiber_start(unsigned lo, unsigned hi) {
    Fiber *f = (Fiber *)(uintptr_t)((uint64_t)hi << 32 | lo);
#endif
    f->fn(f->param);
    ExitThread(0);   /* (a fiber function that returns ends its thread, as on Windows) */
}
void *CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, void *param) {
    (void)flags;
    size_t size = reserve > commit ? reserve : commit;
    if (size < (1u << 20)) size = 1u << 20;
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
#ifdef __PROSPERO__
    /* (ps5) direct memory, backed whole: a guest fiber asks 64 MiB (host/hos_thread.c), which the title's flexible memory
       (448 MiB) cannot give. Phase 4: back only the top (commit) and grow on touch, as the window does, when fibers
       are counted in the game (docs/ps5-port.md, "Threads") */
    /* (2026-11-10: 5 fibers held 320 MiB of real memory, 64 MiB each, while a playable area ran the console's memory
       out. As a thread's stack now: the size reserved, the top HOOKSHOT_PS5_STACK_MB backed (hos_ps5_stack), the rest
       below unbacked, which is the guard too. HOOKSHOT_PS5_FIBER_WHOLE=1: backed whole, as before) */
    static int whole = -1;
    if (whole < 0) whole = getenv("HOOKSHOT_PS5_FIBER_WHOLE") && atoi(getenv("HOOKSHOT_PS5_FIBER_WHOLE")) != 0;
    void *stack = whole ? hos_ps5_reserve(size, 1) : hos_ps5_stack(size);
    if (!stack) { last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    if (whole) mprotect(stack, 16384, PROT_NONE);   /* (a guard page at the bottom: 16 KiB on the console) */
#else
    void *stack = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) { last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    mprotect(stack, 4096, PROT_NONE);   /* (a guard page at the bottom) */
#endif
    Fiber *f = (Fiber *)calloc(1, sizeof *f);
    f->param = param, f->fn = fn, f->stack = stack, f->stack_size = size;
#ifdef __PROSPERO__
    f->rsp = hos_ps5_fiber_init(stack, size, fiber_start, f);
#else
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = stack;
    f->uc.uc_stack.ss_size = size;
    f->uc.uc_link = NULL;
    const uint64_t p = (uint64_t)(uintptr_t)f;
    makecontext(&f->uc, (void (*)(void))fiber_start, 2, (unsigned)p, (unsigned)(p >> 32));
#endif
    return f;
}
void *CreateFiber(SIZE_T stack, LPFIBER_START_ROUTINE fn, void *param) { return CreateFiberEx(stack, stack, 0, fn, param); }
void SwitchToFiber(void *to) {
    Fiber *from = cur_fiber;
    if (!from || from == to) return;
    cur_fiber = (Fiber *)to;
#ifdef __PROSPERO__
    hos_ps5_fiber_switch(&from->rsp, ((Fiber *)to)->rsp);
#else
    swapcontext(&from->uc, &((Fiber *)to)->uc);
#endif
}
void DeleteFiber(void *p) {
    Fiber *f = (Fiber *)p;
    if (!f) return;
    if (f == cur_fiber) ExitThread(0);   /* (deleting the running fiber ends the thread) */
#ifdef __PROSPERO__
    if (f->stack) hos_ps5_release(f->stack, f->stack_size);
#else
    if (f->stack) munmap(f->stack, f->stack_size);
#endif
    free(f);
}
void *GetCurrentFiber(void) { return cur_fiber; }
void *GetFiberData(void) { return cur_fiber ? cur_fiber->param : NULL; }

/* ---- processors ---- */
static int read_int(const char *path) {
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}
BOOL GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info, DWORD *len) {
    /* one RelationProcessorCore entry per physical core, its logical processors (SMT siblings) in the mask */
#ifdef __PROSPERO__
    /* (no /sys: the title's CPUs (0x1fff) in pairs, 2k and 2k+1 taken as one Zen 2 core's two threads. An assumption until
       the platform's topology probe says which pairs share a core: phase 4. host/hos_thread.c keeps the last core for the
       renderer's replay from this) */
    uint64_t cpus = 0;
    hos_ps5_cpus(&cpus);
    ULONG_PTR masks[32];
    int n = 0;
    for (int i = 0; i < 64; i += 2)
        if (cpus >> i & 3) masks[n++] = (ULONG_PTR)(cpus >> i & 3) << i;
#else
    int core_of[64], pkg_of[64], n = 0;
    long cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (cpus > 64) cpus = 64;
    ULONG_PTR masks[64];
    int keys[64][2];
    for (int i = 0; i < cpus; i++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
        core_of[i] = read_int(p);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
        pkg_of[i] = read_int(p);
        if (core_of[i] < 0) core_of[i] = i, pkg_of[i] = 0;
        int k = 0;
        while (k < n && (keys[k][0] != pkg_of[i] || keys[k][1] != core_of[i])) k++;
        if (k == n) keys[n][0] = pkg_of[i], keys[n][1] = core_of[i], masks[n] = 0, n++;
        masks[k] |= (ULONG_PTR)1 << i;
    }
#endif
    const DWORD need = (DWORD)(n * sizeof *info);
    if (*len < need) { *len = need; last_error = 122; return FALSE; }
    memset(info, 0, need);
    for (int k = 0; k < n; k++) {
        info[k].ProcessorMask = masks[k];
        info[k].Relationship = RelationProcessorCore;
        info[k].ProcessorCore.Flags = __builtin_popcountll(masks[k]) > 1;
    }
    *len = need;
    return TRUE;
}
void GetSystemInfo(SYSTEM_INFO *si) {
    memset(si, 0, sizeof *si);
    si->wProcessorArchitecture = 9;   /* (PROCESSOR_ARCHITECTURE_AMD64) */
    si->dwPageSize = (DWORD)sysconf(_SC_PAGESIZE);
    si->dwAllocationGranularity = 0x10000;
#ifdef __PROSPERO__
    si->dwNumberOfProcessors = (DWORD)hos_ps5_cpus(NULL);   /* (13, where sysconf says 16) */
#else
    si->dwNumberOfProcessors = (DWORD)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    DWORD_PTR proc, sys;
    GetProcessAffinityMask(NULL, &proc, &sys);
    si->dwActiveProcessorMask = proc;
    si->lpMinimumApplicationAddress = (void *)0x10000;
    si->lpMaximumApplicationAddress = (void *)0x7FFFFFFEFFFFULL;
}
void OutputDebugStringA(LPCSTR s) { fputs(s, stderr); }

/* ---- memory ---- */
static int prot_of(DWORD p) {
    switch (p & 0xFF) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}
/* the reservations VirtualAlloc made, for MEM_RELEASE (which gives no size) */
static struct { uintptr_t base; size_t size; } regions[256];
static pthread_mutex_t regions_mu = PTHREAD_MUTEX_INITIALIZER;
#ifdef __PROSPERO__
/* (ps5) Memory on the console (ps5_platform.c): the guest window is reserved whole and backed in 64 KiB units of direct
   memory as it is committed (Linux maps it all at once with MAP_NORESERVE, which the console has not), so here
   MEM_COMMIT backs, MEM_DECOMMIT gives back, and VirtualQuery says which, as on Windows. Other reservations are direct
   memory too, placed: an anonymous mmap would take the title's small flexible memory and could land in the GPU window. */
int hos_posix_guest_discard(void *p, size_t n) __attribute__((weak));
int32_t sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);
static int discard_pages(void *p, size_t n) {
    if (hos_posix_guest_discard && hos_ps5_in_guest(p, n)) return hos_posix_guest_discard(p, n);
    memset(p, 0, n);   /* (not the window's: kept, zeroed) */
    return 0;
}
void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect) {
    (void)protect;   /* (read-write: nothing here asks the window or a reservation for another protection) */
    if (type & MEM_RESET) {   /* (the contents are not wanted any more: they read as zero after this) */
        if (hos_ps5_in_guest(addr, size)) {
            hos_ps5_guest_decommit(addr, size);
            return hos_ps5_guest_commit(addr, size) ? NULL : addr;
        }
        memset(addr, 0, size);
        return addr;
    }
    if (addr && (type & MEM_COMMIT) && !(type & MEM_RESERVE)) {
        const int r = hos_ps5_in_guest(addr, size) ? hos_ps5_guest_commit(addr, size) : hos_ps5_commit(addr, size);
        if (r) { last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
        return addr;
    }
    void *p = hos_ps5_reserve(size, (type & MEM_COMMIT) != 0);
    if (!p || (addr && p != addr)) {   /* (a fixed address outside the window: nothing asks for one) */
        if (p) hos_ps5_release(p, size);
        last_error = ERROR_INVALID_PARAMETER;
        return NULL;
    }
    pthread_mutex_lock(&regions_mu);
    for (int i = 0; i < 256; i++)
        if (!regions[i].base) { regions[i].base = (uintptr_t)p, regions[i].size = size; break; }
    pthread_mutex_unlock(&regions_mu);
    return p;
}
BOOL VirtualFree(void *addr, SIZE_T size, DWORD type) {
    if (type & MEM_DECOMMIT) {
        if (hos_ps5_in_guest(addr, size)) return hos_ps5_guest_decommit(addr, size) == 0 ? TRUE : FALSE;
        memset(addr, 0, size);
        return TRUE;
    }
    if (type & MEM_RELEASE) {
        pthread_mutex_lock(&regions_mu);
        for (int i = 0; i < 256; i++)
            if (regions[i].base == (uintptr_t)addr) {
                hos_ps5_release(addr, regions[i].size);
                regions[i].base = 0;
                pthread_mutex_unlock(&regions_mu);
                return TRUE;
            }
        pthread_mutex_unlock(&regions_mu);
        last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    return FALSE;
}
BOOL VirtualProtect(void *addr, SIZE_T size, DWORD protect, DWORD *old) {
    /* (16 KiB pages on the console: the debugging page watches of nvn_native.c round to them) */
    const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)16383, hi = ((uintptr_t)addr + size + 16383) & ~(uintptr_t)16383;
    if (old) *old = PAGE_READWRITE;
    return mprotect((void *)lo, hi - lo, prot_of(protect)) == 0 ? TRUE : fail_errno();
}
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T size) {
    if (size < sizeof *mbi) return 0;
    memset(mbi, 0, sizeof *mbi);
    const uintptr_t a = (uintptr_t)addr, page = a & ~(uintptr_t)16383;
    mbi->BaseAddress = (void *)page;
    if (hos_ps5_in_guest(addr, 1)) {
        size_t run = 0;
        const int on = hos_ps5_guest_committed((const void *)page, &run);
        mbi->AllocationBase = (void *)GUEST_LO;
        mbi->RegionSize = run;
        mbi->State = on ? MEM_COMMIT : MEM_RESERVE;
        mbi->Protect = mbi->AllocationProtect = on ? PAGE_READWRITE : PAGE_NOACCESS;
        mbi->Type = 0x20000;
        return sizeof *mbi;
    }
    /* (anything else: the kernel's own answer, no /proc on the console) */
    void *start = NULL, *end = NULL;
    uint32_t prot = 0;
    if (sceKernelQueryMemoryProtection((void *)page, &start, &end, &prot) != 0 || (uintptr_t)end <= page) {
        mbi->State = MEM_FREE, mbi->Protect = PAGE_NOACCESS, mbi->RegionSize = 16384;
        return sizeof *mbi;
    }
    const int r = prot & 1, w = prot & 2, x = prot & 4;
    mbi->AllocationBase = start;
    mbi->RegionSize = (uintptr_t)end - page;
    mbi->State = r || w || x ? MEM_COMMIT : MEM_RESERVE;
    mbi->Protect = mbi->AllocationProtect = !r ? PAGE_NOACCESS : x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                                                                   : (w ? PAGE_READWRITE : PAGE_READONLY);
    mbi->Type = 0x20000;
    return sizeof *mbi;
}
#else
/* guest pages given back so that they read as zero: MADV_REMOVE when the window is the memfd's (hos_main.c), as
   MADV_DONTNEED leaves a shared mapping's pages as they were; MADV_DONTNEED elsewhere */
static int discard_pages(void *p, size_t n) {
    typedef int (*DiscardFn)(void *, size_t);
    static DiscardFn fn;
    static int looked;
    if (!looked) fn = (DiscardFn)dlsym(RTLD_DEFAULT, "hos_posix_guest_discard"), looked = 1;
    if (fn && (uintptr_t)p >= GUEST_LO && (uintptr_t)p + n <= GUEST_HI) return fn(p, n);
    return madvise(p, n, MADV_DONTNEED);
}
void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect) {
    const uintptr_t page = 4096;
    if (type & MEM_RESET) {   /* (the contents are not wanted any more: they read as zero after this) */
        const uintptr_t lo = ((uintptr_t)addr + page - 1) & ~(page - 1), hi = ((uintptr_t)addr + size) & ~(page - 1);
        if (hi > lo) discard_pages((void *)lo, hi - lo);
        return addr;
    }
    if (addr && (type & MEM_COMMIT) && !(type & MEM_RESERVE)) {
        /* (inside a reservation: made accessible; the guest window is accessible already) */
        if ((uintptr_t)addr >= GUEST_LO && (uintptr_t)addr + size <= GUEST_HI) return addr;
        const uintptr_t lo = (uintptr_t)addr & ~(page - 1), hi = ((uintptr_t)addr + size + page - 1) & ~(page - 1);
        if (mprotect((void *)lo, hi - lo, prot_of(protect))) { fail_errno(); return NULL; }
        return addr;
    }
    const int prot = (type & MEM_COMMIT) ? prot_of(protect) : PROT_NONE;
    void *p = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p == MAP_FAILED || (addr && p != addr)) {
        if (p != MAP_FAILED) munmap(p, size);
        last_error = ERROR_INVALID_PARAMETER;
        return NULL;
    }
    pthread_mutex_lock(&regions_mu);
    for (int i = 0; i < 256; i++)
        if (!regions[i].base) { regions[i].base = (uintptr_t)p, regions[i].size = size; break; }
    pthread_mutex_unlock(&regions_mu);
    return p;
}
BOOL VirtualFree(void *addr, SIZE_T size, DWORD type) {
    if (type & MEM_DECOMMIT) {   /* (kept mapped: decommitted memory reads as zero when touched again) */
        const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)4095, hi = ((uintptr_t)addr + size + 4095) & ~(uintptr_t)4095;
        return discard_pages((void *)lo, hi - lo) == 0 ? TRUE : fail_errno();
    }
    if (type & MEM_RELEASE) {
        pthread_mutex_lock(&regions_mu);
        for (int i = 0; i < 256; i++)
            if (regions[i].base == (uintptr_t)addr) {
                munmap(addr, regions[i].size);
                regions[i].base = 0;
                pthread_mutex_unlock(&regions_mu);
                return TRUE;
            }
        pthread_mutex_unlock(&regions_mu);
        last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    return FALSE;
}
BOOL VirtualProtect(void *addr, SIZE_T size, DWORD protect, DWORD *old) {
    const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)4095, hi = ((uintptr_t)addr + size + 4095) & ~(uintptr_t)4095;
    if (old) *old = PAGE_READWRITE;
    return mprotect((void *)lo, hi - lo, prot_of(protect)) == 0 ? TRUE : fail_errno();
}
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T size) {
    if (size < sizeof *mbi) return 0;
    memset(mbi, 0, sizeof *mbi);
    const uintptr_t a = (uintptr_t)addr, page = a & ~(uintptr_t)4095;
    mbi->BaseAddress = (void *)page;
    if (a >= GUEST_LO && a < GUEST_HI) {
        mbi->AllocationBase = (void *)GUEST_LO;
        mbi->RegionSize = GUEST_HI - page;
        mbi->State = MEM_COMMIT, mbi->Protect = mbi->AllocationProtect = PAGE_READWRITE, mbi->Type = 0x20000;
        return sizeof *mbi;
    }
    /* (anything else: the kernel's map of the process) */
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    uintptr_t prev_end = 0;
    mbi->State = MEM_FREE, mbi->Protect = PAGE_NOACCESS, mbi->RegionSize = 4096;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[8] = "";
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (a < lo) {
            mbi->RegionSize = lo - (page > prev_end ? page : prev_end);
            break;
        }
        prev_end = hi;
        if (a >= hi) continue;
        mbi->AllocationBase = (void *)lo;
        mbi->RegionSize = hi - page;
        const int r = perms[0] == 'r', w = perms[1] == 'w', x = perms[2] == 'x';
        mbi->State = r || w || x ? MEM_COMMIT : MEM_RESERVE;
        mbi->Protect = mbi->AllocationProtect = !r ? PAGE_NOACCESS : x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                                                                       : (w ? PAGE_READWRITE : PAGE_READONLY);
        mbi->Type = 0x20000;
        break;
    }
    if (f) fclose(f);
    return sizeof *mbi;
}
#endif /* !__PROSPERO__ */

/* ---- modules ---- */
#ifdef __PROSPERO__
/* (ps5) No dlopen on the console: a title cannot load a module it brought. What Linux loads at run time is linked into
   eboot.bin, and LoadLibraryA and GetProcAddress name those entry points (ps5_platform.c: hookshot_vk's mkwvk_init,
   Vulkan's vkGetInstanceProcAddr, which is RADV's vk_icdGetInstanceProcAddr). SDL, xinput and Windows' own DLLs are not
   there, as on a PC without them. */
static void exe_path(char *out, size_t cap) { snprintf(out, cap, "%s", hos_ps5_exe_path()); }
HMODULE LoadLibraryA(LPCSTR name) {
    HMODULE m = name ? (HMODULE)hos_ps5_module(name) : NULL;
    if (!m) last_error = ERROR_FILE_NOT_FOUND;
    return m;
}
/* (upstream's hos_load_error, dlerror()'s text off the console: none here, a module is linked in or not there) */
const char *hos_load_error(void) { return ""; }
BOOL FreeLibrary(HMODULE m) { return m != NULL; }
FARPROC GetProcAddress(HMODULE m, LPCSTR name) { return m ? (FARPROC)hos_ps5_symbol(m, name) : NULL; }
HMODULE GetModuleHandleA(LPCSTR name) { return name ? NULL : (HMODULE)hos_ps5_self(); }
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out) {
    *out = NULL;
    if (!(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)) {
        *out = GetModuleHandleA(name);
        return *out != NULL;
    }
    Dl_info di;   /* (the platform's dladdr, bound by the link recipe: the eboot's base, for the crash report's offsets) */
    if (!dladdr(name, &di) || !di.dli_fbase) return FALSE;
    *out = di.dli_fbase;
    return TRUE;
}
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD cap) {
    (void)m;   /* (one module: the eboot) */
    if (!cap) return 0;
    char path[PATH_MAX];
    exe_path(path, sizeof path);
    const size_t n = strlen(path), k = n < cap - 1 ? n : cap - 1;
    memcpy(out, path, k);
    out[k] = 0;
    return (DWORD)k;
}
#else
static void exe_path(char *out, size_t cap) {
    const ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    out[n > 0 ? n : 0] = 0;
}
/* a Windows library name, as the Linux one: SDL3.dll -> libSDL3.so.0, SDL2.dll -> libSDL2-2.0.so.0,
   vulkan-1.dll -> libvulkan.so.1, x.dll -> x.so */
static const char *linux_name(const char *base) {
    static const char *map[][2] = {{"sdl3.dll", "libSDL3.so.0"}, {"sdl2.dll", "libSDL2-2.0.so.0"},
                                   {"vulkan-1.dll", "libvulkan.so.1"}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(base, map[i][0])) return map[i][1];
    return NULL;
}
static __thread char load_error[512];   /* (dlerror()'s text for the last failure: hos_load_error) */
HMODULE LoadLibraryA(LPCSTR name) {
    load_error[0] = 0;
    if (!name) return NULL;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", name);
    for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
    char *slash = strrchr(path, '/');
    char *base = slash ? slash + 1 : path;
    const char *mapped = linux_name(base);
    if (mapped) {
        snprintf(base, sizeof path - (size_t)(base - path), "%s", mapped);
    } else {
        const size_t n = strlen(path);
        if (n > 4 && !strcasecmp(path + n - 4, ".dll")) {
            if (!slash) return NULL;   /* (a Windows system library: kernel32, xinput, imm32 ...) */
            strcpy(path + n - 4, ".so");
        }
    }
    HMODULE m = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        last_error = ERROR_FILE_NOT_FOUND;
        const char *why = dlerror();
        snprintf(load_error, sizeof load_error, "%s", why ? why : "");
    }
    return m;
}
const char *hos_load_error(void) { return load_error; }
BOOL FreeLibrary(HMODULE m) { return m && dlclose(m) == 0; }
FARPROC GetProcAddress(HMODULE m, LPCSTR name) { return m ? (FARPROC)dlsym(m, name) : NULL; }
HMODULE GetModuleHandleA(LPCSTR name) {
    if (!name) return dlopen(NULL, RTLD_NOW);
    return NULL;   /* (Windows' own modules: none here) */
}
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out) {
    *out = NULL;
    if (!(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)) {
        *out = GetModuleHandleA(name);
        return *out != NULL;
    }
    Dl_info di;
    if (!dladdr(name, &di) || !di.dli_fbase) return FALSE;
    *out = di.dli_fbase;   /* (the module's base address, as an HMODULE is on Windows: crash offsets subtract it) */
    return TRUE;
}
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD cap) {
    char path[PATH_MAX] = "";
    Dl_info di;
    if (!m) {
        exe_path(path, sizeof path);
    } else if (dladdr(m, &di) && di.dli_fname) {
        Dl_info exe;
        if (dladdr((void *)&exe_path, &exe) && exe.dli_fbase == di.dli_fbase && strcmp(exe.dli_fname, di.dli_fname) &&
            !strchr(di.dli_fname, '/'))
            exe_path(path, sizeof path);
        else if (!realpath(di.dli_fname, path))
            snprintf(path, sizeof path, "%s", di.dli_fname);
        if (!path[0]) exe_path(path, sizeof path);
    } else {
        void *lm = NULL;   /* (a handle from LoadLibraryA) */
        if (dlinfo(m, RTLD_DI_LINKMAP, &lm) == 0 && lm) snprintf(path, sizeof path, "%s", ((const char **)lm)[1]);
    }
    if (!cap) return 0;
    const size_t n = strlen(path);
    const size_t k = n < cap - 1 ? n : cap - 1;
    memcpy(out, path, k);
    out[k] = 0;
    return (DWORD)k;
}
#endif /* !__PROSPERO__ */
static void slashes(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src);
    for (char *p = dst; *p; p++) if (*p == '\\') *p = '/';
}
BOOL SetCurrentDirectoryA(LPCSTR dir) {
    char p[PATH_MAX];
    slashes(p, sizeof p, dir);
#ifdef __PROSPERO__   /* (no working directory worth trusting on the console: remembered, every path the runtime makes is absolute) */
    return hos_ps5_chdir(p) == 0 ? TRUE : fail_errno();
#else
    return chdir(p) == 0 ? TRUE : fail_errno();
#endif
}
DWORD GetCurrentDirectoryA(DWORD cap, LPSTR out) {
    char p[PATH_MAX];
#ifdef __PROSPERO__   /* (libc's getcwd faults for a title: its __getcwd is only libkernel_sys's) */
    snprintf(p, sizeof p, "%s", hos_ps5_cwd());
#else
    if (!getcwd(p, sizeof p)) return 0;
#endif
    const DWORD n = (DWORD)strlen(p);
    if (n + 1 > cap) return n + 1;
    memcpy(out, p, n + 1);
    return n;
}

/* ---- files ---- */
typedef struct { HObj h; int fd; } HFile;
static HFile std_files[3] = {{{H_FILE}, 0}, {{H_FILE}, 1}, {{H_FILE}, 2}};
HANDLE GetStdHandle(DWORD which) {
    if (which == STD_INPUT_HANDLE) return &std_files[0];
    if (which == STD_OUTPUT_HANDLE) return &std_files[1];
    if (which == STD_ERROR_HANDLE) return &std_files[2];
    return INVALID_HANDLE_VALUE;
}
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, void *attr, DWORD disposition, DWORD flags, HANDLE tmpl) {
    (void)share, (void)attr, (void)flags, (void)tmpl;
    char path[PATH_MAX];
    slashes(path, sizeof path, name);
    int o = (access & GENERIC_READ) && (access & GENERIC_WRITE) ? O_RDWR : (access & GENERIC_WRITE) ? O_WRONLY : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: o |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: o |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: o |= O_CREAT; break;
    case TRUNCATE_EXISTING: o |= O_TRUNC; break;
    default: break;
    }
    const int fd = open(path, o | O_CLOEXEC, 0644);
    if (fd < 0) {
        last_error = errno == EEXIST ? ERROR_FILE_EXISTS : error_of(errno);
        return INVALID_HANDLE_VALUE;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {   /* (Windows opens no folder this way) */
        close(fd);
        last_error = ERROR_ACCESS_DENIED;
        return INVALID_HANDLE_VALUE;
    }
    HFile *f = (HFile *)calloc(1, sizeof *f);
    f->h.type = H_FILE, f->fd = fd;
    return f;
}
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, OVERLAPPED *ov) {
    if (got) *got = 0;
    if (!is(h, H_FILE)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
#ifdef __PROSPERO__   /* (the kernel writes into guest memory with no fault to commit it on: EFAULT on an unbacked unit) */
    if (hos_ps5_in_guest(buf, n)) hos_ps5_guest_commit(buf, n);
#endif
    const int fd = ((HFile *)h)->fd;
    off_t at = ov ? (off_t)((uint64_t)ov->OffsetHigh << 32 | ov->Offset) : 0;
    DWORD done = 0;
    while (done < n) {
        const ssize_t r = ov ? pread(fd, (char *)buf + done, n - done, at + done) : read(fd, (char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { if (got) *got = done; return fail_errno(); }
        if (!r) break;
        done += (DWORD)r;
    }
    if (got) *got = done;
    if (ov && !done && n) { last_error = ERROR_HANDLE_EOF; return FALSE; }
    return TRUE;
}
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, OVERLAPPED *ov) {
    if (put) *put = 0;
    if (!is(h, H_FILE)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
#ifdef __PROSPERO__   /* (likewise for the kernel's reads of it: a guest buffer never touched reads as zero once backed) */
    if (hos_ps5_in_guest(buf, n)) hos_ps5_guest_commit((void *)buf, n);
#endif
    const int fd = ((HFile *)h)->fd;
    off_t at = ov ? (off_t)((uint64_t)ov->OffsetHigh << 32 | ov->Offset) : 0;
    DWORD done = 0;
    while (done < n) {
        const ssize_t r = ov ? pwrite(fd, (const char *)buf + done, n - done, at + done) : write(fd, (const char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (put) *put = done; return fail_errno(); }
        done += (DWORD)r;
    }
    if (put) *put = done;
    return TRUE;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD method) {
    if (!is(h, H_FILE)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    const off_t r = lseek(((HFile *)h)->fd, (off_t)dist.QuadPart, method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) return fail_errno();
    if (pos) pos->QuadPart = r;
    return TRUE;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) {
    struct stat st;
    if (!is(h, H_FILE) || fstat(((HFile *)h)->fd, &st)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    size->QuadPart = st.st_size;
    return TRUE;
}
DWORD GetFileSize(HANDLE h, DWORD *high) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h, &s)) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)((uint64_t)s.QuadPart >> 32);
    return (DWORD)s.QuadPart;
}
BOOL SetEndOfFile(HANDLE h) {
    if (!is(h, H_FILE)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    const int fd = ((HFile *)h)->fd;
    const off_t at = lseek(fd, 0, SEEK_CUR);
    return at >= 0 && ftruncate(fd, at) == 0 ? TRUE : fail_errno();
}
BOOL FlushFileBuffers(HANDLE h) {
    if (!is(h, H_FILE)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    return fdatasync(((HFile *)h)->fd) == 0 || errno == EINVAL ? TRUE : fail_errno();
}
DWORD GetFileType(HANDLE h) {
    struct stat st;
    if (!is(h, H_FILE) || fstat(((HFile *)h)->fd, &st)) return FILE_TYPE_UNKNOWN;
    if (S_ISREG(st.st_mode)) return FILE_TYPE_DISK;
    if (S_ISFIFO(st.st_mode)) return FILE_TYPE_PIPE;
    /* (a terminal is the console; /dev/null and the journal's socket, where a desktop launcher sends it, count as none) */
    if (S_ISCHR(st.st_mode)) return isatty(((HFile *)h)->fd) ? FILE_TYPE_CHAR : FILE_TYPE_UNKNOWN;
    return FILE_TYPE_UNKNOWN;
}
BOOL GetConsoleMode(HANDLE h, DWORD *mode) {
    if (!is(h, H_FILE) || !isatty(((HFile *)h)->fd)) return FALSE;
    if (mode) *mode = 0;
    return TRUE;
}
DWORD GetFileAttributesA(LPCSTR name) {
    char path[PATH_MAX];
    slashes(path, sizeof path, name);
    struct stat st;
    if (stat(path, &st)) {
        fail_errno();
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD at = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    struct stat link;   /* (longshot: a symbolic link is marked as Windows marks one, a reparse point; find_next too) */
    if (lstat(path, &link) == 0 && S_ISLNK(link.st_mode)) at |= FILE_ATTRIBUTE_REPARSE_POINT;
    return at;
}
BOOL CreateDirectoryA(LPCSTR name, void *attr) {
    (void)attr;
    char path[PATH_MAX];
    slashes(path, sizeof path, name);
    if (mkdir(path, 0755) == 0) return TRUE;
    last_error = errno == EEXIST ? ERROR_ALREADY_EXISTS : errno == ENOENT ? ERROR_PATH_NOT_FOUND : error_of(errno);
    return FALSE;
}
BOOL RemoveDirectoryA(LPCSTR name) {
    char path[PATH_MAX];
    slashes(path, sizeof path, name);
    return rmdir(path) == 0 ? TRUE : fail_errno();
}
BOOL DeleteFileA(LPCSTR name) {
    char path[PATH_MAX];
    slashes(path, sizeof path, name);
    return unlink(path) == 0 ? TRUE : fail_errno();
}
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    char a[PATH_MAX], b[PATH_MAX];
    slashes(a, sizeof a, from);
    slashes(b, sizeof b, to);
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(b, &st) == 0) { last_error = ERROR_ALREADY_EXISTS; return FALSE; }
    return rename(a, b) == 0 ? TRUE : fail_errno();
}
BOOL MoveFileA(LPCSTR from, LPCSTR to) { return MoveFileExA(from, to, 0); }
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    char a[PATH_MAX], b[PATH_MAX];
    slashes(a, sizeof a, from);
    slashes(b, sizeof b, to);
    const int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail_errno();
    const int out = open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
    if (out < 0) { close(in); return fail_errno(); }
    char buf[1 << 16];
    ssize_t n;
    BOOL ok = TRUE;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { ok = fail_errno(); break; }
    close(in), close(out);
    return ok;
}
typedef struct { HObj h; DIR *d; char dir[PATH_MAX], pat[256]; } HFind;
static BOOL find_next(HFind *f, WIN32_FIND_DATAA *fd) {
    struct dirent *e;
    while ((e = readdir(f->d)) != NULL) {
#ifdef __PROSPERO__   /* (fnmatch is only libScePosixForWebKit's, NULL in a title) */
        if (hos_ps5_fnmatch_casefold(f->pat, e->d_name) && strcmp(f->pat, "*.*")) continue;
#else
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD) && strcmp(f->pat, "*.*")) continue;
#endif
        memset(fd, 0, sizeof *fd);
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        char full[PATH_MAX + 260];
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            fd->nFileSizeLow = (DWORD)st.st_size, fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        } else {
            fd->dwFileAttributes = e->d_type == DT_DIR ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        }
        struct stat link;   /* (longshot: a symbolic link is marked as Windows marks one, a reparse point) */
        if (lstat(full, &link) == 0 && S_ISLNK(link.st_mode)) fd->dwFileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
        return TRUE;
    }
    last_error = ERROR_NO_MORE_FILES;
    return FALSE;
}
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {
    HFind *f = (HFind *)calloc(1, sizeof *f);
    f->h.type = H_FIND;
    slashes(f->dir, sizeof f->dir, pattern);
    char *slash = strrchr(f->dir, '/');
    if (slash) {
        snprintf(f->pat, sizeof f->pat, "%s", slash + 1);
        if (slash == f->dir) slash[1] = 0;
        else *slash = 0;
    } else {
        snprintf(f->pat, sizeof f->pat, "%s", f->dir);
        strcpy(f->dir, ".");
    }
    f->d = opendir(f->dir);
    if (!f->d) {
        last_error = errno == ENOENT ? ERROR_PATH_NOT_FOUND : error_of(errno);
        free(f);
        return INVALID_HANDLE_VALUE;
    }
    if (!find_next(f, fd)) {
        closedir(f->d);
        free(f);
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    return f;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {
    if (!is(h, H_FIND)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    return find_next((HFind *)h, fd);
}
BOOL FindClose(HANDLE h) {
    if (!is(h, H_FIND)) return FALSE;
    HFind *f = (HFind *)h;
    closedir(f->d);
    f->h.type = 0;
    free(f);
    return TRUE;
}

BOOL CloseHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || h == PSEUDO_THREAD || h == GetCurrentProcess()) return TRUE;
    if (h >= (HANDLE)&std_files[0] && h <= (HANDLE)&std_files[2]) return TRUE;
    HObj *o = (HObj *)h;
    switch (o->type) {
    case H_THREAD: thread_unref((HThread *)h); return TRUE;
    case H_EVENT:
    case H_SEMA: {
        HEvent *e = (HEvent *)h;
        e->h.type = 0;
        pthread_mutex_destroy(&e->mu);
        pthread_cond_destroy(&e->cv);
        free(e);
        return TRUE;
    }
    case H_TIMER: o->type = 0; free(o); return TRUE;
    case H_FILE: close(((HFile *)h)->fd); o->type = 0; free(o); return TRUE;
    case H_FIND: return FindClose(h);
    }
    last_error = ERROR_INVALID_HANDLE;
    return FALSE;
}

/* ---- exceptions ---- */
static PVECTORED_EXCEPTION_HANDLER handlers[16];
static int nhandlers;
static LPTOP_LEVEL_EXCEPTION_FILTER unhandled;
static struct sigaction previous[32];
static pthread_mutex_t handlers_mu = PTHREAD_MUTEX_INITIALIZER;

#ifdef __PROSPERO__
/* (ps5) FreeBSD's mcontext_t, at the console's place (ps5platform/context.h refuses an SDK whose ucontext_t lacks the 48
   bytes); mc_err's bit 1 is a write, as Linux's REG_ERR */
static void context_from(CONTEXT *c, const ucontext_t *uc) {
    const mcontext_t *m = &uc->uc_mcontext;
    c->Rax = (DWORD64)m->mc_rax, c->Rcx = (DWORD64)m->mc_rcx, c->Rdx = (DWORD64)m->mc_rdx, c->Rbx = (DWORD64)m->mc_rbx;
    c->Rsp = (DWORD64)m->mc_rsp, c->Rbp = (DWORD64)m->mc_rbp, c->Rsi = (DWORD64)m->mc_rsi, c->Rdi = (DWORD64)m->mc_rdi;
    c->R8 = (DWORD64)m->mc_r8, c->R9 = (DWORD64)m->mc_r9, c->R10 = (DWORD64)m->mc_r10, c->R11 = (DWORD64)m->mc_r11;
    c->R12 = (DWORD64)m->mc_r12, c->R13 = (DWORD64)m->mc_r13, c->R14 = (DWORD64)m->mc_r14, c->R15 = (DWORD64)m->mc_r15;
    c->Rip = (DWORD64)m->mc_rip, c->EFlags = (DWORD)m->mc_rflags, c->ContextFlags = CONTEXT_FULL;
}
static void context_to(ucontext_t *uc, const CONTEXT *c) {
    mcontext_t *m = &uc->uc_mcontext;
    m->mc_rax = (__register_t)c->Rax, m->mc_rcx = (__register_t)c->Rcx, m->mc_rdx = (__register_t)c->Rdx, m->mc_rbx = (__register_t)c->Rbx;
    m->mc_rsp = (__register_t)c->Rsp, m->mc_rbp = (__register_t)c->Rbp, m->mc_rsi = (__register_t)c->Rsi, m->mc_rdi = (__register_t)c->Rdi;
    m->mc_r8 = (__register_t)c->R8, m->mc_r9 = (__register_t)c->R9, m->mc_r10 = (__register_t)c->R10, m->mc_r11 = (__register_t)c->R11;
    m->mc_r12 = (__register_t)c->R12, m->mc_r13 = (__register_t)c->R13, m->mc_r14 = (__register_t)c->R14, m->mc_r15 = (__register_t)c->R15;
    m->mc_rip = (__register_t)c->Rip;
}
#define FAULT_WRITE(uc) (((uc)->uc_mcontext.mc_err & 2) ? 1 : 0)
#else
#define FAULT_WRITE(uc) (((uc)->uc_mcontext.gregs[REG_ERR] & 2) ? 1 : 0)
static void context_from(CONTEXT *c, const ucontext_t *uc) {
    const greg_t *g = uc->uc_mcontext.gregs;
    c->Rax = (DWORD64)g[REG_RAX], c->Rcx = (DWORD64)g[REG_RCX], c->Rdx = (DWORD64)g[REG_RDX], c->Rbx = (DWORD64)g[REG_RBX];
    c->Rsp = (DWORD64)g[REG_RSP], c->Rbp = (DWORD64)g[REG_RBP], c->Rsi = (DWORD64)g[REG_RSI], c->Rdi = (DWORD64)g[REG_RDI];
    c->R8 = (DWORD64)g[REG_R8], c->R9 = (DWORD64)g[REG_R9], c->R10 = (DWORD64)g[REG_R10], c->R11 = (DWORD64)g[REG_R11];
    c->R12 = (DWORD64)g[REG_R12], c->R13 = (DWORD64)g[REG_R13], c->R14 = (DWORD64)g[REG_R14], c->R15 = (DWORD64)g[REG_R15];
    c->Rip = (DWORD64)g[REG_RIP], c->EFlags = (DWORD)g[REG_EFL], c->ContextFlags = CONTEXT_FULL;
}
static void context_to(ucontext_t *uc, const CONTEXT *c) {
    greg_t *g = uc->uc_mcontext.gregs;
    g[REG_RAX] = (greg_t)c->Rax, g[REG_RCX] = (greg_t)c->Rcx, g[REG_RDX] = (greg_t)c->Rdx, g[REG_RBX] = (greg_t)c->Rbx;
    g[REG_RSP] = (greg_t)c->Rsp, g[REG_RBP] = (greg_t)c->Rbp, g[REG_RSI] = (greg_t)c->Rsi, g[REG_RDI] = (greg_t)c->Rdi;
    g[REG_R8] = (greg_t)c->R8, g[REG_R9] = (greg_t)c->R9, g[REG_R10] = (greg_t)c->R10, g[REG_R11] = (greg_t)c->R11;
    g[REG_R12] = (greg_t)c->R12, g[REG_R13] = (greg_t)c->R13, g[REG_R14] = (greg_t)c->R14, g[REG_R15] = (greg_t)c->R15;
    g[REG_RIP] = (greg_t)c->Rip;
}
#endif
#ifdef __PROSPERO__
/* (ps5, 2026-11-10, the seventh run: the game ended 35 ms after its first mesh draws with no line anywhere. The crash
   report is hos_log's, which is queued; an abort, a broken pipe or a signal from the system had no handler at all. So a
   signal nobody answers, and the ones that end a process quietly, leave a line in the stage trail first (written and
   synced at once: host/posix/ps5_early.c): the signal, where, the fault's address, and the stack's words that point into
   the eboot (its return addresses, to read against link-game/llvm-pie.map; "image" is this function's address there). */
static void ps5_trail_signal(const char *what, int sig, const siginfo_t *si, const ucontext_t *uc) {
    static volatile int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_SEQ_CST) >= 24) return;
    const uintptr_t rip = uc ? (uintptr_t)uc->uc_mcontext.mc_rip : 0, rsp = uc ? (uintptr_t)uc->uc_mcontext.mc_rsp : 0;
    const uintptr_t here = (uintptr_t)&ps5_trail_signal;
    char stack[700];
    int n = 0;
    stack[0] = 0;
    /* (the eboot is loaded low, its code first: from 4 MiB to 64 MiB past this function. The eighth run's range went below
       zero and found nothing) */
    const uintptr_t lo = 4u << 20, hi = here + (64u << 20);
    if (rsp && !(rsp & 7))
        for (int i = 0; i < 4096 && n < 640; i++) {
            /* (the thread's own stack above rsp, up to the end of its 64 KiB unit: a stack's top is at a unit's end, and the
               run that read 8 KiB past rsp without asking went off a 2 MiB stack's end and ended the game itself) */
            if ((((uintptr_t)rsp + (uintptr_t)i * 8) >> 16) != ((uintptr_t)rsp >> 16)) break;
            const uintptr_t v = ((const uintptr_t *)rsp)[i];
            if (v > lo && v < hi) n += snprintf(stack + n, sizeof stack - (size_t)n, " %#lx", (unsigned long)v);
        }
    const int tid = hos_ps5_tid();
    char name[64] = "?";
    for (int i = 0; i < registry_n; i++) {   /* (no lock: a handler; the names are read, never followed) */
        HThread *t = registry[i];
        if (t && t->tid == tid) { memcpy(name, t->name, sizeof name), name[sizeof name - 1] = 0; break; }
    }
    char err[1600];
    if (sig != SIGUSR2) {
        fflush(stderr);
        usleep(50000);   /* (what the aborting code just wrote is still in the pipe: its reader's turn) */
    }
    hos_ps5_stderr_tail(err, sizeof err);
    hos_ps5_trail("crash: %s: signal %d (code %d) on thread %d (%s): rip %#lx, address %p, rsp %#lx; image: ps5_trail_signal at %#lx; the "
                  "stack's words into the eboot:%s",
                  what, sig, si ? si->si_code : 0, tid, name, (unsigned long)rip, si ? si->si_addr : NULL, (unsigned long)rsp,
                  (unsigned long)here, stack);
    static volatile int tails;
    if (sig != SIGUSR2 || !__atomic_fetch_add(&tails, 1, __ATOMIC_RELAXED))
        hos_ps5_trail("crash: standard error's last lines that are not the log's: %s", err[0] ? err : "(none)");
}
static void ps5_trail_exit(void) { hos_ps5_trail("exit: the process's exit handlers run (exit was called, or main returned)"); }
static void on_quiet_signal(int sig, siginfo_t *si, void *uctx) {
    ps5_trail_signal(sig == SIGPIPE ? "a write to a closed pipe or socket (ignored from here on)" :
                     sig == SIGUSR2 ? "no frame for 20 s: where this thread is (it goes on)" : "the process is ending", sig, si,
                     (const ucontext_t *)uctx);
    if (sig == SIGUSR2) return;   /* (the watchdog's question: where the thread is; it goes on) */
    if (sig == SIGPIPE) {   /* (the write returns EPIPE instead of ending the game) */
        signal(SIGPIPE, SIG_IGN);
        return;
    }
    { void hos_log_flush(void); hos_log_flush(); }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif
static void on_signal(int sig, siginfo_t *si, void *uctx) {
    ucontext_t *uc = (ucontext_t *)uctx;
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    CONTEXT ctx;
    context_from(&ctx, uc);
    rec.ExceptionCode = sig == SIGSEGV || sig == SIGBUS ? EXCEPTION_ACCESS_VIOLATION
                      : sig == SIGILL ? EXCEPTION_ILLEGAL_INSTRUCTION
                      : sig == SIGFPE ? EXCEPTION_INT_DIVIDE_BY_ZERO : EXCEPTION_BREAKPOINT;
    rec.ExceptionAddress = (void *)(uintptr_t)ctx.Rip;
    if (sig == SIGSEGV || sig == SIGBUS) {
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = FAULT_WRITE(uc);   /* (1: a write) */
        rec.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
    }
    EXCEPTION_POINTERS ep = {&rec, &ctx};
    for (int i = 0; i < nhandlers; i++)
        if (handlers[i] && handlers[i](&ep) == EXCEPTION_CONTINUE_EXECUTION) {
            context_to(uc, &ctx);
            return;
        }
#ifdef __PROSPERO__
    ps5_trail_signal("a fault no handler answered", sig, si, uc);
#endif
    /* Keep POSIX metadata in the unused tail, without changing NumberParameters
       (the legacy CRASH log's read/write interpretation stays unchanged). */
    rec.ExceptionInformation[13] = (ULONG_PTR)sig;
    rec.ExceptionInformation[14] = (ULONG_PTR)(intptr_t)si->si_code;
    if (unhandled && unhandled(&ep) == EXCEPTION_CONTINUE_EXECUTION) {
        context_to(uc, &ctx);
        return;
    }
    /* not ours: whoever had the signal before, else the default (the fault comes again and ends the process) */
    const struct sigaction *p = &previous[sig];
    if ((p->sa_flags & SA_SIGINFO) && p->sa_sigaction) {
        p->sa_sigaction(sig, si, uctx);
        return;
    }
    if (p->sa_handler != SIG_IGN && p->sa_handler != SIG_DFL && p->sa_handler) {
        p->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    if (si->si_code <= 0) raise(sig);   /* sent signals cannot re-fault; real faults keep their original core siginfo */
}
static void install_signals(void) {
    static int done;
    if (done) return;
    done = 1;
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_signal;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(sigs[i], &sa, &previous[sigs[i]]);
    }
#ifdef __PROSPERO__
    atexit(ps5_trail_exit);
    const int quiet[] = {SIGABRT, SIGSYS, SIGPIPE, SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGXCPU, SIGUSR1, SIGUSR2};
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++) {
        struct sigaction sa, old;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_quiet_signal;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (sigaction(quiet[i], NULL, &old) == 0 && old.sa_handler == SIG_DFL) sigaction(quiet[i], &sa, NULL);   /* (only where nobody listens) */
    }
#endif
}
void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) {
    pthread_mutex_lock(&handlers_mu);
    install_signals();
    if (nhandlers < 16) {
        if (first) {
            memmove(&handlers[1], &handlers[0], sizeof handlers[0] * (size_t)nhandlers);
            handlers[0] = h;
        } else {
            handlers[nhandlers] = h;
        }
        nhandlers++;
    }
    pthread_mutex_unlock(&handlers_mu);
    return (void *)h;
}
ULONG RemoveVectoredExceptionHandler(void *h) {
    pthread_mutex_lock(&handlers_mu);
    for (int i = 0; i < nhandlers; i++)
        if ((void *)handlers[i] == h) {
            memmove(&handlers[i], &handlers[i + 1], sizeof handlers[0] * (size_t)(nhandlers - i - 1));
            nhandlers--;
            pthread_mutex_unlock(&handlers_mu);
            return 1;
        }
    pthread_mutex_unlock(&handlers_mu);
    return 0;
}
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    pthread_mutex_lock(&handlers_mu);
    install_signals();
    LPTOP_LEVEL_EXCEPTION_FILTER old = unhandled;
    unhandled = f;
    pthread_mutex_unlock(&handlers_mu);
    return old;
}

/* ---- the window's state, kept by display_sdl.c in the executable (found by name from the Vulkan back end) ---- */
#ifdef __PROSPERO__   /* (ps5: one eboot, no dlsym: the names are linked, weakly, so a build without the window still links) */
extern volatile uint8_t hos_posix_keys[256] __attribute__((weak));
extern volatile int hos_posix_window[4] __attribute__((weak));
void hos_posix_post_message(uint32_t msg, uintptr_t wp, intptr_t lp) __attribute__((weak));
#define dlsym(h, name) (!strcmp(name, "hos_posix_keys") ? (void *)hos_posix_keys : !strcmp(name, "hos_posix_window") ? \
                        (void *)hos_posix_window : !strcmp(name, "hos_posix_post_message") ? (void *)hos_posix_post_message : NULL)
#endif
static volatile uint8_t no_keys[256];
static volatile uint8_t *keys_table(void) {
    static volatile uint8_t *keys;
    if (!keys) {
        volatile uint8_t *k = (volatile uint8_t *)dlsym(RTLD_DEFAULT, "hos_posix_keys");
        keys = k ? k : no_keys;
    }
    return keys;
}
SHORT GetAsyncKeyState(int vk) { return keys_table()[vk & 255] ? (SHORT)0x8000 : 0; }
SHORT GetKeyState(int vk) { return GetAsyncKeyState(vk); }
static volatile int *window_state(void) {   /* [0] focused, [1] screen width, [2] screen height, [3] fullscreen */
    static volatile int none[4];
    static volatile int *s;
    if (!s) {
        volatile int *k = (volatile int *)dlsym(RTLD_DEFAULT, "hos_posix_window");
        s = k ? k : none;
    }
    return s;
}
int GetSystemMetrics(int index) {
    if (index == SM_CXSCREEN) return window_state()[1];
    if (index == SM_CYSCREEN) return window_state()[2];
    return 0;
}
HWND GetForegroundWindow(void) { return window_state()[0] ? (HWND)(uintptr_t)1 : NULL; }
BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp) {   /* (to the window's thread: display_sdl.c) */
    (void)w;
    typedef void (*PostFn)(uint32_t, uintptr_t, intptr_t);
    static PostFn post;
    if (!post) post = (PostFn)dlsym(RTLD_DEFAULT, "hos_posix_post_message");
    if (!post) return FALSE;
    post(msg, wp, lp);
    return TRUE;
}
LONG GetWindowLongA(HWND w, int index) {
    (void)w;
    return index == GWL_STYLE && !window_state()[3] ? WS_CAPTION : 0;
}
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid) {
    if (pid) *pid = w ? (DWORD)getpid() : 0;
    return w ? GetCurrentThreadId() : 0;
}
int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type) {
    (void)w, (void)type;
    fprintf(stderr, "%s: %s\n", caption ? caption : "Longshot", text ? text : "");
#ifdef __PROSPERO__   /* (ps5: klog only, through stderr. Phase 4: the console's own dialog or a notification) */
    return 1;
#endif
    typedef int (*ShowFn)(uint32_t, const char *, const char *, void *);   /* (SDL3's and SDL2's alike; its result unread) */
    void *sdl = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!sdl) sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
    ShowFn show = sdl ? (ShowFn)dlsym(sdl, "SDL_ShowSimpleMessageBox") : NULL;
    if (show) show(0x10, caption ? caption : "Longshot", text ? text : "", NULL);
    return 1;
}

/* ---- waveOut on SDL's audio (host/hos_sdl.c): a callback takes the written blocks in order and marks them done ---- */
/* (the executable's: this file is in the Vulkan back end too, which plays no sound and has no hos_sdl.c) */
void *hos_sdl_audio_open(unsigned device, int freq, int channels, int s16, void (*pull)(void *user, uint8_t *out, int len), void *user)
    __attribute__((weak));
void hos_sdl_audio_close(void *audio) __attribute__((weak));
int hos_sdl_audio_alive(void *audio) __attribute__((weak));
typedef struct {
    pthread_mutex_t mu;
    WAVEHDR *q[64];
    int head, count;
    DWORD pos;
    void *audio;
} WaveOut;
static void wave_pull(void *user, uint8_t *out, int len) {
    WaveOut *w = (WaveOut *)user;
    pthread_mutex_lock(&w->mu);
    while (len > 0 && w->count) {
        WAVEHDR *h = w->q[w->head];
        DWORD k = h->dwBufferLength - w->pos;
        if (k > (DWORD)len) k = (DWORD)len;
        memcpy(out, h->lpData + w->pos, k);
        out += k, len -= (int)k, w->pos += k;
        if (w->pos >= h->dwBufferLength) {
            __atomic_store_n(&h->dwFlags, (h->dwFlags & ~(DWORD)WHDR_INQUEUE) | WHDR_DONE, __ATOMIC_RELEASE);
            w->head = (w->head + 1) % 64, w->count--, w->pos = 0;
        }
    }
    pthread_mutex_unlock(&w->mu);
    if (len > 0) memset(out, 0, (size_t)len);   /* (behind: silence) */
}
MMRESULT waveOutOpen(HWAVEOUT *out, UINT device, const WAVEFORMATEX *f, DWORD_PTR cb, DWORD_PTR inst, DWORD flags) {
    (void)cb, (void)inst, (void)flags;
    /* (the same SDL as the window's and the controllers', beside the executable first: hos_sdl.c) */
    if (!hos_sdl_audio_open || !hos_sdl_audio_close || !hos_sdl_audio_alive || !out || !f) return MMSYSERR_ERROR;
    WaveOut *w = (WaveOut *)calloc(1, sizeof *w);
    if (!w) return MMSYSERR_ERROR;
    if (pthread_mutex_init(&w->mu, NULL)) { free(w); return MMSYSERR_ERROR; }
    w->audio = hos_sdl_audio_open(device, (int)f->nSamplesPerSec, (int)f->nChannels, f->wBitsPerSample == 16, wave_pull, w);
    if (!w->audio) {
        pthread_mutex_destroy(&w->mu);
        free(w);
        return MMSYSERR_ERROR;
    }
    *out = w;
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutPrepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size) { (void)w, (void)size; h->dwFlags |= WHDR_PREPARED; return 0; }
MMRESULT waveOutUnprepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size) { (void)w, (void)size; h->dwFlags &= ~(DWORD)WHDR_PREPARED; return 0; }
MMRESULT waveOutWrite(HWAVEOUT hw, WAVEHDR *h, UINT size) {
    (void)size;
    WaveOut *w = (WaveOut *)hw;
    if (!hos_sdl_audio_alive(w->audio)) return MMSYSERR_NODRIVER;
    pthread_mutex_lock(&w->mu);
    if (w->count >= 60) {
        WAVEHDR *old = w->q[w->head];
        __atomic_store_n(&old->dwFlags, (old->dwFlags & ~(DWORD)WHDR_INQUEUE) | WHDR_DONE, __ATOMIC_RELEASE);
        w->head = (w->head + 1) % 64;
        w->count--;
        w->pos = 0;
    }
    h->dwFlags = (h->dwFlags & ~(DWORD)WHDR_DONE) | WHDR_INQUEUE;
    w->q[(w->head + w->count) % 64] = h;
    w->count++;
    pthread_mutex_unlock(&w->mu);
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutReset(HWAVEOUT hw) {
    WaveOut *w = (WaveOut *)hw;
    pthread_mutex_lock(&w->mu);
    while (w->count) {
        w->q[w->head]->dwFlags = (w->q[w->head]->dwFlags & ~(DWORD)WHDR_INQUEUE) | WHDR_DONE;
        w->head = (w->head + 1) % 64, w->count--;
    }
    w->pos = 0;
    pthread_mutex_unlock(&w->mu);
    return 0;
}
MMRESULT waveOutClose(HWAVEOUT hw) {
    WaveOut *w = (WaveOut *)hw;
    if (!w) return MMSYSERR_ERROR;
    /* Closing synchronizes callbacks before their queue/userdata are destroyed. Holding mu would deadlock. */
    hos_sdl_audio_close(w->audio);
    waveOutReset(hw);
    pthread_mutex_destroy(&w->mu);
    free(w);
    return MMSYSERR_NOERROR;
}
