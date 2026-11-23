/* (ps5, lot 40) host/hos_thread_times.c on Linux, held against /proc/self/task/<tid>/schedstat (the kernel's own count of a
   thread's CPU time, the first field in ns). tests/test_thread_times.py builds and runs it; see there for what each mode shows.

   The harness stands in for the runtime: hos_log (each line printed as "LOG <line>"; a report's lines are checked here), the
   stage trail ("TRAIL <line>"), the clock, the frames, and win32_posix.c's registry (hos_posix_thread_times) for its own
   threads, which mark around their waits as the console's do (HOS_CPU_MARK's test: hos_cpu_marks, then the slot):
     tt busy      spins, marks every 2 ms, never waits                         ~100 %
     tt half      5 ms of work, 5 ms asleep                                       ~50 %
     tt quarter   2 ms of work, 6 ms asleep                                       ~25 %
     tt sleepy    asleep 100 ms at a time                                          ~0 %
     tt silent    marks once, then spins and never marks again (with a thread's own reading: "running unseen")
     tt blocked   marks on its way into a wait it never leaves                     0 %
     tt hidden    not registered (as RADV's threads): 3 ms of work, 7 ms asleep   ~30 % (kern-proc alone sees it)
   kern-proc is made from /proc: one FreeBSD 11 struct kinfo_proc a thread (ki_structsize 1088, ki_pid at 72, ki_runtime in us at
   328, ki_tdname at 394, ki_tid at 600), or a wrong layout on purpose.

   thread_times_test <mode> [reports]
     run         the readings as Linux has them, the report every 2 s; HOOKSHOT_PS5_THREAD_TIMES_SKIP picks the one in use
     <fault>     a wrong reading, the calibration alone: pthread-caller, rusage-process, tid-wall, own-einval, kp-shift,
                 kp-noname, kp-refused
     bench       the marks' costs: off, on and not due, a thread's own readings */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../host/hos_thread_times.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

unsigned long rx_ps5_presents;
static int64_t t_start;
int64_t hos_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
double hos_ps5_since_start(void) { return (hos_now_ns() - t_start) / 1e9; }
int hos_ps5_tid(void) { return (int)syscall(SYS_gettid); }
void hos_ps5_trail(const char *fmt, ...) {
    char b[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    printf("TRAIL %s\n", b);
    fflush(stdout);
}

/* ---- the harness's threads ---- */
typedef struct wt {
    const char *name;
    int registered, tid;
    pthread_t th;
    struct hos_cpu_slot slot;
    void (*body)(struct wt *);
    volatile int ready;
} wt;
#define MARK(w, b) do { if (__builtin_expect(hos_cpu_marks != 0, 0)) hos_cpu_slot_mark(&(w)->slot, (b)); } while (0)
static void spin_ns(int64_t ns) {
    const int64_t end = hos_now_ns() + ns;
    while (hos_now_ns() < end) {}
}
static void nap_ns(int64_t ns) {
    struct timespec ts = {ns / 1000000000LL, ns % 1000000000LL};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
static void body_busy(wt *w) {
    for (;;) {
        spin_ns(2000000);
        MARK(w, 0);
    }
}
static void body_duty(wt *w, int64_t on, int64_t off, int marks) {
    for (;;) {
        spin_ns(on);
        if (marks) MARK(w, 1);
        nap_ns(off);
        if (marks) MARK(w, 0);
    }
}
static void body_half(wt *w) { body_duty(w, 5000000, 5000000, 1); }
static void body_quarter(wt *w) { body_duty(w, 2000000, 6000000, 1); }
static void body_hidden(wt *w) { body_duty(w, 3000000, 7000000, 0); }
static void body_sleepy(wt *w) {
    for (;;) {
        MARK(w, 1);
        nap_ns(100000000);
        MARK(w, 0);
    }
}
static void body_silent(wt *w) {
    while (!hos_cpu_marks) nap_ns(1000000);
    MARK(w, 0);
    for (;;) spin_ns(1000000000);
}
static void body_blocked(wt *w) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    while (!hos_cpu_marks) nap_ns(1000000);
    nap_ns(1500000000);   /* (some CPU before: the marks' first read) */
    MARK(w, 1);
    pthread_mutex_lock(&mu);
    for (;;) pthread_cond_wait(&cv, &mu);
}
static wt W[] = {
    {"tt busy", 1, 0, 0, {0}, body_busy, 0},       {"tt half", 1, 0, 0, {0}, body_half, 0},
    {"tt quarter", 1, 0, 0, {0}, body_quarter, 0}, {"tt sleepy", 1, 0, 0, {0}, body_sleepy, 0},
    {"tt silent", 1, 0, 0, {0}, body_silent, 0},   {"tt blocked", 1, 0, 0, {0}, body_blocked, 0},
    {"tt hidden", 0, 0, 0, {0}, body_hidden, 0},
};
enum { NW = sizeof W / sizeof W[0] };
static void *wt_main(void *p) {
    wt *w = (wt *)p;
    pthread_setname_np(pthread_self(), w->name);
    w->tid = hos_ps5_tid();
    __atomic_store_n(&w->ready, 1, __ATOMIC_RELEASE);
    w->body(w);
    return NULL;
}
int hos_posix_thread_times(hos_thread_time_row *out, int max) {
    int n = 0;
    for (int i = 0; i < NW && n < max; i++) {
        if (!W[i].registered || !__atomic_load_n(&W[i].ready, __ATOMIC_ACQUIRE)) continue;
        out[n].tid = W[i].tid;
        out[n].pthread = (void *)W[i].th;
        snprintf(out[n].name, sizeof out[n].name, "%s", W[i].name);
        hos_cpu_slot_read(&W[i].slot, &out[n].cpu);
        n++;
    }
    return n;
}

/* ---- /proc ---- */
static int64_t schedstat_ns(int tid) {
    char p[64], b[128];
    snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    long long v = -1;
    if (fgets(b, sizeof b, f)) v = atoll(b);
    fclose(f);
    return v;
}
static int kp_size = 1088, kp_shift = 0, kp_noname = 0, kp_refuse = 0;
static int synth_kproc(void *buf, size_t *len) {
    if (kp_refuse) return EPERM;
    int tids[256], n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return EIO;
    for (struct dirent *e; (e = readdir(d)) && n < 256;)
        if (e->d_name[0] != '.') tids[n++] = atoi(e->d_name);
    closedir(d);
    if (!buf) {
        *len = (size_t)(n + 4) * (size_t)kp_size;
        return 0;
    }
    if (*len < (size_t)n * (size_t)kp_size) return ENOMEM;
    unsigned char *b = (unsigned char *)buf;
    const int pid = getpid();
    for (int i = 0; i < n; i++) {
        unsigned char *r = b + (size_t)i * (size_t)kp_size;
        memset(r, 0, (size_t)kp_size);
        memcpy(r, &kp_size, 4);
        memcpy(r + 72, &pid, 4);
        const int64_t ns = schedstat_ns(tids[i]);
        const uint64_t us = ns > 0 ? (uint64_t)ns / 1000 : 0;
        memcpy(r + 328 + kp_shift, &us, 8);
        char p[64], name[32] = "";
        snprintf(p, sizeof p, "/proc/self/task/%d/comm", tids[i]);
        FILE *f = fopen(p, "r");
        if (f) {
            if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
            fclose(f);
        }
        if (!kp_noname) memcpy(r + 394 + kp_shift, name, strlen(name) < 16 ? strlen(name) : 16);
        memcpy(r + 600 + kp_shift, &tids[i], 4);
    }
    *len = (size_t)n * (size_t)kp_size;
    return 0;
}
/* wrong readings */
static int64_t caller_clock(void *t) {   /* (the console's pthread_getcpuclockid: the caller's clock) */
    (void)t;
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static int64_t process_as_thread(void) {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return ((int64_t)u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000000000LL + ((int64_t)u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1000;
}
static int64_t wall_clock(int tid) { (void)tid; return hos_now_ns(); }
static int64_t einval(void) { return -EINVAL; }

/* ---- the lines ---- */
static pthread_mutex_t lines_mu = PTHREAD_MUTEX_INITIALIZER;
static char log_text[1 << 16];
static size_t log_used;
static int reports, failures, checks, calibrated, cal_lines;
static int64_t ref_ns[NW], ref_at = -1;
static double tol_points = 3.0;
static char used_reading[32];
static void fail(const char *fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    printf("FAIL %s\n", b);
    failures++;
}
/* a busiest line: each harness thread's share against schedstat's since the report before */
static void check_busiest(const char *line) {
    const int64_t now = hos_now_ns();
    int64_t ns[NW];
    for (int i = 0; i < NW; i++) ns[i] = schedstat_ns(W[i].tid);
    if (ref_at >= 0) {
        const double period = (double)(now - ref_at);
        for (int i = 0; i < NW; i++) {
            const double want = (ns[i] - ref_ns[i]) * 100.0 / period;
            char key[48];
            snprintf(key, sizeof key, " [%d] ", W[i].tid);
            const char *at = strstr(line, key);
            const int registered_only = strcmp(used_reading, "kern-proc") != 0;
            const int own = !strncmp(used_reading, "own-", 4);
            if (!at) {   /* (not listed: under the cutoff, unseen, or not a thread the reading sees) */
                if (want >= 5.0 && !(registered_only && !W[i].registered) && !(own && !strcmp(W[i].name, "tt silent")))
                    fail("report %d by %s: %s (schedstat %.1f%%) is not in the busiest line", reports, used_reading, W[i].name, want);
                continue;
            }
            const double got = atof(at + strlen(key));
            checks++;
            const int ok = fabs(got - want) <= tol_points;
            printf("CHECK report %d by %s: %-11s %6.1f%%, schedstat %6.1f%%: %s\n", reports, used_reading, W[i].name, got, want,
                   ok ? "ok" : "OFF");
            if (!ok) fail("report %d by %s: %s read %.1f%%, schedstat %.1f%%", reports, used_reading, W[i].name, got, want);
            if (registered_only && !W[i].registered) fail("%s is not registered and %s listed it", W[i].name, used_reading);
            if (own && !strcmp(W[i].name, "tt silent")) fail("tt silent is listed by %s, which cannot see it", used_reading);
        }
    }
    for (int i = 0; i < NW; i++) ref_ns[i] = ns[i];
    ref_at = now;
}
void hos_log(const char *fmt, ...) {
    char b[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&lines_mu);
    printf("LOG %s\n", b);
    const size_t n = strlen(b);
    if (log_used + n + 2 < sizeof log_text) memcpy(log_text + log_used, b, n), log_used += n, log_text[log_used++] = '\n', log_text[log_used] = 0;
    if (!strncmp(b, "cpu: reading ", 13)) cal_lines++;
    if (!strncmp(b, "cpu: the report every ", 22) || !strncmp(b, "cpu: no reading", 15) || !strncmp(b, "cpu: calibration: no", 20)) calibrated = 1;
    if (!strncmp(b, "cpu: report ", 12)) {
        const char *by = strstr(b, " by ");
        if (by) sscanf(by + 4, "%31[^:]", used_reading);
        if (strlen(b) > 1000) fail("a report line of %zu characters", strlen(b));
    }
    if (!strncmp(b, "cpu: busiest ", 13)) {
        reports++;
        if (strlen(b) > 1000) fail("a busiest line of %zu characters", strlen(b));
        check_busiest(b);
    }
    fflush(stdout);
    pthread_mutex_unlock(&lines_mu);
}
static int has(const char *text) { return strstr(log_text, text) != NULL; }
static void want(const char *text, const char *why) {
    if (!has(text)) fail("%s: no \"%s\" in the log", why, text);
    else printf("PASS %s\n", why);
}

static void bench(void) {
    enum { N = 20000000 };
    struct hos_cpu_slot s;
    memset(&s, 0, sizeof s);
    volatile int sink = 0;
    int64_t t0 = hos_now_ns();
    for (int i = 0; i < N; i++) {   /* (the console's HOS_CPU_MARK with the report off) */
        if (__builtin_expect(hos_cpu_marks != 0, 0)) hos_cpu_slot_mark(&s, i & 1);
        sink += i;
    }
    const double off = (hos_now_ns() - t0) / (double)N;
    t0 = hos_now_ns();
    for (int i = 0; i < N; i++) sink += i;
    const double base = (hos_now_ns() - t0) / (double)N;
    printf("BENCH off: %.2f ns a mark (the empty loop %.2f ns)\n", off, base);
    /* (on: after a real calibration, the marks timed by the TSC as on the console) */
    hos_thread_times_test_ops.kproc = synth_kproc;
    setenv("HOOKSHOT_PS5_THREAD_TIMES", "600", 1);
    hos_thread_times_start();
    for (int i = 0; i < 300 && !hos_cpu_marks; i++) nap_ns(20000000);
    if (!hos_cpu_marks) {
        printf("BENCH FAIL: the calibration did not turn the marks on\n");
        return;
    }
    hos_cpu_slot_mark(&s, 0);
    t0 = hos_now_ns();
    for (int i = 0; i < N / 4; i++) hos_cpu_slot_mark(&s, i & 1);
    const double on = (hos_now_ns() - t0) / (double)(N / 4);
    printf("BENCH on, not due (a blocked flip each time): %.1f ns a mark; reads %u in %.2f s\n", on, s.reads, (hos_now_ns() - t0) / 1e9);
    struct timespec ts;
    t0 = hos_now_ns();
    for (int i = 0; i < 200000; i++) clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    printf("BENCH own-clock read: %.0f ns\n", (hos_now_ns() - t0) / 200000.0);
    struct rusage u;
    t0 = hos_now_ns();
    for (int i = 0; i < 200000; i++) getrusage(RUSAGE_THREAD, &u);
    printf("BENCH own-rusage read: %.0f ns\n", (hos_now_ns() - t0) / 200000.0);
    struct hos_cpu_sample o;
    t0 = hos_now_ns();
    for (int i = 0; i < 1000000; i++) hos_cpu_slot_read(&s, &o);
    printf("BENCH a slot read by the report: %.1f ns\n", (hos_now_ns() - t0) / 1e6);
    (void)sink;
}

int main(int argc, char **argv) {
    t_start = hos_now_ns();
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *mode = argc > 1 ? argv[1] : "run";
    const int want_reports = argc > 2 ? atoi(argv[2]) : 3;
    if (!strcmp(mode, "bench")) {
        bench();
        return 0;
    }
    hos_thread_times_test_ops.kproc = synth_kproc;
    int fault = 1;
    if (!strcmp(mode, "pthread-caller")) hos_thread_times_test_ops.pthread_clock = caller_clock;
    else if (!strcmp(mode, "rusage-process")) hos_thread_times_test_ops.own_rusage = process_as_thread;
    else if (!strcmp(mode, "tid-wall")) hos_thread_times_test_ops.tid_clock = wall_clock;
    else if (!strcmp(mode, "own-einval")) hos_thread_times_test_ops.own_clock = einval;
    else if (!strcmp(mode, "kp-shift")) kp_size = 1096, kp_shift = 8;
    else if (!strcmp(mode, "kp-noname")) kp_noname = 1;
    else if (!strcmp(mode, "kp-refused")) kp_refuse = 1;
    else fault = 0;
    if (getenv("TT_TOLERANCE")) tol_points = atof(getenv("TT_TOLERANCE"));
    for (int i = 0; i < NW; i++) {
        pthread_create(&W[i].th, NULL, wt_main, &W[i]);
        while (!__atomic_load_n(&W[i].ready, __ATOMIC_ACQUIRE)) nap_ns(100000);
    }
    setenv("HOOKSHOT_PS5_THREAD_TIMES", "2", 1);
    hos_thread_times_start();
    const int64_t deadline = hos_now_ns() + (int64_t)(fault ? 30 : 40 + 3 * want_reports) * 1000000000LL;
    while (hos_now_ns() < deadline) {
        nap_ns(50000000);
        rx_ps5_presents += 3;   /* (60 frames a second) */
        pthread_mutex_lock(&lines_mu);
        const int done = fault ? calibrated : reports >= want_reports + 1;   /* (the first report has no reference before it) */
        pthread_mutex_unlock(&lines_mu);
        if (done) break;
    }
    pthread_mutex_lock(&lines_mu);
    if (cal_lines != 5) fail("%d calibration lines, not 5", cal_lines);
    if (!strcmp(mode, "pthread-caller")) want("cpu: reading pthread-clock (", "pthread-clock read"), want("refused: not the witness's CPU", "the caller's clock refused");
    if (!strcmp(mode, "rusage-process")) want("own-rusage (the witness's own getrusage(RUSAGE_THREAD)): burning", "own-rusage read");
    if (!strcmp(mode, "tid-wall")) want("refused: not the witness's CPU", "a wall clock refused");
    if (!strcmp(mode, "own-einval")) want("refused: it failed (error 22)", "a failing reading refused"),
                                     want("the threads' marks: own-rusage", "the marks fall back to own-rusage");
    if (!strcmp(mode, "kp-shift")) want("refused: the records are not laid out as read", "a shifted layout refused"),
                                   want("found at +608", "the witness's tid located"), want("+336 (us)", "ki_runtime located"),
                                   want("its name at +402", "ki_tdname located");
    if (!strcmp(mode, "kp-noname")) want("(not its name: the names are not used)", "names refused"),
                                    want("the report every 2 s by kern-proc (every thread of the process; the kernel's names not used)",
                                         "kern-proc kept without its names");
    if (!strcmp(mode, "kp-refused")) want("sysctl refused: errno 1", "a refused sysctl");
    /* each fault's reading refused, and the others as Linux has them kept */
    static const char *const names[] = {"own-clock", "own-rusage", "pthread-clock", "tid-clock", "kern-proc"};
    const char *refused = !strcmp(mode, "pthread-caller") ? "pthread-clock" : !strcmp(mode, "rusage-process") ? "own-rusage"
                        : !strcmp(mode, "tid-wall") ? "tid-clock" : !strcmp(mode, "own-einval") ? "own-clock"
                        : !strcmp(mode, "kp-shift") || !strcmp(mode, "kp-refused") ? "kern-proc" : "";
    const char *skip = getenv("HOOKSHOT_PS5_THREAD_TIMES_SKIP");
    for (int m = 0; m < 5; m++) {
        char key[64];
        snprintf(key, sizeof key, "cpu: reading %s (", names[m]);
        const char *at = strstr(log_text, key);
        const char *eol = at ? strchr(at, '\n') : NULL;
        if (!at || !eol) { fail("no line for %s", names[m]); continue; }
        char line[1200];
        snprintf(line, sizeof line, "%.*s", (int)(eol - at), at);
        const int skipped = skip && strstr(skip, names[m]);
        const int kept = strstr(line, ": kept") != NULL;
        const int expect_kept = !skipped && strcmp(names[m], refused);
        if (kept != expect_kept) fail("%s %s, expected %s: %s", names[m], kept ? "kept" : "refused", expect_kept ? "kept" : "refused", line);
        else printf("PASS %s %s\n", names[m], kept ? "kept" : skipped ? "not tried" : "refused");
    }
    if (!fault) {
        if (reports < want_reports + 1) fail("%d reports in time, %d wanted", reports, want_reports + 1);
        if (checks < 3 * want_reports) fail("%d checks against schedstat", checks);
        if (!strncmp(used_reading, "own-", 4)) want("running unseen: tt silent (", "tt silent: running unseen");
        if (!strcmp(used_reading, "kern-proc")) want(" tt hidden [", "kern-proc lists a thread the host did not start, by the kernel's name");
        else want("not seen)", "the threads not seen are said");
        if (!strstr(log_text, "cpu: report 1 at ")) fail("no first report");
    }
    printf("%s: %d failures, %d checks against schedstat, %d reports by %s\n", failures ? "FAILED" : "PASSED", failures, checks, reports,
           used_reading[0] ? used_reading : "-");
    fflush(stdout);
    pthread_mutex_unlock(&lines_mu);
    _exit(failures ? 1 : 0);
}
