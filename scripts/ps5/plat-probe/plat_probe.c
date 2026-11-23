/* The platform probe title, PPSA99147 "Hookshot Platform Probe": the PS5 platform layer of branch ps5-plat, run on the
   console check by check. Built from the real code (scripts/ps5/build-plat-probe.sh links the objects nx2/build.py makes
   for the eboot: host/posix's ps5_platform, ps5_window, ps5_files, ps5_broker_client, hos_sdl_ps5 and win32_posix, host/'s
   mkw_glue and pad_sdl, a64-kit's runtime with its 128 MiB write-watch table), never a copy of it; only the runtime's
   logging and thread-naming hooks are stand-ins (probe_runtime.c). The guest window, its backing, the SIGSEGV path, the
   threads, the files, the broker, the pad and the sound are host/'s.

   Every check writes BEGIN, then PASS, FAIL or INFO lines, then a VERDICT where a design choice hangs on it, each line
   written at once to /app0/plat-probe.txt (/data/homebrew/PPSA99147/plat-probe.txt over FTP) and to klog
   ([plat-probe]). A watchdog ends the title with a TIMEOUT line when a check stops making progress (a deadlock reported,
   not hung on). /app0/plat-probe.ini sets the checks and their sizes (EXPECTED.txt lists them). The title ends through
   the shell; nothing is left behind but the results file and, in the data folder (/data/grapnel since lot 80), the broker's
   token.
   SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <windows.h>   /* (host/posix's: the runtime's Win32 calls, win32_posix.c) */
#include <mmsystem.h>

#include "ps5.h"
#include "ps5_broker.h"
#include "plat_probe.h"

#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>
#include <ps5platform/memory.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef A64_HOST_BASE
#error "A64_HOST_BASE comes from the build (nx2/toolchain_ps5.py: 0x8000000000)"
#endif
#define WINDOW_BYTES ((uint64_t)1 << 39)
#define GUEST(a) ((uint8_t *)(uintptr_t)((uint64_t)A64_HOST_BASE + (uint64_t)(a)))
#define MIB (1ull << 20)
#define GIB (1ull << 30)
#ifndef PROBE_REVISION
#define PROBE_REVISION "unknown"
#endif

void hos_commit(uint64_t addr, uint64_t size);            /* host/mkw_glue.c */
void hos_commit_init(void);                               /* host/mkw_glue.c: the SIGSEGV handlers */
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]);    /* host/pad_sdl.c */
void hos_sdl_rumble(float low, float high);               /* host/pad_sdl.c */
unsigned long long hos_ps5_audio_grains(void);            /* host/posix/hos_sdl_ps5.c */
extern uint8_t a64_dirty[];                               /* a64-kit's runtime: a byte per 4 KiB of the window */
int32_t sceKernelVirtualQuery(const void *address, int flags, void *info, size_t size);
int32_t sceKernelGetSystemSwVersion(struct ps5_kernel_sw_version *version);

/* ---- configuration: /app0/plat-probe.ini, key=value; env.NAME=value sets an environment variable before anything ---- */
static struct {
    char tests[256];
    unsigned mem_target_mb, mem_step_mb, mem_reserve_mb;
    unsigned lazy_threads, lazy_mb;
    unsigned stack_threads;
    unsigned fdv_files;
    unsigned broker, broker_write_mb, broker_read_mb, data_read_mb, data_write_mb;
    char read_file[512];
    unsigned pad_seconds, tone_ms;
    unsigned watchdog_s;
    unsigned elevate;
    char nup_file[512], nup_hashes[512];
    unsigned nup_threads, nup_seconds;
    unsigned sched_threads, sched_cores, sched_spinners, sched_seconds;
} cfg = {"all", 9216, 256, 384, 8, 128, 40, 320, 1, 2048, 256, 1024, 2048, "", 10, 1000, 60, 0,
         PS5_DATA_DIR "/game.nup", "/app0/nup-hashes.txt", 8, 5, 24, 6, 0, 3};   /* (lot 80: /data/grapnel) */

static void read_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[640];
    while (fgets(line, sizeof line, f)) {
        char *e = line + strcspn(line, "\r\n");
        *e = 0;
        if (line[0] == '#' || !line[0]) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strncmp(k, "env.", 4)) { setenv(k + 4, v, 1); continue; }
#define U(name) if (!strcmp(k, #name)) { cfg.name = (unsigned)strtoul(v, NULL, 0); continue; }
        U(mem_target_mb) U(mem_step_mb) U(mem_reserve_mb) U(lazy_threads) U(lazy_mb) U(stack_threads) U(fdv_files)
        U(broker) U(broker_write_mb) U(broker_read_mb) U(data_read_mb) U(data_write_mb) U(pad_seconds) U(tone_ms) U(watchdog_s) U(elevate)
        U(nup_threads) U(nup_seconds) U(sched_threads) U(sched_cores) U(sched_spinners) U(sched_seconds)
#undef U
        if (!strcmp(k, "tests")) snprintf(cfg.tests, sizeof cfg.tests, "%s", v);
        if (!strcmp(k, "read_file")) snprintf(cfg.read_file, sizeof cfg.read_file, "%s", v);
        if (!strcmp(k, "nup_file")) snprintf(cfg.nup_file, sizeof cfg.nup_file, "%s", v);
        if (!strcmp(k, "nup_hashes")) snprintf(cfg.nup_hashes, sizeof cfg.nup_hashes, "%s", v);
    }
    fclose(f);
    if (cfg.lazy_threads > 32) cfg.lazy_threads = 32;
    if (cfg.lazy_mb > 1024) cfg.lazy_mb = 1024;
    if (cfg.stack_threads > 64) cfg.stack_threads = 64;
    if (cfg.mem_step_mb < 64) cfg.mem_step_mb = 64;
    if (cfg.mem_target_mb > 11 * 1024) cfg.mem_target_mb = 11 * 1024;   /* (the cap: never the whole pool) */
    if (cfg.fdv_files > 2000) cfg.fdv_files = 2000;
    if (cfg.nup_threads < 1) cfg.nup_threads = 1;
    if (cfg.nup_threads > 32) cfg.nup_threads = 32;
    if (cfg.nup_seconds > 60) cfg.nup_seconds = 60;
    if (cfg.sched_threads < 2) cfg.sched_threads = 2;
    if (cfg.sched_threads > 64) cfg.sched_threads = 64;
    if (cfg.sched_cores < 1) cfg.sched_cores = 1;
    if (cfg.sched_cores > 8) cfg.sched_cores = 8;
    if (cfg.sched_spinners > 32) cfg.sched_spinners = 32;
    if (cfg.sched_seconds < 1) cfg.sched_seconds = 1;
    if (cfg.sched_seconds > 10) cfg.sched_seconds = 10;
}
static int want(const char *test) {
    char list[300];
    snprintf(list, sizeof list, ",%s,", cfg.tests);
    char key[64];
    snprintf(key, sizeof key, ",%s,", test);
    return strstr(list, ",all,") || strstr(list, key);
}

/* ---- the results ---- */
static int out_fd = -1;
static pthread_mutex_t out_mu = PTHREAD_MUTEX_INITIALIZER;
static int npass, nfail, ninfo;
static char failed[1024];
static void out_raw(const char *line) {   /* (no lock: the watchdog's, when a check may hold out_mu) */
    char k[1100];
    snprintf(k, sizeof k, "[plat-probe] %s\n", line);
    sceKernelDebugOutText(0, k);
    if (out_fd >= 0) {
        const size_t n = strlen(line);
        if (write(out_fd, line, n) < 0 || write(out_fd, "\n", 1) < 0) {}
    }
}
void out(const char *format, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    pthread_mutex_lock(&out_mu);
    out_raw(line);
    pthread_mutex_unlock(&out_mu);
}
static void result(int ok, const char *id, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void result(int ok, const char *id, const char *format, ...) {
    char text[900];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof text, format, ap);
    va_end(ap);
    out("%s %s: %s", ok ? "PASS" : "FAIL", id, text);
    if (ok) npass++;
    else {
        nfail++;
        if (strlen(failed) + strlen(id) + 2 < sizeof failed) strcat(strcat(failed, " "), id);
    }
}
#define INFO(id, ...) do { ninfo++; char t_[900]; snprintf(t_, sizeof t_, __VA_ARGS__); out("INFO %s: %s", id, t_); } while (0)
#define VERDICT(id, ...) do { char t_[900]; snprintf(t_, sizeof t_, __VA_ARGS__); out("VERDICT %s: %s", id, t_); } while (0)

static void summary(const char *why) {
    char line[1300];
    snprintf(line, sizeof line, "SUMMARY pass=%d fail=%d info=%d%s%s; failed:%s", npass, nfail, ninfo, why ? "; ended by " : "",
             why ? why : "", failed[0] ? failed : " none");
    out_raw(line);
    out_raw("END plat-probe");
    if (out_fd >= 0) fsync(out_fd);
}

/* ---- the watchdog: a check that stops making progress is reported and the title ends, never hangs ---- */
static volatile int64_t wd_deadline;   /* ns, 0: none */
static const char *volatile wd_check = "";
static char wd_progress[256];
static int64_t now_ns(void) {
    const struct timespec t = probe_now();
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}
static void progress(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(wd_progress, sizeof wd_progress, format, ap);
    va_end(ap);
}
static void *watchdog(void *unused) {
    (void)unused;
    for (;;) {
        usleep(250000);
        const int64_t d = wd_deadline;
        if (d && now_ns() > d) {
            char line[512];
            snprintf(line, sizeof line, "TIMEOUT %s: no end within its deadline; last progress: %s", wd_check, wd_progress);
            out_raw(line);
            nfail++;
            summary("the watchdog");
            hos_ps5_exit(3);
        }
    }
    return NULL;
}
static void check_begin(const char *id, unsigned seconds) {
    out("BEGIN %s", id);
    wd_progress[0] = 0;
    wd_check = id;
    wd_deadline = now_ns() + (int64_t)(seconds ? seconds : cfg.watchdog_s) * 1000000000ll;
}
static void check_end(void) { wd_deadline = 0; }
/* (the same for the checks in files of their own: probe_nup.c) */
void probe_result(int ok, const char *id, const char *format, ...) {
    char text[900];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof text, format, ap);
    va_end(ap);
    result(ok, id, "%s", text);
}
void probe_info(const char *id, const char *format, ...) {
    char text[900];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof text, format, ap);
    va_end(ap);
    INFO(id, "%s", text);
}
void probe_verdict(const char *id, const char *format, ...) {
    char text[900];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof text, format, ap);
    va_end(ap);
    VERDICT(id, "%s", text);
}
void probe_progress(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(wd_progress, sizeof wd_progress, format, ap);
    va_end(ap);
}
void probe_check_begin(const char *id, unsigned seconds) { check_begin(id, seconds); }
void probe_check_end(void) { check_end(); }

/* ---- small helpers ---- */
static unsigned mxcsr(void) {
    unsigned m;
    __asm__ volatile("stmxcsr %0" : "=m"(m));
    return m;
}
struct vq {   /* (the public PS4 SDK's SceKernelVirtualQueryInfo, which the console keeps) */
    void *start, *end;
    int64_t offset;
    int32_t protection, memory_type;
    uint8_t flags;   /* 1 flexible, 2 direct, 4 stack, 8 pooled, 16 committed */
    char name[32];
};
static int vquery(const void *p, struct vq *q) {
    union { struct vq q; char room[256]; } u;   /* (room, should the console's record be longer than the PS4's) */
    memset(&u, 0, sizeof u);
    const int r = sceKernelVirtualQuery(p, 0, &u, sizeof u.q);
    *q = u.q;
    return r;
}
static void vq_text(const void *p, char *s, size_t n) {
    struct vq q;
    const int r = vquery(p, &q);
    if (r) snprintf(s, n, "sceKernelVirtualQuery %#x", (unsigned)r);
    else snprintf(s, n, "%p-%p prot %#x type %d%s%s%s%s", q.start, q.end, q.protection, q.memory_type, q.flags & 1 ? " flexible" : "",
                  q.flags & 2 ? " direct" : "", q.flags & 4 ? " stack" : "", q.flags & 16 ? " committed" : "");
}
static void mem_text(char *s, size_t n) {
    struct ps5_memory_stats m;
    size_t flex = 0;
    sceKernelAvailableFlexibleMemorySize(&flex);
    if (!ps5_memory_query(&m)) { snprintf(s, n, "ps5_memory_query failed; flexible free %zu MiB", flex >> 20); return; }
    snprintf(s, n, "direct free %llu MiB (largest %llu MiB) of %llu MiB, flexible free %zu MiB", (unsigned long long)(m.free_bytes >> 20),
             (unsigned long long)(m.largest_bytes >> 20), (unsigned long long)(m.total_bytes >> 20), flex >> 20);
}
static int vq_state(const void *p, size_t *run) {   /* VirtualQuery's (win32_posix.c, the bitmap): 1 committed */
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return -1;
    if (run) *run = mbi.RegionSize;
    return mbi.State == MEM_COMMIT;
}
static uint64_t window_bytes;   /* what the window check reserved */
static int window_has(uint64_t guest, uint64_t bytes) { return guest + bytes <= window_bytes; }

/* ================================================================================================= 1. the window */
/* The first console run of the boot test (PPSA99145, logs/ps5-boot-run1) found one reservation of 512 GiB at
   0x80_0000_0000 refused. Before the runtime's own reservation (which then takes the first way that works), the probe asks
   the console, each answer logged and every range given back at once:
   - the map: every mapping from 0 to 1 TiB (sceKernelVirtualQuery, the next one each time), and the free gaps;
   - (a) one reservation at 0x80_0000_0000, MAP_FIXED (only where the map shows nothing), 512, 256, 128 and 64 GiB;
   - (b) with no address and at the hint, 512 and 256 GiB: where the kernel puts them;
   - the slots: 16 GiB at each 16 GiB from 64 GiB to 1 TiB, at the hint, and where the user map ends (64 KiB steps);
   - (c) the parts of the guest's layout Hookshot uses (the system region and the image below 4 GiB, the heap from
     64 GiB, the VAMM region at 192..320 GiB, the address regions down from 512 GiB), each at base + guest for the bases
     0x80_0000_0000, 0x7C_0000_0000 (16 GiB lower, the window ending at 0xFC_0000_0000) and 0 (guest = host, as Savage Wind);
   - the candidates: 512 GiB at 0x7C_0000_0000, 496 GiB at 0x80_0000_0000. */
#define VQ_FIND_NEXT 1
#define RADV_LO 0x4000000000ull   /* (RADV's PS5 winsys maps device memory at fixed addresses in [256 GiB, 512 GiB): */
#define RADV_HI 0x8000000000ull   /*  EglantinePS5 src/host_memory.cpp, PS5_Mesa winsys/ps5) */
static struct { uint64_t lo, hi; } maps[512];
static unsigned nmaps;
static int32_t try_reserve(uint64_t at, uint64_t bytes, int flags, uint64_t *placed) {
    void *p = (void *)(uintptr_t)at;
    const int32_t r = sceKernelReserveVirtualRange(&p, (size_t)bytes, flags, 65536);
    *placed = (uint64_t)(uintptr_t)p;
    if (r == 0) sceKernelMunmap(p, (size_t)bytes);
    return r;
}
static int map_free(uint64_t lo, uint64_t hi) {   /* nothing the map showed in [lo, hi) */
    for (unsigned i = 0; i < nmaps; i++) if (maps[i].lo < hi && maps[i].hi > lo) return 0;
    return 1;
}
static void window_map(void) {
    uint64_t a = 0x4000;
    nmaps = 0;
    while (a < (1ull << 40) && nmaps < 512) {
        union { struct vq q; char room[256]; } u;
        memset(&u, 0, sizeof u);
        const int32_t r = sceKernelVirtualQuery((void *)(uintptr_t)a, VQ_FIND_NEXT, &u, sizeof u.q);
        const struct vq q = u.q;
        if (r) {
            if (!nmaps) INFO("window.map", "sceKernelVirtualQuery(%#llx, FIND_NEXT): %#x", (unsigned long long)a, (unsigned)r);
            break;
        }
        const uint64_t lo = (uint64_t)(uintptr_t)q.start, hi = (uint64_t)(uintptr_t)q.end;
        if (hi <= a) break;
        maps[nmaps].lo = lo, maps[nmaps].hi = hi, nmaps++;
        char name[33];
        memcpy(name, q.name, 32), name[32] = 0;
        out("MAP %#012llx-%#012llx %8llu KiB prot %#04x type %2d%s%s%s%s %s", (unsigned long long)lo, (unsigned long long)hi,
            (unsigned long long)((hi - lo) >> 10), q.protection, q.memory_type, q.flags & 1 ? " flexible" : "", q.flags & 2 ? " direct" : "",
            q.flags & 4 ? " stack" : "", q.flags & 16 ? " committed" : "", name);
        a = hi;
    }
    INFO("window.map", "%u mappings from 0 to 1 TiB (MAP lines); the highest ends at %#llx", nmaps,
         nmaps ? (unsigned long long)maps[nmaps - 1].hi : 0ull);
    uint64_t prev = 0;
    for (unsigned i = 0; i <= nmaps; i++) {
        const uint64_t next = i < nmaps ? maps[i].lo : (1ull << 40);
        if (next > prev && next - prev >= 16 * GIB)
            INFO("window.map", "free (as far as the map shows): %#llx-%#llx, %llu GiB", (unsigned long long)prev, (unsigned long long)next,
                 (unsigned long long)((next - prev) >> 30));
        if (i < nmaps && maps[i].hi > prev) prev = maps[i].hi;
    }
}
static void window_explore(void) {
    uint64_t placed = 0;
    int32_t r;
    /* (a) MAP_FIXED at the window's base */
    static const unsigned fixed_gb[] = {512, 256, 128, 64};
    for (size_t i = 0; i < sizeof fixed_gb / sizeof fixed_gb[0]; i++) {
        const uint64_t bytes = (uint64_t)fixed_gb[i] * GIB;
        if (!map_free(A64_HOST_BASE, A64_HOST_BASE + bytes)) {
            INFO("window.a", "%u GiB at %#llx MAP_FIXED: skipped (the map shows a mapping there)", fixed_gb[i], (unsigned long long)A64_HOST_BASE);
            continue;
        }
        r = try_reserve(A64_HOST_BASE, bytes, PS5_KERNEL_MAP_FIXED, &placed);
        INFO("window.a", "%u GiB at %#llx MAP_FIXED: %s (%#x)", fixed_gb[i], (unsigned long long)A64_HOST_BASE,
             r ? "refused" : placed == A64_HOST_BASE ? "granted" : "granted ELSEWHERE", (unsigned)r);
    }
    /* (b) where the kernel puts 512 and 256 GiB, at the hint and with no address */
    static const unsigned hint_gb[] = {512, 496, 256};
    for (size_t i = 0; i < sizeof hint_gb / sizeof hint_gb[0]; i++) {
        const uint64_t bytes = (uint64_t)hint_gb[i] * GIB;
        r = try_reserve(A64_HOST_BASE, bytes, 0, &placed);
        INFO("window.b", "%u GiB at the hint %#llx: %s %#llx (%#x)", hint_gb[i], (unsigned long long)A64_HOST_BASE,
             r ? "refused," : placed == A64_HOST_BASE ? "granted at" : "placed at", (unsigned long long)placed, (unsigned)r);
        r = try_reserve(0, bytes, 0, &placed);
        INFO("window.b", "%u GiB with no address: %s %#llx (%#x)", hint_gb[i], r ? "refused," : "placed at", (unsigned long long)placed, (unsigned)r);
    }
    /* the 16 GiB slots from 64 GiB to 1 TiB, at the hint: + granted there, > moved, - refused */
    char line[200];
    int n = snprintf(line, sizeof line, "16 GiB slots from 64 GiB (+ granted at the slot, > placed higher, - refused):");
    uint64_t top_slot = 0;
    for (uint64_t at = 64 * GIB; at < (1ull << 40); at += 16 * GIB) {
        r = try_reserve(at, 16 * GIB, 0, &placed);
        const char c = r ? '-' : placed == at ? '+' : '>';
        if (c == '+') top_slot = at;
        if (!(at % (256 * GIB))) n += snprintf(line + n, sizeof line - (size_t)n, " %#llx:", (unsigned long long)at);
        n += snprintf(line + n, sizeof line - (size_t)n, "%c", c);
    }
    INFO("window.slots", "%s", line);
    /* where the user map ends: the highest 64 KiB granted at its own address, searched from the top slot granted */
    uint64_t lo = top_slot ? top_slot + 16 * GIB - 65536 : 64 * GIB, hi = 1ull << 40;   /* (lo granted, hi not) */
    while (hi - lo > 65536) {
        const uint64_t mid = ((lo + hi) / 2) & ~0xffffull;
        r = try_reserve(mid, 65536, 0, &placed);
        if (!r && placed == mid) lo = mid;
        else hi = mid;
    }
    INFO("window.top", "the highest 64 KiB granted at its address: %#llx: the title's address space ends at %#llx (%llu GiB)",
         (unsigned long long)lo, (unsigned long long)(lo + 65536), (unsigned long long)((lo + 65536) >> 30));
    /* (c) the guest layout's parts, at base + guest */
    static const struct { const char *what; uint64_t at, bytes; } parts[] = {
        {"system region and image (0..4 GiB)", 0, 4 * GIB},
        {"low holes for regions (4..64 GiB)", 4 * GIB, 60 * GIB},
        {"heap (64..80 GiB)", 64 * GIB, 16 * GIB},
        {"VAMM region (192..320 GiB)", 192 * GIB, 128 * GIB},
        {"address regions (496..512 GiB)", 496 * GIB, 16 * GIB},
        {"address regions (448..496 GiB)", 448 * GIB, 48 * GIB}};
    static const uint64_t bases[] = {0x8000000000ull, 0x7c00000000ull, 0};
    for (size_t b = 0; b < sizeof bases / sizeof bases[0]; b++) {
        int all = 1;
        for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
            const uint64_t at = bases[b] + parts[i].at;
            r = try_reserve(at ? at : 0x10000, parts[i].bytes - (at ? 0 : 0x10000), 0, &placed);
            const int ok = !r && placed == (at ? at : 0x10000);
            const int radv = at < RADV_HI && at + parts[i].bytes > RADV_LO;
            INFO("window.c", "base %#llx: %s at %#llx: %s (%#x)%s", (unsigned long long)bases[b], parts[i].what, (unsigned long long)at,
                 ok ? "granted" : r ? "refused" : "placed elsewhere", (unsigned)r,
                 radv ? "; in RADV's device-memory range: it would map over it later" : "");
            all &= ok && !radv;
        }
        INFO("window.c", "base %#llx: %s", (unsigned long long)bases[b], all ? "every part usable" : "NOT usable as it is");
    }
    /* the candidates for one window */
    r = try_reserve(0x7c00000000ull, 512 * GIB, 0, &placed);
    INFO("window.cand", "512 GiB at 0x7c00000000 (A64_HOST_BASE 16 GiB lower): %s (%#x, %#llx)", !r && placed == 0x7c00000000ull ? "GRANTED" : "refused",
         (unsigned)r, (unsigned long long)placed);
    r = try_reserve(A64_HOST_BASE, 496 * GIB, 0, &placed);
    INFO("window.cand", "496 GiB at %#llx (HOS_SPACE_END lowered by 16 GiB): %s (%#x, %#llx)", (unsigned long long)A64_HOST_BASE,
         !r && placed == A64_HOST_BASE ? "GRANTED" : "refused", (unsigned)r, (unsigned long long)placed);
}
static void check_window(void) {
    check_begin("window", 0);
    window_map();
    window_explore();
    const struct timespec t0 = probe_now();
    int ok = hos_ps5_guest_reserve((uint64_t)A64_HOST_BASE, WINDOW_BYTES);
    const double ms = probe_ms_since(t0);
    struct hos_ps5_guest_stats s;
    hos_ps5_guest_stats(&s);
    static const char *const mode[] = {"none", "one reservation", "adjacent pieces", "the pieces granted from the base up"};
    if (ok) {
        window_bytes = s.size;
        result(1, "window", "the runtime's window (ps5_window.c): %llu GiB at %#llx as %s (%u x %llu GiB; one "
               "reservation answered %#x); the guest's address space ends at %llu GiB; %.2f ms", (unsigned long long)(s.size >> 30),
               (unsigned long long)A64_HOST_BASE, mode[s.mode], s.pieces, (unsigned long long)(s.piece_bytes >> 30), (unsigned)s.whole_result,
               (unsigned long long)(s.space_end >> 30), ms);
        VERDICT("window", "%s", s.mode == HOS_PS5_WINDOW_WHOLE ? "plan A holds: one 512 GiB reservation, no change"
                : s.mode == HOS_PS5_WINDOW_PIECES ? "plan B' holds: 16 GiB pieces at the same addresses, no layout change"
                : "plan C': the console's map ends inside the window; the runtime runs with HOS_SPACE_END lowered (regions from its end); "
                  "the clean fix is A64_HOST_BASE lowered so that 512 GiB fit (window.cand)");
    } else {
        result(0, "window", "the runtime's window: refused every way (one reservation %#x; %u pieces of %llu GiB granted from the base, "
               "fewer than the 336 GiB the heap and the VAMM region need)", (unsigned)s.whole_result, s.pieces, (unsigned long long)(s.piece_bytes >> 30));
        /* the probe goes on with the largest window it can have, to run the other checks */
        for (uint64_t g = 256; g >= 16 && !ok; g /= 2) {
            setenv("HOOKSHOT_PS5_WINDOW", "whole", 1);
            ok = hos_ps5_guest_reserve((uint64_t)A64_HOST_BASE, g * GIB);
            unsetenv("HOOKSHOT_PS5_WINDOW");
            if (ok) window_bytes = g * GIB;
        }
        INFO("window", "the probe goes on in a window of %llu GiB", (unsigned long long)(window_bytes >> 30));
        VERDICT("window", "nothing at %#llx holds the guest's layout: see window.slots, window.top and window.c for the base to move to",
                (unsigned long long)A64_HOST_BASE);
    }
    INFO("window", "first-touch block %llu KiB (HOOKSHOT_PS5_FAULT_KB)", (unsigned long long)(s.fault_bytes >> 10));
    check_end();
}

/* ================================================================== 2. flexible memory and the 128 MiB .bss table */
static void check_bss(size_t flex_entry) {
    check_begin("bss", 0);
    size_t configured = 0, before = 0, after = 0;
    sceKernelConfiguredFlexibleMemorySize(&configured);
    sceKernelAvailableFlexibleMemorySize(&before);
    char vq[160];
    vq_text(a64_dirty, vq, sizeof vq);
    INFO("bss", "a64_dirty (the soft write watch, 128 MiB of .bss) at %p: %s", (void *)a64_dirty, vq);
    for (size_t i = 0; i < (128u << 20); i += 16384) ((volatile uint8_t *)a64_dirty)[i] = 1;
    sceKernelAvailableFlexibleMemorySize(&after);
    for (size_t i = 0; i < (128u << 20); i += 16384) ((volatile uint8_t *)a64_dirty)[i] = 0;
    vq_text(a64_dirty, vq, sizeof vq);
    const long long drop = ((long long)before - (long long)after) >> 20;
    INFO("bss", "flexible memory: configured %zu MiB; free at main %zu MiB, before touching the table %zu MiB, after touching all of it "
                "%zu MiB (%lld MiB less); the table now: %s", configured >> 20, flex_entry >> 20, before >> 20, after >> 20, drop, vq);
    VERDICT("bss", "%s", drop >= 120 ? "the table is flexible memory, charged as it is touched: move it (16 KiB marks, a smaller window, or a "
                                       "direct-memory table at a fixed address)"
                 : (configured >> 20) - (flex_entry >> 20) >= 160 ? "nothing moved when touched, but free flexible memory at main is "
                                       "about 128 MiB short of configured: the table is charged at load (see the line above)"
                 : "the table costs no flexible memory: keep it in .bss");
    check_end();
}

/* ============================================================================================== 3. futexes */
static volatile uint32_t fx_word;
static DWORD WINAPI fx_waker(void *p) {
    Sleep((DWORD)(uintptr_t)p);
    __atomic_store_n(&fx_word, 1, __ATOMIC_SEQ_CST);
    WakeByAddressSingle((void *)&fx_word);
    return 0;
}
static void futex_round(const char *id) {
    uint32_t cmp = 0;
    fx_word = 0;
    struct timespec t0 = probe_now();
    const BOOL r = WaitOnAddress(&fx_word, &cmp, 4, 50);
    const double timeout_ms = probe_ms_since(t0);
    const DWORD err = GetLastError();
    const int timed_out = !r && err == ERROR_TIMEOUT && timeout_ms >= 40 && timeout_ms < 1000;
    HANDLE h = CreateThread(NULL, 0, fx_waker, (void *)(uintptr_t)20, 0, NULL);
    t0 = probe_now();
    int woken = 0;
    while (!woken && probe_ms_since(t0) < 2000) {   /* (a spurious return looks again, as callers do) */
        if (__atomic_load_n(&fx_word, __ATOMIC_SEQ_CST)) { woken = 1; break; }
        WaitOnAddress(&fx_word, &cmp, 4, 2000);
    }
    const double wake_ms = probe_ms_since(t0);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
    SRWLOCK l = SRWLOCK_INIT;
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
    AcquireSRWLockExclusive(&l);
    t0 = probe_now();
    const BOOL cvr = SleepConditionVariableSRW(&cv, &l, 30, 0);
    const double cv_ms = probe_ms_since(t0);
    ReleaseSRWLockExclusive(&l);
    result(timed_out && woken && wake_ms < 1000 && !cvr, id,
           "WaitOnAddress timed out after %.1f ms (asked 50; %s), woken %.1f ms after a 20 ms waker; a condition variable timed out "
           "after %.1f ms (asked 30); mode %s", timeout_ms, r ? "returned TRUE" : err == ERROR_TIMEOUT ? "ERROR_TIMEOUT" : "other error",
           wake_ms, cv_ms, hos_ps5_futex_mode() == 1 ? "_umtx_op" : "condition variables");
}
static void check_futex(void) {
    check_begin("futex", 20);
    futex_round("futex.umtx");
    const int mode = hos_ps5_futex_mode();
    hos_ps5_futex_force_fallback(1);
    futex_round("futex.fallback");
    hos_ps5_futex_force_fallback(0);
    VERDICT("futex", "%s", mode == 1 ? "_umtx_op works for a title: host/posix's locks and condition variables as designed"
                                     : "_umtx_op is refused: the condition-variable table carries every futex (slower; measure)");
    check_end();
}

/* ============================================================================== 4. backing at once (hos_commit) */
static void check_eager(void) {
    check_begin("eager", 0);
    if (!window_has(0x1060000000ull, 0)) { INFO("eager", "the window is too small: skipped"); check_end(); return; }
    static const struct { uint64_t at, bytes; } sizes[] = {
        {0x1000000000ull, 64 << 10}, {0x1001000000ull, 1 << 20}, {0x1002000000ull, 16 << 20}, {0x1010000000ull, 256 << 20}};
    int all = 1;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        const uint64_t a = sizes[i].at, n = sizes[i].bytes;
        const struct timespec t0 = probe_now();
        hos_commit(a, n);   /* (mkw_glue.c: eager up to HOOKSHOT_PS5_EAGER_MB, 256) */
        const double ms = probe_ms_since(t0);
        size_t run = 0;
        const int state = vq_state(GUEST(a), &run);
        int ok = state == 1 && run >= n;
        for (uint64_t off = 0; ok && off < n; off += 4096) ok = GUEST(a)[off] == 0;   /* (zeroed: direct memory comes back dirty) */
        for (uint64_t off = 0; ok && off < n; off += 4096) *(volatile uint64_t *)(GUEST(a) + off) = a + off;
        for (uint64_t off = 0; ok && off < n; off += 4096) ok = *(volatile uint64_t *)(GUEST(a) + off) == a + off;
        result(ok, "eager", "hos_commit of %llu KiB at guest %#llx: %s, zero, written and read back; %.2f ms (%.0f MiB/s)",
               (unsigned long long)(n >> 10), (unsigned long long)a, state == 1 ? "committed" : "NOT committed", ms,
               ms > 0 ? (double)n / MIB / (ms / 1e3) : 0.0);
        all &= ok;
    }
    /* above the limit: left to first touch */
    hos_commit(0x1040000000ull, 512ull << 20);
    const int lazy = vq_state(GUEST(0x1040000000ull), NULL);
    result(lazy == 0, "eager.limit", "hos_commit of 512 MiB (over HOOKSHOT_PS5_EAGER_MB) left it %s", lazy == 0 ? "to first touch" : "committed");
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) VirtualFree(GUEST(sizes[i].at), (SIZE_T)sizes[i].bytes, MEM_DECOMMIT);
    const int back = vq_state(GUEST(0x1010000000ull), NULL);
    result(back == 0, "eager.decommit", "VirtualFree(MEM_DECOMMIT) gave the units back (VirtualQuery: %s)", back == 0 ? "reserved" : "still committed");
    (void)all;
    check_end();
}

/* ======================================================================= 5. backing on first touch, the SIGSEGV path */
#define MAX_LAZY 32
static struct lazy_job {
    uint64_t at, bytes, stride_units, first_unit;
    unsigned id;
    volatile uint64_t done;   /* bytes touched */
    volatile int finished, bad;
} jobs[MAX_LAZY];
static void *lazy_thread(void *p) {
    struct lazy_job *j = (struct lazy_job *)p;
    /* every 4 KiB page of its units, written then read back: the first write to each unit faults (or finds it backed by a
       neighbour's fault) */
    for (uint64_t u = j->first_unit; u * 65536 < j->bytes; u += j->stride_units) {
        for (uint64_t off = u * 65536; off < (u + 1) * 65536; off += 4096) {
            volatile uint64_t *w = (volatile uint64_t *)(GUEST(j->at) + off);
            *w = ((uint64_t)j->id << 56) | off;
            if (*w != (((uint64_t)j->id << 56) | off)) j->bad++;
        }
        j->done += 65536;
    }
    j->finished = 1;
    return NULL;
}
/* run the jobs on threads (pthread_create: the platform's wrap), watching their progress; 0 when one stopped moving */
static int run_jobs(unsigned n, const char *id, double *ms) {
    pthread_t t[MAX_LAZY];
    const struct timespec t0 = probe_now();
    for (unsigned i = 0; i < n; i++) pthread_create(&t[i], NULL, lazy_thread, &jobs[i]);
    uint64_t last[MAX_LAZY] = {0};
    int64_t moved = now_ns();
    for (;;) {
        unsigned left = 0;
        int any = 0;
        for (unsigned i = 0; i < n; i++) {
            if (!jobs[i].finished) left++;
            if (jobs[i].done != last[i]) last[i] = jobs[i].done, any = 1;
        }
        if (!left) break;
        if (any) moved = now_ns();
        progress("%s: %u of %u threads running; thread 0 at %llu MiB", id, left, n, (unsigned long long)(jobs[0].done >> 20));
        if (now_ns() - moved > 10ll * 1000000000ll) {
            char who[400] = "";
            for (unsigned i = 0; i < n; i++)
                if (!jobs[i].finished)
                    snprintf(who + strlen(who), sizeof who - strlen(who), " t%u@guest %#llx", i,
                             (unsigned long long)(jobs[i].at + jobs[i].done));
            result(0, id, "no progress for 10 s: a deadlock in the fault path? stuck:%s", who);
            out("VERDICT lazy: backing from the SIGSEGV handler deadlocked: the design's risk M5 is real; see the stuck addresses");
            summary("a stuck fault (lazy)");
            hos_ps5_exit(3);
        }
        usleep(20000);
    }
    for (unsigned i = 0; i < n; i++) pthread_join(t[i], NULL);
    *ms = probe_ms_since(t0);
    int bad = 0;
    for (unsigned i = 0; i < n; i++) bad += jobs[i].bad;
    return bad;
}
static pthread_mutex_t held_mu = PTHREAD_MUTEX_INITIALIZER;
static void *fault_under_lock(void *p) {
    (void)p;
    pthread_mutex_lock(&out_mu);   /* (the probe's own log lock, as a runtime thread inside hos_log could hold its own) */
    pthread_mutex_lock(&held_mu);
    volatile uint64_t v = *(volatile uint64_t *)GUEST(0x2800000000ull);   /* (unbacked: faults with both held) */
    pthread_mutex_unlock(&held_mu);
    pthread_mutex_unlock(&out_mu);
    return (void *)(uintptr_t)(v == 0);
}
static void check_lazy(void) {
    check_begin("lazy", 120);
    if (!window_has(0x3820000000ull, 0)) { INFO("lazy", "the window is too small: skipped"); check_end(); return; }
    struct hos_ps5_guest_stats s0, s1;
    char m[200];
    /* a. one thread, 64 MiB */
    hos_ps5_guest_stats(&s0);
    memset(jobs, 0, sizeof jobs);
    jobs[0] = (struct lazy_job){.at = 0x2000000000ull, .bytes = 64 * MIB, .stride_units = 1, .id = 1};
    double ms = 0;
    int bad = run_jobs(1, "lazy.one", &ms);
    hos_ps5_guest_stats(&s1);
    uint64_t faults = s1.faults - s0.faults;
    result(!bad && faults > 0, "lazy.one", "64 MiB touched page by page from one thread: %llu faults backed %llu units in %.0f ms "
           "(%.0f us a fault in the handler, the longest %.0f us), every page right", (unsigned long long)faults,
           (unsigned long long)(s1.fault_units - s0.fault_units), ms, faults ? (double)(s1.fault_ns - s0.fault_ns) / faults / 1e3 : 0.0,
           s1.fault_ns_max / 1e3);
    /* b. threads, each its own GiB */
    const unsigned n = cfg.lazy_threads ? cfg.lazy_threads : 1;
    hos_ps5_guest_stats(&s0);
    for (unsigned i = 0; i < n; i++)
        jobs[i] = (struct lazy_job){.at = 0x3000000000ull + (uint64_t)i * GIB, .bytes = (uint64_t)cfg.lazy_mb * MIB, .stride_units = 1, .id = i + 2};
    bad = run_jobs(n, "lazy.threads", &ms);
    hos_ps5_guest_stats(&s1);
    faults = s1.faults - s0.faults;
    result(!bad, "lazy.threads", "%u threads, %u MiB each in its own GiB, at once: %llu faults, %llu units, %.0f ms (%.0f MiB/s in all)",
           n, cfg.lazy_mb, (unsigned long long)faults, (unsigned long long)(s1.fault_units - s0.fault_units), ms,
           ms > 0 ? (double)n * cfg.lazy_mb / (ms / 1e3) : 0.0);
    /* c. threads interleaved unit by unit in one region: they fault on each other's blocks, under one GiB's lock */
    hos_ps5_guest_stats(&s0);
    for (unsigned i = 0; i < n; i++)
        jobs[i] = (struct lazy_job){.at = 0x3800000000ull, .bytes = 512 * MIB, .stride_units = n, .first_unit = i, .id = 0x40 + i};
    bad = run_jobs(n, "lazy.shared", &ms);
    hos_ps5_guest_stats(&s1);
    result(!bad && s1.fault_failures == s0.fault_failures, "lazy.shared",
           "%u threads interleaved over one 512 MiB region: %llu faults, %llu found the unit backed by another (retried), %llu refused; %.0f ms",
           n, (unsigned long long)(s1.faults - s0.faults), (unsigned long long)(s1.fault_retries - s0.fault_retries),
           (unsigned long long)(s1.fault_refused - s0.fault_refused), ms);
    /* d. a fault while the faulting thread holds locks others want (the log's): the handler must need none of them */
    pthread_t t;
    void *ret = NULL;
    progress("lazy.locks: a thread faulting with the log lock held");
    pthread_create(&t, NULL, fault_under_lock, NULL);
    pthread_join(t, &ret);
    result(ret != NULL, "lazy.locks", "a fault taken while holding the log lock and another mutex was backed (no lock in the handler)");
    /* e. decommitted, then touched again: zero */
    VirtualFree(GUEST(0x2000000000ull), (SIZE_T)(64 * MIB), MEM_DECOMMIT);
    const int st = vq_state(GUEST(0x2000000000ull), NULL);
    int zero = 1;
    for (uint64_t off = 0; off < 64 * MIB; off += 4096) zero &= *(volatile uint64_t *)(GUEST(0x2000000000ull) + off) == 0;
    result(st == 0 && zero, "lazy.decommit", "64 MiB decommitted (%s), then read again: %s", st == 0 ? "reserved" : "still committed",
           zero ? "every page zero (backed again on touch)" : "OLD DATA came back");
    /* f. the kernel's own writes into unbacked memory: read() gets EFAULT, ReadFile backs its buffer first (win32_posix.c) */
    const char *path = "/app0/plat-probe-efault.bin";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    static uint8_t page[65536];
    memset(page, 0xa5, sizeof page);
    if (fd >= 0 && write(fd, page, sizeof page) == (ssize_t)sizeof page) {
        const ssize_t r = pread(fd, GUEST(0x2900000000ull), sizeof page, 0);
        const int e = errno;
        INFO("lazy.efault", "read() into unbacked guest memory: %zd%s%s (the reason ReadFile and WriteFile back guest buffers first)", r,
             r < 0 ? ", errno " : "", r < 0 ? (e == EFAULT ? "EFAULT" : strerror(e)) : "");
        close(fd);
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD got = 0;
        OVERLAPPED ov = {0};
        const BOOL ok = h != INVALID_HANDLE_VALUE && ReadFile(h, GUEST(0x2a00000000ull), sizeof page, &got, &ov);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        result(ok && got == sizeof page && !memcmp(GUEST(0x2a00000000ull), page, sizeof page), "lazy.readfile",
               "ReadFile into unbacked guest memory: %s, %lu bytes", ok ? "TRUE" : "FALSE", (unsigned long)got);
    } else {
        INFO("lazy.efault", "could not write %s (errno %d): skipped", path, errno);
        if (fd >= 0) close(fd);
    }
    unlink(path);
    /* given back for what follows */
    VirtualFree(GUEST(0x2000000000ull), (SIZE_T)(64 * MIB), MEM_DECOMMIT);
    for (unsigned i = 0; i < n; i++) VirtualFree(GUEST(0x3000000000ull + (uint64_t)i * GIB), (SIZE_T)cfg.lazy_mb * MIB, MEM_DECOMMIT);
    VirtualFree(GUEST(0x3800000000ull), (SIZE_T)(512 * MIB), MEM_DECOMMIT);
    VirtualFree(GUEST(0x2800000000ull), 65536, MEM_DECOMMIT);
    VirtualFree(GUEST(0x2900000000ull), 65536, MEM_DECOMMIT);
    VirtualFree(GUEST(0x2a00000000ull), 65536, MEM_DECOMMIT);
    hos_ps5_guest_stats(&s1);
    mem_text(m, sizeof m);
    INFO("lazy", "after: %llu MiB of the window backed (peak %llu MiB), tables %llu KiB, %llu decommit fallbacks; %s",
         (unsigned long long)(s1.backed_bytes >> 20), (unsigned long long)(s1.peak_backed_bytes >> 20), (unsigned long long)(s1.meta_bytes >> 10),
         (unsigned long long)s1.rereserve_fallbacks, m);
    VERDICT("lazy", "backing from the SIGSEGV handler works from %u threads at once with no deadlock (risk M5 answered by ps5_window.c)", n);
    check_end();
}

/* ====================================================================================== 6. stacks and MXCSR */
struct stack_report {
    unsigned mxcsr;
    void *addr;
    size_t size;
    char vq[160];
    uint64_t deep_sum;
    size_t used;
};
static __attribute__((noinline)) uint64_t deep(int n, volatile uint8_t *low) {
    volatile uint8_t frame[16384];
    frame[0] = (uint8_t)n, frame[16383] = (uint8_t)(n * 3);
    if ((uintptr_t)frame < (uintptr_t)*(uint8_t *volatile *)&low) {}
    if (!n) return frame[0] + (uint64_t)(uintptr_t)frame;   /* (the lowest frame's address, returned up) */
    const uint64_t below = deep(n - 1, frame);
    return below + 0 * frame[16383];
}
static void stack_of_this_thread(struct stack_report *r) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (!pthread_attr_get_np(pthread_self(), &a)) pthread_attr_getstack(&a, &r->addr, &r->size);
    pthread_attr_destroy(&a);
    int local;
    vq_text(&local, r->vq, sizeof r->vq);
}
static DWORD WINAPI guest_like(void *p) {   /* a 64 MiB CreateThread thread: the guest threads' path */
    struct stack_report *r = (struct stack_report *)p;
    r->mxcsr = mxcsr();
    stack_of_this_thread(r);
    char top;
    /* 448 frames of 16 KiB: 7 MiB of the 8 backed */
    const uint64_t lowest = deep(447, (volatile uint8_t *)&top);
    r->used = (size_t)((uintptr_t)&top - (uintptr_t)lowest);
    r->deep_sum = 1;
    return 0;
}
static void *plain_thread(void *p) {   /* pthread_create, no attributes: the platform's wrap */
    struct stack_report *r = (struct stack_report *)p;
    r->mxcsr = mxcsr();
    stack_of_this_thread(r);
    return NULL;
}
static volatile LONG crowd_ready;
static HANDLE crowd_go;
static DWORD WINAPI crowd(void *p) {
    (void)p;
    volatile uint8_t big[4u << 20];   /* (4 MiB of each 64 MiB stack touched) */
    for (size_t i = 0; i < sizeof big; i += 4096) big[i] = (uint8_t)i;
    InterlockedIncrement(&crowd_ready);
    WaitForSingleObject(crowd_go, 30000);
    return big[4096];
}
/* fibers: two of them and the thread's own, switched 1,000 times round, each checking that what it keeps in registers and on
   its stack is what it left (the switch is ps5_fiber.c's own: the ucontext one corrupted the heap, run 1) */
static unsigned fiber_mxcsr;
static void *fibers[3];
static volatile unsigned fiber_rounds, fiber_bad;
static void WINAPI fiber_fn(void *p) {
    const unsigned me = (unsigned)(uintptr_t)p;   /* 1 or 2 */
    fiber_mxcsr = mxcsr();
    volatile uint64_t mark[64];
    for (unsigned i = 0; i < 64; i++) mark[i] = (uint64_t)me << 32 | i;
    register uint64_t keep = 0x5a5a000000000000ull | me;
    for (;;) {
        SwitchToFiber(fibers[me == 1 ? 2 : 0]);   /* (1 -> 2 -> the thread -> 1 ...) */
        __asm__ volatile("" : "+r"(keep));
        if (keep != (0x5a5a000000000000ull | me)) fiber_bad++;
        for (unsigned i = 0; i < 64; i++) if (mark[i] != ((uint64_t)me << 32 | i)) fiber_bad++;
    }
}
static DWORD WINAPI fiber_host(void *p) {
    (void)p;
    fibers[0] = ConvertThreadToFiber(NULL);
    fibers[1] = CreateFiberEx(64 << 10, 64 << 20, 0, fiber_fn, (void *)1);
    fibers[2] = CreateFiberEx(64 << 10, 64 << 20, 0, fiber_fn, (void *)2);
    if (!fibers[0] || !fibers[1] || !fibers[2]) return 1;
    volatile uint64_t mine[64];
    for (unsigned i = 0; i < 64; i++) mine[i] = 0xf00d0000u + i;
    for (unsigned r = 0; r < 1000; r++) {
        SwitchToFiber(fibers[1]);
        fiber_rounds++;
        for (unsigned i = 0; i < 64; i++) if (mine[i] != 0xf00d0000u + i) fiber_bad++;
    }
    DeleteFiber(fibers[1]);
    DeleteFiber(fibers[2]);
    void *again = malloc(4096);   /* (the heap after: run 1 crashed in free here) */
    free(again);
    ConvertFiberToThread();
    return 2;
}
static void check_stacks(void) {
    check_begin("stacks", 90);
    const unsigned main_m = mxcsr();
    /* (the control bits are the state: 0x1f80, every exception masked, round to nearest, FTZ and DAZ clear. Bits 0-5 are sticky
       flags the thread's own arithmetic sets (0x22: precision and denormal, run 1's), which change no result and which the
       lifted code does not read) */
    result((main_m & ~0x3fu) == 0x1f80, "mxcsr.main", "the main thread after hos_ps5_start: %#x: control %#x (IEEE is 0x1f80; a title starts at "
           "0x9fe0), sticky flags %#x", main_m, main_m & ~0x3fu, main_m & 0x3fu);
    struct stack_report g = {0}, plain = {0};
    HANDLE h = CreateThread(NULL, 64u << 20, guest_like, &g, 0, NULL);
    const DWORD w = h ? WaitForSingleObject(h, 30000) : WAIT_FAILED;
    if (h) CloseHandle(h);
    result(h && w == WAIT_OBJECT_0 && g.mxcsr == 0x1f80 && g.deep_sum, "stacks.guest",   /* (CreateThread's: 0x1f80, flags clear) */
           "CreateThread(64 MiB): MXCSR %#x; stack %p + %zu MiB (%s); 7 MiB of recursion ran (%zu KiB used)", g.mxcsr, g.addr,
           g.size >> 20, g.vq, g.used >> 10);
    pthread_t t;
    pthread_create(&t, NULL, plain_thread, &plain);
    pthread_join(t, NULL);
    result((plain.mxcsr & ~0x3fu) == 0x1f80, "stacks.pthread", "pthread_create with no attributes (the platform's wrap): MXCSR %#x (its "
           "creator's, sticky flags included), control %#x, stack %zu KiB (%s)", plain.mxcsr, plain.mxcsr & ~0x3fu, plain.size >> 10, plain.vq);
    /* many guest-like threads at once: 64 MiB reserved each, 8 MiB backed: none of it flexible memory */
    char before[200], after[200];
    size_t flex0 = 0, flex1 = 0;
    sceKernelAvailableFlexibleMemorySize(&flex0);
    mem_text(before, sizeof before);
    crowd_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    crowd_ready = 0;
    HANDLE hs[64];
    unsigned made = 0;
    for (unsigned i = 0; i < cfg.stack_threads; i++)
        if ((hs[made] = CreateThread(NULL, 64u << 20, crowd, NULL, 0, NULL)) != NULL) made++;
    const struct timespec t0 = probe_now();
    while (crowd_ready < (LONG)made && probe_ms_since(t0) < 20000) Sleep(10);
    sceKernelAvailableFlexibleMemorySize(&flex1);
    mem_text(after, sizeof after);
    SetEvent(crowd_go);
    for (unsigned i = 0; i < made; i++) WaitForSingleObject(hs[i], 10000), CloseHandle(hs[i]);
    CloseHandle(crowd_go);
    const long long flex_drop = ((long long)flex0 - (long long)flex1) >> 20;
    result(made == cfg.stack_threads && crowd_ready == (LONG)made && flex_drop < 16, "stacks.crowd",
           "%u of %u threads of 64 MiB at once, 4 MiB of each touched: flexible memory %lld MiB less; before: %s; with them: %s", made,
           cfg.stack_threads, flex_drop, before, after);
    INFO("stacks.crowd", "the stacks of ended threads are not given back yet (census T3): %u x 64 MiB reserved, 8 MiB each backed, stay", made);
    /* a fiber: its own 64 MiB stack, a switch there and back */
    fiber_mxcsr = 0, fiber_rounds = 0, fiber_bad = 0;
    progress("stacks.fiber: switching");
    h = CreateThread(NULL, 0, fiber_host, NULL, 0, NULL);
    DWORD code = 0;
    if (h) WaitForSingleObject(h, 10000), GetExitCodeThread(h, &code), CloseHandle(h);
    result(code == 2 && fiber_rounds == 1000 && !fiber_bad && (fiber_mxcsr & ~0x3fu) == 0x1f80, "stacks.fiber",
           "two CreateFiberEx(64 KiB, 64 MiB) fibers and the thread switched 3,000 times (ps5_fiber.c's switch): %s, %u rounds, %u values "
           "found changed, MXCSR in a fiber %#x; both deleted and the heap used after", code == 2 ? "done" : "FAILED", fiber_rounds,
           fiber_bad, fiber_mxcsr);
    check_end();
}

/* ======================================================================== 7. RADV's import of unit-backed memory */
static int vk_ready;
static int recommit_result;
static void recommit(void *at) { recommit_result = hos_ps5_guest_commit(at, 1 * MIB); }
static void check_vk(void) {
    check_begin("vk", 120);
    char why[400] = "", info[400] = "";
    progress("vk: device");
    if (vk_probe_init(why, sizeof why, info, sizeof info)) {
        result(0, "vk.device", "%s%s%s", why, info[0] ? "; " : "", info);
        check_end();
        return;
    }
    vk_ready = 1;
    result(1, "vk.device", "%s", info);
    const uint64_t at = 0x4000000000ull, bytes = 64 * MIB, units = bytes >> 16;
    if (!window_has(at, 3 * bytes)) { INFO("vk", "the window is too small for the import checks: skipped"); check_end(); return; }
    /* a. backed unit by unit in a shuffled order (1,024 separate direct-memory allocations, not in address order) */
    static uint32_t order[4096];
    for (uint32_t i = 0; i < units; i++) order[i] = i;
    uint32_t x = 2463534242u;
    for (uint32_t i = (uint32_t)units - 1; i > 0; i--) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        const uint32_t j = x % (i + 1), k = order[i];
        order[i] = order[j], order[j] = k;
    }
    struct timespec t0 = probe_now();
    int committed = 1;
    for (uint32_t i = 0; i < units && committed; i++) committed = !hos_ps5_guest_commit(GUEST(at + ((uint64_t)order[i] << 16)), 65536);
    const double commit_ms = probe_ms_since(t0);
    double ms = 0;
    progress("vk.import: 64 MiB of 1,024 units");
    int64_t bad = committed ? vk_probe_import(GUEST(at), bytes, 0x1234567u, NULL, NULL, &ms, why, sizeof why) : -1;
    result(bad == 0, "vk.import", "64 MiB backed as 1,024 units of 64 KiB in shuffled order (%.0f ms), imported with "
           "VK_EXT_external_memory_host, written by a compute shader, every word right on the CPU: %s (%.1f ms)",
           commit_ms, bad == 0 ? "yes" : committed ? why : "the commit failed", ms);
    /* b. an import that meets a commit over its range while it lives: units already backed must keep their GPU access
       (ps5_window.c never re-protects them; the platform's ps5_vrange_commit would have taken it away) */
    progress("vk.recommit");
    recommit_result = 1;
    const int64_t bad2 = vk_probe_import(GUEST(at), bytes, 0x0badf00du, recommit, GUEST(at + bytes - 128 * 1024), &ms, why, sizeof why);
    result(recommit_result == 0 && bad2 == 0, "vk.recommit", "a commit across the end of the range while it was imported (%d: its last "
           "two units and 14 new ones), then the GPU's writes over the whole range: %s", recommit_result, bad2 == 0 ? "every word right" : why);
    /* c. backed on first touch: the CPU's first writes (the probe's sentinel fill) fault the units in, then the import */
    progress("vk.lazy");
    bad = vk_probe_import(GUEST(at + 128 * MIB), bytes, 0x5a5a5a5au, NULL, NULL, &ms, why, sizeof why);
    result(bad == 0, "vk.lazy", "64 MiB backed by first-touch faults, imported and written by the GPU: %s (%.1f ms)",
           bad == 0 ? "every word right" : why, ms);
    VERDICT("vk", "%s", bad == 0 && bad2 == 0 ? "RADV imports guest memory made of 64 KiB units in place: vk_memory.cpp's path holds (risk M15 answered)"
                                              : "the import of unit-backed memory fails: pools must be backed as one allocation (see the FAIL lines)");
    hos_ps5_guest_decommit(GUEST(at), (size_t)(256 * MIB));
    check_end();
}

/* ============================================================================== 8. the memory budget, in steps */
static void check_memory(void) {
    check_begin("memory", 300);
    const uint64_t step = (uint64_t)cfg.mem_step_mb * MIB, target = (uint64_t)cfg.mem_target_mb * MIB;
    const uint64_t at = window_has(0x5000000000ull, target + step) ? 0x5000000000ull : 0x1080000000ull;   /* (+320 GiB, else +66 GiB) */
    if (!window_has(at, target + step)) { INFO("memory", "the window is too small for %u MiB: skipped", cfg.mem_target_mb); check_end(); return; }
    char m[200];
    mem_text(m, sizeof m);
    INFO("memory", "before: %s; the Vulkan device %s", m, vk_ready ? "is alive (RADV's own memory counted)" : "is not made");
    uint64_t got = 0;
    int32_t error = 0;
    const char *stop = "the target";
    const struct timespec all0 = probe_now();
    while (got < target) {
        struct ps5_memory_stats ms_;
        if (ps5_memory_query(&ms_) && ms_.free_bytes < step + (uint64_t)cfg.mem_reserve_mb * MIB) {
            stop = "the reserve kept for the rest of the title (mem_reserve_mb)";
            break;
        }
        const uint64_t n = target - got < step ? target - got : step;
        const struct timespec t0 = probe_now();
        error = hos_ps5_guest_commit(GUEST(at + got), (size_t)n);
        if (error) { stop = "a refused allocation"; break; }
        for (uint64_t off = 0; off < n; off += 16384) *(volatile uint32_t *)(GUEST(at + got + off)) = (uint32_t)(got + off);
        const double ms = probe_ms_since(t0);
        got += n;
        mem_text(m, sizeof m);
        out("STEP memory: %llu MiB backed and touched (+%llu MiB in %.0f ms); %s", (unsigned long long)(got >> 20),
            (unsigned long long)(n >> 20), ms, m);
        progress("memory: %llu MiB", (unsigned long long)(got >> 20));
    }
    const double all_ms = probe_ms_since(all0);
    int right = 1;
    for (uint64_t off = 0; off + 16384 <= got; off += 16384) right &= *(volatile uint32_t *)(GUEST(at + off)) == (uint32_t)off;
    struct hos_ps5_guest_stats s;
    hos_ps5_guest_stats(&s);
    result(right && (got >= target || error), "memory", "the title held %llu MiB of guest memory (target %u MiB, stopped by %s%s%s) in %.1f s; "
           "every page right; the window's backing at its peak %llu MiB", (unsigned long long)(got >> 20), cfg.mem_target_mb, stop,
           error ? ", error " : "", error ? "set" : "", all_ms / 1e3, (unsigned long long)(s.peak_backed_bytes >> 20));
    if (error) INFO("memory", "the refusal: %#x after %llu MiB", (unsigned)error, (unsigned long long)(got >> 20));
    const struct timespec t0 = probe_now();
    hos_ps5_guest_decommit(GUEST(at), (size_t)((got + step) & ~(step - 1)));   /* (and a partial last step's units) */
    const double back_ms = probe_ms_since(t0);
    mem_text(m, sizeof m);
    INFO("memory", "all given back in %.0f ms; after: %s", back_ms, m);
    VERDICT("memory", "the guest can hold %llu MiB beside RADV%s; HOS_MEMORY_MB is 9809 on a PC: %s", (unsigned long long)(got >> 20),
            vk_ready ? "'s device" : "", got >= 9809ull * MIB ? "it fits as it is (before the renderer's own textures and caches)"
                                                              : "lower HOS_MEMORY_MB (and the Minimum preset) below what this line gives, minus the GPU's needs");
    check_end();
}

/* ======================================================================================= 9. open files (fdv) */
static void check_files(void) {
    check_begin("files", 120);
    const char *dir = "/app0/plat-probe-files";
    mkdir(dir, 0777);
    const unsigned n = cfg.fdv_files;
    char path[256];
    static uint8_t buf[4096];
    unsigned made = 0;
    for (unsigned i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/f%04u", dir, i);
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) break;
        memset(buf, (int)(i * 7 + 1), sizeof buf);
        memcpy(buf, &i, sizeof i);
        made += write(fd, buf, sizeof buf) == (ssize_t)sizeof buf;
        close(fd);
    }
    /* the limit, plainly (libc's open, the same file again and again) */
    static int fds[600];
    unsigned raw = 0;
    int raw_errno = 0;
    snprintf(path, sizeof path, "%s/f0000", dir);
    while (raw < 600) {
        const int fd = open(path, O_RDONLY);
        if (fd < 0) { raw_errno = errno; break; }
        fds[raw++] = fd;
    }
    for (unsigned i = 0; i < raw; i++) close(fds[i]);
    INFO("files.limit", "libc's open() of one file again and again: %u opened, then %s", raw,
         raw_errno == EMFILE ? "EMFILE" : raw == 600 ? "stopped at 600" : strerror(raw_errno));
    /* through the runtime's CreateFileA (win32_posix.c -> ps5_files.c -> fdv): all of them open at once */
    static HANDLE h[2000];
    unsigned opened = 0, right = 0;
    const struct timespec t0 = probe_now();
    for (unsigned i = 0; i < made; i++) {
        snprintf(path, sizeof path, "%s/f%04u", dir, i);
        h[i] = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        opened += h[i] != INVALID_HANDLE_VALUE;
        progress("files: %u opened", opened);
    }
    for (int pass = 0; pass < 2; pass++)   /* (twice, the second backwards: the files parked first are reopened) */
        for (unsigned k = 0; k < made; k++) {
            const unsigned i = pass ? made - 1 - k : k;
            if (h[i] == INVALID_HANDLE_VALUE) continue;
            DWORD got = 0;
            OVERLAPPED ov = {0};
            unsigned seen = ~0u;
            if (ReadFile(h[i], buf, sizeof buf, &got, &ov) && got == sizeof buf) memcpy(&seen, buf, sizeof seen);
            right += seen == i && buf[100] == (uint8_t)(i * 7 + 1);
        }
    const double ms = probe_ms_since(t0);
    struct hos_ps5_files_stats fs;
    hos_ps5_files_stats(&fs);
    for (unsigned i = 0; i < made; i++) if (h[i] != INVALID_HANDLE_VALUE) CloseHandle(h[i]);
    const int fdv_off_by_design = fs.disabled && !strncmp(fs.diag, "off:", 4);
    if (fdv_off_by_design) {
        /* (fdv off: the console refuses a title dup2 over a file, run 3. What counts: the files that open read right, and the
           limit is said once in klog when it is met) */
        result(opened && right == 2 * opened, "files.reads", "%u files created; %u open at once through CreateFileA (the console's "
               "limit, fdv off); read twice, %u of %u reads right; %.0f ms", made, opened, right, 2 * opened, ms);
        INFO("files.fdv", "%s", fs.diag);
    } else {
        result(made == n && opened == made && right == 2 * made && made > 249, "files.fdv",
               "%u files created; %u open at once through CreateFileA; read twice, %u of %u reads right; %.0f ms; fdv: %s, high water %u, "
               "%lu parks, %lu reopens, %lu EMFILE answered, %lu failures", made, opened, right, 2 * made, ms, fs.disabled ? "OFF" : "on",
               fs.high_water, fs.parks, fs.unparks, fs.limit_hits, fs.failures);
        INFO("files.fdv", "what fdv's start found: %s", fs.diag);
    }
    for (unsigned i = 0; i < made; i++) snprintf(path, sizeof path, "%s/f%04u", dir, i), unlink(path);
    rmdir(dir);
    VERDICT("files", "%s", opened == made && made > 249 ? "fdv lifts the 249-file limit for the runtime's files (census F9)"
                           : fdv_off_by_design ? "the console's 249 files by path stand (fdv cannot work without dup2); the RomFS is one descriptor"
                                               : "the runtime still stops at the console's limit: see the FAIL line");
    check_end();
}

/* ====================================================================================== 10. /data and the broker */
static void fill(uint8_t *b, size_t n, uint64_t at) {
    for (size_t i = 0; i < n; i += 8) *(uint64_t *)(b + i) = (at + i) * 0x9e3779b97f4a7c15ull;
}
static int same(const uint8_t *b, size_t n, uint64_t at) {
    for (size_t i = 0; i < n; i += 8) if (*(const uint64_t *)(b + i) != (at + i) * 0x9e3779b97f4a7c15ull) return 0;
    return 1;
}
static void read_speed(const char *id, const char *path, uint64_t limit, int verify) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) { INFO(id, "%s: errno %d", path, errno); return; }
    static uint8_t *b;
    if (!b) b = malloc(16 * MIB);
    uint64_t total = 0;
    int ok = 1;
    const struct timespec t0 = probe_now();
    while (total < limit) {
        const ssize_t r = read(fd, b, 16 * MIB);
        if (r <= 0) break;
        if (verify) ok &= same(b, (size_t)r, total);
        total += (uint64_t)r;
        progress("%s: %llu MiB", id, (unsigned long long)(total >> 20));
    }
    const double ms = probe_ms_since(t0);
    close(fd);
    INFO(id, "%s read directly: %llu MiB in %.0f ms, %.0f MiB/s%s", path, (unsigned long long)(total >> 20), ms,
         ms > 0 ? (double)total / MIB / (ms / 1e3) : 0.0, verify ? (ok ? ", every byte right" : ", WRONG BYTES") : "");
}
/* the runtime's own writes under the data folder (HB_ROOT) through the broker, as the game makes them: win32_posix.c's WriteFile
   (ps5_files.c) and stdio's fwrite (ps5_stdio.c, the renderer's caches), each 256 MiB, then read back by the title */
static void check_routing(void) {
    struct hos_ps5_files_stats f0, f1;
    uint8_t *b = malloc(16 * MIB);
    if (!b) return;
    /* WriteFile */
    const char *wpath = HB_ROOT "probe/writefile.bin";
    hos_ps5_files_stats(&f0);
    HANDLE h = CreateFileA(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    uint64_t done = 0;
    struct timespec t0 = probe_now();
    while (h != INVALID_HANDLE_VALUE && done < 256 * MIB) {
        DWORD put = 0;
        fill(b, 16 * MIB, done);
        if (!WriteFile(h, b, 16 * MIB, &put, NULL) || put != 16 * MIB) break;
        done += 16 * MIB;
    }
    double ms = probe_ms_since(t0);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    hos_ps5_files_stats(&f1);
    result(done == 256 * MIB && f1.broker_bytes - f0.broker_bytes == 256 * MIB, "broker.writefile",
           "256 MiB by the runtime's WriteFile (CreateFileA under the data folder): %llu MiB in %.0f ms, %llu MiB of it by the broker "
           "(%lu writes, %lu fell back to the title)", (unsigned long long)(done >> 20), ms,
           (unsigned long long)((f1.broker_bytes - f0.broker_bytes) >> 20), f1.broker_writes - f0.broker_writes,
           f1.broker_fallbacks - f0.broker_fallbacks);
    struct stat wst;
    if (f1.broker_fallbacks != f0.broker_fallbacks || stat(wpath, &wst))
        INFO("broker.writefile", "the broker's last refusal: %d (%s); the file's mode %o", f1.broker_error,
             f1.broker_error < 0 ? strerror(-f1.broker_error) : "none", stat(wpath, &wst) ? 0 : (unsigned)(wst.st_mode & 07777));
    else
        INFO("broker.writefile", "the file's mode %o (0666: the broker's to write)", (unsigned)(wst.st_mode & 07777));
    read_speed("broker.writefile", wpath, 256 * MIB, 1);
    unlink(wpath);
    /* stdio, as the renderer writes its caches: "wb" in big pieces, then "ab" with small and big pieces in turn */
    const char *spath = HB_ROOT "probe/stdio.bin";
    hos_ps5_files_stats(&f0);
    FILE *f = fopen(spath, "wb");
    done = 0;
    t0 = probe_now();
    while (f && done < 256 * MIB) {
        fill(b, 16 * MIB, done);
        if (fwrite(b, 1, 16 * MIB, f) != 16 * MIB) break;
        done += 16 * MIB;
    }
    ms = probe_ms_since(t0);
    if (f) fclose(f);
    hos_ps5_files_stats(&f1);
    result(done == 256 * MIB && f1.broker_bytes - f0.broker_bytes == 256 * MIB, "broker.stdio",
           "256 MiB by fopen(\"wb\")/fwrite under the data folder: %llu MiB in %.0f ms, %llu MiB of it by the broker (%lu writes)",
           (unsigned long long)(done >> 20), ms, (unsigned long long)((f1.broker_bytes - f0.broker_bytes) >> 20),
           f1.broker_writes - f0.broker_writes);
    read_speed("broker.stdio", spath, 256 * MIB, 1);
    /* appends in order: 100 bytes (stdio's), 64 KiB (the broker's), 100 bytes, 64 KiB, as vk_shader.cpp appends records */
    f = fopen(spath, "wb");
    if (f) fclose(f);
    f = fopen(spath, "ab");
    int order = f != NULL;
    for (int i = 0; f && i < 4; i++) {
        const size_t n = i % 2 ? 65536 : 100;
        memset(b, 'a' + i, n);
        order &= fwrite(b, 1, n, f) == n;
        fseeko(f, 0, SEEK_END);
        order &= ftello(f) == (off_t)(i == 0 ? 100 : i == 1 ? 65636 : i == 2 ? 65736 : 131272);
    }
    if (f) fclose(f);
    const int fd = open(spath, O_RDONLY);
    const ssize_t got = fd >= 0 ? read(fd, b, 1 << 20) : -1;
    if (fd >= 0) close(fd);
    order &= got == 131272;
    for (ssize_t i = 0; order && i < got; i++) {
        const int part = i < 100 ? 0 : i < 65636 ? 1 : i < 65736 ? 2 : 3;
        order &= b[i] == 'a' + part;
    }
    result(order, "broker.append", "appends to an \"ab\" stream, small (stdio's) and 64 KiB (the broker's) in turn: %s (%zd bytes)",
           order ? "in order, every byte right, ftello right after each" : "WRONG", got);
    unlink(spath);
    free(b);
}

static void check_data(void) {
    check_begin("data", 300);
    const char *status = hos_ps5_data_status(), *root = hos_ps5_data_root();
    const int visible = hb_serves_root(root);   /* (lot 80: /data/grapnel, or /data/hookshot used in place) */
    INFO("data", "/data for the title: %s; the game folder would be %s", status, root);
    if (visible) {
        /* the title's own writes there: past the 1.3 GiB burst, to see whether the console still holds them back */
        char probe_dir[96], p[128];
        snprintf(probe_dir, sizeof probe_dir, "%s/probe", root);
        snprintf(p, sizeof p, "%s/direct.bin", probe_dir);
        mkdir(probe_dir, 0777);
        const int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0666);
        uint8_t *b = malloc(16 * MIB);
        const uint64_t total = (uint64_t)cfg.data_write_mb * MIB;
        uint64_t done = 0;
        int ok = fd >= 0 && b;
        double last_rate = 0;
        struct timespec mark = probe_now();
        const struct timespec t0 = mark;
        int held = 0;
        while (ok && done < total) {
            fill(b, 16 * MIB, done);
            const struct timespec c0 = probe_now();
            ok = write(fd, b, 16 * MIB) == (ssize_t)(16 * MIB);
            if (!ok) break;
            done += 16 * MIB;
            if (probe_ms_since(c0) > 1000 && done >= 512 * MIB) {   /* (under 16 MiB/s: held back; the rest would take minutes) */
                held = 1;
                out("STEP data.direct: %llu MiB, then a 16 MiB write took %.0f ms: held back, stopped", (unsigned long long)(done >> 20),
                    probe_ms_since(c0));
                break;
            }
            progress("data.direct: %llu MiB written", (unsigned long long)(done >> 20));
            if (!(done % (256 * MIB))) {
                last_rate = 256.0 / (probe_ms_since(mark) / 1e3);
                out("STEP data.direct: %llu MiB, the last 256 MiB at %.0f MiB/s", (unsigned long long)(done >> 20), last_rate);
                mark = probe_now();
            }
        }
        const double ms = probe_ms_since(t0);
        const int e = errno;
        if (fd >= 0) fchmod(fd, 0666), close(fd);
        free(b);
        if (held) last_rate = 16.0 / 1.0;
        result(ok && (done == total || held), "data.direct", "%s written by the title itself (no broker, no elevation): %llu MiB in %.1f s, "
               "%.0f MiB/s, the last 256 MiB at %.0f MiB/s%s", root, (unsigned long long)(done >> 20), ms / 1e3,
               ms > 0 ? (double)done / MIB / (ms / 1e3) : 0.0, last_rate, ok ? "" : " (stopped: errno set)");
        if (!ok) INFO("data.direct", "the write that failed: errno %d", e);
        VERDICT("data.direct", "%s", held ? "the title's writes are held back past the burst: big writes (the renderer caches) go through the broker"
                : done > 1400 * MIB && last_rate > 100 ? "no throttle on /data for this title: the broker is not needed for writes"
                : done > 1400 * MIB ? "the title's writes are held back past the burst: big writes (the renderer caches) go through the broker"
                                    : "too little written to tell (data_write_mb)");
        read_speed("data.direct", p, (uint64_t)cfg.data_read_mb * MIB, 1);
        unlink(p);
    } else {
        INFO("data.direct", "the title does not see /data: the root is /app0 (ShadowMountPlus 1.7beta3+ mounts /data; or /app0/hookshot-elevate for Lapy)");
    }
    if (cfg.read_file[0]) read_speed("data.read_file", cfg.read_file, (uint64_t)cfg.data_read_mb * MIB, 0);
    if (!cfg.broker) { INFO("broker", "off (broker=0)"); check_end(); return; }
    /* the storage broker: started from the title as Savage Wind's is ("[broker] started from the title") */
    progress("broker: starting");
    int detail = 0;
    const struct timespec t0 = probe_now();
    const enum hb_start s = hb_start("/app0/hookshot-broker.elf", &detail);
    const double start_ms = probe_ms_since(t0);
    out("[broker] %s from the title (%s, %.0f ms, detail %d)", s == HB_STARTED ? "started" : "not started", hb_start_name(s), start_ms, detail);
    result(s == HB_PRESENT || s == HB_STARTED, "broker.start", "%s in %.0f ms (detail %d)%s", hb_start_name(s), start_ms, detail,
           s == HB_LOADER_UNAVAILABLE ? ": no ELF loader on 127.0.0.1:9021 (start elfldr, as for ps5upload, then run the probe again)" : "");
    if (s != HB_PRESENT && s != HB_STARTED) { check_end(); return; }
    const char *big = HB_ROOT "probe/broker.bin";
    int r = hb_mkdir(HB_ROOT "probe");
    r = r ? r : hb_create(big, 1);
    if (r) { result(0, "broker.write", "creating %s: %d", big, r); check_end(); return; }
    uint8_t *b = malloc(16 * MIB);
    const uint64_t total = (uint64_t)cfg.broker_write_mb * MIB;
    uint64_t done = 0;
    int64_t w = 0;
    struct timespec mark = probe_now(), w0 = mark;
    while (done < total && b) {
        fill(b, 16 * MIB, done);
        w = hb_write(big, done, b, 16 * MIB);
        if (w != (int64_t)(16 * MIB)) break;
        done += 16 * MIB;
        progress("broker: %llu MiB written", (unsigned long long)(done >> 20));
        if (!(done % (256 * MIB))) {
            const double ms = probe_ms_since(mark);
            out("STEP broker.write: %llu MiB, the last 256 MiB at %.0f MiB/s", (unsigned long long)(done >> 20), 256.0 / (ms / 1e3));
            mark = probe_now();
        }
    }
    const double wms = probe_ms_since(w0);
    result(done == total, "broker.write", "%llu MiB written to %s through the broker in %.1f s: %.0f MiB/s%s", (unsigned long long)(done >> 20), big,
           wms / 1e3, wms > 0 ? (double)done / MIB / (wms / 1e3) : 0.0, done == total ? "" : " (stopped: see the error)");
    if (done != total) INFO("broker.write", "the write that failed: %lld", (long long)w);
    uint64_t bsize = 0, blocks = 0, bavail = 0;
    if (!hb_statfs(HB_ROOT "probe", &bsize, &blocks, &bavail))
        INFO("broker.statfs", "/data: %llu GiB free of %llu GiB", (unsigned long long)(bavail * bsize >> 30), (unsigned long long)(blocks * bsize >> 30));
    /* read back through the broker (what a title that does not see /data would do) */
    const uint64_t rtotal = (uint64_t)cfg.broker_read_mb * MIB < done ? (uint64_t)cfg.broker_read_mb * MIB : done;
    uint64_t rdone = 0;
    int rok = 1;
    const struct timespec r0 = probe_now();
    while (rdone < rtotal && b) {
        const int64_t got = hb_read(big, rdone, b, 16 * MIB);
        if (got <= 0) { rok = 0; break; }
        rok &= same(b, (size_t)got, rdone);
        rdone += (uint64_t)got;
        progress("broker: %llu MiB read", (unsigned long long)(rdone >> 20));
    }
    const double rms = probe_ms_since(r0);
    result(rok && rdone == rtotal, "broker.read", "%llu MiB read back through the broker in %.0f ms: %.0f MiB/s, %s", (unsigned long long)(rdone >> 20),
           rms, rms > 0 ? (double)rdone / MIB / (rms / 1e3) : 0.0, rok ? "every byte right" : "WRONG or short");
    if (visible) read_speed("broker.direct_read", big, (uint64_t)cfg.data_read_mb * MIB, 1);   /* (the title's own read of the broker's file) */
    free(b);
    if (visible) check_routing();
    const int rm = hb_remove(big);
    INFO("broker", "the probe's file removed (%d); the broker keeps running for the next launch", rm);
    VERDICT("broker", "%s", done == total && done > 1400 * MIB && wms > 0 && (double)done / MIB / (wms / 1e3) > 50
                ? "the broker writes past the title's 1.3 GiB burst at full speed: the renderer caches and saves go through it"
                : "the broker did not beat the throttle: see the STEP lines");
    check_end();
}

/* ====================================================================================== 11. the pad and the sound */
#define HOS_PAD_N_PROBE 17   /* (gpu/vk/vk_api.h's HOS_PAD_* in SDL's order, LTRIGGER and RTRIGGER last) */
static const char *const pad_names[HOS_PAD_N_PROBE] = {"cross (SOUTH)", "circle (EAST)", "square (WEST)", "triangle (NORTH)", "touch pad (BACK)",
                                                       "PS (GUIDE)", "OPTIONS (START)", "L3", "R3", "L1", "R1", "up", "down", "left", "right",
                                                       "L2", "R2"};
/* nn_hid.c's default binding (pad_default), HOS_PAD_* -> the Switch's button */
static const char *switch_of(int pad) {
    static const char *const sw[HOS_PAD_N_PROBE] = {"B", "A", "Y", "X", "MINUS", "-", "PLUS", "LS", "RS", "L", "R", "UP", "DOWN", "LEFT",
                                                    "RIGHT", "ZL", "ZR"};
    return pad >= 0 && pad < HOS_PAD_N_PROBE ? sw[pad] : "?";
}
/* what scePad said, and the comings and goings pad_sdl.c saw (run 3, an unattended controller: 46 "disconnected" lines
   before hos_sdl_ps5.c kept its handles and gave presence a grace period) */
extern unsigned probe_pad_disconnects;   /* (probe_runtime.c: the "disconnected" lines pad_sdl.c logged) */
static void pad_text(const char *id) {
    struct hos_ps5_pad_stats p;
    hos_ps5_pad_stats(&p);
    INFO(id, "scePad: %llu reads, %llu latest states, %llu samples (%llu not connected, %llu the shell's), %llu read errors (last %#x); "
             "%u handles opened; pad_sdl.c opened %u, closed %u (%u \"disconnected\" lines); %u there now", p.reads, p.states, p.samples,
         p.not_connected, p.intercepted, p.read_errors, (unsigned)p.last_error, p.handles_opened, p.opens, p.closes,
         probe_pad_disconnects, p.present);
}
static void check_pad_audio(void) {
    check_begin("pad", cfg.pad_seconds + 30);
    out("PAD: press buttons and move the sticks for %u s (the DualSense of the user who started the title; it rumbles at the "
        "start and at the end)", cfg.pad_seconds);
    uint64_t b0 = 0;
    int32_t s0[4] = {0};
    hos_sdl_pads(&b0, s0);   /* (starts pad_sdl.c's thread) */
    Sleep(1500);             /* (scePadOpen may take a second) */
    hos_sdl_rumble(0.6f, 0.6f), Sleep(400), hos_sdl_rumble(0, 0);   /* (the cue: pad_sdl.c -> scePadSetVibration) */
    uint64_t last = 0;
    int32_t last_st[4] = {0};
    unsigned presses = 0, controllers = 0, stick_moves = 0;
    const struct timespec t0 = probe_now();
    while (probe_ms_since(t0) < cfg.pad_seconds * 1000.0) {
        uint64_t b = 0;
        int32_t st[4] = {0};
        const int n = hos_sdl_pads(&b, st);   /* (pad_sdl.c: its thread on hos_sdl_ps5.c's scePad) */
        if (n > (int)controllers) controllers = (unsigned)n;
        const uint64_t changed = b ^ last;
        for (int k = 0; k < HOS_PAD_N_PROBE; k++)
            if (changed >> k & 1) {
                out("PAD %s %s -> Switch %s (nn_hid.c's default)", pad_names[k], b >> k & 1 ? "pressed" : "released", switch_of(k));
                presses += b >> k & 1;
            }
        last = b;
        for (int k = 0; k < 4; k++)
            if ((st[k] > 16000) != (last_st[k] > 16000) || (st[k] < -16000) != (last_st[k] < -16000)) {
                out("PAD stick %s %s: %d", k < 2 ? "left" : "right", k % 2 ? "y" : "x", (int)st[k]);
                stick_moves++;
                last_st[k] = st[k];
            }
        progress("pad: %u presses", presses);
        Sleep(10);
    }
    hos_sdl_rumble(0.6f, 0.6f), Sleep(400), hos_sdl_rumble(0, 0);
    result(controllers > 0, "pad", "%u controller(s) through scePad; %u presses, %u stick moves in %u s", controllers, presses, stick_moves, cfg.pad_seconds);
    if (controllers && !presses) INFO("pad", "no button was pressed: nobody at the console? (not a failure)");
    pad_text("pad");
    check_end();
    if (!cfg.tone_ms) return;
    /* the tone: 440 Hz through the runtime's waveOut (win32_posix.c), nn_audio.c's path, onto sceAudioOut */
    check_begin("tone", 20);
    HWAVEOUT wo = NULL;
    const WAVEFORMATEX f = {.wFormatTag = WAVE_FORMAT_PCM, .nChannels = 2, .nSamplesPerSec = 48000, .nAvgBytesPerSec = 192000,
                            .nBlockAlign = 4, .wBitsPerSample = 16};
    const MMRESULT r = waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) { result(0, "tone", "waveOutOpen: %u", (unsigned)r); check_end(); return; }
    const unsigned blocks = (cfg.tone_ms + 99) / 100;
    static int16_t pcm[100][4800 * 2];
    static WAVEHDR hdr[100];
    const unsigned long long g0 = hos_ps5_audio_grains();
    for (unsigned i = 0; i < blocks && i < 100; i++) {
        for (unsigned s = 0; s < 4800; s++) {
            const double t = (double)(i * 4800 + s) / 48000.0;
            const int16_t v = (int16_t)(8000.0 * sin(2 * 3.14159265358979 * 440.0 * t));
            pcm[i][2 * s] = pcm[i][2 * s + 1] = v;
        }
        memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)pcm[i], hdr[i].dwBufferLength = sizeof pcm[i];
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
    }
    const struct timespec p0 = probe_now();
    unsigned done = 0;
    while (probe_ms_since(p0) < cfg.tone_ms + 3000.0) {
        done = 0;
        for (unsigned i = 0; i < blocks && i < 100; i++) done += (__atomic_load_n(&hdr[i].dwFlags, __ATOMIC_ACQUIRE) & WHDR_DONE) != 0;
        if (done == blocks) break;
        Sleep(20);
    }
    const double played = probe_ms_since(p0);
    const unsigned long long grains = hos_ps5_audio_grains() - g0;
    result(done == blocks && grains >= blocks * 4800 / 256, "tone", "%u ms of 440 Hz through waveOut: %u of %u blocks played in %.0f ms; "
           "%llu grains of 256 frames went to sceAudioOutOutput (you should have heard it)", cfg.tone_ms, done, blocks, played, grains);
    waveOutReset(wo);
    check_end();
}

/* ============================================================================================= the title */
int main(int argc, char **argv) {
    (void)argc, (void)argv;
    size_t flex_entry = 0;
    sceKernelAvailableFlexibleMemorySize(&flex_entry);
    char mem_entry[200];
    mem_text(mem_entry, sizeof mem_entry);
    read_config("/app0/plat-probe.ini");
    out_fd = open("/app0/plat-probe.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd >= 0) fchmod(out_fd, 0666);
    struct ps5_kernel_sw_version v = {.size = sizeof v};
    sceKernelGetSystemSwVersion(&v);
    out("Hookshot platform probe (PPSA99147), ps5-plat %s; system software %s; tests %s; at main: %s; MXCSR %#x", PROBE_REVISION,
        v.text, cfg.tests, mem_entry, mxcsr());
    /* 1. the window first, as hos_main.c's main: before any thread's stack or the renderer's memory is placed */
    check_window();
    /* 2. hos_ps5_start: /data (and the Lapy daemon when asked), the IEEE state, klog, the splash */
    int marker = 0;
    if (cfg.elevate) marker = close(open("/app0/hookshot-elevate", O_WRONLY | O_CREAT, 0666)) == 0;
    hos_ps5_start();
    if (marker) unlink("/app0/hookshot-elevate");
    hos_commit_init();   /* (mkw_glue.c: commit on first touch, the crash report) */
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    if (want("bss")) check_bss(flex_entry);
    if (want("futex")) check_futex();
    if (want("sched")) check_sched(cfg.sched_threads, cfg.sched_cores, cfg.sched_spinners, cfg.sched_seconds);   /* (probe_sched.c) */
    if (want("pad")) check_pad_audio();   /* (early: whoever launched the title has the controller in hand) */
    if (window_bytes && want("eager")) check_eager();
    if (window_bytes && want("lazy")) check_lazy();
    if (want("stacks")) check_stacks();
    if (window_bytes && want("vk")) check_vk();
    if (window_bytes && want("memory")) check_memory();
    if (vk_ready) vk_probe_fini();
    if (want("files")) check_files();
    if (want("nup")) check_nup(cfg.nup_file, cfg.nup_hashes, cfg.nup_threads, cfg.nup_seconds);   /* (probe_nup.c) */
    if (want("data")) check_data();
    if (want("pad")) pad_text("pad.flaps");   /* (the whole run's: a controller that came and went) */
    struct hos_ps5_guest_stats s;
    hos_ps5_guest_stats(&s);
    INFO("window", "at the end: %llu commits (%llu units, %llu refused), %llu faults (%llu units, %llu retried, %llu failed), %llu decommitted "
                   "units, peak %llu MiB", (unsigned long long)s.commits, (unsigned long long)s.commit_units, (unsigned long long)s.commit_failures,
         (unsigned long long)s.faults, (unsigned long long)s.fault_units, (unsigned long long)s.fault_retries, (unsigned long long)s.fault_failures,
         (unsigned long long)s.decommit_units, (unsigned long long)(s.peak_backed_bytes >> 20));
    summary(NULL);
    hos_ps5_exit(nfail ? 1 : 0);
}
