"""A video decoder takes only so many submissions at a time (host/nn_video.c: the input limit, from longshot ae7b1165).

OoT's movie player submits compressed pictures and takes decoded ones on one thread. The console's decoder refuses a
submission it has no room for, and the player then goes on with its pictures and submits again later. Here every
submission was taken, so the player put a whole movie in at once and, while it was busy with that, took a picture only
about every 105 ms (longshot's measure). On the PS5 each submission is also a copy in the host heap: the title screen's
100 s movie, 230.7 MiB, was in the heap within seconds (notes-agents/ecran-titre-memoire.md).

Longshot's own test, adapted to this branch's decoder: the decoder's code (the rule, dec_submit, dec_acquire with its
catch-up and its 5 s line, decode_next, dec_flush, dec_let_go with the pictures' blocks, the ready event, the movie's
line) is cut from the source and compiled with a stand-in for libavcodec that holds seven pictures back (the console's
dav1d holds three) and counts the bytes of the submissions it is given and frees. A player like the game's (each frame:
submit until refused, take one picture, give the buffer back) plays a movie of 600 pictures, a frame every 60th of a
second:

- the rule: full with the limit's count of submissions waiting, decoded or not; never with no limit;
- with the limit, never more than 16 wait (and so never more than 16 submissions' bytes held, plus the stream's end);
  every Acquire from the first gives the next picture, none is skipped, none waits for a picture; the last submission is
  taken though the decoder is full, and the movie ends with its last picture marked;
- full, the stream's last submission is taken all the same;
- the submission-ready event is clear exactly while the decoder is full, and signalled again by the Acquire, by a Flush
  and at the decoder's end, whose event the next decoder gets;
- a refusal returns the result the game expects and changes nothing: no submission id is used up;
- a frame two seconds late: the catch-up skips through what waits and the movie is on its clock again within a second;
- the movie's line when it is flushed ("... N submissions put off while 16 waited") and the 5 s line's "put off";
- HOS_VIDEO_INPUT_LIMIT=0: as before, the whole movie taken at once (its bytes all held), the event never cleared;
- HOS_VIDEO_INPUT_LIMIT=4, under the pictures the stand-in holds back: the first Acquire finds no picture and waits;
- in the source: the refusal before a submission id, the three codecs' events their decoder's, the decoder's end giving
  its event back under its lock.
Then a few sabotages of host/nn_video.c, each of which has to make a case fail.

python -I -B tests/test_video_input_limit.py [--cc cc] [--no-mutations]
python -I -B tests/test_video_input_limit.py --sizes <csv of packet sizes, one "size,..." a line>: plays a movie of those
    sizes with the limit and without it and prints the most submissions and bytes held at once (a measure, not a check)
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "host" / "nn_video.c"


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


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


HARNESS = r"""
#define AV_INPUT_BUFFER_PADDING_SIZE 64   /* (FFmpeg's; the decoder's submissions are checked against it since the merge) */
#include <limits.h>
#include <stdint.h>
static void *hos_guest_range(uint64_t a, uint64_t n) { (void)n; return (void *)(uintptr_t)a; }   /* (the fixture's guest memory is its own) */
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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct AVCodec { const char *name; } AVCodec;
typedef struct AVCodecContext { int thread_count; } AVCodecContext;
typedef struct AVPacket { uint8_t *data; int size; int64_t pts; } AVPacket;
typedef struct AVFrame AVFrame;
struct SwsContext;
typedef long LONG;
typedef long long LONG64;
typedef struct { int locked; } SRWLOCK;
#define SRWLOCK_INIT {0}
/* (the thread that decodes ahead, host/nn_video.c AHEAD: none here, dec.ahead stays 0 and a submission is decoded in
   Acquire; tests/test_video_ahead.py has the thread) */
typedef void *HANDLE;
typedef struct { int unused; } CONDITION_VARIABLE;
static void WakeConditionVariable(CONDITION_VARIABLE *v) { (void)v; }
static void WakeAllConditionVariable(CONDITION_VARIABLE *v) { (void)v; }
static int SleepConditionVariableSRW(CONDITION_VARIABLE *v, SRWLOCK *l, unsigned ms, unsigned long flags) { (void)v, (void)l, (void)ms, (void)flags; return 0; }
static int hos_cv_sleep(void *v, void *l, unsigned ms) { (void)v, (void)l, (void)ms; return 0; }
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
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static int video_log(void) { return 0; }
static char logs[1 << 16];
static size_t logn;
static void hos_log(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    if (logn < sizeof logs - 2) logn += (size_t)vsnprintf(logs + logn, sizeof logs - logn - 1, f, ap);
    if (logn > sizeof logs - 2) logn = sizeof logs - 2;
    logs[logn++] = '\n', logs[logn] = 0;
    va_end(ap);
}
static int64_t clock_ns = 1000000000;
static int64_t hos_now_ns(void) { return clock_ns; }
static void AcquireSRWLockExclusive(SRWLOCK *l) { l->locked = 1; }
static int TryAcquireSRWLockExclusive(SRWLOCK *l) { l->locked = 1; return 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { l->locked = 0; }
static int hos_vcore_away(void) { return 0; }
static void hos_vcore_back(void) {}
static int sleeps;
static void hos_sleep_ns(int64_t ns) { sleeps++, clock_ns += ns; }
/* the system events (host/nn_os_sync.c): made from the system region, never freed */
static int event_state[64], events_made, clears, signals;
static uint64_t hos_sys_alloc(uint64_t size, uint64_t align) { (void)size, (void)align; return (uint64_t)++events_made; }
static void hos_system_event_init(uint64_t key, int autoclear) { (void)autoclear; event_state[key] = 0; }
static void hos_system_event_signal(uint64_t key) { event_state[key] = 1, signals++; }
static void hos_system_event_clear(uint64_t key) { event_state[key] = 0, clears++; }
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
static void mf_picture(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts) {
    (void)user, (void)y, (void)uv, (void)w, (void)h, (void)stride, (void)pts;
}
/* libavcodec's stand-in: a decoder that holds DELAY pictures back, as dav1d's frame threads do; the end lets them out.
   The submissions' bytes it is given (av_new_packet) and frees are counted: what the host heap holds of a movie */
#define DELAY 7
static AVCodecContext context;
static int64_t held_pts[DELAY + 1];
static int nheld, out_pts_n;
static int64_t out_pts[DELAY + 1];
static long long live_bytes, peak_bytes, live_packets, peak_packets;
static AVPacket *av_packet_alloc(void) { return (AVPacket *)calloc(1, sizeof(AVPacket)); }
static int av_new_packet(AVPacket *p, int n) {
    p->data = (uint8_t *)malloc((size_t)n + 64), p->size = n;
    if (!p->data) return -1;
    live_bytes += n, live_packets++;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    if (live_packets > peak_packets) peak_packets = live_packets;
    return 0;
}
static void av_packet_free(AVPacket **p) {
    if (*p && (*p)->data) live_bytes -= (*p)->size, live_packets--;
    if (*p) free((*p)->data), free(*p);
    *p = NULL;
}
static int avcodec_send_packet(AVCodecContext *x, AVPacket *p) {
    (void)x;
    if (!p) {
        for (int i = 0; i < nheld; i++) out_pts[out_pts_n++] = held_pts[i];
        nheld = 0;
        return 0;
    }
    held_pts[nheld++] = p->pts;
    if (nheld > DELAY) {
        out_pts[out_pts_n++] = held_pts[0];
        memmove(held_pts, held_pts + 1, sizeof held_pts[0] * (size_t)--nheld);
    }
    return 0;
}
static void avcodec_flush_buffers(AVCodecContext *x) { (void)x; nheld = out_pts_n = 0; }
static void picture_free(Dec *d, Picture *p);
static void drain(Dec *d) {
    for (int i = 0; i < out_pts_n; i++) {
        if (d->qn == MAX_QUEUE) { picture_free(d, &d->queue[d->qhead]); d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--; }
        Picture *p = &d->queue[(d->qhead + d->qn) % MAX_QUEUE];
        memset(p, 0, sizeof *p);
        p->w = p->h = 4, p->pts = out_pts[i];
        p->mem.p = (uint8_t *)calloc(1, 4 * 4 * 3 / 2), p->mem.size = 4 * 4 * 3 / 2;
        p->nv12 = p->mem.p;
        d->qn++;
    }
    out_pts_n = 0;
}

static void decode_next(Dec *d, int ahead);
static void dec_acquire_picture(Ctx *c);
static int dec_lock(Dec *d);
static void dec_unlock(Dec *d, int away);
static void movie_report(Dec *d);
static void ready_event_give_back(uint64_t event);
@CODE@

static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
#define FRAME_NS 16666667
static uint8_t data[1 << 20];
static int *sizes, nsizes;   /* (--sizes: a movie's packets) */
static uint64_t submit(int64_t pts, int last, uint64_t *id, int n) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)id, c.x[1] = 1, c.x[2] = (uint64_t)(uintptr_t)data, c.x[3] = (uint64_t)(sizes ? sizes[n] : 16);
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
static uint64_t ready(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = 1;
    return ready_event(&c);
}
static void flush(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = 1;
    dec_flush(&c, "Flush");
}
static void fresh(void) {
    size_t bytes;
    pending_free(&dec);
    for (int i = 0; i < dec.qn; i++) picture_free(&dec, &dec.queue[(dec.qhead + i) % MAX_QUEUE]);
    spares_release(&dec.spares, &bytes);
    const uint64_t event = dec.ready;
    memset(&dec, 0, sizeof dec);
    dec.ctx = &context, dec.slots[0] = 2, dec.ready = event, dec.codec_name = "stand-in";
    if (event) hos_system_event_signal(event);
    if (!pool.nv12) pool.w = pool.h = 4, pool.count = 1, pool.nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    nheld = out_pts_n = sleeps = 0;
    live_bytes = peak_bytes = live_packets = peak_packets = 0;
}
/* the game's player: each frame it submits until it is refused (or the movie is all in), takes one picture and gives
   the buffer back. late_at: the frame that comes two seconds late (-1: none) */
static int most_waiting, wrong_event, taken, out_of_order, starved_frames, late_recovered;
static int64_t last_pts;
static void play(int pictures, int late_at) {
    const uint64_t event = ready();
    int next = 0;
    most_waiting = wrong_event = taken = out_of_order = starved_frames = 0, late_recovered = -1, last_pts = -1;
    for (int frame = 0; frame < pictures + 400 && !(dec.eos && !dec.qn && taken); frame++) {
        while (next < pictures) {
            uint64_t id = 0;
            const uint64_t before = ids;
            const uint64_t r = submit((int64_t)next * FRAME_NS, next == pictures - 1, &id, next);
            if (r == RESULT_INPUT_FULL) {
                if (ids != before || id) check(0, "a refused submission used a submission id");
                break;
            }
            if (r) { check(0, "a submission failed with another result"); return; }
            next++;
        }
        const int waiting = dec.npending + dec.qn;
        if (waiting > most_waiting) most_waiting = waiting;
        if (event_state[event] != !input_full(dec.npending, dec.qn, input_limit())) wrong_event++;
        const int slept = sleeps;
        const int64_t handed = dec.acquires;
        if (acquire() != 0) { check(0, "an Acquire failed"); return; }
        if (sleeps != slept) starved_frames++;
        if (dec.last_pts != last_pts || (dec.acquires != handed && !dec.eos && dec.last_pts > last_pts)) {
            if (dec.last_pts < last_pts) out_of_order++;
            if (dec.last_pts != last_pts) taken++;
            last_pts = dec.last_pts;
        }
        if (late_at >= 0 && frame > late_at && late_recovered < 0 &&
            (clock_ns - dec.clock_ns) - (dec.last_pts - dec.clock_pts) < 25000000) late_recovered = frame - late_at;
        if (event_state[event] != !input_full(dec.npending, dec.qn, input_limit())) wrong_event++;
        release(slot);
        clock_ns += frame == late_at ? 2000000000 : FRAME_NS;
    }
}
static int load_sizes(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        const int n = atoi(line);
        if (n <= 0 || n > (int)sizeof data) continue;
        if (nsizes == cap) cap = cap ? cap * 2 : 4096, sizes = (int *)realloc(sizes, (size_t)cap * sizeof *sizes);
        sizes[nsizes++] = n;
    }
    fclose(f);
    return nsizes;
}
int main(int argc, char **argv) {
    const int limit = input_limit();
    if (argc > 2 && !strcmp(argv[1], "sizes")) {   /* (a measure: a movie of real packet sizes) */
        if (!load_sizes(argv[2])) { printf("no sizes in %s\n", argv[2]); return 2; }
        long long total = 0, biggest = 0;
        for (int i = 0; i < nsizes; i++) total += sizes[i], biggest = sizes[i] > biggest ? sizes[i] : biggest;
        fresh();
        play(nsizes, -1);
        printf("limit %d: a movie of %d submissions, %.1f MiB (the biggest %lld KiB): at most %lld submissions and %.2f MiB of them "
               "held at once; %lld put off; %d pictures taken, %lld skipped, %d Acquires that waited\n", limit, nsizes,
               total / 1048576.0, biggest >> 10, peak_packets, peak_bytes / 1048576.0, (long long)dec.refused, taken,
               (long long)dec.skipped, starved_frames);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "off")) {
        check(limit == 0, "HOS_VIDEO_INPUT_LIMIT=0 is no limit");
        fresh();
        play(600, -1);
        check(dec.refused == 0 && most_waiting >= 600, "no limit: the whole movie taken at once, as before");
        check(peak_packets == 600, "no limit: every submission's bytes held at once");
        check(clears == 0 && wrong_event == 0, "no limit: the ready event never cleared");
        check(taken == 600 && dec.eos, "no limit: the movie plays to its end");
        if (!bad) puts("ok");
        return bad;
    }
    if (argc > 1 && !strcmp(argv[1], "small")) {
        check(limit == 4, "HOS_VIDEO_INPUT_LIMIT=4");
        fresh();
        play(600, -1);
        check(starved_frames > 0, "a limit under the decoder's delay starves the first Acquire (it waits for a picture)");
        if (!bad) puts("ok");
        return bad;
    }
    if (argc > 1 && !strcmp(argv[1], "stats")) {   /* (HOS_VIDEO_STATS=1: the 5 s line) */
        fresh();
        play(600, -1);
        check(strstr(logs, "submissions waiting, ") && strstr(logs, " put off (the input limit, 16)"), "the 5 s line says what was put off");
        if (!bad) puts("ok");
        return bad;
    }
    check(limit == INPUT_LIMIT && INPUT_LIMIT > DELAY, "the default limit, above the pictures a decoder holds back");
    check(!input_full(INPUT_LIMIT - 1, 0, INPUT_LIMIT) && input_full(INPUT_LIMIT, 0, INPUT_LIMIT), "full at the limit, not under it");
    check(input_full(INPUT_LIMIT - 3, 3, INPUT_LIMIT) && !input_full(INPUT_LIMIT - 4, 3, INPUT_LIMIT), "decoded pictures not taken count too");
    check(!input_full(100000, 8, 0), "no limit: never full");

    fresh();
    play(600, -1);
    /* (the stream's last submission and its end mark are always taken: two over the limit, once) */
    check(dec.refused > 0 && most_waiting <= INPUT_LIMIT + 2, "never more than the limit waiting");
    check(peak_packets <= INPUT_LIMIT + 1, "never more than the limit's submissions' bytes held (and the last)");
    check(taken == 600 && out_of_order == 0 && dec.skipped == 0, "every picture taken, in order, none skipped");
    check(starved_frames == 0, "no Acquire waited for a picture: the decoder is never starved");
    check(dec.eos && dec.slot_last[slot], "the last submission taken though the decoder was full, the end marked");
    check(wrong_event == 0 && clears > 0, "the ready event clear exactly while the decoder is full");
    check(event_state[dec.ready] == 1, "and signalled when the movie has ended");
    {
        char want[200];
        snprintf(want, sizeof want, "video: a movie of 600 pictures on stand-in: ");
        const long long refused = dec.refused;
        logn = 0, logs[0] = 0;
        flush();
        char tail[120];
        snprintf(tail, sizeof tail, "; 0 skipped to keep up with its sound; %lld submissions put off while 16 waited\n", refused);
        check(strstr(logs, want) && strstr(logs, tail), "the movie's line when it is flushed");
        check(strstr(logs, "video: the longest wait between two of its pictures 17 ms") != NULL, "the movie's longest wait");
        logn = 0, logs[0] = 0;
        flush();
        check(!strstr(logs, "video: a movie of"), "a movie said once (its counts start again)");
    }

    /* a Flush while it is full (a movie stopped early): room again, the next submission taken */
    fresh();
    const uint64_t event = ready();
    uint64_t id = 0;
    int n = 0;
    while (n < 1000 && submit((int64_t)n * FRAME_NS, 0, &id, n) == 0) n++;
    check(n == INPUT_LIMIT && event_state[event] == 0 && dec.refused == 1, "full after the limit's count of submissions");
    check(submit((int64_t)n * FRAME_NS, 1, &id, n) == 0 && dec.npending == INPUT_LIMIT + 2 && dec.refused == 1,
          "full, the stream's last submission is taken all the same (with its end)");
    flush();
    check(dec.npending == 0 && event_state[event] == 1 && live_packets == 0, "a Flush empties it, its bytes freed: the event signalled again");
    check(submit(0, 0, &id, 0) == 0 && dec.npending == 1, "and the next movie's first submission is taken");

    /* a frame two seconds late: the catch-up works through what waits, a limit's worth at a time */
    fresh();
    play(600, 100);
    check(dec.skipped > 100 && late_recovered >= 0 && late_recovered <= 60, "after a 2 s frame the movie is on its clock again within a second");
    check(starved_frames == 0 && out_of_order == 0 && dec.eos, "without starving the decoder, to the movie's end");

    /* the decoder's end gives its event back, signalled; the next decoder gets it */
    fresh();
    const uint64_t first = ready();
    for (n = 0; n < 1000 && submit(0, 0, &id, n) == 0; n++) {}
    check(event_state[first] == 0, "full: clear");
    const int made = events_made;
    ready_event_give_back(dec.ready);
    dec.ready = 0;
    check(event_state[first] == 1, "a decoder that ends leaves its event signalled");
    fresh();
    check(ready() == first && events_made == made, "the next decoder takes the same event: none is leaked per movie");
    if (!bad) puts("ok");
    return bad;
}
"""

# (what is changed in host/nn_video.c, into what): each has to make a case fail
MUTATIONS = [
    ("static int input_full(int pending, int queued, int limit) { return limit > 0 && pending + queued >= limit; }",
     "static int input_full(int pending, int queued, int limit) { (void)queued; return limit > 0 && pending >= limit; }"),
    ("    const int full = !last && input_full(d->npending, d->qn, input_limit());",
     "    const int full = input_full(d->npending, d->qn, input_limit()); (void)last;"),
    ("    input_room(d);   /* (longshot ae7b1165: a picture taken, the skipped ones gone: the game may submit again) */\n", ""),
    ("    input_room(d);   /* (longshot ae7b1165: emptied, there is room again) */\n", ""),
    ("        if (d->npending > 0) d->npending--;\n", ""),
    ("    if (!event) return;\n    hos_system_event_signal(event);\n", "    if (!event) return;\n"),
    ("    if (d && (d->ctx || d->mf) && submit_refused(d, (int)(ARG(5) & 1))) { RET(RESULT_INPUT_FULL); return; }\n    const uint64_t id = submitted();",
     "    const uint64_t id = submitted();\n    if (d && (d->ctx || d->mf) && submit_refused(d, (int)(ARG(5) & 1))) { RET(RESULT_INPUT_FULL); return; }"),
]


def fixture(src):
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
                      block(src, "static void queue_picture(Dec *d, Picture p) {"),
                      block(src, "static void decode_pending(Dec *d) {"),
                      block(src, "static void decode_next(Dec *d, int ahead) {"),
                      through(src, "static SRWLOCK ahead_lock", "static HANDLE ahead_thread;"),
                      block(src, "static int ahead_wanted(const Dec *d) {"),
                      block(src, "static void ahead_kick(Dec *d) {"),
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
                      block(src, "static void dec_release(Ctx *c) {"),
                      through(src, "static SRWLOCK ready_lock", "static int nready_spare;"),
                      block(src, "static void ready_event_give_back(uint64_t event) {"),
                      block(src, "static uint64_t ready_event_new(void) {"),
                      block(src, "static uint64_t ready_event(Ctx *c) {")))
    return HARNESS.replace("@TYPES@", types).replace("@CODE@", code)


def check_source(src):
    submit = block(src, "static void dec_submit(Ctx *c) {")
    need(submit.index("RESULT_INPUT_FULL") < submit.index("submitted()"), "dec_submit uses a submission id before it refuses")
    need(src.count("{ RET(ready_event(c)); }") == 3 and "ready_event()" not in src, "a codec's submission-ready event is not its decoder's")
    end = block(src, "static void dec_finalize(Ctx *c) {")
    need(end.index("dec_lock(d)") < end.index("movie_report(d);") < end.index("dec_let_go(d, \"a decoder's end\", 1);") <
         end.index("ready_event_give_back(d->ready);") < end.index("dec_unlock(d, away);"),
         "the decoder's end does not report, let go and give its event back under its lock")
    flush = block(src, "static void dec_flush(Ctx *c, const char *what) {")
    need(flush.index("movie_report(d);") < flush.index("dec_let_go(d, what, 1);") < flush.index("input_room(d);"),
         "a Flush or Stop does not report the movie before it lets go, or does not make room after")
    need("dec_lock(d)" in block(src, "static int submit_refused(Dec *d, int last) {") and "dec_lock(d)" in block(src, "static uint64_t ready_event(Ctx *c) {"),
         "the refusal or the ready event takes the decoder's lock without giving the virtual core up (dec_lock)")
    need(block(src, "static void dec_acquire_picture(Ctx *c) {").count("input_room(d);") == 3, "an Acquire's way out does not follow the rule with the event")
    need('getenv("HOS_VIDEO_INPUT_LIMIT")' in block(src, "static int input_limit(void) {"), "HOS_VIDEO_INPUT_LIMIT is not read at first use")


def build(folder, src, cc, name):
    d = Path(folder) / name
    d.mkdir()
    (d / "fixture.c").write_text(fixture(src), encoding="utf-8")
    exe = d / "fixture"
    subprocess.run([cc, "-std=gnu11", "-O1", "-w", str(d / "fixture.c"), "-o", str(exe)], check=True, timeout=120)
    return exe


def run(exe, mode, value, extra=()):
    env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_VIDEO_")}
    if value is not None:
        env["HOS_VIDEO_INPUT_LIMIT"] = value
    if mode == "stats":
        env["HOS_VIDEO_STATS"] = "1"
    return subprocess.run([str(exe)] + ([mode] if mode else []) + list(extra), capture_output=True, text=True, timeout=60, env=env)


MODES = (("", None), ("off", "0"), ("small", "4"), ("stats", None))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    p.add_argument("--no-mutations", action="store_true")
    p.add_argument("--sizes", type=Path)
    a = p.parse_args()
    src = SOURCE.read_text(encoding="utf-8")
    check_source(src)
    with tempfile.TemporaryDirectory(prefix="video-input-limit-") as temporary:
        exe = build(temporary, src, a.cc, "fixture")
        if a.sizes:
            for value in (None, "0"):
                r = run(exe, "sizes", value, [str(a.sizes)])
                print(r.stdout, end="")
            return
        for mode, value in MODES:
            r = run(exe, mode, value)
            print(r.stdout, end="")
            need(r.returncode == 0 and "ok" in r.stdout, "the video decoder's input limit (%s)" % (mode or "default"))
        if a.no_mutations:
            return
        for i, (old, new) in enumerate(MUTATIONS):
            need(src.count(old) == 1, "a sabotage's text is not once in host/nn_video.c: " + old.strip()[:80])
            mexe = build(temporary, src.replace(old, new), a.cc, "mutation-%d" % i)
            caught = any(run(mexe, mode, value).returncode != 0 for mode, value in MODES)
            need(caught, "a sabotage of host/nn_video.c went through: " + old.strip()[:100])
        print("%d sabotages caught" % len(MUTATIONS))


if __name__ == "__main__":
    main()
