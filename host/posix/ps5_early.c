/* (ps5) The eboot's first code, before its constructors and main: the stage trail and the write-watch table.

   The stage trail, hookshot_stages.txt. The first launch of the game's eboot (f7c8633, 2026-11-09) powered the console off
   about 3 s in and left nothing: klog is lost at power-off, and hookshot_log.txt is opened by the game's thread, after stage 6.
   The trail is a file the eboot appends a line to at each step of its start, from its .preinit_array entry (the CRT,
   PS5_Vulkan tooling/native/app_crt.cpp _init, runs it after libc's own start and before every constructor) to its first 600
   frames; each line is one write(2) on a descriptor opened O_APPEND, then fsync(2), never through stdio (wrapped for the
   storage broker) nor the broker. A power-off keeps every line written before it: the last line of the last launch says how
   far that launch got. The file is <data folder>/hookshot_stages.txt when the title sees /data at its first line (/data/grapnel,
   or /data/hookshot before the first Grapnel start: the takeover renames it with the file open, and the trail goes on in the
   same file under the new name: hos_ps5_trail_renamed), else /app0/hookshot_stages.txt (= /data/homebrew/GRPN00001 over
   FTP), and moves to the data root after stage 3 (hos_main.c),
   the lines so far copied (a buffer of the launch's first lines). Each launch starts with a "==== launch" line; a file past
   512 KiB is started again. Every line goes to klog too ([hookshot] trail: ...).

   The write-watch table (a64-kit's a64_dirty, 128 MiB: a mark per 4 KiB page of the 512 GiB window). It was .bss, which the
   console charges to the title's flexible memory at load (the platform probe, PPSA99147). The eboot's link now places the
   symbol past the image's end and drops its .bss storage (nx2/toolchain_ps5.py, the link's script hookshot-ps5.ld): the
   lifted code reaches it by a RIP-relative address fixed at link time, so it has to be backed exactly there, before anything
   touches it. Done here, first: a reservation that takes the range only if free (ps5_vrange_reserve_at), 128 MiB of direct
   memory mapped over it, zeroed. An eboot linked without the script has the table in its .bss: then nothing is done.
   main says which at stage 1, and stops there when the table could not be backed (the game would fault at its first mark).

   SPDX-License-Identifier: GPL-3.0-or-later (hookshot-ps5) */
#ifdef __PROSPERO__
#include "ps5.h"
#include "ps5_data_folder.h"   /* (lot 80: the data folders' names) */

#include <ps5platform/kernel.h>
#include <ps5platform/memory.h>
#include <ps5platform/shm.h>

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int sceKernelDebugOutText(int channel, const char *text);

/* ---- the trail ---- */
#define TRAIL_NAME "hookshot_stages.txt"
#define TRAIL_MAX (512u << 10)     /* (a file past this is started again at the next launch) */
#define KEEP_BYTES (24u << 10)     /* (the launch's first lines, copied when the trail moves to the data root) */

/* (a spinlock that yields, not a pthread mutex: the first line is written before any constructor, and a static mutex is set
   up by libthr at its first lock; held across one write and its fsync) */
static volatile int trail_busy;
static void trail_lock(void) {
    while (__atomic_exchange_n(&trail_busy, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&trail_busy, __ATOMIC_RELAXED)) sched_yield();
}
static void trail_unlock(void) { __atomic_store_n(&trail_busy, 0, __ATOMIC_RELEASE); }
static int trail_fd = -1;
static char trail_path[256];
static char kept[KEEP_BYTES];
static size_t kept_n;
static struct timespec t0;
static int early_done;
static const char *first_by = "?";

double hos_ps5_since_start(void);
static double since_start(void) { return hos_ps5_since_start(); }
/* seconds since the eboot's first code (the trail's clock; host/hos_thread.c's log lines carry it too) */
double hos_ps5_since_start(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
}

/* one whole line, its write and its fsync (the lock held) */
static void put_line(int fd, const char *line, size_t n) {
    if (fd < 0) return;
    while (n) {
        const ssize_t w = write(fd, line, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;
        }
        line += w, n -= (size_t)w;
    }
    fsync(fd);
}

/* <dir>/hookshot_stages.txt for appending; a file past TRAIL_MAX is emptied first. -1 when it cannot be opened */
static int open_trail(const char *dir, char *path, size_t cap, size_t *was) {
    snprintf(path, cap, "%s/" TRAIL_NAME, dir);
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return -1;
    fchmod(fd, 0666);   /* (read and deleted over FTP, written by the next launch whoever made it) */
    struct stat st;
    *was = 0;
    if (fstat(fd, &st) == 0 && (uint64_t)st.st_size > TRAIL_MAX) {
        *was = (size_t)st.st_size;
        ftruncate(fd, 0);
    }
    return fd;
}

/* (2026-11-10, after runs 13 and 14: the console itself went down at the first 3D scene, the log's last 17 s were lost with
   it and the second time its storage was damaged. HOOKSHOT_PS5_LOG_UDP=<IPv4>:<port> (hookshot_env.txt): every stage-trail
   line and every log line is also sent to a PC as one UDP datagram, which needs no write on the console's disk and is out
   of the console before the next instruction runs. tools/udp-log.py on the PC keeps them. Never waits: a full socket
   buffer drops the line. The variable is looked for at each line until it is found (the settings file is read at stage 4). */
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
static int udp_fd = -2;   /* -2 not looked for yet, -1 none */
static struct sockaddr_in udp_to;
static unsigned udp_looks;
void hos_ps5_udp_line(const char *line, size_t n) {
    if (udp_fd == -1) return;
    if (udp_fd == -2) {
        const char *e = getenv("HOOKSHOT_PS5_LOG_UDP");
        if (!e || !*e) {
            if (++udp_looks > 20000) udp_fd = -1;   /* (never set: stop asking) */
            return;
        }
        char ip[64];
        const char *colon = strrchr(e, ':');
        const size_t len = colon ? (size_t)(colon - e) : strlen(e);
        if (len == 0 || len >= sizeof ip) { udp_fd = -1; return; }
        memcpy(ip, e, len), ip[len] = 0;
        memset(&udp_to, 0, sizeof udp_to);
        udp_to.sin_len = sizeof udp_to, udp_to.sin_family = AF_INET;
        udp_to.sin_port = htons((uint16_t)(colon ? atoi(colon + 1) : 9148));
        if (inet_pton(AF_INET, ip, &udp_to.sin_addr) != 1) { udp_fd = -1; return; }
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { udp_fd = -1; return; }
        int expected = -2;
        if (!__atomic_compare_exchange_n(&udp_fd, &expected, fd, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) close(fd);   /* (another thread's) */
    }
    if (udp_fd >= 0) (void)sendto(udp_fd, line, n > 1400 ? 1400 : n, MSG_DONTWAIT, (const struct sockaddr *)&udp_to, sizeof udp_to);
}

static void vtrail(const char *fmt, va_list ap) {
    char line[1024];
    int n = snprintf(line, sizeof line, "[%8.3f] ", since_start());
    const int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    n = m < 0 ? n : n + m < (int)sizeof line - 1 ? n + m : (int)sizeof line - 2;
    while (n > 0 && line[n - 1] == '\n') n--;
    line[n++] = '\n';
    line[n] = 0;
    {   /* (to the PC first: before the write and its sync) */
        char u[1100];
        const int un = snprintf(u, sizeof u, "trail: %s", line);
        hos_ps5_udp_line(u, (size_t)(un < (int)sizeof u ? un : (int)sizeof u - 1));
    }
    trail_lock();
    put_line(trail_fd, line, (size_t)n);
    if (kept_n + (size_t)n <= sizeof kept) memcpy(kept + kept_n, line, (size_t)n), kept_n += (size_t)n;
    trail_unlock();
    char k[1100];
    snprintf(k, sizeof k, "[hookshot] trail: %s", line);
    sceKernelDebugOutText(0, k);
}

void hos_ps5_trail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vtrail(fmt, ap);
    va_end(ap);
}

/* " (flexible <n> MiB free of <configured>; direct <n> MiB free of <n>, largest <n>)" for a trail line */
const char *hos_ps5_trail_memory(char *buf, size_t cap) {
    struct ps5_memory_stats m;
    if (!ps5_memory_query(&m)) {
        snprintf(buf, cap, " (memory: no answer)");
        return buf;
    }
    size_t configured = 0;
    if (sceKernelConfiguredFlexibleMemorySize(&configured)) configured = 0;
    snprintf(buf, cap, " (flexible %llu MiB free of %llu; direct %llu MiB free of %llu, largest %llu)",
             (unsigned long long)(m.flexible_bytes >> 20), (unsigned long long)(configured >> 20),
             (unsigned long long)(m.free_bytes >> 20), (unsigned long long)(m.total_bytes >> 20),
             (unsigned long long)(m.largest_bytes >> 20));
    return buf;
}

/* the title's direct memory in MiB, all of it (11712 with 1 GiB of flexible memory, 12086 with 650 MiB; 11712 with no answer) */
uint64_t hos_ps5_memory_total_mb(void) {
    struct ps5_memory_stats m;
    return ps5_memory_query(&m) && m.total_bytes ? (uint64_t)(m.total_bytes >> 20) : 11712;
}
/* free direct and flexible memory in MiB now (0 and 0 with no answer); the return says whether the console answered */
/* (2026-11-10) the free direct memory as last measured here, less what was allocated since and plus what was released
   (ps5_platform.c's wraps keep it): what an allocation under 16 MiB is checked against without a walk of the free pieces.
   -1 before the first measure. And the number of free pieces of that measure, and how many measures had no walk. */
int64_t hos_ps5_free_estimate = -1;
static uint64_t free_pieces, walks_failed;
uint64_t hos_ps5_memory_pieces(uint64_t *no_walk) {
    if (no_walk) *no_walk = __atomic_load_n(&walks_failed, __ATOMIC_RELAXED);
    return __atomic_load_n(&free_pieces, __ATOMIC_RELAXED);
}
int hos_ps5_memory_free(uint64_t *direct_mb, uint64_t *flexible_mb, uint64_t *largest_mb) {
    struct ps5_memory_stats m;
    if (!ps5_memory_query(&m)) {
        /* (2026-11-10) no answer from the walk: it asks the kernel once or twice a free piece and gives up past 8192 questions,
           and the memory watch then saw nothing at all. The largest free piece alone (one question) stands for the free
           memory then: never more than the truth, so the title still ends itself at its floor, early if anything. */
        const int64_t total = sceKernelGetDirectMemorySize();
        int64_t start = 0;
        size_t bytes = 0, flexible = 0;
        const int32_t r = total > 0 ? sceKernelAvailableDirectMemorySize(0, total, PS5_KERNEL_PAGE_SIZE, &start, &bytes) : -1;
        if (r != 0 && (uint32_t)r != UINT32_C(0x8002000c)) {   /* (0x8002000c: nothing free at all, which is an answer) */
            *direct_mb = *flexible_mb = *largest_mb = 0;
            return 0;
        }
        if (r != 0) bytes = 0;
        if (sceKernelAvailableFlexibleMemorySize(&flexible)) flexible = 0;
        *direct_mb = *largest_mb = bytes >> 20, *flexible_mb = flexible >> 20;
        __atomic_store_n(&hos_ps5_free_estimate, (int64_t)bytes, __ATOMIC_RELAXED);
        if (__atomic_fetch_add(&walks_failed, 1, __ATOMIC_RELAXED) < 5)
            hos_ps5_trail("memory: the free pieces could not be walked: the largest one, %llu MiB, stands for the free direct memory",
                          (unsigned long long)(bytes >> 20));
        return 1;
    }
    *direct_mb = m.free_bytes >> 20, *flexible_mb = m.flexible_bytes >> 20, *largest_mb = m.largest_bytes >> 20;
    __atomic_store_n(&hos_ps5_free_estimate, (int64_t)m.free_bytes, __ATOMIC_RELAXED);
    __atomic_store_n(&free_pieces, (uint64_t)m.free_ranges, __ATOMIC_RELAXED);
    return 1;
}

void hos_ps5_trail_root(const char *root) {
    if (!root || !*root) return;
    char want[256], path[256];
    snprintf(want, sizeof want, "%s/" TRAIL_NAME, root);
    trail_lock();
    const int same = trail_fd >= 0 && !strcmp(want, trail_path);
    trail_unlock();
    if (same) return;
    size_t was = 0;
    const int fd = open_trail(root, path, sizeof path, &was);
    if (fd < 0) {
        hos_ps5_trail("trail: %s cannot be opened (errno %d): it stays in %s", want, errno, trail_path[0] ? trail_path : "(none)");
        return;
    }
    char line[640];
    trail_lock();
    const int n = snprintf(line, sizeof line, "[%8.3f] trail: continued in %s (the data root)\n", since_start(), path);
    put_line(trail_fd, line, (size_t)n);
    const int old = trail_fd;
    char from[256];
    snprintf(from, sizeof from, "%s", trail_path[0] ? trail_path : "nowhere (no folder could be written)");
    if (was) {
        const int k = snprintf(line, sizeof line, "(the trail was %zu bytes: started again)\n", was);
        put_line(fd, line, (size_t)k);
    }
    put_line(fd, kept, kept_n);   /* (this launch's lines so far, its "==== launch" line first) */
    const int c = snprintf(line, sizeof line, "[%8.3f] trail: the lines above were written to %s first; from here on, here\n",
                           since_start(), from);
    put_line(fd, line, (size_t)c);
    trail_fd = fd;
    snprintf(trail_path, sizeof trail_path, "%s", path);
    trail_unlock();
    if (old >= 0) close(old);
}

const char *hos_ps5_trail_file(void) { return trail_path[0] ? trail_path : "(none)"; }

/* (lot 80) The folder that holds the trail's file was renamed (the takeover of /data/hookshot as /data/grapnel, ps5_platform.c
   hos_ps5_data_init): the descriptor writes on in the same file, whose path is now under the new name; so that stage 3's
   move to the data root finds it there already instead of copying this launch's lines into the same file again */
void hos_ps5_trail_renamed(const char *from, const char *to) {
    const size_t n = strlen(from);
    char path[256];
    int moved = 0;
    trail_lock();
    if (trail_fd >= 0 && !strncmp(trail_path, from, n) && trail_path[n] == '/') {
        snprintf(path, sizeof path, "%s%s", to, trail_path + n);
        snprintf(trail_path, sizeof trail_path, "%s", path);
        moved = 1;
    }
    trail_unlock();
    if (moved) hos_ps5_trail("trail: %s renamed to %s: the same file, now %s", from, to, trail_path);
}

/* ---- the write-watch table ---- */
extern uint8_t a64_dirty[];
#define DIRTY_BYTES ((size_t)1 << 27)
#define UNIT64K ((uintptr_t)1 << 16)
static int dirty_state;           /* 0 not looked at, 1 in the image's .bss, 2 backed here, 3 reserved here and backed at each
                                     part's first touch, -1 could not be backed */
/* (2026-11-10) The table is backed where it is touched: a part of 256 KiB (the marks of 1 GiB of the window) at its first
   access, from the fault handler (hos_ps5_dirty_fault, called by host/mkw_glue.c's commit_on_touch before the window's own).
   The game's memory lies in a few stretches of the 512 GiB, so most of the 128 MiB was never written: the area a save loads
   stopped at the memory floor with about 0.8 GiB to go. HOOKSHOT_PS5_DIRTY_WHOLE cannot be read this early: the eboot's
   build chooses (-DHOOKSHOT_PS5_DIRTY_WHOLE: the whole table at start, as before). */
#define DIRTY_PART ((size_t)1 << 18)
#define DIRTY_PARTS (((size_t)1 << 27) / DIRTY_PART)
static uint8_t dirty_part_backed[DIRTY_PARTS];
static int dirty_lock;
static uint64_t dirty_backed_parts, dirty_refused;
static char dirty_text[200] = "not looked at";

static void dirty_back(void) {
    const uintptr_t at = (uintptr_t)a64_dirty;
    void *s = NULL, *e = NULL;
    uint32_t prot = 0;
    if (sceKernelQueryMemoryProtection((void *)at, &s, &e, &prot) == 0 && (uintptr_t)s <= at && at < (uintptr_t)e) {
        /* (mapped already: the image's .bss, an eboot linked without hookshot-ps5.ld; charged to flexible memory at load) */
        dirty_state = 1;
        snprintf(dirty_text, sizeof dirty_text, "in the image's .bss at %#lx (prot %#x): charged to flexible memory",
                 (unsigned long)at, (unsigned)prot);
        return;
    }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    const uintptr_t lo = at & ~(UNIT64K - 1), hi = (at + DIRTY_BYTES + UNIT64K - 1) & ~(UNIT64K - 1);
    const size_t len = (size_t)(hi - lo);
    int r = ps5_vrange_reserve_at((void *)lo, len);
    if (r) {
        dirty_state = -1;
        snprintf(dirty_text, sizeof dirty_text, "NOT BACKED: %#lx-%#lx is not free (reserve %#x)", (unsigned long)lo,
                 (unsigned long)hi, (unsigned)r);
        return;
    }
#ifndef HOOKSHOT_PS5_DIRTY_WHOLE
    if (lo == at && len == DIRTY_BYTES) {   /* (placed on a 2 MiB boundary by the link's script: parts start on 64 KiB ones) */
        dirty_state = 3;
        snprintf(dirty_text, sizeof dirty_text, "%zu MiB reserved at %#lx (past the image), each %zu KiB backed at its first touch",
                 len >> 20, (unsigned long)at, DIRTY_PART >> 10);
        return;
    }
#endif
    int64_t start = -1;
    r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len, UNIT64K, PS5_KERNEL_DIRECT_TYPE_CPU, &start);
    if (r) {
        ps5_vrange_release((void *)lo, len);
        dirty_state = -1;
        snprintf(dirty_text, sizeof dirty_text, "NOT BACKED: no %zu MiB of direct memory (%#x)", len >> 20, (unsigned)r);
        return;
    }
    void *placed = (void *)lo;
    r = sceKernelMapDirectMemory(&placed, len, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE, PS5_KERNEL_MAP_FIXED, start,
                                 PS5_KERNEL_PAGE_SIZE);
    if (r == 0 && placed != (void *)lo) sceKernelMunmap(placed, len), r = PS5_SHM_NO_PLACE;
    if (r) {
        sceKernelReleaseDirectMemory(start, len);
        ps5_vrange_release((void *)lo, len);
        dirty_state = -1;
        snprintf(dirty_text, sizeof dirty_text, "NOT BACKED: mapping %zu MiB at %#lx failed (%#x)", len >> 20, (unsigned long)lo,
                 (unsigned)r);
        return;
    }
    memset((void *)lo, 0, len);   /* (direct memory comes back with what it held: PROBE.md) */
    clock_gettime(CLOCK_MONOTONIC, &b);
    dirty_state = 2;
    snprintf(dirty_text, sizeof dirty_text, "%zu MiB of direct memory at %#lx (past the image; not flexible memory), backed and "
             "zeroed in %.1f ms", len >> 20, (unsigned long)at,
             (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6);
}

/* A fault at p: 1 when p is in the table and its part is backed now (the access is tried again), 0 otherwise (not the table's,
   or no memory for the part: the fault goes on to the other handlers and ends the title with its address in the trail).
   Nothing here can wait on the faulting thread: a spin lock of its own, kernel calls, no log, no heap. */
int hos_ps5_dirty_fault(const void *p) {
    const uintptr_t a = (uintptr_t)p, at = (uintptr_t)a64_dirty;
    if (dirty_state != 3 || a < at || a >= at + DIRTY_BYTES) return 0;
    const size_t part = (a - at) / DIRTY_PART;
    int ok = 1;
    while (__atomic_exchange_n(&dirty_lock, 1, __ATOMIC_ACQUIRE)) sched_yield();
    if (!dirty_part_backed[part]) {
        void *want = (void *)(at + part * DIRTY_PART), *placed = want;
        int64_t start = -1;
        int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), DIRTY_PART, UNIT64K, PS5_KERNEL_DIRECT_TYPE_CPU, &start);
        if (r == 0) {
            r = sceKernelMapDirectMemory(&placed, DIRTY_PART, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE, PS5_KERNEL_MAP_FIXED,
                                         start, PS5_KERNEL_PAGE_SIZE);
            if (r == 0 && placed != want) sceKernelMunmap(placed, DIRTY_PART), r = PS5_SHM_NO_PLACE;
            if (r) sceKernelReleaseDirectMemory(start, DIRTY_PART);
        }
        if (r == 0) {
            memset(want, 0, DIRTY_PART);   /* (direct memory comes back with what it held) */
            __atomic_store_n(&dirty_part_backed[part], 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&dirty_backed_parts, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_add_fetch(&dirty_refused, 1, __ATOMIC_RELAXED);
            ok = 0;
        }
    }
    __atomic_store_n(&dirty_lock, 0, __ATOMIC_RELEASE);
    return ok;
}

/* The table's memory in bytes: all of it (states 1 and 2), or the parts backed so far. *refused: parts no memory was found for. */
uint64_t hos_ps5_dirty_bytes(uint64_t *refused) {
    if (refused) *refused = __atomic_load_n(&dirty_refused, __ATOMIC_RELAXED);
    if (dirty_state == 3) return __atomic_load_n(&dirty_backed_parts, __ATOMIC_RELAXED) * DIRTY_PART;
    return dirty_state == 1 || dirty_state == 2 ? DIRTY_BYTES : 0;
}

int hos_ps5_dirty_state(const char **text) {
    if (text) *text = dirty_text;
    return dirty_state;
}

/* ---- the first code ---- */
static void early(const char *by) {
    if (early_done) return;
    early_done = 1;
    first_by = by;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* the trail's file: the data folder when the sandbox shows it (ShadowMount+ registers the title with /data): /data/grapnel,
       else /data/hookshot (lot 80: before the first Grapnel start, which renames it to /data/grapnel after this line,
       host/posix/ps5_data_folder.h), else /app0 */
    struct stat st;
    size_t was = 0;
    const char *why = "the data root";
    if (stat(PS5_DATA_DIR, &st) == 0 && S_ISDIR(st.st_mode)) trail_fd = open_trail(PS5_DATA_DIR, trail_path, sizeof trail_path, &was);
    else if (stat(PS5_DATA_OLD_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        why = "the data root before lot 80: taken over as " PS5_DATA_DIR " at the start, the file with it (unless that rename fails)";
        trail_fd = open_trail(PS5_DATA_OLD_DIR, trail_path, sizeof trail_path, &was);
    }
    if (trail_fd < 0) {
        why = "no data folder there (yet): moved to the data root after stage 3";
        trail_fd = open_trail("/app0", trail_path, sizeof trail_path, &was);
    }
    if (trail_fd < 0) trail_path[0] = 0, why = "NO FOLDER COULD BE WRITTEN: klog only, until stage 3";
    char head[600], when[40] = "?";
    const time_t now = time(NULL);
    struct tm tm;
    if (gmtime_r(&now, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &tm);
    snprintf(head, sizeof head, "==== launch: pid %d, %s; the trail %s (%s)%s\n", (int)getpid(), when,
             trail_path[0] ? trail_path : "nowhere", why, was ? "; the file was past 512 KiB: started again" : "");
    trail_lock();
    put_line(trail_fd, head, strlen(head));
    const size_t hn = strlen(head);
    if (hn <= sizeof kept) memcpy(kept, head, hn), kept_n = hn;
    trail_unlock();
    char mem[200];
    hos_ps5_trail("start: the eboot's first code (%s), before its constructors and main; code at %p, the table's symbol at %p%s",
                  by, (void *)early, (void *)a64_dirty, hos_ps5_trail_memory(mem, sizeof mem));
    dirty_back();
    hos_ps5_trail("start: the write-watch table (a64_dirty, 128 MiB): %s%s", dirty_text, hos_ps5_trail_memory(mem, sizeof mem));
}

static void early_preinit(void) { early(".preinit_array"); }
/* (the CRT runs .preinit_array before .init_array: this is the eboot's first code after libc's start) */
__attribute__((used, section(".preinit_array"))) static void (*early_entry)(void) = early_preinit;
/* (should a CRT ever skip .preinit_array: the first constructor instead; the trail's first line says which ran) */
__attribute__((constructor(101))) static void early_ctor(void) { early("constructor(101)"); }

const char *hos_ps5_early_by(void) { return first_by; }
#endif /* __PROSPERO__ */
