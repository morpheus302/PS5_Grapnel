/* hookshot-broker: Hookshot's storage broker, a PS5 payload (not part of eboot.bin: built on its own with the payload
   SDK, scripts/ps5/build-plat-probe.sh, and shipped in the title's folder as hookshot-broker.elf). The title sends it to
   the console's ELF loader on 127.0.0.1:9021 when no broker of its version answers on 127.0.0.1:9147 (host/posix/
   ps5_broker_client.c); it then serves the title's reads and writes under the data folder (/data/grapnel since lot 80, and
   /data/hookshot, the folder before, which the title uses in place when it could not rename it), which the console does
   not throttle for a payload and which a payload sees whatever the title's sandbox has. The protocol, and why: host/posix/
   ps5_broker.h.

   Adapted from Forever Launcher's write broker: bizkut/ps5-forever ps5/writebroker/writebroker.c (GPL-3.0-or-later,
   Copyright (C) 2026 bizkut and the Forever Launcher contributors), revision 0b6abf0bb924954a58ae937ab23f4397a0c44c5f
   (release 01.000.024). Kept from it: the token written to a file only the title and the broker read, its adoption when
   the file changed under a running broker, the replacement of a running broker of another version (asked to exit
   through its own hello and a QUIT), a thread and a 1 MiB buffer per connection, the bounded no-follow permission repair
   of the data folder before the token is published, and the klog prefix. Changed: the roots (/data/grapnel/ and
   /data/hookshot/), the port (9147), the requests (reads, creation, folders, stat, removal besides writes: a title may
   not see /data at all), the token copies in the title folders, and no menu, icon or Wine-specific roots.

   This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
   version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
   implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
   details. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <ps5/klog.h>

#include "ps5_broker.h"

#define CHUNK (1u << 20)
#define REPAIR_MAX_ENTRIES 20000u
#define REPAIR_MAX_DEPTH 32u

static unsigned char token[HB_TOKEN_BYTES];
static unsigned long long served_requests, served_bytes;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    klog_printf("hookshot-broker: %s\n", line);
}

/* all of size bytes, or -1 with errno (EPIPE at a closed connection) */
static int transfer(int fd, void *buffer, size_t size, int sending) {
    unsigned char *bytes = buffer;
    size_t done = 0;
    while (done < size) {
        const ssize_t count = sending ? send(fd, bytes + done, size - done, MSG_NOSIGNAL) : recv(fd, bytes + done, size - done, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            if (!count) errno = EPIPE;
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}
static int reply(int client, int32_t result) {
    struct hb_reply r = {.magic = HB_HELLO_MAGIC, .result = result};
    return transfer(client, &r, sizeof r, 1);
}
static void count(uint64_t bytes) {
    pthread_mutex_lock(&stats_lock);
    const unsigned long long n = ++served_requests;
    served_bytes += bytes;
    const unsigned long long b = served_bytes;
    pthread_mutex_unlock(&stats_lock);
    if (!(n % 4096)) say("served %llu requests, %llu MiB", n, b >> 20);
}

/* ---- the data folder's modes, before the token is published: bounded, never following a link, never leaving /data's
   filesystem (writebroker.c's repair, its roots Hookshot's: the folder itself, user/ and probe/, of each data folder that
   is there; romfs/ is left as ps5upload made it, 186,583 files) ---- */
struct repair {
    unsigned scanned, changed, skipped, failed;
    int result;
    dev_t device;
};
static void repair_note(struct repair *r, int error) {
    r->failed++;
    if (!r->result) r->result = error;
}
static int repair_mode(int fd, const struct stat *st, struct repair *r) {
    const mode_t wanted = S_ISDIR(st->st_mode) || (st->st_mode & 0111) ? 0777 : 0666;
    if ((st->st_mode & 07777) == wanted) return 0;
    if (!fchmod(fd, wanted)) { r->changed++; return 0; }
    repair_note(r, -errno);
    return -errno;
}
static int repair_scan(int dir_fd, unsigned depth, struct repair *r);
static int repair_entry(int parent, const char *name, unsigned depth, struct repair *r) {
    struct stat before, opened;
    if (r->scanned >= REPAIR_MAX_ENTRIES || depth > REPAIR_MAX_DEPTH) { repair_note(r, -E2BIG); return -E2BIG; }
    r->scanned++;
    if (fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW)) { repair_note(r, -errno); return -errno; }
    if (before.st_dev != r->device || S_ISLNK(before.st_mode) || (!S_ISDIR(before.st_mode) && !S_ISREG(before.st_mode)) ||
        (S_ISREG(before.st_mode) && before.st_nlink > 1)) {
        r->skipped++;
        return 0;
    }
    const mode_t wanted = S_ISDIR(before.st_mode) || (before.st_mode & 0111) ? 0777 : 0666;
    if (S_ISREG(before.st_mode) && (before.st_mode & 07777) == wanted) return 0;
    const int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (S_ISDIR(before.st_mode) ? O_DIRECTORY : 0));
    if (fd < 0) { repair_note(r, -errno); return -errno; }
    int result = 0;
    if (fstat(fd, &opened)) result = -errno, repair_note(r, result);
    else if (opened.st_dev != before.st_dev || opened.st_ino != before.st_ino) result = -ESTALE, repair_note(r, result);
    else {
        result = repair_mode(fd, &opened, r);
        if (S_ISDIR(opened.st_mode)) {
            const int scanned = repair_scan(fd, depth, r);
            if (!result) result = scanned;
        }
    }
    close(fd);
    return result;
}
static int repair_scan(int dir_fd, unsigned depth, struct repair *r) {
    const int scan = dup(dir_fd);
    if (scan < 0) { repair_note(r, -errno); return -errno; }
    DIR *d = fdopendir(scan);
    if (!d) { close(scan); repair_note(r, -errno); return -errno; }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { if (errno) result = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (repair_entry(dir_fd, e->d_name, depth + 1, r) == -E2BIG) { result = -E2BIG; break; }
    }
    closedir(d);
    return result;
}
static void repair_permissions(void) {
    struct repair r = {0};
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    const int data = open(PS5_DATA_PARENT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (data >= 0 && !fstat(data, &st)) {
        r.device = st.st_dev;
        static const struct { const char *name; int recursive; } roots[] = {
            {PS5_DATA_NAME, 0}, {PS5_DATA_NAME "/user", 1}, {PS5_DATA_NAME "/probe", 1},
            {PS5_DATA_OLD_NAME, 0}, {PS5_DATA_OLD_NAME "/user", 1}, {PS5_DATA_OLD_NAME "/probe", 1}};
        for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
            const int fd = openat(data, roots[i].name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) { r.skipped++; continue; }
            struct stat rs;
            if (!fstat(fd, &rs) && rs.st_dev == r.device) {
                r.scanned++;
                repair_mode(fd, &rs, &r);
                if (roots[i].recursive) repair_scan(fd, 0, &r);
            }
            close(fd);
        }
    }
    if (data >= 0) close(data);
    gettimeofday(&t1, NULL);
    say("permission repair scanned=%u changed=%u skipped=%u failed=%u ms=%lld result=%d", r.scanned, r.changed, r.skipped,
        r.failed, (long long)((t1.tv_sec - t0.tv_sec) * 1000LL + (t1.tv_usec - t0.tv_usec) / 1000), r.result);
}

/* ---- the token: the credential is the file (whoever reads it is trusted). A broker whose folder was replaced adopts the
   file's current token when a client offers exactly that (writebroker.c's token_accepts), the file of either data folder ---- */
static const char *const data_dirs[] = HB_ROOT_DIRS;
#define DATA_DIRS (sizeof data_dirs / sizeof data_dirs[0])
static int folder_there(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}
static int write_token_at(const char *folder) {
    char name[256], temporary[272];
    snprintf(name, sizeof name, "%s/%s", folder, HB_TOKEN_NAME);
    snprintf(temporary, sizeof temporary, "%s.new", name);
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const int ok = write(fd, token, sizeof token) == (ssize_t)sizeof token;
    fchmod(fd, 0644);
    close(fd);
    return ok && !rename(temporary, name) ? 0 : -1;
}
/* (lot 80) the token in each data folder that is there, and in /data/grapnel made only when neither is: a /data/grapnel made
   beside a /data/hookshot the title uses in place (its rename failed) would be taken for the data folder at the title's next
   start, an empty one, its saves left in the old */
static int write_token(void) {
    const int fd = open("/dev/urandom", O_RDONLY);
    const int ok = fd >= 0 && read(fd, token, sizeof token) == (ssize_t)sizeof token;
    if (fd >= 0) close(fd);
    if (!ok) return -1;
    int written = 0;
    for (size_t i = 0; i < DATA_DIRS; i++) {
        if (!folder_there(data_dirs[i])) continue;
        chmod(data_dirs[i], 0777);
        if (write_token_at(data_dirs[i])) return -1;
        written++;
    }
    if (!written) {
        if (mkdir(data_dirs[0], 0777) && errno != EEXIST) return -1;
        chmod(data_dirs[0], 0777);
        if (write_token_at(data_dirs[0])) return -1;
    }
    static const char *const folders[] = HB_TITLE_FOLDERS;
    for (size_t i = 0; i < sizeof folders / sizeof folders[0]; i++) {
        struct stat st;
        if (!stat(folders[i], &st) && S_ISDIR(st.st_mode) && write_token_at(folders[i]))
            say("token copy refused in %s errno=%d", folders[i], errno);
    }
    return 0;
}
/* (lot 80) the tokens a running broker may know, read before this one writes its own: those of the data folders and the
   title folders. A broker of version 1 whose folder the takeover renamed (/data/hookshot to /data/grapnel) finds no file
   to adopt (it looks in /data/hookshot): only the token it had, now in /data/grapnel, makes it answer and exit */
#define KNOWN_MAX 8
static unsigned char known[KNOWN_MAX][HB_TOKEN_BYTES];
static size_t known_n;
static void know_token_at(const char *folder) {
    char name[256];
    unsigned char t[HB_TOKEN_BYTES];
    snprintf(name, sizeof name, "%s/%s", folder, HB_TOKEN_NAME);
    const int fd = open(name, O_RDONLY);
    if (fd < 0) return;
    const int ok = read(fd, t, sizeof t) == (ssize_t)sizeof t;
    close(fd);
    if (!ok) return;
    for (size_t i = 0; i < known_n; i++)
        if (!memcmp(known[i], t, sizeof t)) return;
    if (known_n < KNOWN_MAX) memcpy(known[known_n++], t, sizeof t);
}
static void know_tokens(void) {
    static const char *const folders[] = HB_TITLE_FOLDERS;
    for (size_t i = 0; i < DATA_DIRS; i++) know_token_at(data_dirs[i]);
    for (size_t i = 0; i < sizeof folders / sizeof folders[0]; i++) know_token_at(folders[i]);
}
static int token_accepts(const unsigned char offered[HB_TOKEN_BYTES]) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    unsigned char current[HB_TOKEN_BYTES];
    pthread_mutex_lock(&lock);
    int ok = !memcmp(offered, token, sizeof token);
    for (size_t i = 0; !ok && i < DATA_DIRS; i++) {
        char name[256];
        snprintf(name, sizeof name, "%s/%s", data_dirs[i], HB_TOKEN_NAME);
        const int fd = open(name, O_RDONLY);
        if (fd >= 0) {
            ok = read(fd, current, sizeof current) == (ssize_t)sizeof current && !memcmp(offered, current, sizeof current);
            close(fd);
            if (ok) {
                memcpy(token, current, sizeof token);
                say("adopted the current token file of %s", data_dirs[i]);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

/* ---- the requests ---- */
static int take_path(int client, const struct hb_request *q, char path[HB_MAX_PATH + 1]) {
    if (!q->path_length || q->path_length > HB_MAX_PATH) return -1;
    if (transfer(client, path, q->path_length, 0)) return -1;
    path[q->path_length] = 0;
    if (memchr(path, 0, q->path_length)) return -1;
    return 0;
}
static int serve_write(int client, const struct hb_request *q, unsigned char *buffer) {
    char path[HB_MAX_PATH + 1];
    if (q->length > HB_MAX_IO || take_path(client, q, path)) return -1;
    int fd = -1, result = hb_path_root(path) ? 0 : -EACCES;
    struct stat st;
    if (!result && (fd = open(path, O_WRONLY | O_NOFOLLOW)) < 0) result = -errno;
    if (!result && (fstat(fd, &st) || !S_ISREG(st.st_mode))) result = -EINVAL;
    uint32_t remaining = q->length;
    uint64_t offset = q->offset;
    while (remaining) {
        const uint32_t chunk = remaining < CHUNK ? remaining : CHUNK;
        if (transfer(client, buffer, chunk, 0)) { if (fd >= 0) close(fd); return -1; }
        for (uint32_t put = 0; !result && put < chunk;) {
            const ssize_t n = pwrite(fd, buffer + put, chunk - put, (off_t)(offset + put));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { result = n ? -errno : -EIO; break; }
            put += (uint32_t)n;
        }
        offset += chunk, remaining -= chunk;
    }
    if (fd >= 0) close(fd);
    if (result) say("write refused %d: %s", result, path);
    else count(q->length);
    return reply(client, result ? result : (int32_t)q->length);
}
static int serve_read(int client, const struct hb_request *q, unsigned char *buffer) {
    char path[HB_MAX_PATH + 1];
    if (take_path(client, q, path)) return -1;
    const uint32_t want = q->length < CHUNK ? q->length : CHUNK;   /* (a read moves at most the buffer: the client loops) */
    int fd = -1, result = hb_path_root(path) ? 0 : -EACCES;
    if (!result && (fd = open(path, O_RDONLY | O_NOFOLLOW)) < 0) result = -errno;
    uint32_t got = 0;
    while (!result && got < want) {
        const ssize_t n = pread(fd, buffer + got, want - got, (off_t)(q->offset + got));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { result = -errno; break; }
        if (!n) break;
        got += (uint32_t)n;
    }
    if (fd >= 0) close(fd);
    if (result) return reply(client, result);
    count(got);
    if (reply(client, (int32_t)got)) return -1;
    return got ? transfer(client, buffer, got, 1) : 0;
}
static int serve_path(int client, const struct hb_request *q) {
    char path[HB_MAX_PATH + 1];
    if (q->length || take_path(client, q, path)) return -1;
    if (!hb_path_root(path)) return reply(client, -EACCES);
    struct stat st;
    int result = 0;
    switch (q->magic) {
    case HB_CREATE: {
        const int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | (q->reserved & 1 ? O_TRUNC : 0), 0666);
        if (fd < 0) result = -errno;
        else {
            if (fstat(fd, &st) || !S_ISREG(st.st_mode)) result = -EINVAL;
            else fchmod(fd, 0666);   /* (FTP's too: files-and-io.md) */
            close(fd);
        }
        break;
    }
    case HB_MKDIR:
        if (mkdir(path, 0777)) result = errno == EEXIST && !stat(path, &st) && S_ISDIR(st.st_mode) ? 0 : -errno;
        if (!result) chmod(path, 0777);
        break;
    case HB_REMOVE:
        if (lstat(path, &st)) result = -errno;
        else if (!S_ISREG(st.st_mode)) result = -EISDIR;
        else if (unlink(path)) result = -errno;
        break;
    case HB_STAT: {
        struct hb_stat s = {0};
        if (stat(path, &st)) result = -errno;
        else s.size = (uint64_t)st.st_size, s.mode = (uint64_t)st.st_mode;
        return reply(client, result) || transfer(client, &s, sizeof s, 1);
    }
    case HB_STATFS: {
        struct hb_stat s = {0};
        struct statfs f;
        if (statfs(path, &f)) result = -errno;
        else s.bsize = f.f_bsize, s.blocks = f.f_blocks, s.bavail = f.f_bavail > 0 ? (uint64_t)f.f_bavail : 0;
        return reply(client, result) || transfer(client, &s, sizeof s, 1);
    }
    }
    if (result) say("request %#x refused %d: %s", q->magic, result, path);
    return reply(client, result);
}

static void *serve(void *arg) {
    const int client = (int)(intptr_t)arg;
    struct hb_hello hello;
    struct hb_request q;
    int32_t version_result = 0;
    unsigned char *const buffer = malloc(CHUNK);
    if (!buffer || transfer(client, &hello, sizeof hello, 0) || hello.magic != HB_HELLO_MAGIC || !hello.version ||
        !token_accepts(hello.token)) {
        say("connection refused (hello)");
        goto done;
    }
    if (hello.version != HB_VERSION) version_result = -EPROTONOSUPPORT;   /* (another version may only ask it to exit) */
    if (reply(client, version_result)) goto done;
    while (!transfer(client, &q, sizeof q, 0)) {
        if (q.magic == HB_QUIT) {
            say("replaced by a new broker; exiting");
            exit(0);
        }
        if (version_result) break;
        int r;
        switch (q.magic) {
        case HB_WRITE: r = serve_write(client, &q, buffer); break;
        case HB_READ: r = serve_read(client, &q, buffer); break;
        case HB_CREATE: case HB_MKDIR: case HB_REMOVE: case HB_STAT: case HB_STATFS: r = serve_path(client, &q); break;
        default: r = -1;
        }
        if (r) break;
    }
done:
    free(buffer);
    close(client);
    return NULL;
}

/* ask a running broker to exit: it trusts whoever reads its token file, which this broker has just rewritten (and which a
   running broker adopts), or the token it had (lot 80: know_tokens, read before); an older one answers only its own
   version's hello (another version's may only ask it to exit), so step down until one answers. A hello it refuses closes
   the connection: the next token or version is tried on a new one */
static void replace_running(void) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(HB_PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct hb_hello hello = {.magic = HB_HELLO_MAGIC};
    struct hb_request quit = {.magic = HB_QUIT};
    struct hb_reply r;
    for (size_t k = 0; k <= known_n; k++) {   /* (this broker's token first, then those known before it wrote it) */
        memcpy(hello.token, k ? known[k - 1] : token, sizeof hello.token);
        for (hello.version = HB_VERSION; hello.version; hello.version--) {
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return;
            if (connect(fd, (struct sockaddr *)&address, sizeof address)) {   /* (nothing listens any more) */
                close(fd);
                return;
            }
            if (!transfer(fd, &hello, sizeof hello, 1) && !transfer(fd, &r, sizeof r, 0)) {
                transfer(fd, &quit, sizeof quit, 1);
                say("asked the running broker to exit (version %u hello, %s token, reply %d)", hello.version,
                    k ? "an earlier" : "this broker's", r.result);
                close(fd);
                return;
            }
            close(fd);
        }
    }
    say("the running broker answered none of %zu tokens", known_n + 1);
}

int main(void) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(HB_PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    signal(SIGPIPE, SIG_IGN);   /* (the ELF loader's socket is closed by the title once the ELF is sent) */
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1, prepared = 0;
    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one)) {
        say("not started: socket errno=%d", errno);
        return 1;
    }
    /* (an older copy holds the port: ask it to exit, then retry for 2 s. This broker's token first: the folder may be new) */
    for (int attempt = 0; bind(listener, (struct sockaddr *)&address, sizeof address); attempt++) {
        if (errno != EADDRINUSE || attempt == 10) {
            say("not started: port %d errno=%d", HB_PORT, errno);
            return 1;
        }
        if (!attempt) {
            know_tokens();   /* (before write_token replaces them: the running broker's may be among them) */
            repair_permissions();
            if (write_token()) {
                say("not started: cannot write the token %s errno=%d", HB_TOKEN_NAME, errno);
                return 1;
            }
            prepared = 1;
            replace_running();
        }
        usleep(200000);
    }
    if (!prepared) {
        repair_permissions();
        if (write_token()) {
            say("not started: cannot write the token %s errno=%d", HB_TOKEN_NAME, errno);
            return 1;
        }
    }
    if (listen(listener, 64)) {
        say("not started: listen errno=%d", errno);
        return 1;
    }
    say("listening on 127.0.0.1:%d for %s and %s version=%u uid=%u euid=%u", HB_PORT, HB_ROOT, HB_ROOT_OLD, HB_VERSION,
        (unsigned)getuid(), (unsigned)geteuid());
    for (;;) {
        const int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            say("accept errno=%d", errno);
            break;
        }
        struct timeval timeout = {.tv_sec = 30};   /* (an idle title connection may wait; a stuck reply may not) */
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        pthread_t thread;
        if (pthread_create(&thread, NULL, serve, (void *)(intptr_t)client)) { close(client); continue; }
        pthread_detach(thread);
    }
    close(listener);
    return 1;
}
