/* nn::codec's hardware video decoders (VP8/VP9 here) and their image pools, on FFmpeg's libavcodec (the static
   libraries in Eglantine's package cache: work/eg/.cache/cpm/ffmpeg). MKW's movie library (subsdk0, movie::) plays
   its WebM movies through them: the cup openings, the courses' billboards, the emblems.

   How the library uses them (subsdk0, movie::detail::VideoDecoderBuffer): each picture buffer is an image pool of
   one image, sized by MakeDecodeImagePreference + CalculateDecoderImagePoolBuffer, registered with the decoder by
   index (RegisterXDecoderBuffer). It submits each compressed frame (SubmitXDecoderInputBuffer), takes finished
   pictures by buffer index (AcquireXDecoderOutputBuffer, GetXDecoderOutputBufferProperties), reads them back to
   its own memory as RGBA or NV12 (ReadDecoderOutputImageRgba / Nv12) and gives the buffer back (Release).

   Here a submission is decoded at once; a finished picture waits in a queue until an Acquire puts it in the first
   free registered buffer (host memory: the pool's guest memory is not used). The video service itself (nn::gsv) is
   not emulated: nn::codec::video::Initialize does nothing. */
#include "hos.h"
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef _WIN32
#include <shellapi.h>
#endif
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#ifndef AV_INPUT_BUFFER_PADDING_SIZE   /* (the console's stand-in FFmpeg headers, host/posix/ps5_noffmpeg: FFmpeg's value) */
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#endif
#ifndef LONGSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/longshot_env.h"
#endif

#define RESULT_NO_OUTPUT 0x2a1ca   /* (a failure: "no picture yet"; the library polls again) */
#define POOL_MAGIC 0x4c4f4f50u     /* 'POOL' */
#define DEC_MAGIC 0x43454456u      /* 'VDEC' */
#define PREF_MAGIC 0x46455250u     /* 'PREF' */
#define MAX_SLOTS 16
#define MAX_QUEUE 16
/* (hookshot, ps5; 2026-11-15) Decoding ahead. A submission was decoded when the game took a picture and found none ready,
   on the thread that takes it. OoT's movies are coded in groups of 32 pictures: half the submissions are a few bytes (a
   picture decoded already) and one of 0.4-0.9 MB comes every 32, which takes several pictures' time to decode (100 ms at
   worst on a 13900K; on the console 54-167 ms, an Acquire up to 251 ms). The picture then went out late, and the catch-up
   dropped the 1-3 that had come due: about 280 of the title movie's 5947 on that PC with the game at 60 fps throughout,
   about 1090 on the console (2026-11-14, its 5 s lines), at the same places of the movie whatever the input limit. Now a
   thread of the host's decodes the waiting submissions until AHEAD pictures are ready (133 ms of movie), with the
   decoder's lock let go while the codec works: an Acquire only takes the oldest (on that PC: 3 skipped, at the movie's
   start). The queue is twice AHEAD: the stream's end brings out at once the pictures the codec held back. A picture is
   3 MiB at 1080p: up to 11 were ready at once on the PC, against 1-3 before. HOS_VIDEO_AHEAD=<n>: that many (at most
   MAX_QUEUE / 2), 0: none, decoded in Acquire as before */
#define AHEAD 8
/* (from longshot ae7b1165, #110) The decoder's input limit. The console's decoder takes only so much compressed data at a
   time: a submission it has no room for fails with RESULT_INPUT_FULL (module 111 is Codec in switchbrew's table of error
   codes; the game takes this description as "not now": it keeps the data, goes on with its pictures and submits again
   later), and its submission-ready event is clear until there is room. Here every submission was taken, so OoT's movie
   player put a whole movie in at once, and the thread that does it is also the one that takes the pictures: while it was
   busy submitting it took a picture only every ~105 ms, and the catch-up dropped the 4-6 that had come due meanwhile
   (longshot's measure on a PC: about 245 pictures of the first movie skipped in its first 5 s).
   (hookshot, ps5) And each submission is a copy in the host heap (av_new_packet), so the title screen's 100 s movie, 230.7
   MiB of AV1, was in the heap within 5 to 11 s, each movie in another of its arenas (notes-agents/ecran-titre-memoire.md:
   +208 to +224 MiB of heap kept at each pass). With the limit the heap holds 16 submissions of a movie at a time.
   Now a submission is refused while INPUT_LIMIT of them wait (not decoded, or decoded and not taken): above the pictures
   the AV1 decoder holds back before one comes out (on the console dav1d's 8 threads, "3 pictures decoded at once" in its
   line; longshot sized it for 8 frame threads), so the next Acquire finds one, and a quarter of a second of movie for the
   catch-up to skip through at a time. The stream's last submission is always taken. HOS_VIDEO_INPUT_LIMIT=<n>: another
   limit, 0: none, as before */
#define RESULT_INPUT_FULL 0x1c266f   /* (module 111, description 3603) */
#define INPUT_LIMIT 16

static int video_log(void) {   /* HOS_VIDEO_LOG=1: submissions and acquisitions */
    static int on = -1;
    if (on < 0) on = env_on("HOS_VIDEO_LOG");
    return on;
}

/* (hookshot, ps5; 2026-11-10, the console's seventh run: the opening movie, 1080p at 60 pictures a second, jerky and
   behind its sound, the game at 20 to 30 frames a second while it played)
   - VIDEO_RGBA_ON_DEMAND: a planar picture is kept as NV12 alone, as Media Foundation's are, and its RGBA made by the pool
     when a reader asks (pool_rgba). OoT reads the NV12 only; the RGBA of every picture was 6 of the 7 ms a picture cost
     the thread that takes it (measured on the PC, the console's flags; about twice that on its CPU), more than a 60 per
     second movie leaves, and the pictures skipped to catch up paid it too. The console only: elsewhere as upstream.
   - video_core_away: AcquireXDecoderOutputBuffer gives its virtual core up for the call (the decoder's wait for a picture,
     the copies), as every blocking call does, and a submission or a flush that finds the decoder's lock held gives its own
     up while it waits. The hardware's call blocks without holding a core; here the caller held one through all of it, and
     the threads allowed on that core alone waited (the per-core job workers: "vcores:" waits of 100 ms and more).
     HOS_VIDEO_CORE_AWAY=0 or 1; the default is 1 on the console, 0 elsewhere (as upstream).
   - video_stats: a "video:" line every 5 s while pictures are taken, saying where the time went. HOS_VIDEO_STATS=0 or 1;
     the default is 1 on the console, elsewhere HOS_VIDEO_LOG's. */
#ifdef __PROSPERO__
#define VIDEO_RGBA_ON_DEMAND 1
#define VIDEO_PS5_DEFAULT 1
#else
#define VIDEO_RGBA_ON_DEMAND 0
#define VIDEO_PS5_DEFAULT 0
#endif
static int video_core_away(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HOS_VIDEO_CORE_AWAY");
        on = e ? atoi(e) != 0 : VIDEO_PS5_DEFAULT;
    }
    return on;
}
static int video_stats(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HOS_VIDEO_STATS");
        on = e ? atoi(e) != 0 : VIDEO_PS5_DEFAULT || video_log();
    }
    return on;
}
typedef struct {
    int64_t since;
    int acquires, empty, packets, pictures, skipped, dropped, lock_waits, refused;
    int64_t acquire_ns, acquire_max, decode_ns, decode_max, convert_ns, convert_max, lock_wait_max, behind_ns, behind_max;
} VStats;
static volatile LONG reads_nv12, reads_rgba, rgba_made;   /* (since the start: which picture the game reads) */

/* (hookshot, ps5; 2026-11-10) A movie's large buffers, out of the host heap.
   The count of direct memory by the site that asked for it, ten minutes into play: six 16 MiB blocks of the host heap asked by
   queue_frame, 96 MiB, and two asked by dav1d, long after the title screen's movie, the only one played, had stopped (an
   image pool and the decoder's planes had each asked for one more while it played). Nothing of the movie was allocated any
   more: when it stops the game flushes, stops and finalizes its decoder and finalizes its two image pools (seen on the PC
   build: FlushAv1Decoder, StopAv1Decoder, FinalizeDecoderImagePool twice, FinalizeAv1Decoder), and each of those frees what
   it holds here. But a picture was a malloc of 3 MiB and a free, 60 times a second, beside a game that loads: where the heap
   had no room for one it mapped a 16 MiB block (124 times in the movie's first 20 s, 119 of them given back), other threads'
   small allocations landed in some of them, and a block goes back only when all of it is free.
   - VIDEO_BLOCKS (video_blocks): a picture's NV12 and the decoder's planes (posix/ps5_dav1d.c) are blocks of direct memory of
     their own on the console (hos_ps5_block), given back to it when they are let go; and a decoder keeps the blocks of the
     pictures taken or skipped, up to MAX_SPARE, for its next pictures, until playback stops: a flush or a stop, the stream's
     end once its last picture is out, the decoder's end (dec_let_go). Where a block cannot be made the heap is asked, as
     before. HOS_VIDEO_BLOCKS=0 or 1; the default is 1 on the console and 0 elsewhere (as upstream: a malloc and a free for
     each picture; 1 there keeps and reuses the pictures' buffers, from the heap).
     A block given back is unmapped, where freed heap memory stays readable: the blocks are touched and let go under their
     decoder's lock alone, a decoder's end included (dec_finalize takes it now). An image pool's pictures, which the game's
     readers copy from under no lock of ours, stay the heap's.
   - an image pool's RGBA pictures (8 MiB each at 1080p) are made when a reader first asks for one (VIDEO_RGBA_ON_DEMAND), not
     with the pool: OoT never reads them.
   - "video: ... let go" says what a flush, a stop, a stream's end or a decoder's end gave back, and "video: ... finalized"
     that a decoder or an image pool is gone. */
static int video_blocks(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HOS_VIDEO_BLOCKS");
        on = e ? atoi(e) != 0 : VIDEO_PS5_DEFAULT;
    }
    return on;
}
#ifdef __PROSPERO__
void *hos_ps5_block(size_t size);   /* (posix/ps5.h: direct memory of its own, backed at once, in 64 KiB units) */
void hos_ps5_block_free(void *p, size_t size);
#endif
/* a large buffer: a block of direct memory of its own (own), or the heap's */
typedef struct { uint8_t *p; size_t size; int own; } VBlock;
/* n bytes: zeros when zero is asked, else whatever they hold. 1 when made */
static int vblock_make(VBlock *b, size_t n, int zero) {
    memset(b, 0, sizeof *b);
    if (!n) return 0;
#ifdef __PROSPERO__
    if (video_blocks()) {
        const size_t size = (n + 0xFFFF) & ~(size_t)0xFFFF;
        void *p = hos_ps5_block(size);
        if (p) {
            if (zero) memset(p, 0, n);
            b->p = (uint8_t *)p, b->size = size, b->own = 1;
            return 1;
        }
    }
#endif
    b->p = (uint8_t *)(zero ? calloc(1, n) : malloc(n));
    b->size = b->p ? n : 0;
    return b->p != NULL;
}
static void vblock_free(VBlock *b) {
#ifdef __PROSPERO__
    if (b->own) hos_ps5_block_free(b->p, b->size);
    else
#endif
        free(b->p);
    memset(b, 0, sizeof *b);
}
/* (posix/ps5_dav1d.c: the decoder's planes) */
uint8_t *hos_video_block(size_t n, size_t *size, int *own) {
    VBlock b;
    vblock_make(&b, n, 0);
    *size = b.size, *own = b.own;
    return b.p;
}
void hos_video_block_free(uint8_t *p, size_t size, int own) {
    VBlock b = {p, size, own};
    vblock_free(&b);
}
/* the blocks a decoder keeps for its next pictures, and how many it made and took again */
#define MAX_SPARE 4
typedef struct { VBlock b[MAX_SPARE]; int n; int64_t made, reused; } VSpares;
/* a block of n bytes for a picture: one the decoder kept (of that size, to its 64 KiB), else a new one. 1 when there is one */
static int spare_get(VSpares *s, VBlock *b, size_t n, int zero) {
    for (int i = 0; i < s->n; i++)
        if (s->b[i].size >= n && s->b[i].size - n < 0x10000) {
            *b = s->b[i];
            s->b[i] = s->b[--s->n];
            memset(&s->b[s->n], 0, sizeof s->b[s->n]);
            if (zero) memset(b->p, 0, n);
            s->reused++;
            return 1;
        }
    if (!vblock_make(b, n, zero)) return 0;
    s->made++;
    return 1;
}
/* a picture's block, the picture taken or skipped: kept while the decoder keeps blocks and has room for it, else let go */
static void spare_put(VSpares *s, VBlock *b) {
    if (!b->p) return;
    if (video_blocks() && s->n < MAX_SPARE) {
        s->b[s->n++] = *b;
        memset(b, 0, sizeof *b);
    } else {
        vblock_free(b);
    }
}
/* the blocks kept, let go; how many there were, and their bytes */
static int spares_release(VSpares *s, size_t *bytes) {
    const int n = s->n;
    *bytes = 0;
    for (int i = 0; i < n; i++) {
        *bytes += s->b[i].size;
        vblock_free(&s->b[i]);
    }
    s->n = 0;
    return n;
}

void hos_system_event_init(uint64_t key, int autoclear);   /* nn_os_sync.c */
void hos_system_event_signal(uint64_t key);
void hos_system_event_clear(uint64_t key);

/* an image pool: its pictures, w x h, as RGBA and as NV12 (both kept: the reader asks for either) */
typedef struct {
    int w, h, format, count;
    uint8_t *rgba, *nv12;
    uint64_t mem, mem_size;   /* (hookshot) the pool's guest memory: where the hardware decoder writes its pictures */
    int rgba_stale;           /* (hookshot) its NV12 picture not converted to RGBA yet (pool_rgba, when it is asked for) */
    int read_by_api;          /* (hookshot) read through ReadDecoderOutputImage*: no copy into its memory (dec_acquire) */
    struct SwsContext *sws;   /* (that conversion's) */
} Pool;
/* the preference MakeDecodeImagePreference returns (the caller's buffer is larger: only these fields are ours) */
typedef struct { uint32_t magic; uint16_t w, h; uint32_t format, tiling; } Pref;

/* (rgba NULL: a picture decoded as NV12 is kept as that alone; its pool makes the RGBA if a reader wants it. mem: nv12's
   memory, the decoder's to keep for a next picture) */
typedef struct { uint8_t *rgba, *nv12; VBlock mem; int w, h; int64_t pts; int last; } Picture;
/* a submission waiting to be decoded (pkt NULL: the stream's end) */
typedef struct Pending { AVPacket *pkt; struct Pending *next; } Pending;
#ifdef _WIN32
/* (hookshot, issue 380) once, when Windows has no AV1 decoder: OoT's movies are AV1, and Windows decodes it only with
   Microsoft's free "AV1 Video Extension" (preinstalled on most PCs, not all). Yes opens its Store page */
static DWORD WINAPI av1_notice(LPVOID unused) {
    (void)unused;
    if (MessageBoxA(NULL, "The game's movies need Microsoft's free \"AV1 Video Extension\", which this PC doesn't have, so they "
                          "show black (the sound plays).\n\nOpen its page in the Microsoft Store now? Install it, then start the game again.",
                    "Hookshot", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES)
        ShellExecuteA(NULL, "open", "ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V", NULL, NULL, SW_SHOWNORMAL);
    return 0;
}
#endif
/* (hookshot) host/mf_av1.c: AV1 on Media Foundation, as the static FFmpeg has no AV1 decoder */
typedef void (*mf_av1_picture_fn)(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts);
void *mf_av1_open(int w, int h);
void mf_av1_decode(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user);
void mf_av1_flush(void *m);
void mf_av1_close(void *m);
/* (hookshot) an AV1 decoder of our own, when FFmpeg has none: dav1d (host/dav1d_av1.c, when the build has it, issue 2316), else
   Media Foundation. The same calls for both */
typedef struct {
    const char *name;
    void *(*open)(int w, int h);
    void (*decode)(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user);
    void (*flush)(void *m);
    void (*close)(void *m);
} Av1Decoder;
#ifdef LONGSHOT_DAV1D
void *dav1d_av1_open(int w, int h);
void dav1d_av1_decode(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user);
void dav1d_av1_flush(void *m);
void dav1d_av1_close(void *m);
#endif
static const Av1Decoder av1_decoders[] = {
#ifdef LONGSHOT_DAV1D
    {"dav1d", dav1d_av1_open, dav1d_av1_decode, dav1d_av1_flush, dav1d_av1_close},
#endif
    {"Media Foundation", mf_av1_open, mf_av1_decode, mf_av1_flush, mf_av1_close},
};

typedef struct {
    AVCodecContext *ctx;
    void *mf;                        /* (hookshot) an AV1 decoder of our own instead of ctx (av1: which one) */
    const Av1Decoder *av1;
    const char *codec_name;          /* (ctx's codec, for the log: the console's stand-in AVCodecContext has no codec member) */
    int64_t decode_ns, decode_worst_ns, decoded, skipped;   /* (hookshot) the movie's decoding, logged when it stops */
    int64_t first_pts, end_pts;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws;
    int sws_w, sws_h, sws_fmt;
    uint64_t slots[MAX_SLOTS];       /* registered pools (guest pointers), by buffer index */
    int busy[MAX_SLOTS];             /* acquired, not released */
    int64_t slot_pts[MAX_SLOTS];
    int slot_last[MAX_SLOTS];        /* the stream's last picture (the submission marked last, all decoded) */
    Picture queue[MAX_QUEUE];
    int qhead, qn;
    int eos;                         /* the last submission is in and decoded: an empty queue is the end */
    int stopped;                     /* (hookshot) Stop or Flush since the last submission: no picture is coming (dec_acquire) */
    Pending *phead, *ptail;          /* submissions not decoded yet, oldest first */
    int64_t submits, acquires;
    int clock_set;                   /* (hookshot) the movie's clock: the picture at clock_pts was due at clock_ns */
    int64_t clock_ns, clock_pts;
    int64_t last_pts;                /* the pts of the last picture handed out */
    int64_t log_submits;             /* (HOS_VIDEO_LOG) submissions still to log, after a flush or the end */
    VStats st;                       /* (hookshot, ps5) the 5 s line's counts (video_stats), under the lock */
    VSpares spares;                  /* (hookshot, ps5) the blocks of pictures taken or skipped, for the next ones (the lock) */
    /* (longshot ae7b1165 and 6f4f28ac) the movie, said when it stops (movie_report): decode_ns ... codec_name above (rc.1 has
       them there too) */
    int npending;                   /* submissions not decoded yet (the list's length): the input limit's count */
    int64_t refused;                 /* submissions refused while the decoder was full */
    uint64_t ready;                  /* its submission-ready event (0: not asked for); ready_clear: clear now, the decoder full */
    int ready_clear;
    int64_t out_ns, gap_worst_ns;    /* when the last picture was handed out; the longest time between two, for the report */
    int64_t gaps[2][4];              /* (HOS_VIDEO_LOG) those times, the movie's first 5 s and after: under 20 ms, 20-50, 50-90, over */
    /* (hookshot, ps5) decoding ahead (AHEAD, above). Under `lock`, but ahead_ask and ahead_in (ahead_lock), and `staged`
       and convert_call_ns, which are the decoding thread's own (the one that decodes ahead while ahead_busy) */
    int ahead;                       /* its submissions are decoded by the thread (0: in Acquire, as before) */
    int ahead_ask;                   /* (ahead_lock) there is something for the thread to do in it (ahead_kick) */
    int ahead_in;                    /* (ahead_lock) the thread is in it */
    CONDITION_VARIABLE ahead_done;   /* from the thread: it holds the lock again after a submission (pictures queued, or none) */
    int ahead_busy;                  /* it is decoding a submission with the lock let go: the codec is its own meanwhile */
    int ahead_end;                   /* ... and that submission is the stream's end */
    int ahead_hold;                  /* Flush or Stop in progress: it starts no other submission */
    int ahead_quit;                  /* the decoder ends: it starts no other submission */
    Picture staged[MAX_QUEUE];       /* the pictures of the submission it is decoding, queued once it holds the lock */
    int nstaged;
    int64_t convert_call_ns;         /* queue_frame's time within the submission being decoded (decode_next takes it off) */
    int64_t ahead_waits, ahead_wait_ns, ahead_wait_worst_ns;   /* Acquires that waited for it, for the report */
    int ahead_most;                  /* the most pictures it had ready at once */
    SRWLOCK lock;
} Dec;

/* (hookshot) the host objects of the game's pool and decoder objects, by the guest object's address: kept here, not in
   the guest object (guest memory). Few at a time (a movie's decoder and its pools). */
typedef struct { uint64_t obj; uint32_t magic; void *p; } HostObj;
static HostObj *host_objs;
static size_t nhost_objs, cap_host_objs;
static SRWLOCK host_objs_lock = SRWLOCK_INIT;
static void *host_of(uint64_t obj, uint32_t magic) {
    void *p = NULL;
    if (!obj) return NULL;
    AcquireSRWLockShared(&host_objs_lock);
    for (size_t i = 0; i < nhost_objs; i++)
        if (host_objs[i].obj == obj && host_objs[i].magic == magic) p = host_objs[i].p;
    ReleaseSRWLockShared(&host_objs_lock);
    return p;
}
static void set_host(uint64_t obj, uint32_t magic, void *p) {
    if (!obj) return;
    AcquireSRWLockExclusive(&host_objs_lock);
    size_t i = 0;
    while (i < nhost_objs && host_objs[i].obj != obj) i++;
    if (!p) {
        if (i < nhost_objs) host_objs[i] = host_objs[--nhost_objs];
    } else {
        if (i == nhost_objs) {
            if (nhost_objs == cap_host_objs) {
                cap_host_objs = cap_host_objs ? cap_host_objs * 2 : 16;
                host_objs = (HostObj *)realloc(host_objs, cap_host_objs * sizeof *host_objs);
                if (!host_objs) hos_fatal(NULL, "video: out of memory");
            }
            nhost_objs++;
        }
        host_objs[i] = (HostObj){obj, magic, p};
    }
    ReleaseSRWLockExclusive(&host_objs_lock);
}

/* ---- the service ---- */
HLE(_ZN2nn5codec5video10InitializeEv) {
    static int logged;
    if (!logged++) hos_log("video: nn::codec::video on libavcodec");
    RET(0);
}
HLE(_ZN2nn5codec5video8FinalizeEv) { RET(0); }

/* ---- image pools ---- */
/* MakeDecodeImagePreference(u16 w, u16 h, OutputFormat, TilingMode) -> the preference (returned through x8) */
HLE(_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE) {
    const uint64_t out = c->x[8];
    Pref p = {PREF_MAGIC, (uint16_t)ARG(0), (uint16_t)ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3)};
    if (out) memcpy(GP(out), &p, sizeof p);
    RET(out);
}
/* CalculateDecoderImagePoolBuffer(u64* size, u64* alignment, Preference const&, u32 count) */
HLE(_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj) {
    const Pref *p = (const Pref *)GP(ARG(2));
    const uint64_t one = p->magic == PREF_MAGIC ? (uint64_t)p->w * p->h * 4 : 0x400000;
    if (ARG(0)) G64(ARG(0)) = ((one * (uint32_t)ARG(3)) + 0xFFF) & ~0xFFFull;
    if (ARG(1)) G64(ARG(1)) = 0x1000;
    RET(0);
}
/* InitializeDecoderImagePool(pool, void* memory, size, Preference const&, u32 count) */
HLE(_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj) {
    const Pref *p = (const Pref *)GP(ARG(3));
    Pool *o = (Pool *)calloc(1, sizeof *o);
    if (!o) hos_fatal(NULL, "video: out of memory for an image pool");
    o->w = p->magic == PREF_MAGIC && p->w ? p->w : 1280, o->h = p->magic == PREF_MAGIC && p->h ? p->h : 720;
    o->format = p->magic == PREF_MAGIC ? (int)p->format : 2;
    o->count = (int)ARG(4) > 0 ? (int)ARG(4) : 1;
    /* (hookshot, ps5: the RGBA when a reader first asks for it, pool_rgba) */
    o->rgba = VIDEO_RGBA_ON_DEMAND ? NULL : (uint8_t *)calloc((size_t)o->count, (size_t)o->w * o->h * 4);
    /* (hookshot, issue 342: an allocation that failed went on as a NULL picture, and the movie thread read address 0) */
    o->nv12 = (uint8_t *)calloc((size_t)o->count, (size_t)o->w * o->h * 3 / 2);
    /* (hookshot) video black, not zeros. A buffer the game takes before the decoder's first picture is shown as it is
       (dec_acquire: "nothing decoded, the buffer again", up to ~100 ms into a movie's start), and NV12 zeros are bright
       green (luma 0 with no chroma): one green frame at each scene change that opens a movie. Luma 16, chroma 128 is
       black; the RGBA opaque black */
    if (o->nv12) {
        for (int i = 0; i < o->count; i++) {
            uint8_t *img = o->nv12 + (size_t)i * o->w * o->h * 3 / 2;
            memset(img, 0x10, (size_t)o->w * o->h);
            memset(img + (size_t)o->w * o->h, 0x80, (size_t)o->w * o->h / 2);
        }
    }
    if (o->rgba)
        for (size_t k = 3; k < (size_t)o->count * o->w * o->h * 4; k += 4) o->rgba[k] = 0xff;
    o->mem = ARG(1), o->mem_size = ARG(2);
    if (video_log())
        hos_log("video: image pool %#llx: %dx%d format %d, %d images, memory %#llx+%#llx", (unsigned long long)ARG(0),
                o->w, o->h, o->format, o->count, (unsigned long long)o->mem, (unsigned long long)o->mem_size);
    set_host(ARG(0), POOL_MAGIC, o);
    RET(0);
}
HLE(_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE) {
    Pool *o = (Pool *)host_of(ARG(0), POOL_MAGIC);
    if (o && o->sws) sws_freeContext(o->sws);
    if (o) {
        static volatile LONG told;
        if (video_stats() && InterlockedIncrement(&told) <= 24)
            hos_log("video: an image pool is finalized: %d pictures of %dx%d, %.1f MiB freed (NV12 %.1f, RGBA %.1f%s)", o->count, o->w,
                    o->h, (double)o->count * o->w * o->h * (o->rgba ? 5.5 : 1.5) / 1048576.0,
                    (double)o->count * o->w * o->h * 1.5 / 1048576.0, o->rgba ? (double)o->count * o->w * o->h * 4 / 1048576.0 : 0.0,
                    o->rgba ? "" : ": never asked for");
        free(o->rgba), free(o->nv12), free(o);
    }
    set_host(ARG(0), POOL_MAGIC, NULL);
}
/* (hookshot) the pool's picture as RGBA, from its NV12, if it was not made yet. Each 1080p frame of OoT's movies was
   converted on the movie thread, in plain C, though OoT reads only the NV12: nearly half that thread's time, which put the
   picture behind the sound */
/* (hookshot, ps5) and the pool's RGBA pictures themselves, if they were not made with the pool (VIDEO_RGBA_ON_DEMAND): opaque
   black, as a new pool's are */
static SRWLOCK pool_rgba_lock = SRWLOCK_INIT;
static void pool_rgba(Pool *o) {
    AcquireSRWLockExclusive(&pool_rgba_lock);
    if (VIDEO_RGBA_ON_DEMAND && !o->rgba) {
        uint8_t *rgba = (uint8_t *)calloc((size_t)o->count, (size_t)o->w * o->h * 4);
        if (rgba)
            for (size_t k = 3; k < (size_t)o->count * o->w * o->h * 4; k += 4) rgba[k] = 0xff;
        o->rgba = rgba;
    }
    if (o->rgba_stale && o->rgba && o->nv12) {
        if (!o->sws)
            o->sws = sws_getContext(o->w, o->h, AV_PIX_FMT_NV12, o->w, o->h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        const uint8_t *src[4] = {o->nv12, o->nv12 + (size_t)o->w * o->h, NULL, NULL};
        const int src_stride[4] = {o->w, o->w, 0, 0};
        uint8_t *dst[4] = {o->rgba, NULL, NULL, NULL};
        int dst_stride[4] = {o->w * 4, 0, 0, 0};
        if (o->sws) sws_scale(o->sws, src, src_stride, 0, o->h, dst, dst_stride);
        InterlockedIncrement(&rgba_made);
        o->rgba_stale = 0;
    }
    ReleaseSRWLockExclusive(&pool_rgba_lock);
}
/* ReadDecoderOutputImageRgba(pool, u32 index, u16 x, u16 y, u16 w, u16 h, void* dst, size_t stride, size_t size) */
HLE(_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm) {
    Pool *o = (Pool *)host_of(ARG(0), POOL_MAGIC);
    const uint32_t i = (uint32_t)ARG(1), x = (uint16_t)ARG(2), y = (uint16_t)ARG(3), w = (uint16_t)ARG(4), h = (uint16_t)ARG(5);
    const uint64_t dst = ARG(6), stride = ARG(7) ? ARG(7) : (uint64_t)w * 4;
    {   /* (hookshot, HOS_VIDEO_LOG) the first reads, with where they go */
        static volatile LONG n;
        if (video_log() && InterlockedIncrement(&n) <= 4)
            hos_log("video: ReadDecoderOutputImageRgba(pool %s, index %u, %u,%u %ux%u, dst %#llx stride %llu)",
                    o ? "ok" : "unknown", i, x, y, w, h, (unsigned long long)dst, (unsigned long long)stride);
    }
    if (!o || i >= (uint32_t)o->count || !dst || x >= (uint32_t)o->w) { RET(0); return; }
    if (VIDEO_RGBA_ON_DEMAND && !o->rgba) pool_rgba(o);   /* (ps5: the pool's RGBA pictures are made at their first read) */
    if (!o->rgba) { RET(0); return; }
    InterlockedIncrement(&reads_rgba);
    o->read_by_api = 1;
    if (i == 0 && o->rgba_stale) pool_rgba(o);
    const uint8_t *src = o->rgba + (size_t)i * o->w * o->h * 4;
    const uint32_t cw = x + w <= (uint32_t)o->w ? w : o->w - x;
    for (uint32_t r = 0; r < h && y + r < (uint32_t)o->h; r++) {
        void *row = hos_guest_range(dst + r * stride, (size_t)cw * 4);   /* (hookshot: only into guest memory) */
        if (row) memcpy(row, src + ((size_t)(y + r) * o->w + x) * 4, (size_t)cw * 4);
    }
    HOS_WRITTEN(GP(dst), (size_t)(h ? h - 1 : 0) * stride + (size_t)cw * 4);   /* (the game may give it to the GPU) */
    RET(0);
}
/* ReadDecoderOutputImageNv12(pool, u32 index, u16 x, u16 y, u16 w, u16 h, void* y_dst, size_t y_stride, [stack:]
   size_t y_size, void* uv_dst, size_t uv_stride, size_t uv_size) */
HLE(_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm) {
    Pool *o = (Pool *)host_of(ARG(0), POOL_MAGIC);
    const uint32_t i = (uint32_t)ARG(1), x = (uint16_t)ARG(2), y = (uint16_t)ARG(3), w = (uint16_t)ARG(4), h = (uint16_t)ARG(5);
    const uint64_t ydst = ARG(6), ystride = ARG(7) ? ARG(7) : w;
    const uint64_t uvdst = G64(c->x[31] + 8), uvstride = G64(c->x[31] + 16) ? G64(c->x[31] + 16) : w;
    {   /* (HOS_VIDEO_LOG) each new destination, with the pool and image */
        static uint64_t seen[32];
        static volatile LONG nseen;
        int k;
        for (k = 0; k < nseen && seen[k] != ydst; k++) {}
        if (video_log() && k == nseen && nseen < 32) {
            seen[InterlockedIncrement(&nseen) - 1] = ydst;
            hos_log("video: ReadDecoderOutputImageNv12(pool %#llx %s, index %u, %u,%u %ux%u, y %#llx/%llu, uv %#llx/%llu) "
                    "lr %#llx", (unsigned long long)ARG(0), o ? "ok" : "unknown", i, x, y, w, h, (unsigned long long)ydst,
                    (unsigned long long)ystride, (unsigned long long)uvdst, (unsigned long long)uvstride,
                    (unsigned long long)c->x[30]);
        }
    }
    /* (hookshot) and no NV12 when its allocation failed: o->nv12 + offset was then any host address, copied to the game */
    if (!o || !o->nv12 || i >= (uint32_t)o->count || x >= (uint32_t)o->w) { RET(0); return; }
    InterlockedIncrement(&reads_nv12);
    o->read_by_api = 1;
    const uint8_t *yp = o->nv12 + (size_t)i * o->w * o->h * 3 / 2, *uvp = yp + (size_t)o->w * o->h;
    const uint32_t cw = x + w <= (uint32_t)o->w ? w : o->w - x;
    for (uint32_t r = 0; ydst && r < h && y + r < (uint32_t)o->h; r++) {
        void *row = hos_guest_range(ydst + r * ystride, cw);   /* (hookshot: only into guest memory) */
        if (row) memcpy(row, yp + (size_t)(y + r) * o->w + x, cw);
    }
    for (uint32_t r = 0; uvdst && r < h / 2 && y / 2 + r < (uint32_t)o->h / 2; r++) {
        void *row = hos_guest_range(uvdst + r * uvstride, cw);
        if (row) memcpy(row, uvp + (size_t)(y / 2 + r) * o->w + (x & ~1u), cw);
    }
    if (ydst) HOS_WRITTEN(GP(ydst), (size_t)(h ? h - 1 : 0) * ystride + cw);
    if (uvdst) HOS_WRITTEN(GP(uvdst), (size_t)(h / 2 ? h / 2 - 1 : 0) * uvstride + cw);
    RET(0);
}

/* ---- decoders ---- */
static void ahead_start(Dec *d);
static void dec_init_sized(Ctx *c, enum AVCodecID id, const char *what, int w, int h);
static void dec_init(Ctx *c, enum AVCodecID id, const char *what) { dec_init_sized(c, id, what, 0, 0); }
static void dec_init_sized(Ctx *c, enum AVCodecID id, const char *what, int w, int h) {
    const AVCodec *codec = NULL;
#ifndef _WIN32   /* (linux) FFmpeg's own AV1 decoder needs a hardware context, and there is no Media Foundation to fall
                    back on: dav1d, else libaom, by name */
    if (id == AV_CODEC_ID_AV1) codec = avcodec_find_decoder_by_name("libdav1d");
    if (!codec && id == AV_CODEC_ID_AV1) codec = avcodec_find_decoder_by_name("libaom-av1");
#endif
    if (!codec) codec = avcodec_find_decoder(id);
    Dec *d = (Dec *)calloc(1, sizeof *d);
    InitializeSRWLock(&d->lock);
    if (codec) {
        d->ctx = avcodec_alloc_context3(codec);
        d->codec_name = codec->name;
        d->ctx->thread_count = id == AV_CODEC_ID_AV1 ? 8 : 4;   /* (1080p AV1 in software) */
        if (avcodec_open2(d->ctx, codec, NULL) < 0) avcodec_free_context(&d->ctx);
    }
    for (size_t i = 0; !d->ctx && !d->mf && id == AV_CODEC_ID_AV1 && i < sizeof av1_decoders / sizeof av1_decoders[0]; i++)
        if ((d->mf = av1_decoders[i].open(w, h))) d->av1 = &av1_decoders[i];
    d->codec_name = d->ctx ? codec->name : d->mf ? d->av1->name : NULL;   /* (the movie's report) */
#ifdef _WIN32
    if (!d->ctx && !d->mf && id == AV_CODEC_ID_AV1) {   /* (issue 380: no AV1 decoder in Windows: the movies were black, with sound) */
        static volatile LONG told;
        if (!InterlockedExchange(&told, 1)) {
            hos_log("video: no AV1 decoder: install Microsoft's free AV1 Video Extension for the movies");
            if (!env_on("HOS_HEADLESS") && !env_on("LONGSHOT_NO_AV1_NOTICE"))
                CreateThread(NULL, 0, av1_notice, NULL, 0, NULL);
        }
    }
#endif
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    ahead_start(d);
    set_host(ARG(0), DEC_MAGIC, d);
    static int logged;
    if (logged++ < 4)
        hos_log("video: %s decoder %s%s (%dx%d)", what, d->ctx || d->mf ? "ready on " : "not available in this FFmpeg",
                d->ctx ? d->codec_name : d->mf ? d->av1->name : "", w, h);
    RET(0);
}
/* (longshot ae7b1165) the input limit (above) */
static int input_limit(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("HOS_VIDEO_INPUT_LIMIT");
        n = e && *e ? atoi(e) : INPUT_LIMIT;
        if (n < 0) n = 0;
    }
    return n;
}
/* (hookshot, ps5) the pictures the thread that decodes ahead keeps ready (AHEAD, above); 0: none, and no thread */
static int ahead_n(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("HOS_VIDEO_AHEAD");
        int v = e && *e ? atoi(e) : AHEAD;
        if (v < 0) v = 0;
        if (v > MAX_QUEUE / 2) v = MAX_QUEUE / 2;
        n = v;
    }
    return n;
}
/* the rule: full with `limit` submissions waiting, not decoded (pending) or decoded and not taken (queued); never with
   no limit */
static int input_full(int pending, int queued, int limit) { return limit > 0 && pending + queued >= limit; }
/* the decoder's submission-ready event follows the rule: clear while it is full, signalled once there is room (an
   Acquire took a picture or decoded on, a Flush or Stop emptied it). Holding d->lock */
static void input_room(Dec *d) {
    if (!d->ready) return;
    const int full = input_full(d->npending, d->qn, input_limit());
    if (full && !d->ready_clear) d->ready_clear = 1, hos_system_event_clear(d->ready);
    else if (!full && d->ready_clear) d->ready_clear = 0, hos_system_event_signal(d->ready);
}
static void picture_free(Dec *d, Picture *p) {
    free(p->rgba);
    spare_put(&d->spares, &p->mem);
    memset(p, 0, sizeof *p);
}
static void pending_free(Dec *d) {
    while (d->phead) {
        Pending *n = d->phead;
        d->phead = n->next;
        av_packet_free(&n->pkt);
        free(n);
    }
    d->ptail = NULL;
    d->npending = 0;
}
/* (hookshot, ps5) Playback stops: the submissions not decoded (all), the pictures not taken and the blocks the decoder kept
   are let go, and a line says how many and how much. all 0: the blocks alone (the stream's end, its last picture out) */
static void dec_let_go(Dec *d, const char *what, int all) {
    int packets = 0, pictures = 0;
    size_t packet_bytes = 0, picture_bytes = 0, spare_bytes = 0;
    if (all) {
        for (const Pending *n = d->phead; n; n = n->next)
            if (n->pkt) packets++, packet_bytes += (size_t)n->pkt->size;
        pending_free(d);
        pictures = d->qn;
        for (int i = 0; i < d->qn; i++) {
            Picture *p = &d->queue[(d->qhead + i) % MAX_QUEUE];
            picture_bytes += p->mem.size + (p->rgba ? (size_t)p->w * p->h * 4 : 0);
            picture_free(d, p);
        }
        d->qhead = d->qn = 0;
    }
    const int spares = spares_release(&d->spares, &spare_bytes);
    static volatile LONG told;
    if ((packets || pictures || spares) && video_stats() && InterlockedIncrement(&told) <= 60)
        hos_log("video: %s: %d pictures not taken (%.1f MiB), %d submissions not decoded (%.1f MiB) and %d picture buffers kept (%.1f MiB) "
                "let go; the decoder made %lld picture buffers and took one again %lld times", what, pictures,
                picture_bytes / 1048576.0, packets, packet_bytes / 1048576.0, spares, spare_bytes / 1048576.0,
                (long long)d->spares.made, (long long)d->spares.reused);
}
static int dec_lock(Dec *d);
static void dec_unlock(Dec *d, int away);
static void movie_report(Dec *d);
static void ready_event_give_back(uint64_t event);
static void ahead_stop(Dec *d);
static void dec_finalize(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (!d) return;
    /* (hookshot, ps5: under the decoder's lock, as a flush. An Acquire still copying a picture out when the game finalizes
       its decoder read freed heap memory before; a block given back is not there to read) */
    ahead_stop(d);   /* (the thread that decodes ahead out of it first, and never in it again) */
    const int away = dec_lock(d);
    movie_report(d);
    dec_let_go(d, "a decoder's end", 1);
    ready_event_give_back(d->ready);   /* (longshot ae7b1165: signalled, for the next decoder) */
    d->ready = 0;
    {
        static volatile LONG told;
        if (video_stats() && InterlockedIncrement(&told) <= 24)
            hos_log("video: a decoder is finalized after %lld submissions and %lld buffers taken: closed, with all it held",
                    (long long)d->submits, (long long)d->acquires);
    }
    if (d->sws) sws_freeContext(d->sws);
    avcodec_free_context(&d->ctx);
    if (d->mf) d->av1->close(d->mf);
    d->mf = NULL;
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    set_host(ARG(0), DEC_MAGIC, NULL);
    dec_unlock(d, away);
    free(d);
}
/* (hookshot, ps5) What the decoder's lock guards, from queue_frame: the thread that decodes ahead works with the lock let
   go (ahead_busy, its own to read then) and takes it for those few lines; any other caller holds it already */
static void aside_lock(Dec *d) {
    if (d->ahead_busy) AcquireSRWLockExclusive(&d->lock);
}
static void aside_unlock(Dec *d) {
    if (d->ahead_busy) ReleaseSRWLockExclusive(&d->lock);
}
/* a picture into the queue (the oldest dropped when the reader falls behind). Holding d->lock */
static void queue_picture(Dec *d, Picture p) {
    if (d->qn == MAX_QUEUE) {
        picture_free(d, &d->queue[d->qhead]);
        d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--;
        d->st.dropped++;
    }
    d->queue[(d->qhead + d->qn) % MAX_QUEUE] = p;
    d->qn++;
}
/* a decoded picture out of the codec: into the queue, or (hookshot, ps5) kept aside while the thread that decodes ahead
   works with the lock let go, and queued once it holds the lock again (decode_next) */
static void picture_out(Dec *d, Picture p) {
    if (d->ahead_busy) {
        if (d->nstaged < MAX_QUEUE) {
            d->staged[d->nstaged++] = p;
        } else {
            AcquireSRWLockExclusive(&d->lock);
            picture_free(d, &p);
            d->st.dropped++;
            ReleaseSRWLockExclusive(&d->lock);
        }
        return;
    }
    queue_picture(d, p);
}
/* a decoded frame into the queue as NV12, deferring RGBA to pool_rgba() unless eager conversion is forced */
static void queue_frame(Dec *d, const AVFrame *f) {
    const int w = f->width, h = f->height;
    if (w <= 0 || h <= 0) return;
    const int64_t t0 = hos_now_ns();
    const int nv12 = f->format == AV_PIX_FMT_NV12;   /* (hookshot: Media Foundation's pictures) */
    const int planar = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVA420P;
    const char *rgba_env = getenv("HOS_VIDEO_RGBA");
    const int force_rgba = rgba_env && atoi(rgba_env) != 0;
    /* (kept as NV12, the RGBA made by the pool when a reader asks, pool_rgba: upstream's own since, the console's
       VIDEO_RGBA_ON_DEMAND before. HOS_VIDEO_RGBA=1: each picture's RGBA made here, as before) */
    const int nv12_only = !force_rgba && (nv12 || planar);
    if (!nv12_only && (!d->sws || d->sws_w != w || d->sws_h != h || d->sws_fmt != f->format)) {
        if (d->sws) sws_freeContext(d->sws);
        d->sws = sws_getContext(w, h, (enum AVPixelFormat)f->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        d->sws_w = w, d->sws_h = h, d->sws_fmt = f->format;
    }
    Picture p = {0};
    p.w = w, p.h = h, p.pts = f->pts;
    const size_t nv12_size = (size_t)w * h * 3 / 2;
    p.rgba = nv12_only ? NULL : (uint8_t *)malloc((size_t)w * h * 4);
    /* (every byte is written below when the picture is NV12, or planar with even sides: no need for zeros first.
       hookshot, ps5: from the blocks the decoder kept, spare_get) */
    aside_lock(d);   /* (the blocks the decoder kept are the lock's) */
    if (spare_get(&d->spares, &p.mem, (size_t)w * h * 3 / 2, !(nv12 || (planar && !((w | h) & 1))))) p.nv12 = p.mem.p;
    aside_unlock(d);
    if (!p.nv12 || (!nv12_only && !p.rgba)) {   /* (out of memory: this frame is dropped, not written through NULL; issue 342) */
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 4) hos_log("video: no memory for a %dx%d frame: dropped", w, h);
        free(p.rgba);
        aside_lock(d);
        spare_put(&d->spares, &p.mem);
        aside_unlock(d);
        return;
    }
    uint8_t *dst[4] = {p.rgba, NULL, NULL, NULL};
    int dst_stride[4] = {w * 4, 0, 0, 0};
    if (d->sws && p.rgba) sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dst, dst_stride);
    if (nv12) {   /* (hookshot: Media Foundation's pictures) */
        for (int y = 0; y < h; y++) memcpy(p.nv12 + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
        for (int y = 0; y < h / 2; y++)
            memcpy(p.nv12 + (size_t)w * h + (size_t)y * w, f->data[1] + (size_t)y * f->linesize[1], (size_t)w);
    } else if (planar) {   /* (planar 4:2:0 -> NV12) */
        for (int y = 0; y < h; y++) memcpy(p.nv12 + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
        uint8_t *uv = p.nv12 + (size_t)w * h;
        const uint8_t *u_src = f->data[1], *v_src = f->data[2];
        const int u_stride = f->linesize[1], v_stride = f->linesize[2];
        for (int y = 0; y < h / 2; y++) {
            const uint8_t *u_row = u_src + (size_t)y * u_stride;
            const uint8_t *v_row = v_src + (size_t)y * v_stride;
            uint8_t *uv_row = uv + (size_t)y * w;
            int x = 0;
#if defined(__x86_64__) || defined(_M_X64)
            for (; x + 16 <= w / 2; x += 16) {
                __m128i u16 = _mm_loadu_si128((const __m128i *)(u_row + x));
                __m128i v16 = _mm_loadu_si128((const __m128i *)(v_row + x));
                _mm_storeu_si128((__m128i *)(uv_row + 2 * x), _mm_unpacklo_epi8(u16, v16));
                _mm_storeu_si128((__m128i *)(uv_row + 2 * x + 16), _mm_unpackhi_epi8(u16, v16));
            }
#endif
            for (; x < w / 2; x++) {
                uv_row[2 * x] = u_row[x];
                uv_row[2 * x + 1] = v_row[x];
            }
        }
    } else {
        memset(p.nv12, 0, nv12_size);
    }
    const int64_t dt = hos_now_ns() - t0;
    d->convert_call_ns += dt;
    aside_lock(d);
    d->st.pictures++, d->st.convert_ns += dt;
    if (dt > d->st.convert_max) d->st.convert_max = dt;
    aside_unlock(d);
    picture_out(d, p);
}
static void mf_picture(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts) {
    AVFrame f;
    memset(&f, 0, sizeof f);
    f.width = w, f.height = h, f.format = AV_PIX_FMT_NV12, f.pts = pts;
    f.data[0] = (uint8_t *)y, f.data[1] = (uint8_t *)uv;
    f.linesize[0] = f.linesize[1] = stride;
    queue_frame((Dec *)user, &f);
}
static void drain(Dec *d) {
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        queue_frame(d, d->frame);
        av_frame_unref(d->frame);
    }
}
/* the oldest submissions decoded until a picture comes out (or none are left). The stream's end is taken as soon as
   it is next, pictures queued or not: the last picture decoded must carry the last flag, as the hardware's does. Media
   Foundation's decoder holds nothing back, so waiting for an empty queue gave the flag only to a buffer handed out
   again, with an older picture's pts, after the real last one went out unmarked */
static void decode_next(Dec *d, int ahead);
static void decode_pending(Dec *d) {
    while (d->phead && (!d->qn || !d->phead->pkt)) decode_next(d, 0);
}
/* one submission decoded (its pictures into the queue, if any). Holding d->lock. (hookshot, ps5) `ahead`, from the thread
   that decodes ahead: the lock is let go while the codec works, so an Acquire or a Submit goes on meanwhile. What the
   lock guards is read and written before and after (and in queue_frame's few lines), and the pictures come into the
   queue once it is held again */
static void decode_next(Dec *d, int ahead) {
    if (d->phead) {
        const int64_t t0 = hos_now_ns();
        Pending *n = d->phead;
        d->phead = n->next;
        if (!d->phead) d->ptail = NULL;
        if (d->npending > 0) d->npending--;
        const int picture = n->pkt != NULL;
        if (n->pkt) {   /* (the movie's report) */
            if (!d->decoded++) d->first_pts = n->pkt->pts;
            d->end_pts = n->pkt->pts;
            d->eos = 0;
        }
        d->convert_call_ns = 0;
        if (ahead) {
            d->ahead_busy = 1;
            d->ahead_end = !picture;
            ReleaseSRWLockExclusive(&d->lock);
        }
        if (d->mf) {   /* (hookshot) */
            if (n->pkt) d->av1->decode(d->mf, n->pkt->data, (size_t)n->pkt->size, n->pkt->pts, mf_picture, d);
            else d->av1->decode(d->mf, NULL, 0, 0, mf_picture, d);
        } else if (n->pkt) {
            if (avcodec_send_packet(d->ctx, n->pkt) == AVERROR(EAGAIN)) {   /* (full: its pictures out first) */
                drain(d);
                avcodec_send_packet(d->ctx, n->pkt);
            }
            drain(d);
        } else {   /* (the end: the rest out) */
            avcodec_send_packet(d->ctx, NULL);
            drain(d);
            avcodec_flush_buffers(d->ctx);   /* (a stream after it starts again) */
        }
        av_packet_free(&n->pkt);
        if (ahead) {
            AcquireSRWLockExclusive(&d->lock);
            d->ahead_busy = d->ahead_end = 0;
            for (int i = 0; i < d->nstaged; i++) queue_picture(d, d->staged[i]);
            d->nstaged = 0;
            if (d->qn > d->ahead_most) d->ahead_most = d->qn;
        }
        if (!picture) {   /* (the end: the last picture marked) */
            d->eos = 1;
            d->log_submits = 8;   /* (data after the end: a restart, worth seeing) */
            if (d->qn) d->queue[(d->qhead + d->qn - 1) % MAX_QUEUE].last = 1;
        }
        free(n);
        const int64_t dt = hos_now_ns() - t0 - d->convert_call_ns;   /* (the decoder's time: the call's, less queue_frame's) */
        d->st.packets++, d->st.decode_ns += dt;
        if (dt > d->st.decode_max) d->st.decode_max = dt;
        d->decode_ns += dt;
        if (dt > d->decode_worst_ns) d->decode_worst_ns = dt;
    }
}
/* (hookshot, ps5) The thread that decodes ahead (AHEAD, above): one for every decoder, started with the first and never
   ended. A decoder is a movie, and a thread for each would be a stack for each: an ended thread's stack is not given
   back here (win32_posix.c, CreateThread). It goes to a decoder that asked (ahead_kick) and decodes its oldest
   submission there if fewer than ahead_n() pictures are ready, or the stream's end as soon as it is next
   (decode_pending's rule), one submission a visit, each decoder in its turn. Two locks, which it never holds together:
   ahead_lock for who asked and where it is, the decoder's for the rest; a caller holds the decoder's and takes ahead_lock
   inside it */
static SRWLOCK ahead_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE ahead_asked, ahead_left;   /* (zeroed, as InitializeConditionVariable leaves them) */
static Dec *ahead_decs[8];
static int ahead_ndecs, ahead_turn;
static HANDLE ahead_thread;
static void input_room(Dec *d);
static int ahead_wanted(const Dec *d) {
    return !d->ahead_hold && !d->ahead_quit && d->phead && (d->qn < ahead_n() || !d->phead->pkt);
}
/* the thread to this decoder, if there is something for it to do there. Holding d->lock */
static void ahead_kick(Dec *d) {
    if (!d->ahead || !ahead_wanted(d)) return;
    AcquireSRWLockExclusive(&ahead_lock);
    d->ahead_ask = 1;
    WakeConditionVariable(&ahead_asked);
    ReleaseSRWLockExclusive(&ahead_lock);
}
static DWORD WINAPI ahead_main(LPVOID unused) {
    (void)unused;
    AcquireSRWLockExclusive(&ahead_lock);
    for (;;) {
        Dec *d = NULL;
        for (int i = 0; i < ahead_ndecs && !d; i++) {
            Dec *o = ahead_decs[(ahead_turn + i) % ahead_ndecs];
            if (o->ahead_ask) d = o, ahead_turn = (ahead_turn + i + 1) % ahead_ndecs;
        }
        if (!d) {
            SleepConditionVariableSRW(&ahead_asked, &ahead_lock, INFINITE, 0);
            continue;
        }
        d->ahead_ask = 0, d->ahead_in = 1;
        ReleaseSRWLockExclusive(&ahead_lock);
        AcquireSRWLockExclusive(&d->lock);
        if (ahead_wanted(d)) {
            decode_next(d, 1);
            input_room(d);   /* (a submission that brought no picture yet leaves room for another) */
            WakeAllConditionVariable(&d->ahead_done);
        }
        const int more = ahead_wanted(d);
        ReleaseSRWLockExclusive(&d->lock);
        AcquireSRWLockExclusive(&ahead_lock);
        d->ahead_in = 0;
        if (more) d->ahead_ask = 1;   /* (a decoder that ends meanwhile is out of ahead_decs: not asked again) */
        WakeAllConditionVariable(&ahead_left);
    }
    return 0;
}
/* a decoder begins: the thread started if this is the first, and the decoder its own from now (not when the thread
   cannot be had, or with more decoders at once than ahead_decs holds: those decode in Acquire, as before). Its stack: 2
   MiB, once (dav1d's frames are decoded on dav1d's own threads; this one reads the stream and copies the planes) */
static void ahead_start(Dec *d) {
    InitializeConditionVariable(&d->ahead_done);
    if ((!d->ctx && !d->mf) || ahead_n() <= 0) return;
    AcquireSRWLockExclusive(&ahead_lock);
    if (!ahead_thread) {
        ahead_thread = CreateThread(NULL, 2u << 20, ahead_main, NULL, 0, NULL);
        hos_log("video: submissions are decoded ahead by a thread of the host's, %d pictures (HOS_VIDEO_AHEAD)%s", ahead_n(),
                ahead_thread ? "" : ": the thread could not be started, decoded in Acquire as before");
    }
    if (ahead_thread && ahead_ndecs < (int)(sizeof ahead_decs / sizeof ahead_decs[0])) ahead_decs[ahead_ndecs++] = d, d->ahead = 1;
    ReleaseSRWLockExclusive(&ahead_lock);
}
/* a decoder ends: the thread out of it (after the submission it is decoding there, if any), and never in it again */
static void ahead_stop(Dec *d) {
    if (!d->ahead) return;
    AcquireSRWLockExclusive(&d->lock);
    d->ahead_quit = 1;
    ReleaseSRWLockExclusive(&d->lock);
    const int away = hos_vcore_away();
    AcquireSRWLockExclusive(&ahead_lock);
    for (int i = 0; i < ahead_ndecs; i++)
        if (ahead_decs[i] == d) {
            ahead_decs[i] = ahead_decs[--ahead_ndecs];
            break;
        }
    while (d->ahead_in) SleepConditionVariableSRW(&ahead_left, &ahead_lock, 50, 0);
    ReleaseSRWLockExclusive(&ahead_lock);
    if (away) hos_vcore_back();
    AcquireSRWLockExclusive(&d->lock);
    d->ahead = 0;   /* (from here a submission left is decoded in Acquire, as with no thread) */
    ReleaseSRWLockExclusive(&d->lock);
}
/* Flush and Stop: the thread out of the codec, and kept out until ahead_release. Holding d->lock (let go while it
   finishes the submission it is decoding) */
static void ahead_hold(Dec *d) {
    if (!d->ahead) return;
    d->ahead_hold++;
    while (d->ahead_busy) hos_cv_sleep(&d->ahead_done, &d->lock, 50);
}
static void ahead_release(Dec *d) {
    if (!d->ahead) return;
    if (d->ahead_hold > 0) d->ahead_hold--;
    ahead_kick(d);
}
/* An Acquire's decode_pending with the thread: the same pictures, waited for instead of decoded here. The stream's end,
   when it is next, before a picture goes out (the last one must carry the flag); with nothing ready, the picture the
   thread is decoding or is about to, however long that takes, as the decoding here did: giving up would hand the game
   a buffer's old picture. Holding d->lock (let go while waiting) */
static void ahead_wait(Dec *d) {
    int64_t t0 = 0;
    while (!d->ahead_hold && !d->ahead_quit &&
           (d->ahead_end || (d->phead && !d->phead->pkt) || (!d->qn && (d->phead || d->ahead_busy)))) {
        if (!t0) t0 = hos_now_ns();
        ahead_kick(d);
        SleepConditionVariableSRW(&d->ahead_done, &d->lock, 20, 0);
    }
    if (t0) {
        const int64_t ns = hos_now_ns() - t0;
        d->ahead_waits++, d->ahead_wait_ns += ns;
        if (ns > d->ahead_wait_worst_ns) d->ahead_wait_worst_ns = ns;
    }
}
static void decode_or_wait(Dec *d) {
    if (d->ahead) ahead_wait(d);
    else decode_pending(d);
}
/* (longshot, issue 2316 in 6f4f28ac, and ae7b1165) one line when a movie stops (a Flush or Stop, the decoder's end): its
   decoder, the time it took for each submission against the time between the movie's pictures, the pictures the catch-up
   skipped and the submissions refused while the decoder was full; a second line the longest wait between two of its
   pictures as the game took them, and with HOS_VIDEO_LOG=1 a third those waits by length. Holding d->lock */
static void movie_report(Dec *d) {
    if (d->decoded > 1) {
        const double each = (double)d->decode_ns / d->decoded / 1e6,
                     every = (double)(d->end_pts - d->first_pts) / (d->decoded - 1) / 1e6;   /* (pts in ns) */
        hos_log("video: a movie of %lld pictures on %s: %.1f ms to decode each (worst %.1f), one shown every %.1f ms%s; "
                "%lld skipped to keep up with its sound; %lld submissions put off while %d waited", (long long)d->decoded,
                d->codec_name ? d->codec_name : "no decoder", each, (double)d->decode_worst_ns / 1e6,
                every, each > every ? " (slower than it plays)" : "", (long long)d->skipped, (long long)d->refused,
                input_limit());
        /* (the pictures' pace as the game took them: a movie that shows ten pictures a second while the frames are on time
           (the player busy elsewhere) has long waits here and nothing in the frame times) */
        hos_log("video: the longest wait between two of its pictures %.0f ms", (double)d->gap_worst_ns / 1e6);
        if (d->ahead)   /* (hookshot, ps5) */
            hos_log("video: decoded ahead by the host's thread, up to %d pictures ready (%d asked); %lld acquires waited "
                    "for it, %.1f ms each (the longest %.1f)", d->ahead_most, ahead_n(), (long long)d->ahead_waits,
                    d->ahead_waits ? (double)d->ahead_wait_ns / d->ahead_waits / 1e6 : 0.0, (double)d->ahead_wait_worst_ns / 1e6);
        if (video_log())
            hos_log("video: time between pictures, the first 5 s: under 20 ms %lld, 20-50 %lld, 50-90 %lld, over %lld; after: "
                    "%lld, %lld, %lld, %lld; at the end the picture was %.3f s behind the movie's clock",
                    (long long)d->gaps[0][0], (long long)d->gaps[0][1], (long long)d->gaps[0][2], (long long)d->gaps[0][3],
                    (long long)d->gaps[1][0], (long long)d->gaps[1][1], (long long)d->gaps[1][2], (long long)d->gaps[1][3],
                    d->clock_set && d->out_ns ? (double)((d->out_ns - d->clock_ns) - (d->last_pts - d->clock_pts)) / 1e9 : 0.0);
    }
    d->out_ns = d->gap_worst_ns = 0;
    memset(d->gaps, 0, sizeof d->gaps);
    d->decode_ns = d->decode_worst_ns = d->decoded = d->skipped = d->refused = 0;
    d->ahead_waits = d->ahead_wait_ns = d->ahead_wait_worst_ns = 0, d->ahead_most = 0;
}
static void pending_push(Dec *d, AVPacket *pkt) {
    Pending *n = (Pending *)calloc(1, sizeof *n);
    n->pkt = pkt;
    d->npending++;
    if (d->ptail) d->ptail->next = n;
    else d->phead = n;
    d->ptail = n;
}
/* SubmitXDecoderInputBuffer(u64* id, decoder, data, size, TimeSpan pts, bool last): kept, decoded when a picture is
   acquired (the library submits a whole movie at once: its pictures decoded then would not fit any queue) */
static volatile LONG64 submissions;

/* (hookshot) A buffer handed out with nothing decoded into it, by a decoder with nothing left to decode, gets a sync
   handle that is not reached until something is submitted again (anywhere: the count is global). Every other buffer's
   is 0, reached. The hardware's is a fence the decoder signals once the picture is in the buffer, so an idle decoder's
   never is; the movie library's VideoDecoder::Flush recycles buffers until it finds one not reached, and with all of
   them reached it never returned: the movie never stopped, and the game sat on a black screen after its intro.
   The library also waits for the handle as a sync point event (VideoDecoderEventManager::SetFence), and the callback
   that event runs aborts unless TryWaitNativeSync says reached: an event for a handle not reached is signalled at the
   next submission, not when it is requested (hos_video_sync_defer, from nn_syncpt.c). */
#define SYNC_IDLE_TAG 0x5ec0000000000000ull
#define SYNC_IDLE_MASK 0x0000ffffffffffffull
int hos_video_sync_reached(uint64_t handle) {   /* (nn_gs.c: TryWaitNativeSync, TimedWaitNativeSync) */
    if ((handle & ~SYNC_IDLE_MASK) != SYNC_IDLE_TAG) return 1;
    return (handle & SYNC_IDLE_MASK) != ((uint64_t)InterlockedCompareExchange64(&submissions, 0, 0) & SYNC_IDLE_MASK);
}

static SRWLOCK waiter_lock = SRWLOCK_INIT;
static uint64_t waiters[32];   /* sync point events (their guest addresses) waiting for the next submission */
static int nwaiters;

/* RequestSyncPointEvent(event, handle): 1 when the event waits for the next submission, 0 when it is due now */
int hos_video_sync_defer(uint64_t event, uint64_t handle) {
    AcquireSRWLockExclusive(&waiter_lock);   /* (checked under the lock submitted() takes: no submission slips between) */
    int deferred = !hos_video_sync_reached(handle);
    if (deferred) {
        int i = 0;
        while (i < nwaiters && waiters[i] != event) i++;
        if (i == nwaiters && nwaiters < (int)(sizeof waiters / sizeof waiters[0])) waiters[nwaiters++] = event;
        else if (i == nwaiters) deferred = 0;   /* (full: signalled now, as before) */
    }
    ReleaseSRWLockExclusive(&waiter_lock);
    return deferred;
}

/* CancelSyncPointEvent(event) */
void hos_video_sync_cancel(uint64_t event) {
    AcquireSRWLockExclusive(&waiter_lock);
    for (int i = 0; i < nwaiters; i++)
        if (waiters[i] == event) { waiters[i] = waiters[--nwaiters]; break; }
    ReleaseSRWLockExclusive(&waiter_lock);
}

int hos_video_sync_waiting(uint64_t event) {   /* TryWaitSyncPointEvent(event): 1 while it waits for a submission */
    AcquireSRWLockExclusive(&waiter_lock);
    int i = 0;
    while (i < nwaiters && waiters[i] != event) i++;
    const int waiting = i < nwaiters;
    ReleaseSRWLockExclusive(&waiter_lock);
    return waiting;
}

void hos_system_event_signal(uint64_t key);
static uint64_t submitted(void) {   /* a submission: every handle given out before it is reached */
    uint64_t due[32];
    AcquireSRWLockExclusive(&waiter_lock);
    const uint64_t id = (uint64_t)InterlockedIncrement64(&submissions);
    const int n = nwaiters;
    memcpy(due, waiters, sizeof due);
    nwaiters = 0;
    ReleaseSRWLockExclusive(&waiter_lock);
    for (int i = 0; i < n; i++) hos_system_event_signal(due[i]);
    return id;
}

/* (hookshot, ps5) the decoder's lock for a submission or a flush: held by an Acquire while it decodes, so a caller that finds
   it taken gives its virtual core up for the wait (video_core_away); dec_unlock takes the core again, the lock let go first */
static int dec_lock(Dec *d) {
    if (TryAcquireSRWLockExclusive(&d->lock)) return 0;
    const int away = video_core_away() ? hos_vcore_away() : 0;
    const int64_t t0 = hos_now_ns();
    AcquireSRWLockExclusive(&d->lock);
    const int64_t dt = hos_now_ns() - t0;
    d->st.lock_waits++;
    if (dt > d->st.lock_wait_max) d->st.lock_wait_max = dt;
    return away;
}
static void dec_unlock(Dec *d, int away) {
    ReleaseSRWLockExclusive(&d->lock);
    if (away) hos_vcore_back();
}
/* (longshot ae7b1165) 1 when the decoder has no room for a submission now (the input limit, above): it is counted, the ready
   event cleared, and nothing else changes: the caller answers RESULT_INPUT_FULL. Never the stream's last submission: the
   end is always taken. It returns at once and waits for nothing but the decoder's lock, so a game that submits from the
   thread that also takes the pictures (OoT's does) goes on to its Acquire, which makes the room */
static int submit_refused(Dec *d, int last) {
    const int away = dec_lock(d);
    const int full = !last && input_full(d->npending, d->qn, input_limit());
    if (full) {
        d->refused++, d->st.refused++;
        input_room(d);
    }
    dec_unlock(d, away);
    return full;
}
static void dec_submit(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(1), DEC_MAGIC);
    if (d && (d->ctx || d->mf) && submit_refused(d, (int)(ARG(5) & 1))) { RET(RESULT_INPUT_FULL); return; }
    const uint64_t id = submitted();
    if (ARG(0)) G64(ARG(0)) = id;
    if (!d || (!d->ctx && !d->mf)) { RET(0); return; }
    const int away = dec_lock(d);
    /* (hookshot) the size is 64-bit: one past what an AVPacket holds was cut to a small packet and copied in whole; and
       the data must lie in guest memory */
    const void *data = ARG(2) && ARG(3) && ARG(3) <= INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE ? hos_guest_range(ARG(2), ARG(3)) : NULL;
    if (ARG(2) && ARG(3) && !data) hos_log("video: submit of %#llx bytes at %#llx left out", (unsigned long long)ARG(3),
                                          (unsigned long long)ARG(2));
    if (data) {
        AVPacket *pkt = av_packet_alloc();
        if (pkt && av_new_packet(pkt, (int)ARG(3)) == 0) {
            memcpy(pkt->data, data, (size_t)ARG(3));
            pkt->pts = (int64_t)ARG(4);
            pending_push(d, pkt);
        } else {
            av_packet_free(&pkt);
        }
    }
    d->submits++;
    d->stopped = 0;   /* (hookshot: pictures are coming again: dec_acquire waits for one as before) */
    if (ARG(5) & 1) pending_push(d, NULL);
    const int logged_one = d->log_submits > 0;
    if (logged_one) d->log_submits--;
    if (video_log() && (d->submits <= 8 || (ARG(5) & 1) || logged_one))
        hos_log("video: submit %lld (%llu bytes, pts %lld%s)", (long long)d->submits, (unsigned long long)ARG(3),
                (long long)ARG(4), (ARG(5) & 1) ? ", last" : "");
    ahead_kick(d);   /* (hookshot, ps5: something to decode ahead) */
    input_room(d);
    dec_unlock(d, away);
    RET(0);
}
static void dec_flush(Ctx *c, const char *what) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (d->ctx || d->mf)) {
        const int away = dec_lock(d);
        ahead_hold(d);   /* (hookshot, ps5: the thread that decodes ahead out of the codec until the end of this) */
        if (video_log())
            hos_log("video: %s after %lld submits and %lld acquires (%d queued, %s, lr %#llx)", what, (long long)d->submits,
                    (long long)d->acquires, d->qn, d->eos ? "at the end" : d->phead ? "submissions left" : "none left",
                    (unsigned long long)c->x[30]);
        d->log_submits = 8;
        movie_report(d);
        if (d->ctx) avcodec_flush_buffers(d->ctx);
        else d->av1->flush(d->mf);
        dec_let_go(d, what, 1);
        d->eos = 0;
        d->clock_set = 0;
        d->stopped = 1;   /* (hookshot: until the next submission, dec_acquire has nothing to wait for) */
        for (int i = 0; i < MAX_SLOTS; i++) d->slot_last[i] = 0;
        input_room(d);   /* (longshot ae7b1165: emptied, there is room again) */
        ahead_release(d);
        dec_unlock(d, away);
    }
    RET(0);
}
/* RegisterXDecoderBuffer(decoder, u32 index, pool, u32 image) / UnregisterXDecoderBuffer(decoder, u32 index) */
static void dec_register(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (uint32_t)ARG(1) < MAX_SLOTS) d->slots[(uint32_t)ARG(1)] = ARG(2), d->busy[(uint32_t)ARG(1)] = 0;
    RET(0);
}
static void dec_unregister(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (uint32_t)ARG(1) < MAX_SLOTS) d->slots[(uint32_t)ARG(1)] = 0;
    RET(0);
}
/* AcquireXDecoderOutputBuffer(u32* index, NativeSyncHandle*, decoder): the oldest picture into a free buffer */
static void dec_acquire_picture(Ctx *c);
/* (hookshot, ps5) the 5 s line: this call's time counted, and the line written when it is due */
static void dec_acquire_stats(Dec *d, int64_t call_ns) {
    char line[700];
    line[0] = 0;
    AcquireSRWLockExclusive(&d->lock);
    VStats *s = &d->st;
    const int64_t now = hos_now_ns();
    if (!s->since) s->since = now - call_ns;
    s->acquires++, s->acquire_ns += call_ns;
    if (call_ns > s->acquire_max) s->acquire_max = call_ns;
    if (now - s->since >= 5000000000LL) {
        int pending = 0;
        for (const Pending *n = d->phead; n; n = n->next) pending++;
        snprintf(line, sizeof line,
                 "video: %.1f s: %d acquires (%d without a picture) of %.1f ms each (the longest %.1f); %d submissions decoded in %.1f ms "
                 "each (the longest %.1f), %d pictures queued in %.1f ms each (the longest %.1f), %d skipped to catch up, %d dropped "
                 "(queue full); the picture given is %.2f s behind the movie's clock (at most %.2f); %d decoded ahead, %d submissions "
                 "waiting, %d put off (the input limit, %d); the decoder's lock waited for %d times (the longest %.1f ms); reads so "
                 "far: %ld NV12, %ld RGBA (%ld RGBA pictures made); the virtual core %s",
                 (now - s->since) / 1e9, s->acquires, s->empty, s->acquires ? s->acquire_ns / 1e6 / s->acquires : 0.0, s->acquire_max / 1e6,
                 s->packets, s->packets ? s->decode_ns / 1e6 / s->packets : 0.0, s->decode_max / 1e6,
                 s->pictures, s->pictures ? s->convert_ns / 1e6 / s->pictures : 0.0, s->convert_max / 1e6, s->skipped, s->dropped,
                 s->behind_ns / 1e9, s->behind_max / 1e9, d->qn, pending, s->refused, input_limit(), s->lock_waits, s->lock_wait_max / 1e6,
                 (long)reads_nv12, (long)reads_rgba, (long)rgba_made, video_core_away() ? "given up for the call" : "kept");
        memset(s, 0, sizeof *s);
        s->since = now;
    }
    ReleaseSRWLockExclusive(&d->lock);
    if (line[0]) hos_log("%s", line);
}
static void dec_acquire(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(2), DEC_MAGIC);
    const int away = video_core_away() ? hos_vcore_away() : 0;   /* (before the decoder's lock: none is held while a core is waited for) */
    const int64_t t0 = hos_now_ns();
    dec_acquire_picture(c);
    if (d && video_stats()) dec_acquire_stats(d, hos_now_ns() - t0);
    if (away) hos_vcore_back();
}
static void dec_acquire_picture(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(2), DEC_MAGIC);
    if (ARG(1)) G64(ARG(1)) = 0;
    if (!d) { RET(RESULT_NO_OUTPUT); return; }
    /* (the library aborts on any failure here: the hardware call waits for a picture. Up to ~100 ms for one decoded
       by another thread's submission; then a free buffer as it was, so the caller is never refused) */
    /* (hookshot, #1929) and no virtual core held through it: the decoding blocks (dav1d waits for its own frame
       threads), and with the core held the movie thread kept it 100-270 ms on Linux while a thread waiting for that core
       ran without one. It is taken again only once d->lock is let go, as in hos_cv_sleep: waiting for a core while
       holding the lock could stall a thread that holds a core and waits for the lock (dec_submit) */
    const int away = hos_vcore_away();
    AcquireSRWLockExclusive(&d->lock);
    decode_or_wait(d);   /* (hookshot, ps5: decoded by the thread that decodes ahead, when the decoder is its own) */
    /* (hookshot) The wait is for a picture another thread's submission is about to bring, ~100 ms of it. It was 50 tries of a
       2 ms sleep, and such a sleep takes 4-5 ms: 245-257 ms, on the thread a movie's end joins (title to game: the game's main
       thread in hos_thread_join for a frame of ~250 ms, every start). Now by the clock, and none after a Stop or a Flush with
       nothing submitted since: no picture is coming then. HOS_VIDEO_ACQUIRE_TRIES=1: 50 tries, as before */
    static int by_tries = -1;
    if (by_tries < 0) by_tries = env_on("HOS_VIDEO_ACQUIRE_TRIES");
    const int64_t wait_until = hos_now_ns() + 100000000;
    for (int tries = 0; !d->qn && !d->eos && (by_tries ? tries < 50 : !d->stopped && hos_now_ns() < wait_until); tries++) {
        ReleaseSRWLockExclusive(&d->lock);
        hos_sleep_ns(2000000);
        AcquireSRWLockExclusive(&d->lock);
        decode_or_wait(d);
    }
    int slot = -1;
    for (int i = 0; i < MAX_SLOTS && slot < 0; i++)
        if (d->slots[i] && !d->busy[i] && host_of(d->slots[i], POOL_MAGIC)) slot = i;
    if (slot < 0) {
        input_room(d);   /* (hookshot, not in ae7b1165: the submissions decoded above made room too) */
        ReleaseSRWLockExclusive(&d->lock);
        if (away) hos_vcore_back();
        RET(RESULT_NO_OUTPUT);
        return;
    }
    d->acquires++;
    if (!d->qn) {   /* (nothing decoded: the buffer's last picture again; after the last submission, as the last) */
        d->st.empty++;
        d->busy[slot] = 1;
        if (d->eos && !d->phead && d->spares.n) dec_let_go(d, "a stream's end", 0);   /* (hookshot, ps5: no picture is coming) */
        if (d->eos) d->slot_last[slot] = 1, d->slot_pts[slot] = d->last_pts;   /* (the end at the last picture's time) */
        const int idle = !d->eos && !d->phead;   /* (nothing will come into this buffer: its sync is not reached) */
        if (idle && ARG(1))
            G64(ARG(1)) = SYNC_IDLE_TAG | ((uint64_t)InterlockedCompareExchange64(&submissions, 0, 0) & SYNC_IDLE_MASK);
        if (video_log()) hos_log("video: acquire %lld: nothing decoded%s, buffer %d again", (long long)d->acquires,
                                 d->eos ? " (the end)" : idle ? " (idle: its sync not reached)" : "", slot);
        else if (d->acquires == 1 && !d->eos)   /* (the start: the frame shows the buffer as it was, black since the pool's fill) */
            hos_log("video: the first buffer went out before the first picture was decoded (shown black)");
        input_room(d);   /* (longshot ae7b1165: the pending ones were decoded on) */
        ReleaseSRWLockExclusive(&d->lock);
        if (away) hos_vcore_back();
        if (ARG(0)) G32(ARG(0)) = (uint32_t)slot;
        RET(0);
        return;
    }
    {   /* (hookshot) The game takes one picture a frame, and the sound plays on: under 60 fps the movie fell behind its
           sound (the intro "slow motion, audio ahead", issues 146 and 179). Pictures behind the movie's clock are skipped:
           the next ones decoded, as the whole movie is submitted at once. HOS_VIDEO_NO_CATCHUP=1: as before */
        static int off = -1;
        if (off < 0) off = env_on("HOS_VIDEO_NO_CATCHUP");
        const int64_t now = hos_now_ns(), late = 25000000;   /* (a frame and a half at 60 fps) */
        Picture *h = &d->queue[d->qhead];
        if (!d->clock_set || h->pts < d->clock_pts) d->clock_set = 1, d->clock_ns = now, d->clock_pts = h->pts;   /* (a start, a loop) */
        int skipped = 0;
        for (int guard = 0; !off && guard < 600 && !d->queue[d->qhead].last; guard++) {
            const int64_t due = d->clock_pts + (now - d->clock_ns);
            if (d->queue[d->qhead].pts + late >= due) break;
            if (d->qn < 2) {
                if (d->ahead) break;   /* (hookshot, ps5: the thread decodes; what is late then goes at the next Acquire) */
                if (!d->phead) break;
                decode_next(d, 0);
                continue;
            }
            picture_free(d, &d->queue[d->qhead]);
            d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--;
            skipped++;
        }
        if (skipped && video_log()) hos_log("video: %d pictures skipped to catch up with the movie's clock", skipped);
        d->st.skipped += skipped, d->skipped += skipped;   /* (once: rc.1 had its own count of the same here, merged in twice) */
        d->st.behind_ns = hos_now_ns() - d->clock_ns - (d->queue[d->qhead].pts - d->clock_pts);   /* (the picture given, below) */
        if (d->st.behind_ns > d->st.behind_max) d->st.behind_max = d->st.behind_ns;
    }
    Picture *p = &d->queue[d->qhead];
    Pool *o = (Pool *)host_of(d->slots[slot], POOL_MAGIC);
    /* (the picture at the pool's size: rows and columns past either are left out) */
    const int ok = o->nv12 != NULL && p->nv12 != NULL;   /* (an allocation that failed: nothing copied; issue 342) */
    const int w = !ok ? 0 : p->w < o->w ? p->w : o->w, h = !ok ? 0 : p->h < o->h ? p->h : o->h;
    for (int y = 0; y < h; y++) {
        if (p->rgba && o->rgba) memcpy(o->rgba + (size_t)y * o->w * 4, p->rgba + (size_t)y * p->w * 4, (size_t)w * 4);
        memcpy(o->nv12 + (size_t)y * o->w, p->nv12 + (size_t)y * p->w, (size_t)w);
    }
    for (int y = 0; y < h / 2; y++)
        memcpy(o->nv12 + (size_t)o->w * o->h + (size_t)y * o->w, p->nv12 + (size_t)p->w * p->h + (size_t)y * p->w,
               (size_t)w);
    o->rgba_stale = !p->rgba;
    /* (hookshot) and into the pool's guest memory, as the hardware decoder leaves it (NV12, pitch = width, chroma right
       after the luma rows; RGBA when the memory has room for it and the pool's format is not NV12's). Not once the
       game reads the pool through ReadDecoderOutputImageNv12 / Rgba, as OoT's movie library does: its pools are
       format 3, so each 1080p picture was made RGBA and copied there for nothing (the movie is the same without it) */
    if (o->mem && o->mem_size && !o->read_by_api) {
        const size_t nv12_size = (size_t)o->w * o->h * 3 / 2, rgba_size = (size_t)o->w * o->h * 4;
        const int rgba = o->format != 0 && o->mem_size >= rgba_size;   /* (VideoDecoderOutputFormat 0: NV12) */
        const size_t need = rgba ? rgba_size : nv12_size;
        void *to = hos_guest_range(o->mem, need);   /* (hookshot: only into guest memory) */
        if (o->mem_size >= need && ok && to && rgba && (o->rgba_stale || !o->rgba)) pool_rgba(o);
        if (o->mem_size >= need && ok && to && (rgba ? o->rgba != NULL : 1)) {
            memcpy(to, rgba ? o->rgba : o->nv12, need);
            HOS_WRITTEN(to, need);
        }
    }
    d->slot_pts[slot] = d->last_pts = p->pts;
    d->slot_last[slot] = p->last;
    d->busy[slot] = 1;
    {   /* (longshot ae7b1165) the time since the picture before it, for the movie's report */
        const int64_t now = hos_now_ns(), gap = d->out_ns ? now - d->out_ns : 0;
        if (gap > d->gap_worst_ns) d->gap_worst_ns = gap;
        if (d->out_ns) d->gaps[now - d->clock_ns >= 5000000000ll][gap < 20000000 ? 0 : gap < 50000000 ? 1 : gap < 90000000 ? 2 : 3]++;
        d->out_ns = now;
    }
    {   /* (hookshot) HOS_VIDEO_LOG: the picture's time against this decoder's movie clock, to see it fall behind the sound */
        if (video_log() && d->acquires % 60 == 0) {
            const int64_t now = hos_now_ns();
            hos_log("video: acquire %lld: picture at %.2f s, wall clock %.2f s (behind by %.2f s), %d decoded ahead", (long long)d->acquires,
                    (double)(p->pts - d->clock_pts) / 1e9, (double)(now - d->clock_ns) / 1e9,
                    (double)(now - d->clock_ns - (p->pts - d->clock_pts)) / 1e9, d->qn);
        }
    }
    if (video_log() && (d->acquires <= 8 || p->last || d->acquires % 300 == 0)) {
        uint64_t sum = 0, n = 0;   /* (the picture's mean luma and RGB, sampled: a black or garbage decode shows) */
        uint64_t rgb[3] = {0, 0, 0};
        for (int y = 0; y < p->h; y += 37)
            for (int x = 0; x < p->w; x += 41, n++) {
                sum += p->nv12[(size_t)y * p->w + x];
                for (int k = 0; k < 3 && p->rgba; k++) rgb[k] += p->rgba[((size_t)y * p->w + x) * 4 + k];
            }
        hos_log("video: acquire %lld: %dx%d pts %lld%s into buffer %d (mean Y %llu, RGB %llu %llu %llu)",
                (long long)d->acquires, p->w, p->h, (long long)p->pts, p->last ? " (last)" : "", slot,
                (unsigned long long)(n ? sum / n : 0), (unsigned long long)(n ? rgb[0] / n : 0),
                (unsigned long long)(n ? rgb[1] / n : 0), (unsigned long long)(n ? rgb[2] / n : 0));
    }
    picture_free(d, p);
    d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--;
    ahead_kick(d);   /* (hookshot, ps5: room for another picture decoded ahead) */
    input_room(d);   /* (longshot ae7b1165: a picture taken, the skipped ones gone: the game may submit again) */
    ReleaseSRWLockExclusive(&d->lock);
    if (away) hos_vcore_back();
    if (ARG(0)) G32(ARG(0)) = (uint32_t)slot;
    RET(0);
}
/* ReleaseXDecoderOutputBuffer(decoder, u32 index, NativeSyncHandle) */
static void dec_release(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (uint32_t)ARG(1) < MAX_SLOTS) d->busy[(uint32_t)ARG(1)] = 0;
    RET(0);
}
/* GetXDecoderOutputBufferProperties(u16* w, u16* h, u16* display w, u16* display h, TimeSpan* pts, bool* last,
   decoder, u32 index) */
static void dec_properties(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(6), DEC_MAGIC);
    const uint32_t i = (uint32_t)ARG(7);
    const Pool *o = d && i < MAX_SLOTS ? (const Pool *)host_of(d->slots[i], POOL_MAGIC) : NULL;
    const uint16_t w = o ? (uint16_t)o->w : 0, h = o ? (uint16_t)o->h : 0;
    if (ARG(0)) *(uint16_t *)GP(ARG(0)) = w;
    if (ARG(1)) *(uint16_t *)GP(ARG(1)) = h;
    if (ARG(2)) *(uint16_t *)GP(ARG(2)) = w;
    if (ARG(3)) *(uint16_t *)GP(ARG(3)) = h;
    if (ARG(4)) G64(ARG(4)) = d && i < MAX_SLOTS ? (uint64_t)d->slot_pts[i] : 0;
    if (ARG(5)) *(uint8_t *)GP(ARG(5)) = d && i < MAX_SLOTS ? (uint8_t)d->slot_last[i] : 0;
    {   /* (HOS_VIDEO_LOG) the first times the game is told a picture is the last */
        static volatile LONG n;
        if (video_log() && d && i < MAX_SLOTS && d->slot_last[i] && InterlockedIncrement(&n) <= 8)
            hos_log("video: properties of buffer %u: the last picture, pts %lld (lr %#llx)", i, (long long)d->slot_pts[i],
                    (unsigned long long)c->x[30]);
    }
    RET(0);
}
/* (longshot ae7b1165) GetXDecoderSubmissionReadyEvent(decoder): the decoder's own event, signalled while it has room
   (input_room). It was one event for all, signalled for ever. The events are never freed (hos_sys_alloc), so a decoder that
   ends gives its event back, signalled, for the next one: a game still holding it sees "ready", as before. The order of
   the locks: a decoder's, then ready_lock (ready_event, dec_finalize) */
static SRWLOCK ready_lock = SRWLOCK_INIT;
static uint64_t ready_spare[8];
static int nready_spare;
static void ready_event_give_back(uint64_t event) {
    if (!event) return;
    hos_system_event_signal(event);
    AcquireSRWLockExclusive(&ready_lock);
    if (nready_spare < (int)(sizeof ready_spare / sizeof ready_spare[0])) ready_spare[nready_spare++] = event;
    ReleaseSRWLockExclusive(&ready_lock);
}
static uint64_t ready_event_new(void) {
    AcquireSRWLockExclusive(&ready_lock);
    uint64_t event = nready_spare ? ready_spare[--nready_spare] : 0;
    ReleaseSRWLockExclusive(&ready_lock);
    if (!event) {
        event = hos_sys_alloc(0x40, 16);
        hos_system_event_init(event, 0);
    }
    hos_system_event_signal(event);
    return event;
}
static uint64_t ready_event(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (!d) {   /* (a decoder this file does not know: an event that is always ready, as before) */
        static uint64_t event;
        if (!event) event = ready_event_new();
        return event;
    }
    const int away = dec_lock(d);
    if (!d->ready) {
        d->ready = ready_event_new();
        d->ready_clear = 0;
        input_room(d);
    }
    const uint64_t event = d->ready;
    dec_unlock(d, away);
    return event;
}

/* VP9 */
HLE(_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP9, "VP9"); }
HLE(_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP9, "VP9"); }
HLE(_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE) { dec_finalize(c); }
HLE(_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj) { dec_register(c); }
HLE(_ZN2nn5codec26UnregisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEj) { dec_unregister(c); }
HLE(_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE) { RET(0); }
HLE(_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE) { dec_flush(c, "Stop"); }
HLE(_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE) { dec_flush(c, "Flush"); }
HLE(_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPNS0_14Vp9DecoderTypeE) { RET(ready_event(c)); }
HLE(_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb) { dec_submit(c); }
HLE(_ZN2nn5codec36SubmitEncryptedVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb) {
    const uint64_t id = submitted();
    if (ARG(0)) G64(ARG(0)) = id;
    RET(0);
}
HLE(_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE) { dec_acquire(c); }
HLE(_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE) { dec_release(c); }
HLE(_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj) { dec_properties(c); }

/* (hookshot) AV1: OoT's movies. FFmpeg's AV1 decoder if the build has one, else dav1d (host/dav1d_av1.c) when the build has
   it, else Media Foundation (host/mf_av1.c).
   Initialize...ByTransferMemory(decoder, memory, size, ?, u16 width, u16 height, ...), ...ByRemoteMemory(decoder,
   size, u16 width, u16 height, ...) */
HLE(_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) {
    dec_init_sized(c, AV_CODEC_ID_AV1, "AV1", (int)(uint16_t)ARG(4), (int)(uint16_t)ARG(5));
}
HLE(_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) {
    dec_init_sized(c, AV_CODEC_ID_AV1, "AV1", (int)(uint16_t)ARG(2), (int)(uint16_t)ARG(3));
}
HLE(_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE) { dec_finalize(c); }
HLE(_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj) { dec_register(c); }
HLE(_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE) { RET(0); }
HLE(_ZN2nn5codec14StopAv1DecoderEPNS0_14Av1DecoderTypeE) { dec_flush(c, "Stop"); }
HLE(_ZN2nn5codec15FlushAv1DecoderEPNS0_14Av1DecoderTypeE) { dec_flush(c, "Flush"); }
HLE(_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE) { RET(ready_event(c)); }
HLE(_ZN2nn5codec27SubmitAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb) { dec_submit(c); }
HLE(_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE) { dec_acquire(c); }
HLE(_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE) { dec_release(c); }
HLE(_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj) { dec_properties(c); }

/* VP8 */
HLE(_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP8, "VP8"); }
HLE(_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP8, "VP8"); }
HLE(_ZN2nn5codec18FinalizeVp8DecoderEPNS0_14Vp8DecoderTypeE) { dec_finalize(c); }
HLE(_ZN2nn5codec24RegisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj) { dec_register(c); }
HLE(_ZN2nn5codec26UnregisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEj) { dec_unregister(c); }
HLE(_ZN2nn5codec15StartVp8DecoderEPNS0_14Vp8DecoderTypeE) { RET(0); }
HLE(_ZN2nn5codec14StopVp8DecoderEPNS0_14Vp8DecoderTypeE) { dec_flush(c, "Stop"); }
HLE(_ZN2nn5codec15FlushVp8DecoderEPNS0_14Vp8DecoderTypeE) { dec_flush(c, "Flush"); }
HLE(_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPNS0_14Vp8DecoderTypeE) { RET(ready_event(c)); }
HLE(_ZN2nn5codec27SubmitVp8DecoderInputBufferEPmPNS0_14Vp8DecoderTypeEPKvmNS_8TimeSpanEb) { dec_submit(c); }
HLE(_ZN2nn5codec29AcquireVp8DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp8DecoderTypeE) { dec_acquire(c); }
HLE(_ZN2nn5codec29ReleaseVp8DecoderOutputBufferEPNS0_14Vp8DecoderTypeEjNS_2gs16NativeSyncHandleE) { dec_release(c); }
HLE(_ZN2nn5codec35GetVp8DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp8DecoderTypeEj) { dec_properties(c); }
