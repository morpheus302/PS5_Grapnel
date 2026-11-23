"""A video decoder's submissions are decoded ahead by a thread of the host's (host/nn_video.c: AHEAD).

A submission was decoded when the game took a picture and found none ready, on the thread that takes it. OoT's movies
have a submission of 0.4-0.9 MB every 32 pictures that takes several pictures' time to decode: the picture went out
late and the catch-up dropped the 1-3 that had come due, all along every movie. Now one thread, started with the
first decoder and kept for all, keeps AHEAD pictures ready and an Acquire only takes the oldest.

The decoder's own code (dec_submit, dec_acquire with its catch-up and its 5 s line, decode_next, the thread and what
holds it back, dec_flush, dec_let_go with the pictures' blocks) is cut from the source and compiled with real threads
and locks (pthreads) and a stand-in for libavcodec that holds three pictures back, takes the time each submission says
and takes its pictures' blocks from the decoder's as queue_frame does (longshot's test of 2026-11-15, adapted to this
branch's decoder). A player like the game's (each frame: submit
until refused, take one picture, give the buffer back) plays in real time, one frame every 60th of a second:

- a movie with a submission of 80 ms every 32 pictures: every picture comes out, in order, none skipped, the last one
  marked, and the thread never has more than half the queue ready;
- the same movie with HOS_VIDEO_AHEAD=0 (no thread, as before): pictures are skipped at each of those submissions,
  so the movie above measures the thread and not the stand-in;
- a game at 30 frames a second: the catch-up still drops about every other picture, from what the thread has ready;
- a Flush while the thread decodes an 80 ms submission: it returns with the decoder empty and the thread out of the
  codec, nothing of the old stream comes out afterwards, and the next stream plays whole;
- the decoder's end while the thread decodes: the thread is out of the decoder when it returns;
- one thread serves every decoder: none is started for the second and the third.

Timing: a frame that comes late on a busy PC is a skipped picture that is not the decoder's doing, so the checks that
count skipped pictures are tried up to three times. With --tsan (clang's ThreadSanitizer, when it links) a data race in
the cut code fails the check.

python -I -B tests/test_video_ahead.py [--cxx clang++] [--tsan]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def through(source, marker, last):
    """from marker to the end of the line holding last (after marker): a typedef with its name"""
    start = source.index(marker)
    end = source.index("\n", source.index(last, start))
    return source[start:end]


HARNESS = r"""
#define AV_INPUT_BUFFER_PADDING_SIZE 64   /* (FFmpeg's; the decoder's submissions are checked against it since the merge) */
#include <stdlib.h>
#include <string.h>
/* (env_on, the switches' rule of gpu/vk/compat/longshot_env.h, as the decoder reads its switches since the merge) */
static int env_on(const char *n) {
    const char *v = getenv(n);
    if (!v) return 0;
    while (*v == ' ') v++;
    return *v && strcmp(v, "0") && strcasecmp(v, "false") && strcasecmp(v, "off") && strcasecmp(v, "no");
}
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct AVCodec { const char *name; } AVCodec;
typedef struct AVCodecContext { int thread_count; } AVCodecContext;
typedef struct AVPacket { uint8_t *data; int size; int64_t pts; } AVPacket;
typedef struct AVFrame AVFrame;
struct SwsContext;
typedef long LONG;
typedef long long LONG64;
typedef unsigned DWORD;
typedef void *HANDLE, *LPVOID;
#define WINAPI
#define INFINITE 0xFFFFFFFFu
/* the locks, the condition variables and the threads the source uses (windows.h's names), on pthreads */
typedef struct { pthread_mutex_t m; } SRWLOCK;
#define SRWLOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
typedef struct { pthread_cond_t c; } CONDITION_VARIABLE;
static void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_mutex_lock(&l->m); }
static int TryAcquireSRWLockExclusive(SRWLOCK *l) { return pthread_mutex_trylock(&l->m) == 0; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_mutex_unlock(&l->m); }
static void InitializeConditionVariable(CONDITION_VARIABLE *v) { pthread_cond_init(&v->c, NULL); }
static void WakeConditionVariable(CONDITION_VARIABLE *v) { pthread_cond_signal(&v->c); }
static void WakeAllConditionVariable(CONDITION_VARIABLE *v) { pthread_cond_broadcast(&v->c); }
static int SleepConditionVariableSRW(CONDITION_VARIABLE *v, SRWLOCK *l, DWORD ms, unsigned long flags) {
    (void)flags;
    if (ms == INFINITE) return pthread_cond_wait(&v->c, &l->m) == 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000, ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    return pthread_cond_timedwait(&v->c, &l->m, &ts) == 0;
}
static int hos_cv_sleep(void *v, void *l, unsigned ms) { return SleepConditionVariableSRW((CONDITION_VARIABLE *)v, (SRWLOCK *)l, ms, 0); }
typedef struct { pthread_t t; DWORD (*fn)(LPVOID); void *arg; } Thread;
static void *thread_entry(void *p) { Thread *t = (Thread *)p; t->fn(t->arg); return NULL; }
static int threads_made;
static HANDLE CreateThread(void *attr, size_t stack, DWORD (*fn)(LPVOID), void *arg, DWORD flags, DWORD *id) {
    (void)attr, (void)stack, (void)flags, (void)id;
    Thread *t = (Thread *)calloc(1, sizeof *t);
    t->fn = fn, t->arg = arg;
    if (pthread_create(&t->t, NULL, thread_entry, t)) { free(t); return NULL; }
    threads_made++;
    return t;
}

typedef struct { uint64_t x[32]; } Ctx;
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(uintptr_t)(a))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
#define HOS_WRITTEN(p, n) ((void)0)
#define AVERROR(e) (-(e))
#define VIDEO_RGBA_ON_DEMAND 0   /* (a PC's: host/nn_video.c's #else) */
#define VIDEO_PS5_DEFAULT 0
static volatile LONG64 submissions;
static LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 x, LONG64 cmp) { (void)x, (void)cmp; return *p; }
static LONG InterlockedIncrement(volatile LONG *p) { return __sync_add_and_fetch(p, 1); }
static int video_log(void) { return 0; }
static void hos_log(const char *f, ...) { (void)f; }
static int64_t hos_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static void hos_sleep_ns(int64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&ts, NULL);
}
static void *hos_guest_range(uint64_t a, uint64_t n) { (void)n; return GP(a); }
static int hos_vcore_away(void) { return 0; }
static void hos_vcore_back(void) {}
/* the decoder's submission-ready event: signalled and cleared under the decoder's lock, by the game's thread and by
   the decoder's */
static int event_state;
static void hos_system_event_signal(uint64_t key) { (void)key; event_state = 1; }
static void hos_system_event_clear(uint64_t key) { (void)key; event_state = 0; }
static uint64_t ids;
static uint64_t submitted(void) { submissions++; return ++ids; }

@TYPES@

static Dec dec;
static Pool pool;
static void *host_of(uint64_t obj, uint32_t magic) {
    if (magic == DEC_MAGIC) return obj == 1 ? &dec : NULL;
    return magic == POOL_MAGIC && obj == 2 ? &pool : NULL;
}
static void pool_rgba(Pool *o) { (void)o; }
static void mf_av1_decode(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user) {
    (void)m, (void)data, (void)n, (void)pts, (void)fn, (void)user;
}
static void mf_av1_flush(void *m) { (void)m; }
static void picture_out(Dec *d, Picture p);
static void aside_lock(Dec *d);
static void aside_unlock(Dec *d);
static int spare_get(VSpares *s, VBlock *b, size_t n, int zero);
/* libavcodec's stand-in: a decoder that holds DELAY pictures back, as dav1d's frame threads do, and takes for each
   submission the time its first four bytes say (microseconds). One thread at a time is in it: the decoder's, or the
   game's with no thread. calls_at_once: more than one was */
#define DELAY 3
static AVCodecContext context;
static int64_t held_pts[DELAY + 1], out_pts[DELAY + 1];
static int nheld, out_pts_n;
static volatile int in_codec, calls_at_once;
static AVPacket *av_packet_alloc(void) { return (AVPacket *)calloc(1, sizeof(AVPacket)); }
static int av_new_packet(AVPacket *p, int n) { p->data = (uint8_t *)malloc((size_t)n), p->size = n; return p->data ? 0 : -1; }
static void av_packet_free(AVPacket **p) { if (*p) free((*p)->data), free(*p); *p = NULL; }
static int avcodec_send_packet(AVCodecContext *x, AVPacket *p) {
    (void)x;
    if (__sync_add_and_fetch(&in_codec, 1) != 1) calls_at_once = 1;
    if (!p) {
        for (int i = 0; i < nheld; i++) out_pts[out_pts_n++] = held_pts[i];
        nheld = 0;
    } else {
        uint32_t us = 0;
        memcpy(&us, p->data, sizeof us);
        hos_sleep_ns((int64_t)us * 1000);
        held_pts[nheld++] = p->pts;
        if (nheld > DELAY) {
            out_pts[out_pts_n++] = held_pts[0];
            memmove(held_pts, held_pts + 1, sizeof held_pts[0] * (size_t)--nheld);
        }
    }
    __sync_sub_and_fetch(&in_codec, 1);
    return 0;
}
static void avcodec_flush_buffers(AVCodecContext *x) {
    (void)x;
    if (__sync_add_and_fetch(&in_codec, 1) != 1) calls_at_once = 1;
    nheld = out_pts_n = 0;
    __sync_sub_and_fetch(&in_codec, 1);
}
static void mf_picture(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts) {}
static void drain(Dec *d) {   /* (as queue_frame: a block from the decoder's under its lock, the picture out through picture_out) */
    for (int i = 0; i < out_pts_n; i++) {
        Picture p;
        memset(&p, 0, sizeof p);
        p.w = p.h = 4, p.pts = out_pts[i];
        aside_lock(d);
        if (spare_get(&d->spares, &p.mem, 4 * 4 * 3 / 2, 1)) p.nv12 = p.mem.p;
        aside_unlock(d);
        picture_out(d, p);
    }
    out_pts_n = 0;
}
static void decode_next(Dec *d, int ahead);
static void dec_acquire_picture(Ctx *c);
static int dec_lock(Dec *d);
static void dec_unlock(Dec *d, int away);
static void movie_report(Dec *d);

@CODE@

static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
#define FRAME_NS 16666667
static uint64_t submit(int64_t pts, int last, uint32_t cost_us) {
    static uint8_t data[16];
    uint64_t id = 0;
    Ctx c;
    memset(&c, 0, sizeof c);
    memcpy(data, &cost_us, sizeof cost_us);
    c.x[0] = (uint64_t)(uintptr_t)&id, c.x[1] = 1, c.x[2] = (uint64_t)(uintptr_t)data, c.x[3] = sizeof data;
    c.x[4] = (uint64_t)pts, c.x[5] = (uint64_t)last;
    dec_submit(&c);
    return c.x[0];
}
static uint32_t slot;
static uint64_t acquire(void) {
    static uint64_t sync;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)&slot, c.x[1] = (uint64_t)(uintptr_t)&sync, c.x[2] = 1;
    dec_acquire(&c);
    return c.x[0];
}
static void release(uint32_t i) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = 1, c.x[1] = i;
    dec_release(&c);
}
static void flush(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = 1;
    dec_flush(&c, "Flush");
}
/* a decoder as dec_init_sized leaves it (the thread's, when HOS_VIDEO_AHEAD is not 0), and its end as dec_finalize's:
   the thread out of it first */
static void fresh(void) {
    memset(&dec, 0, sizeof dec);
    pthread_mutex_init(&dec.lock.m, NULL);
    dec.ctx = &context, dec.slots[0] = 2, dec.ready = 1, event_state = 1, dec.codec_name = "stand-in";
    if (!pool.nv12) pool.w = pool.h = 4, pool.count = 1, pool.nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    nheld = out_pts_n = 0;
    ahead_start(&dec);
}
static void end(void) {
    ahead_stop(&dec);
    const int away = dec_lock(&dec);
    dec_let_go(&dec, "a decoder's end", 1);
    dec_unlock(&dec, away);
}
/* the game's player, in real time: each frame (frame_ns) it submits until it is refused (or the movie is all in),
   takes one picture and gives the buffer back. Every 32nd submission from the 16th takes spike_us to decode, the others
   300 us. The movie's pts start at base */
static int taken, out_of_order, old_stream, last_marked, marked_early;
static int64_t last_pts;
static void play(int pictures, int64_t frame_ns, uint32_t spike_us, int64_t base) {
    int next = 0;
    taken = out_of_order = old_stream = last_marked = marked_early = 0, last_pts = -1;
    const int64_t t0 = hos_now_ns();
    for (int frame = 0; frame < pictures * 4 + 400; frame++) {
        while (next < pictures) {
            const uint64_t r = submit(base + (int64_t)next * FRAME_NS, next == pictures - 1, next % 32 == 16 ? spike_us : 300);
            if (r == RESULT_INPUT_FULL) break;
            if (r) { check(0, "a submission failed with another result"); return; }
            next++;
        }
        const int64_t handed = dec.acquires;
        if (acquire() != 0) { check(0, "an Acquire failed"); return; }
        AcquireSRWLockExclusive(&dec.lock);
        const int64_t pts = dec.last_pts;
        const int end = dec.eos && !dec.qn, last = dec.slot_last[slot], got = dec.acquires != handed;
        ReleaseSRWLockExclusive(&dec.lock);
        if (got && pts != last_pts) {
            if (pts < base) old_stream++;
            if (pts < last_pts) out_of_order++;
            taken++, last_pts = pts;
            if (last && pts != base + (int64_t)(pictures - 1) * FRAME_NS) marked_early++;
            if (last && pts == base + (int64_t)(pictures - 1) * FRAME_NS) last_marked = 1;
        }
        release(slot);
        if (end && last_marked) break;
        const int64_t due = t0 + (int64_t)(frame + 1) * frame_ns, now = hos_now_ns();
        if (due > now) hos_sleep_ns(due - now);
    }
}
int main(int argc, char **argv) {
    const int ahead = ahead_n();
    if (argc > 1 && !strcmp(argv[1], "off")) {   /* (HOS_VIDEO_AHEAD=0) */
        check(ahead == 0, "HOS_VIDEO_AHEAD=0 is no thread");
        fresh();
        check(!dec.ahead && ahead_thread == NULL && threads_made == 0, "no thread is started");
        play(96, FRAME_NS, 80000, 0);
        check(taken + dec.skipped == 96 && out_of_order == 0 && last_marked, "no thread: the movie plays to its end, as before");
        printf("skipped %lld\n", (long long)dec.skipped);
        end();
        if (!bad) puts("ok");
        return bad;
    }
    if (argc > 1 && !strcmp(argv[1], "slow")) {   /* (a game at 30 frames a second) */
        fresh();
        play(120, 2 * FRAME_NS, 300, 0);
        check(out_of_order == 0 && last_marked && !marked_early, "30 fps: in order to the last picture, which is marked");
        check(taken + dec.skipped == 120, "30 fps: every picture is taken or skipped");
        printf("skipped %lld\n", (long long)dec.skipped);
        end();
        if (!bad) puts("ok");
        return bad;
    }
    check(ahead == AHEAD && ahead <= MAX_QUEUE / 2, "the thread keeps AHEAD pictures ready, at most half the queue");

    /* a movie with a submission of 80 ms every 32 pictures */
    fresh();
    check(dec.ahead && ahead_thread != NULL && threads_made == 1 && ahead_ndecs == 1, "the first decoder starts the thread and is its own");
    play(128, FRAME_NS, 80000, 0);
    check(taken + dec.skipped == 128, "every picture is taken or skipped");
    check(out_of_order == 0, "the pictures come in order");
    check(last_marked && !marked_early, "the last picture, and only it, is marked");
    check(dec.ahead_most >= 2 && dec.ahead_most <= MAX_QUEUE, "the thread had pictures ready, never more than the queue holds");
    check(dec.qn == 0 && dec.npending == 0 && !dec.ahead_busy, "at the end nothing waits and the thread is out of the codec");
    printf("skipped %lld\n", (long long)dec.skipped);
    end();
    check(!dec.ahead && !dec.ahead_in && ahead_ndecs == 0, "the decoder's end: the thread is out of it");

    /* a Flush while the thread decodes an 80 ms submission; then another stream */
    fresh();
    for (int i = 0; i < 12; i++) submit((int64_t)i * FRAME_NS, 0, i == 2 ? 80000 : 300);
    hos_sleep_ns(15000000);   /* (the thread is in the third by now) */
    const int64_t before = hos_now_ns();
    flush();
    const int64_t took = hos_now_ns() - before;
    AcquireSRWLockExclusive(&dec.lock);
    check(dec.qn == 0 && dec.npending == 0 && !dec.phead, "a Flush leaves the decoder empty");
    check(!dec.ahead_busy && dec.nstaged == 0 && dec.ahead_hold == 0, "... with the thread out of the codec and free to go on");
    check(dec.eos == 0 && dec.spares.n == 0, "... not at its end, and with no block kept, as before");
    ReleaseSRWLockExclusive(&dec.lock);
    check(took < 500000000, "a Flush waits for the submission being decoded, no more");
    hos_sleep_ns(100000000);   /* (nothing of the old stream turns up late) */
    AcquireSRWLockExclusive(&dec.lock);
    check(dec.qn == 0, "nothing of the flushed stream comes into the queue afterwards");
    ReleaseSRWLockExclusive(&dec.lock);
    const int64_t base = 1000 * (int64_t)FRAME_NS;
    play(64, FRAME_NS, 300, base);
    check(old_stream == 0 && out_of_order == 0, "the next stream's pictures alone come out, in order");
    check(taken + dec.skipped == 64 && last_marked, "the next stream plays to its end");

    /* the decoder's end while the thread decodes */
    for (int i = 0; i < 8; i++) submit(base * 2 + (int64_t)i * FRAME_NS, 0, i == 0 ? 80000 : 300);
    hos_sleep_ns(10000000);
    end();
    check(!dec.ahead && !dec.ahead_in && !dec.ahead_busy && ahead_ndecs == 0, "the decoder's end while the thread decodes in it: the thread is out of it");
    check(threads_made == 1, "one thread for every decoder: none started for the second");
    check(!calls_at_once, "one thread at a time is in the codec");
    if (!bad) puts("ok");
    return bad;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_video.c")
    parser.add_argument("--tsan", action="store_true", help="with clang's ThreadSanitizer, when it links")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    defines = "\n".join(re.findall(r"^#define (?:RESULT_NO_OUTPUT|RESULT_INPUT_FULL|INPUT_LIMIT|POOL_MAGIC|DEC_MAGIC|MAX_SLOTS|"
                                   r"MAX_QUEUE|AHEAD|MAX_SPARE|SYNC_IDLE_TAG|SYNC_IDLE_MASK)\b.*$", src, re.M))
    types = "\n".join((defines,
                       through(src, "typedef struct {\n    int64_t since;", "} VStats;"),
                       through(src, "static volatile LONG reads_nv12", ";"),
                       through(src, "typedef struct { uint8_t *p; size_t size; int own; } VBlock;", "} VBlock;"),
                       through(src, "typedef struct { VBlock b[MAX_SPARE];", "} VSpares;"),
                       through(src, "typedef struct {\n    int w, h, format, count;", "} Pool;"),
                       through(src, "typedef struct { uint8_t *rgba, *nv12; VBlock mem;", "} Picture;"),
                       through(src, "typedef struct Pending {", "} Pending;"),
                       through(src, "typedef void (*mf_av1_picture_fn)", "int64_t pts);"),
                       through(src, "typedef struct {\n    const char *name;\n    void *(*open)(int w, int h);", "} Av1Decoder;"),   # (rc.1's AV1 decoders' table)
                       through(src, "typedef struct {\n    AVCodecContext *ctx;", "} Dec;")))
    code = "\n".join((block(src, "static int video_core_away(void) {"),
                      block(src, "static int video_stats(void) {"),
                      block(src, "static int video_blocks(void) {"),
                      block(src, "static int vblock_make(VBlock *b, size_t n, int zero) {"),
                      block(src, "static void vblock_free(VBlock *b) {"),
                      block(src, "static int spare_get(VSpares *s, VBlock *b, size_t n, int zero) {"),
                      block(src, "static void spare_put(VSpares *s, VBlock *b) {"),
                      block(src, "static int spares_release(VSpares *s, size_t *bytes) {"),
                      block(src, "static int input_limit(void) {"),
                      block(src, "static int ahead_n(void) {"),
                      block(src, "static int input_full(int pending, int queued, int limit) {"),
                      block(src, "static void input_room(Dec *d) {"),
                      block(src, "static void picture_free(Dec *d, Picture *p) {"),
                      block(src, "static void pending_free(Dec *d) {"),
                      block(src, "static void dec_let_go(Dec *d, const char *what, int all) {"),
                      block(src, "static void aside_lock(Dec *d) {"),
                      block(src, "static void aside_unlock(Dec *d) {"),
                      block(src, "static void queue_picture(Dec *d, Picture p) {"),
                      block(src, "static void picture_out(Dec *d, Picture p) {"),
                      block(src, "static void decode_pending(Dec *d) {"),
                      block(src, "static void decode_next(Dec *d, int ahead) {"),
                      through(src, "static SRWLOCK ahead_lock", "static HANDLE ahead_thread;"),
                      block(src, "static int ahead_wanted(const Dec *d) {"),
                      block(src, "static void ahead_kick(Dec *d) {"),
                      block(src, "static DWORD WINAPI ahead_main(LPVOID unused) {"),
                      block(src, "static void ahead_start(Dec *d) {"),
                      block(src, "static void ahead_stop(Dec *d) {"),
                      block(src, "static void ahead_hold(Dec *d) {"),
                      block(src, "static void ahead_release(Dec *d) {"),
                      block(src, "static void ahead_wait(Dec *d) {"),
                      block(src, "static void decode_or_wait(Dec *d) {"),
                      block(src, "static void movie_report(Dec *d) {"),
                      block(src, "static void pending_push(Dec *d, AVPacket *pkt) {"),
                      block(src, "static int dec_lock(Dec *d) {"),
                      block(src, "static void dec_unlock(Dec *d, int away) {"),
                      block(src, "static int submit_refused(Dec *d, int last) {"),
                      block(src, "static void dec_submit(Ctx *c) {"),
                      block(src, "static void dec_flush(Ctx *c, const char *what) {"),
                      block(src, "static void dec_acquire_stats(Dec *d, int64_t call_ns) {"),
                      block(src, "static void dec_acquire(Ctx *c) {"),
                      block(src, "static void dec_acquire_picture(Ctx *c) {"),
                      block(src, "static void dec_release(Ctx *c) {")))
    # (the source's own checks: the decoder's end takes the thread out of it before anything is freed, a decoder asks for
    # the thread, and the thread is started in one place)
    finalize = block(src, "static void dec_finalize(Ctx *c) {")
    if "ahead_stop(d)" not in finalize or finalize.index("ahead_stop(d)") > finalize.index("dec_lock(d)"):
        raise SystemExit("dec_finalize takes the decoder's lock and frees before the thread that decodes ahead is out of it")
    if "ahead_start(d)" not in block(src, "static void dec_init_sized(Ctx *c, enum AVCodecID id, const char *what, int w, int h) {"):
        raise SystemExit("dec_init_sized does not give the decoder to the thread that decodes ahead")
    if src.count("CreateThread(NULL, 2u << 20, ahead_main") != 1:
        raise SystemExit("the thread that decodes ahead is started in more than one place, or not at all")
    fixture = HARNESS.replace("@TYPES@", types).replace("@CODE@", code)
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="video-ahead-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture, encoding="utf-8")
        exe = folder / ("fixture.exe" if os.name == "nt" else "fixture")
        base = [compiler, "-std=c++17", "-O1", "-w", "-pthread", str(folder / "fixture.cpp"), "-o", str(exe)]
        tsan = False
        if args.tsan:
            tsan = subprocess.run(base + ["-fsanitize=thread", "-g"], capture_output=True, timeout=180).returncode == 0
            if not tsan:
                print("ThreadSanitizer does not link here: without it")
        if not tsan:
            subprocess.run(base, check=True, timeout=120)
        env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_VIDEO_")}
        env["TSAN_OPTIONS"] = "halt_on_error=0 exitcode=66"
        env["HOS_VIDEO_STATS"] = "1"   # (the 5 s line's counts are written by both threads: with them)

        def run(mode, ahead):
            e = dict(env)
            if ahead is not None:
                e["HOS_VIDEO_AHEAD"] = ahead
            r = subprocess.run([str(exe)] + ([mode] if mode else []), capture_output=True, text=True, timeout=120, env=e)
            skipped = [int(m) for m in re.findall(r"^skipped (\d+)$", r.stdout, re.M)]
            return r, skipped

        # (the thread: no picture skipped; a frame late on a busy PC is one, so up to three tries)
        for attempt in range(3):
            r, skipped = run("", None)
            print(r.stdout, end="")
            if r.returncode or "ThreadSanitizer" in r.stderr:
                print(r.stderr[-3000:])
                raise SystemExit("FAIL: the decoder's thread" + (" (a data race)" if "ThreadSanitizer" in r.stderr else ""))
            if skipped == [0]:
                break
            print("skipped with the thread: %s (try %d)" % (skipped, attempt + 1))
        else:
            raise SystemExit("FAIL: pictures are skipped with the decoder's thread")
        # (no thread: the same movie skips pictures at each long submission, as before)
        r, skipped = run("off", "0")
        print(r.stdout, end="")
        if r.returncode or not skipped or skipped[0] < 3:   # (at least one at each of the three)
            print(r.stderr[-2000:])
            raise SystemExit("FAIL: HOS_VIDEO_AHEAD=0 (expected pictures skipped at the three long submissions)")
        # (a game at 30 fps: about every other picture dropped)
        for attempt in range(3):
            r, skipped = run("slow", None)
            print(r.stdout, end="")
            if r.returncode or "ThreadSanitizer" in r.stderr:
                print(r.stderr[-3000:])
                raise SystemExit("FAIL: 30 fps")
            if skipped and 48 <= skipped[0] <= 72:
                break
            print("skipped at 30 fps: %s (try %d)" % (skipped, attempt + 1))
        else:
            raise SystemExit("FAIL: at 30 fps the catch-up does not drop about every other picture")
    print("PASS" + (" (with ThreadSanitizer)" if tsan else ""))


if __name__ == "__main__":
    main()
