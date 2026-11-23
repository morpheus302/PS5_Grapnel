/* (ps5) stdio's writes under the data folder through the storage broker: the renderer's caches (gpu/vk vk_shader.cpp's
   hookshot_programs.bin and its index, vk_draw.cpp's mkw_vk_pipelines.bin: gigabytes on a first run) are written with
   fopen and fwrite, which no hook in win32_posix.c sees, and the console holds a title's own writes to 16 MiB/s past about
   1 GiB (PPSA99147 run 2), while the broker wrote 2 GiB at 151 MiB/s.

   Linked in by the eboot's --wrap=fopen --wrap=fwrite (nx2/toolchain_ps5.py link_title, scripts/ps5/build-plat-probe.sh):
   this object is pulled from libhookshot_host.a only by those wraps, so a link without them is as before. A stream
   fopen opened for writing on a path under the data folder is noted (with its descriptor, device and inode, checked again
   at each write: fclose is the platform's wrap, so a closed stream is not seen go; (fileno)(), not the macro, which
   reads __isthreaded); an fwrite of
   HOOKSHOT_PS5_BROKER_MIN_KB (16) or more to it flushes what stdio holds, has the broker write the bytes at the stream's
   position (the end for an "a" stream) and moves the stream past them, as fwrite would. Smaller writes, other streams,
   and every write when the broker is not running are stdio's own. */
#ifdef __PROSPERO__
#include "ps5.h"
#include "ps5_broker.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

FILE *__real_fopen(const char *path, const char *mode);
size_t __real_fwrite(const void *p, size_t size, size_t n, FILE *f);

#define STREAMS 64
static struct { FILE *f; int fd, append; dev_t dev; ino_t ino; char *path; } streams[STREAMS];
static unsigned next_slot;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static size_t broker_min(void) {
    static size_t min;
    if (!min) {
        const char *e = getenv("HOOKSHOT_PS5_BROKER_MIN_KB");
        min = (size_t)(e && atoi(e) > 0 ? atoi(e) : 16) << 10;
    }
    return min;
}

FILE *__wrap_fopen(const char *path, const char *mode) {
    FILE *const f = __real_fopen(path, mode);
    struct stat st;
    if (!f || !path || !mode || !strpbrk(mode, "wa+") || !hb_path_root(path) || fstat((fileno)(f), &st)) return f;
    if (S_ISREG(st.st_mode) && (st.st_mode & 0666) != 0666) fchmod((fileno)(f), st.st_mode | 0666);   /* (the broker's too) */
    pthread_mutex_lock(&mu);
    unsigned k = 0;
    while (k < STREAMS && streams[k].f && streams[k].f != f) k++;
    if (k == STREAMS) k = next_slot++ % STREAMS;   /* (full: the oldest note goes; its stream is stdio's again) */
    free(streams[k].path);
    streams[k].f = f, streams[k].fd = (fileno)(f), streams[k].append = strchr(mode, 'a') != NULL;
    streams[k].dev = st.st_dev, streams[k].ino = st.st_ino, streams[k].path = strdup(path);
    pthread_mutex_unlock(&mu);
    return f;
}

/* the noted stream's path (copied), if it is still the file it was opened on */
static int noted(FILE *f, char out[HB_MAX_PATH + 1], int *append) {
    int found = 0;
    struct stat st;
    pthread_mutex_lock(&mu);
    for (unsigned k = 0; k < STREAMS && !found; k++) {
        if (streams[k].f != f) continue;
        if ((fileno)(f) == streams[k].fd && !fstat(streams[k].fd, &st) && st.st_dev == streams[k].dev && st.st_ino == streams[k].ino) {
            snprintf(out, HB_MAX_PATH + 1, "%s", streams[k].path);
            *append = streams[k].append, found = 1;
        } else {   /* (closed since, and the address reused: forgotten) */
            free(streams[k].path);
            memset(&streams[k], 0, sizeof streams[k]);
        }
    }
    pthread_mutex_unlock(&mu);
    return found;
}

size_t __wrap_fwrite(const void *p, size_t size, size_t n, FILE *f) {
    const size_t bytes = size * n;
    char path[HB_MAX_PATH + 1];
    int append = 0;
    if (!size || bytes < broker_min() || bytes > 0xffffffffu || !hb_ready() || !noted(f, path, &append))
        return __real_fwrite(p, size, n, f);
    /* (what stdio holds goes first, so the bytes land in order) */
    struct stat st;
    if (fflush(f) || (append && fstat((fileno)(f), &st))) return __real_fwrite(p, size, n, f);
    const off_t at = append ? st.st_size : ftello(f);
    if (at < 0) return __real_fwrite(p, size, n, f);
    const int64_t r = hb_write(path, (uint64_t)at, p, (uint32_t)bytes);
    if (r <= 0) {
        hos_ps5_broker_count(0, (int)r);
        return __real_fwrite(p, size, n, f);
    }
    hos_ps5_broker_count((uint64_t)r, 1);
    fseeko(f, at + (off_t)r, SEEK_SET);   /* (the stream past what the broker wrote, as fwrite leaves it) */
    if ((size_t)r == bytes) return n;
    const size_t rest = __real_fwrite((const char *)p + r, 1, bytes - (size_t)r, f);   /* (the broker's shortfall: stdio's) */
    return ((size_t)r + rest) / size;
}
#endif /* __PROSPERO__ */
