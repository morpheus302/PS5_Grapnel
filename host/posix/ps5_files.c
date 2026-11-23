/* (ps5) win32_posix.c's file calls on the console (host/posix/ps5_fdv_hooks.h renames them to these): the modes the console
   wants, the storage broker for big writes, and (off) PS5_PayloadSDK's fdv for open files past the limit.

   Modes. Everything the runtime creates under /data is 0666 (folders 0777): the storage broker is another process with
   another identity, and the console's FTP server too (PS5_VulkanTemplate files-and-io.md). CreateFileA's 0644 kept the
   broker out of the files it made (PPSA99147 run 3: broker.writefile, every write fell back to the title's). A file the
   runtime opens for writing there is made 0666 if it is not.

   Writes through the storage broker: a file under the data folder opened for writing has its writes of
   HOOKSHOT_PS5_BROKER_MIN_KB (16) or more made by the broker (host/posix/ps5_broker_client.c), when it runs, at the
   descriptor's own position (or the end, for O_APPEND), which then moves past them as a write would move it. The
   console holds a title's own writes to 16 MiB/s past about 1 GiB (PPSA99147 runs 2 and 3); the broker, a payload, wrote
   2 GiB at 137-151 MiB/s. host/posix/ps5_stdio.c does the same for stdio (the renderer's caches). A write the broker
   refuses is the title's own, and the reason is kept (hos_ps5_files_stats).

   Open files. A title holds 249 files opened by path (PROBE.md "Open files"; the probe measured it). fdv parks the files
   idle longest by laying a socket over their numbers with dup2, and the console refuses a title that dup2 over a file
   (PPSA99147 run 3: EPERM). So fdv is off unless HOOKSHOT_PS5_FDV=1 asks to try it again (another firmware). What it
   was for is gone: the RomFS is one descriptor (host/nup_fs.c reads game.nup in place), and what else the game holds
   open at once is a handful (saves, the log, the renderer's caches, mods). An open refused for the limit (EMFILE) is
   said once, with the count, should a mod-heavy setup ever reach it; the way then is a cache of open files in
   win32_posix.c's file handles (a handle keeps its path and offset, its descriptor opened on use), which needs no dup2. */
#ifdef __PROSPERO__
#include "ps5.h"
#include "ps5_broker.h"

#include <ps5platform/fdv.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int fdv_on;
static int holder_family = AF_UNIX;
static const char *null_path = "/dev/null";
static char diag[192] = "not started";

static int real_open(const char *path, int flags, mode_t mode) {
    /* (fdv tries its placeholder on "/dev/null": the title's own eboot when the sandbox has none) */
    if (path && !strcmp(path, "/dev/null")) path = null_path;
    return open(path, flags, mode);
}
static int real_close(int fd) { return close(fd); }
static int real_dup2(int from, int to) { return dup2(from, to); }
static off_t real_lseek(int fd, off_t at, int whence) { return lseek(fd, at, whence); }
static int real_fstat(int fd, struct stat *st) { return fstat(fd, st); }
/* (a number holder that is not a file: sockets do not count against the limit, PROBE.md) */
static int placeholder(void) { return socket(holder_family, SOCK_STREAM, 0); }
static void say(const char *line) { fprintf(stderr, "ps5: %s\n", line); }

/* what fdv's check rests on, tried here first, each answer kept for hos_ps5_files_stats */
static void preflight(void) {
    int n = 0;
    const int u = socket(AF_UNIX, SOCK_STREAM, 0), ue = errno;
    const int i = u < 0 ? socket(AF_INET, SOCK_STREAM, 0) : -1, ie = errno;
    if (u < 0 && i >= 0) holder_family = AF_INET;
    n += snprintf(diag + n, sizeof diag - (size_t)n, "socket AF_UNIX %s", u >= 0 ? "ok" : strerror(ue));
    if (u < 0) n += snprintf(diag + n, sizeof diag - (size_t)n, ", AF_INET %s", i >= 0 ? "ok" : strerror(ie));
    const int holder = u >= 0 ? u : i;
    int probe = open("/dev/null", O_RDONLY);
    const int ne = errno;
    if (probe < 0) {
        null_path = "/app0/eboot.bin";
        probe = open(null_path, O_RDONLY);
    }
    n += snprintf(diag + n, sizeof diag - (size_t)n, "; /dev/null %s", probe >= 0 && null_path[1] == 'd' ? "ok" : strerror(ne));
    if (probe >= 0 && null_path[1] != 'd') n += snprintf(diag + n, sizeof diag - (size_t)n, " (/app0/eboot.bin instead)");
    if (holder >= 0 && probe >= 0) {
        const int d = dup2(holder, probe), de = errno;
        struct stat st;
        const int f = d >= 0 ? fstat(probe, &st) : -1, fe = errno;
        n += snprintf(diag + n, sizeof diag - (size_t)n, "; dup2 %s; fstat %s", d >= 0 ? "ok" : strerror(de),
                      d < 0 ? "-" : f ? strerror(fe) : S_ISSOCK(st.st_mode) ? "a socket" : "NOT a socket");
    }
    if (probe >= 0) close(probe);
    if (u >= 0) close(u);
    if (i >= 0) close(i);
}

static void start(void) {
    const char *want = getenv("HOOKSHOT_PS5_FDV");
    if (!want || strcmp(want, "1")) {
        snprintf(diag, sizeof diag, "off: a title may not dup2 over a file on this console (EPERM, PPSA99147 run 3), so fdv cannot "
                                    "park files; the RomFS is one descriptor (game.nup). HOOKSHOT_PS5_FDV=1 tries it");
        return;
    }
    preflight();
    static const struct ps5_fdv_ops ops = {real_open, real_close, real_dup2, real_lseek, real_fstat, placeholder, say};
    const char *high = getenv("HOOKSHOT_PS5_FDV_HIGH");
    fdv_on = ps5_fdv_init(&ops, high ? (unsigned)atoi(high) : 0, 0) == 0;
    fprintf(stderr, "ps5: open files: fdv %s (%s)\n", fdv_on ? "on" : "could not start: 249 files at most", diag);
}

/* under /data: what another process (the broker, FTP) must be able to write too */
static int shared_path(const char *path) { return path && !strncmp(path, "/data/", 6); }
static unsigned open_files;   /* (files this module opened and has not closed: for the EMFILE line) */

/* ---- the files the broker writes: those under its root opened for writing, by descriptor ---- */
#define BROKERED 128
static struct { int fd, append; char *path; } brokered[BROKERED];
static pthread_mutex_t brokered_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long broker_writes, broker_fallbacks;
static unsigned long long broker_bytes;
static size_t broker_min(void) {
    static size_t min;
    if (!min) {
        const char *e = getenv("HOOKSHOT_PS5_BROKER_MIN_KB");
        min = (size_t)(e && atoi(e) > 0 ? atoi(e) : 16) << 10;
    }
    return min;
}
static void broker_note(int fd, const char *path, int flags) {
    if (fd < 0 || !path || !(flags & (O_WRONLY | O_RDWR)) || !hb_path_root(path)) return;
    pthread_mutex_lock(&brokered_mu);
    for (int k = 0; k < BROKERED; k++)
        if (!brokered[k].path) {
            brokered[k].fd = fd, brokered[k].append = (flags & O_APPEND) != 0, brokered[k].path = strdup(path);
            break;
        }
    pthread_mutex_unlock(&brokered_mu);
}
static void broker_forget(int fd) {
    pthread_mutex_lock(&brokered_mu);
    for (int k = 0; k < BROKERED; k++)
        if (brokered[k].path && brokered[k].fd == fd) free(brokered[k].path), brokered[k].path = NULL;
    pthread_mutex_unlock(&brokered_mu);
}
/* the path to broker this write to (copied into out), or 0 */
static int broker_of(int fd, size_t n, char out[HB_MAX_PATH + 1], int *append) {
    if (n < broker_min() || !hb_ready()) return 0;
    int found = 0;
    pthread_mutex_lock(&brokered_mu);
    for (int k = 0; k < BROKERED && !found; k++)
        if (brokered[k].path && brokered[k].fd == fd) {
            snprintf(out, HB_MAX_PATH + 1, "%s", brokered[k].path);
            *append = brokered[k].append, found = 1;
        }
    pthread_mutex_unlock(&brokered_mu);
    return found;
}
static volatile int broker_error;   /* (the last refusal: -errno, or the client's own error) */
void hos_ps5_broker_count(uint64_t bytes, int brokered_ok) {
    if (brokered_ok > 0) __atomic_add_fetch(&broker_writes, 1, __ATOMIC_RELAXED), __atomic_add_fetch(&broker_bytes, bytes, __ATOMIC_RELAXED);
    else {
        __atomic_add_fetch(&broker_fallbacks, 1, __ATOMIC_RELAXED);
        if (brokered_ok < 0) broker_error = brokered_ok;
    }
}

int hos_ps5_fd_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        if (shared_path(path)) mode = 0666;
    }
    pthread_once(&once, start);
    const int fd = fdv_on && path && path[0] == '/' ? ps5_fdv_open(path, flags, mode) : open(path, flags, mode);
    if (fd < 0) {
        static volatile int said;
        if (errno == EMFILE && !__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
            fprintf(stderr, "ps5: open files: %s refused, too many open (the console allows 249 by path; %u through the runtime's "
                            "CreateFileA)\n", path ? path : "?", open_files);
        return fd;
    }
    __atomic_add_fetch(&open_files, 1, __ATOMIC_RELAXED);
    struct stat st;
    if ((flags & (O_WRONLY | O_RDWR)) && shared_path(path) && !fstat(fd, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0666) != 0666)
        fchmod(fd, st.st_mode | 0666);   /* (one an older build made 0644: the broker's from now on too) */
    broker_note(fd, path, flags);
    return fd;
}
int hos_ps5_mkdir(const char *path, mode_t mode) {
    if (!shared_path(path)) return mkdir(path, mode);
    const int r = mkdir(path, 0777);
    if (!r) chmod(path, 0777);
    return r;
}
int hos_ps5_fd_close(int fd) {
    broker_forget(fd);
    unsigned n = __atomic_load_n(&open_files, __ATOMIC_RELAXED);
    while (n && !__atomic_compare_exchange_n(&open_files, &n, n - 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return fdv_on ? ps5_fdv_close(fd) : close(fd);
}

/* (enter: the file reopened if it was parked, and pinned until leave; a descriptor fdv does not track costs a lookup) */
#define THROUGH(type, fd, call)                    \
    do {                                           \
        if (!fdv_on) return call;                  \
        ps5_fdv_pin pin;                           \
        if (ps5_fdv_enter((fd), &pin)) return (type)-1; \
        const type r = call;                       \
        const int e = errno;                       \
        ps5_fdv_leave(pin);                        \
        errno = e;                                 \
        return r;                                  \
    } while (0)

ssize_t hos_ps5_fd_read(int fd, void *b, size_t n) { THROUGH(ssize_t, fd, read(fd, b, n)); }
static ssize_t direct_write(int fd, const void *b, size_t n) { THROUGH(ssize_t, fd, write(fd, b, n)); }
static ssize_t direct_pwrite(int fd, const void *b, size_t n, off_t at) { THROUGH(ssize_t, fd, pwrite(fd, b, n, at)); }
ssize_t hos_ps5_fd_pread(int fd, void *b, size_t n, off_t at) { THROUGH(ssize_t, fd, pread(fd, b, n, at)); }
off_t hos_ps5_fd_lseek(int fd, off_t at, int whence) { THROUGH(off_t, fd, lseek(fd, at, whence)); }
int hos_ps5_fd_fstat(int fd, struct stat *st) { THROUGH(int, fd, fstat(fd, st)); }
/* a write the broker makes at `at` (-1: the descriptor's position, or the end for O_APPEND), the descriptor's position moved
   past it as the write would; the bytes it wrote (the rest is the caller's), or 0 when it did not */
static ssize_t brokered_write(int fd, const void *b, size_t n, off_t at) {
    char path[HB_MAX_PATH + 1];
    int append = 0;
    if (!broker_of(fd, n, path, &append)) return 0;
    const int positional = at >= 0;
    if (!positional) {
        struct stat st;
        at = append ? (hos_ps5_fd_fstat(fd, &st) ? -1 : st.st_size) : hos_ps5_fd_lseek(fd, 0, SEEK_CUR);
        if (at < 0) return 0;
    }
    const uint32_t k = n > 0xffffffffu ? 0xffffffffu : (uint32_t)n;
    int64_t r = hb_write(path, (uint64_t)at, b, k);
    if (r == -EACCES && !fchmod(fd, 0666)) r = hb_write(path, (uint64_t)at, b, k);   /* (a file the broker may not write: opened up once) */
    if (r <= 0) { hos_ps5_broker_count(0, (int)r); return 0; }
    if (!positional && !append) hos_ps5_fd_lseek(fd, at + (off_t)r, SEEK_SET);
    hos_ps5_broker_count((uint64_t)r, 1);
    return (ssize_t)r;
}
ssize_t hos_ps5_fd_write(int fd, const void *b, size_t n) {
    const ssize_t k = brokered_write(fd, b, n, -1);
    if (k == (ssize_t)n) return k;
    const ssize_t rest = direct_write(fd, (const char *)b + k, n - (size_t)k);
    return rest < 0 ? (k ? k : rest) : k + rest;
}
ssize_t hos_ps5_fd_pwrite(int fd, const void *b, size_t n, off_t at) {
    const ssize_t k = brokered_write(fd, b, n, at);
    if (k == (ssize_t)n) return k;
    const ssize_t rest = direct_pwrite(fd, (const char *)b + k, n - (size_t)k, at + k);
    return rest < 0 ? (k ? k : rest) : k + rest;
}
int hos_ps5_fd_ftruncate(int fd, off_t size) { THROUGH(int, fd, ftruncate(fd, size)); }
int hos_ps5_fd_fdatasync(int fd) { THROUGH(int, fd, fdatasync(fd)); }

void hos_ps5_files_stats(struct hos_ps5_files_stats *s) {
    memset(s, 0, sizeof *s);
    snprintf(s->diag, sizeof s->diag, "%s", diag);
    s->broker_writes = __atomic_load_n(&broker_writes, __ATOMIC_RELAXED);
    s->broker_bytes = __atomic_load_n(&broker_bytes, __ATOMIC_RELAXED);
    s->broker_fallbacks = __atomic_load_n(&broker_fallbacks, __ATOMIC_RELAXED);
    s->broker_error = broker_error;
    s->open_files = open_files;
    if (!fdv_on) { s->disabled = 1; return; }
    struct ps5_fdv_stats f;
    ps5_fdv_get_stats(&f);
    s->live = f.live, s->parked = f.parked, s->high_water = f.high_water;
    s->parks = f.parks, s->unparks = f.unparks, s->limit_hits = f.limit_hits, s->failures = f.failures;
    s->disabled = f.disabled;
}
#endif /* __PROSPERO__ */
