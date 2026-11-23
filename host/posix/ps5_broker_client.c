/* (ps5) The title's side of Hookshot's storage broker (host/posix/ps5_broker.h has the protocol and why; the broker is
   host/posix/ps5_broker_payload.c). The start follows Forever Launcher's (bizkut/ps5-forever, GPL-3.0,
   ps5/probes/psmkit-wine/writebroker-autostart.h and native-entry.c at 0b6abf0bb924954a58ae937ab23f4397a0c44c5f): a probe
   of the port (a refusal there means none runs), the ELF streamed to the ELF loader on 127.0.0.1:9021 and that socket
   closed once it is all sent (a title may not make a socket non-blocking: EACCES, so the calls stay blocking, which the
   loopback answers at once), then the port probed every 100 ms for 10 s (the loader takes 3.1 to 5.7 s, Forever measured).

   One connection for the process, a lock around each request: the probe's and the runtime's few big writes do not need
   Forever's pool of 16. */
#ifdef __PROSPERO__
#include "ps5_broker.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int sceKernelUsleep(uint32_t microseconds);

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int conn = -1;

const char *hb_start_name(enum hb_start s) {
    static const char *const names[] = {"present", "started", "elf loader unavailable", "no broker ELF", "no token",
                                        "timeout", "error"};
    return (unsigned)s < sizeof names / sizeof names[0] ? names[s] : "?";
}

static int transfer(int fd, void *buffer, size_t size, int sending) {
    unsigned char *bytes = buffer;
    size_t done = 0;
    while (done < size) {
        const ssize_t n = sending ? send(fd, bytes + done, size - done, MSG_NOSIGNAL) : recv(fd, bytes + done, size - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (!n) errno = EPIPE;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}
static int connect_port(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) {
        const int e = errno;
        close(fd);
        return -e;
    }
    struct timeval t = {.tv_sec = 30};   /* (a broker that stops answering is an error, not a hang) */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}
/* the token: in the data folder when the title sees /data (/data/grapnel; lot 80: /data/hookshot when the title uses it in
   place), else the broker's copy in the title's own folder */
static int read_token(unsigned char token[HB_TOKEN_BYTES]) {
    static const char *const where[] = {HB_ROOT HB_TOKEN_NAME, HB_ROOT_OLD HB_TOKEN_NAME, "/app0/" HB_TOKEN_NAME};
    for (size_t i = 0; i < sizeof where / sizeof where[0]; i++) {
        const int fd = open(where[i], O_RDONLY);
        if (fd < 0) continue;
        const int ok = read(fd, token, HB_TOKEN_BYTES) == HB_TOKEN_BYTES;
        close(fd);
        if (ok) return 1;
    }
    return 0;
}
/* 1: a broker of this version answered (conn set); 0: none, or one to replace; -ENOENT: one runs but no token is readable;
   another -errno: the probe itself failed */
static int probe(void) {
    const int fd = connect_port(HB_PORT);
    if (fd < 0) return fd == -ECONNREFUSED ? 0 : fd;
    struct hb_hello hello = {.magic = HB_HELLO_MAGIC, .version = HB_VERSION};
    if (!read_token(hello.token)) {
        close(fd);
        return -ENOENT;
    }
    struct hb_reply r;
    if (!transfer(fd, &hello, sizeof hello, 1) && !transfer(fd, &r, sizeof r, 0) && r.magic == HB_HELLO_MAGIC && !r.result) {
        conn = fd;
        return 1;
    }
    close(fd);   /* (another version, or a token it does not know: the broker sent now replaces it) */
    return 0;
}
static int send_elf(const char *path, int *detail) {
    const int file = open(path, O_RDONLY);
    struct stat st;
    if (file < 0 || fstat(file, &st) || !S_ISREG(st.st_mode) || st.st_size < 4 || st.st_size > (8 << 20)) {
        *detail = file < 0 ? errno : EINVAL;
        if (file >= 0) close(file);
        return HB_NO_ELF;
    }
    const int fd = connect_port(HB_ELF_LOADER_PORT);
    if (fd < 0) {
        *detail = -fd;
        close(file);
        return HB_LOADER_UNAVAILABLE;
    }
    static unsigned char bytes[65536];
    off_t sent = 0;
    int result = HB_STARTED;
    while (sent < st.st_size) {
        const ssize_t n = read(file, bytes, sizeof bytes);
        if (n <= 0 || (!sent && (n < 4 || memcmp(bytes, "\177ELF", 4))) || transfer(fd, bytes, (size_t)n, 1)) {
            *detail = n < 0 ? errno : n == 0 ? EIO : !sent ? ENOEXEC : errno;
            result = HB_ERROR;
            break;
        }
        sent += n;
    }
    if (result == HB_STARTED && shutdown(fd, SHUT_WR)) *detail = errno, result = HB_ERROR;
    close(fd);
    close(file);
    return result;
}

enum hb_start hb_start(const char *elf, int *detail) {
    int unused;
    if (!detail) detail = &unused;
    *detail = 0;
    pthread_mutex_lock(&mu);
    enum hb_start s;
    int p = conn >= 0 ? 1 : probe();
    if (p == 1) {
        s = HB_PRESENT;
    } else if (p < 0 && p != -ENOENT) {
        *detail = -p, s = HB_ERROR;
    } else if ((s = (enum hb_start)send_elf(elf, detail)) == HB_STARTED) {
        struct timespec t0, t;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            sceKernelUsleep(100000);
            if ((p = probe()) == 1) break;
            clock_gettime(CLOCK_MONOTONIC, &t);
            if (t.tv_sec - t0.tv_sec >= 10) {
                s = p == -ENOENT ? HB_NO_TOKEN : HB_TIMEOUT, *detail = p < 0 ? -p : ETIMEDOUT;
                break;
            }
        }
    }
    pthread_mutex_unlock(&mu);
    return s;
}

int hb_ready(void) { return __atomic_load_n(&conn, __ATOMIC_RELAXED) >= 0; }

/* one request: the header, the path, the data out; the reply, then the data in. Lock held by the caller */
static int64_t request(uint32_t magic, const char *path, uint64_t offset, uint32_t length, uint32_t reserved, const void *out,
                       void *in, uint32_t in_max, struct hb_stat *st) {
    if (conn < 0) return -ENOTCONN;
    const size_t n = strlen(path);
    if (!n || n > HB_MAX_PATH) return -ENAMETOOLONG;
    struct hb_request q = {.magic = magic, .path_length = (uint32_t)n, .offset = offset, .length = length, .reserved = reserved};
    struct hb_reply r;
    if (transfer(conn, &q, sizeof q, 1) || transfer(conn, (void *)path, n, 1) ||
        (out && length && transfer(conn, (void *)out, length, 1)) || transfer(conn, &r, sizeof r, 0) ||
        r.magic != HB_HELLO_MAGIC)
        goto broken;
    if (in && r.result > 0) {
        if ((uint32_t)r.result > in_max || transfer(conn, in, (size_t)r.result, 0)) goto broken;
    }
    if (st) {
        struct hb_stat s;
        if (transfer(conn, &s, sizeof s, 0)) goto broken;
        *st = s;
    }
    return r.result;
broken: {
    const int e = errno ? errno : EIO;
    close(conn);   /* (the stream is out of step: the next hb_start connects again) */
    conn = -1;
    return -e;
}
}
int64_t hb_write(const char *path, uint64_t offset, const void *data, uint32_t length) {
    int64_t done = 0;
    pthread_mutex_lock(&mu);
    while (done < length) {
        const uint32_t k = length - done < HB_MAX_IO ? (uint32_t)(length - done) : HB_MAX_IO;
        const int64_t r = request(HB_WRITE, path, offset + (uint64_t)done, k, 0, (const uint8_t *)data + done, NULL, 0, NULL);
        if (r < 0) { done = done ? done : r; break; }
        done += r;
    }
    pthread_mutex_unlock(&mu);
    return done;
}
int64_t hb_read(const char *path, uint64_t offset, void *data, uint32_t length) {
    int64_t done = 0;
    pthread_mutex_lock(&mu);
    while (done < length) {   /* (the broker answers at most 1 MiB a request) */
        const uint32_t k = length - done;
        const int64_t r = request(HB_READ, path, offset + (uint64_t)done, k, 0, NULL, (uint8_t *)data + done, k, NULL);
        if (r < 0) { done = done ? done : r; break; }
        if (r == 0) break;
        done += r;
    }
    pthread_mutex_unlock(&mu);
    return done;
}
static int simple(uint32_t magic, const char *path, uint32_t reserved, struct hb_stat *st) {
    pthread_mutex_lock(&mu);
    const int64_t r = request(magic, path, 0, 0, reserved, NULL, NULL, 0, st);
    pthread_mutex_unlock(&mu);
    return (int)r;
}
int hb_create(const char *path, int truncate) { return simple(HB_CREATE, path, truncate ? 1 : 0, NULL); }
int hb_mkdir(const char *path) { return simple(HB_MKDIR, path, 0, NULL); }
int hb_remove(const char *path) { return simple(HB_REMOVE, path, 0, NULL); }
int hb_stat(const char *path, uint64_t *size, uint64_t *mode) {
    struct hb_stat s = {0};
    const int r = simple(HB_STAT, path, 0, &s);
    if (size) *size = s.size;
    if (mode) *mode = s.mode;
    return r;
}
int hb_statfs(const char *path, uint64_t *bsize, uint64_t *blocks, uint64_t *bavail) {
    struct hb_stat s = {0};
    const int r = simple(HB_STATFS, path, 0, &s);
    if (bsize) *bsize = s.bsize;
    if (blocks) *blocks = s.blocks;
    if (bavail) *bavail = s.bavail;
    return r;
}
#endif /* __PROSPERO__ */
