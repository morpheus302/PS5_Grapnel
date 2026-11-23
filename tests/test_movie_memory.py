"""(ps5) A movie's large buffers do not come from the host heap, and a decoder keeps its pictures' buffers while it plays.

Ten minutes into play the console's count of direct memory by the site that asked for it named six 16 MiB blocks of the
host heap asked by queue_frame, two by dav1d and others by the image pools and the decoder's planes, though the title
screen's movie had long stopped and the game had flushed, stopped and finalized its decoder and its pools (the PC build
shows the calls). Each picture was a malloc of 3 MiB and a free, 60 times a second, beside a game that loads: where the
heap had no room it mapped a 16 MiB block (125 times in 20 s), other threads' small allocations landed in some, and a
block goes back only when all of it is free.

host/nn_video.c now takes a picture's NV12 and (host/posix/ps5_dav1d.c) the decoder's planes from blocks of direct memory
of their own on the console, keeps the blocks of the pictures taken or skipped for the next pictures, and lets them go
when playback stops. These checks keep:
- HOS_VIDEO_BLOCKS read at first use, on by default on the console only; elsewhere a malloc and a free a picture, as upstream;
- a block's size rounded to the console's 64 KiB unit and given back with that size; the heap asked where a block is refused;
- every picture's buffer taken from and returned to the decoder's spares, whatever path frees the picture;
- a flush, a stop and a decoder's end let go of the submissions, the pictures and the spares, and say how much; a stream's
  end lets go of the spares;
- a block given back is unmapped, where freed heap memory stays readable: the blocks are let go under the decoder's lock, a
  decoder's end included, and an image pool's pictures, which the game's readers copy from under no lock, stay the heap's;
- an image pool's RGBA pictures made at their first use on the console, not with the pool;
- the memory line's count of backed reservations brought back down when a block goes.
The blocks and the spares are compiled and run, as the console builds them (over a model of its two calls) and as a PC does.

python -I -B tests/test_movie_memory.py [--cc cc]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cc", "gcc"]   # (tests/run_all.py reads it)
VIDEO = ROOT / "host" / "nn_video.c"
DAV1D = ROOT / "host" / "posix" / "ps5_dav1d.c"
PLATFORM = ROOT / "host" / "posix" / "ps5_platform.c"
HEADER = ROOT / "host" / "posix" / "ps5.h"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def console_only(text, name):
    """Every use of name in the code (not its comments) lies between an #ifdef __PROSPERO__ and its #else or #endif"""
    inside, depth, uses = [], 0, 0
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#if"):
            depth += 1
            if "__PROSPERO__" in s and "!" not in s and "||" not in s:
                inside.append(depth)
        elif s.startswith("#else") or s.startswith("#elif"):
            if inside and inside[-1] == depth:
                inside.pop()
        elif s.startswith("#endif"):
            if inside and inside[-1] == depth:
                inside.pop()
            depth -= 1
        elif name in line and not s.startswith("//"):
            uses += 1
            if not inside:
                return False
    return uses > 0


def check_video(video):
    on = block(video, "static int video_blocks(void)")
    need(video.count('getenv("HOS_VIDEO_BLOCKS")') == 1 and 'getenv("HOS_VIDEO_BLOCKS")' in on,
         "HOS_VIDEO_BLOCKS is not read inside video_blocks alone (at first use: the console's settings file is read in main)")
    need("on = e ? atoi(e) != 0 : VIDEO_PS5_DEFAULT;" in on, "the blocks are not on by default on the console and off elsewhere")
    need(re.search(r"#ifdef __PROSPERO__\s+#define VIDEO_RGBA_ON_DEMAND 1\s+#define VIDEO_PS5_DEFAULT 1\s+#else\s+#define VIDEO_RGBA_ON_DEMAND 0\s+"
                   r"#define VIDEO_PS5_DEFAULT 0\s+#endif", video), "the console's defaults are no longer the console's alone")
    need(console_only(video, "hos_ps5_block"), "hos_ps5_block or hos_ps5_block_free is named outside #ifdef __PROSPERO__")
    make = block(video, "static int vblock_make(VBlock *b, size_t n, int zero)")
    need("const size_t size = (n + 0xFFFF) & ~(size_t)0xFFFF;" in make and "void *p = hos_ps5_block(size);" in make,
         "a block is not asked for in whole 64 KiB units")
    need(make.index("hos_ps5_block(size)") < make.index("zero ? calloc(1, n) : malloc(n)"), "the heap is not what is asked when a block is refused")
    need("if (video_blocks()) {" in make, "a block of direct memory is made with the blocks switched off")

    frame = block(video, "static void queue_frame(Dec *d, const AVFrame *f)")
    need("if (spare_get(&d->spares, &p.mem, (size_t)w * h * 3 / 2, !(nv12 || (planar && !((w | h) & 1))))) p.nv12 = p.mem.p;" in frame,
         "a picture's NV12 is not taken from the decoder's blocks, or is no longer zeroed where it is not written whole")
    need("malloc((size_t)w * h * 3 / 2)" not in frame and "calloc(1, (size_t)w * h * 3 / 2)" not in frame, "a picture's NV12 still comes from the heap")
    dropped = frame[frame.index("if (!p.nv12 || (!nv12_only && !p.rgba)) {"):frame.index("uint8_t *dst[4]")]
    need("spare_put(&d->spares, &p.mem);" in dropped, "a picture dropped for want of memory does not give its block back")
    free = block(video, "static void picture_free(Dec *d, Picture *p)")
    need("spare_put(&d->spares, &p->mem);" in free and "free(p->rgba);" in free, "a picture freed does not give its block back to its decoder")
    need("free(p->nv12)" not in video and "free(p.nv12)" not in video, "a picture's NV12 is given to free, whatever it was made by")
    need(len(re.findall(r"\bpicture_free\(d, ", video)) == 5 and len(re.findall(r"\bpicture_free\(", video)) == 6,
         "a picture is freed from somewhere this test does not know (the queue's oldest, a skipped one, the one taken, dec_let_go, "
         "and one decoded ahead that found no place aside: picture_out)")
    out = block(video, "static void picture_out(Dec *d, Picture p)")
    need(out.index("AcquireSRWLockExclusive(&d->lock);") < out.index("picture_free(d, &p);") < out.index("ReleaseSRWLockExclusive(&d->lock);"),
         "the thread that decodes ahead gives a block back to the decoder without the decoder's lock")
    need(frame.index("aside_lock(d);") < frame.index("if (spare_get(&d->spares") < frame.index("aside_unlock(d);"),
         "the thread that decodes ahead takes a block from the decoder's without the decoder's lock")

    go = block(video, "static void dec_let_go(Dec *d, const char *what, int all)")
    for part in ("pending_free(d);", "picture_free(d, p);", "d->qhead = d->qn = 0;", "spares_release(&d->spares, &spare_bytes)"):
        need(part in go, "dec_let_go no longer does: " + part)
    need(go.index("if (all) {") < go.index("pending_free(d);") < go.index("spares_release("), "the spares are let go before the pictures are back in them")
    line = re.sub(r'"\s*\n\s*"', "", go)
    need("video: %s: %d pictures not taken (%.1f MiB), %d submissions not decoded (%.1f MiB) and %d picture buffers kept (%.1f MiB) let go" in line,
         "the line does not say how many pictures, submissions and buffers went, and how many MiB")
    need("InterlockedIncrement(&told) <= 60" in go and "video_stats()" in go, "the line is not limited, or is written where the 5 s line is not")
    flush = block(video, "static void dec_flush(Ctx *c, const char *what)")
    need("dec_let_go(d, what, 1);" in flush and flush.index("dec_lock(d)") < flush.index("dec_let_go(d, what, 1);") < flush.index("dec_unlock(d, away);"),
         "a flush or a stop does not let go of everything, under the decoder's lock")
    end = block(video, "static void dec_finalize(Ctx *c)")
    need(end.index("const int away = dec_lock(d);") < end.index('dec_let_go(d, "a decoder\'s end", 1);') <
         end.index("avcodec_free_context(&d->ctx);") < end.index("set_host(ARG(0), DEC_MAGIC, NULL);") < end.index("dec_unlock(d, away);") <
         end.index("free(d);"), "a decoder's end does not let go of its pictures and close its decoder under the decoder's lock")
    need("video: a decoder is finalized after %lld submissions and %lld buffers taken" in end, "a decoder's end is not said")
    acquire = block(video, "static void dec_acquire_picture(Ctx *c) {")   # (the definition, not the declaration before it)
    empty = block(acquire, "if (!d->qn) {")
    need('if (d->eos && !d->phead && d->spares.n) dec_let_go(d, "a stream\'s end", 0);' in empty,
         "the spares are not let go at a stream's end (and only there: no picture is coming)")

    init = block(video, "HLE(_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj)")
    need("o->rgba = VIDEO_RGBA_ON_DEMAND ? NULL : (uint8_t *)calloc((size_t)o->count, (size_t)o->w * o->h * 4);" in init,
         "a pool's RGBA pictures are made with the pool on the console, or not made with it elsewhere")
    need("o->nv12 = (uint8_t *)calloc((size_t)o->count, (size_t)o->w * o->h * 3 / 2);" in init,
         "a pool's NV12 pictures are no longer the heap's (a reader may be copying from them when the game finalizes the pool)")
    need("memset(img, 0x10, (size_t)o->w * o->h);" in init and "memset(img + (size_t)o->w * o->h, 0x80, (size_t)o->w * o->h / 2);" in init,
         "a new pool's pictures are no longer black")
    rgba = block(video, "static void pool_rgba(Pool *o)")
    made = block(rgba, "if (VIDEO_RGBA_ON_DEMAND && !o->rgba) {")
    need("calloc((size_t)o->count, (size_t)o->w * o->h * 4)" in made and "rgba[k] = 0xff;" in made and "o->rgba = rgba;" in made,
         "the RGBA made on demand is not what a new pool's is (opaque black)")
    need(rgba.index("AcquireSRWLockExclusive(&pool_rgba_lock);") < rgba.index("if (VIDEO_RGBA_ON_DEMAND && !o->rgba) {") <
         rgba.index("if (o->rgba_stale && o->rgba && o->nv12) {"), "the RGBA is made outside the lock its conversion takes, or after it")
    read = block(video, "HLE(_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm)")
    need(read.index("if (VIDEO_RGBA_ON_DEMAND && !o->rgba) pool_rgba(o);") < read.index("if (!o->rgba) { RET(0); return; }") <
         read.index("const uint8_t *src = o->rgba"), "a reader of RGBA is not given pictures made at its first read")
    need("if (o->mem_size >= need && ok && to && rgba && (o->rgba_stale || !o->rgba)) pool_rgba(o);" in acquire,
         "the copy into the pool's guest memory does not make the RGBA it copies")
    final = block(video, "HLE(_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE)")
    need("free(o->rgba), free(o->nv12), free(o);" in final and "vblock_free" not in final, "a pool's end does not free its pictures as before")
    need("video: an image pool is finalized" in final, "a pool's end is not said")
    print("PASS: host/nn_video.c: pictures from the decoder's blocks, let go when playback stops; a pool's RGBA on demand", flush=True)


def check_others(dav1d, platform, header):
    need("hos_video_block((size_t)w * h + 2 * (size_t)cw * ch, &a->planes_size, &a->planes_own)" in dav1d,
         "the decoder's planes are not one block")
    need("a->plane[1] = a->planes ? a->planes + (size_t)w * h : NULL;" in dav1d and "a->plane[2] = a->planes ? a->plane[1] + (size_t)cw * ch : NULL;" in dav1d,
         "the chroma planes do not follow the luma in the block")
    need(dav1d.count("hos_video_block_free(a->planes, a->planes_size, a->planes_own);") == 2 and "free(a->plane[" not in dav1d,
         "the planes' block is not given back when the decoder closes and when it is made again")
    close = block(dav1d, "void avcodec_free_context(AVCodecContext **ctx)")
    need(close.index("dav1d_close(&a->dc);") < close.index("hos_video_block_free("), "the planes go before dav1d is closed")
    need("void *hos_ps5_block(size_t size);" in header and "void hos_ps5_block_free(void *p, size_t size);" in header,
         "host/posix/ps5.h does not declare the blocks")
    need("void *hos_ps5_block(size_t size) { return hos_ps5_reserve(size, 1); }" in platform, "a block is not a reservation backed at once")
    free = block(platform, "void hos_ps5_block_free(void *p, size_t size)")
    need("if (p && !hos_ps5_release(p, size)) __atomic_sub_fetch(&acct_reserved_backed, size, __ATOMIC_RELAXED);" in free and
         "size = (size + UNIT - 1) & ~(size_t)(UNIT - 1);" in free, "a block given back stays in the memory line's backed reservations")
    need("video: dav1d closed after %lu pictures" in close, "dav1d's end is not said")
    print("PASS: the decoder's planes are a block; a block given back leaves the memory line's count", flush=True)


FIXTURE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static long heap_made, heap_freed;
static void *t_malloc(size_t n) { heap_made++; return malloc(n); }
static void *t_calloc(size_t a, size_t b) { heap_made++; return calloc(a, b); }
static void t_free(void *p) { if (p) heap_freed++; free(p); }
#ifdef CONSOLE
#define __PROSPERO__ 1
#define VIDEO_PS5_DEFAULT 1
static long own_made, own_freed, own_refuse;
static size_t own_bytes, last_own_size;
/* the console's two calls: sizes in whole 64 KiB units, given back with the size they were made with */
static struct { void *p; size_t size; } owned[64];
void *hos_ps5_block(size_t size) {
    CHECK(size && !(size & 0xFFFF));
    if (own_refuse) return NULL;
    for (int i = 0; i < 64; i++)
        if (!owned[i].p) {
            owned[i].p = malloc(size), owned[i].size = size;
            memset(owned[i].p, 0xA5, size);   /* (not zeros: whoever asks for zeros must make them) */
            own_made++, own_bytes += size, last_own_size = size;
            return owned[i].p;
        }
    return NULL;
}
void hos_ps5_block_free(void *p, size_t size) {
    for (int i = 0; i < 64; i++)
        if (owned[i].p == p) {
            CHECK(owned[i].size == size);
            free(p);
            owned[i].p = NULL, own_freed++, own_bytes -= size;
            return;
        }
    CHECK(!"a block given back that was never made");
}
#else
#define VIDEO_PS5_DEFAULT 0
#endif
#define malloc(n) t_malloc(n)
#define calloc(a, b) t_calloc(a, b)
#define free(p) t_free(p)
/* ---- host/nn_video.c's own ---- */
@BLOCKS@
@VBLOCK@
@MAKE@
@FREE@
@SPARE@
@VSPARES@
@GET@
@PUT@
@RELEASE@

#define PICTURE ((size_t)1920 * 1080 * 3 / 2)
int main(int argc, char **argv) {
    if (argc > 1) setenv("HOS_VIDEO_BLOCKS", argv[1], 1);
    else unsetenv("HOS_VIDEO_BLOCKS");
    const int on = video_blocks();
    VSpares s;
    memset(&s, 0, sizeof s);
    VBlock q[8];
    size_t bytes = 0;
    /* a movie: 600 pictures, two decoded ahead at most, each taken or skipped in turn */
    int head = 0, n = 0;
    for (int i = 0; i < 600; i++) {
        CHECK(spare_get(&s, &q[(head + n) % 8], PICTURE, 0) && q[(head + n) % 8].p);
        memset(q[(head + n) % 8].p, i, PICTURE);   /* (written whole, as a picture is) */
        n++;
        if (n == 2) spare_put(&s, &q[head]), head = (head + 1) % 8, n--;
    }
    while (n) spare_put(&s, &q[head]), head = (head + 1) % 8, n--;
#ifdef CONSOLE
    if (on) {
        CHECK(own_made == 2 && heap_made == 0 && s.made == 2 && s.reused == 598);   /* (two blocks for the whole movie, none from the heap) */
        CHECK(last_own_size == 0x300000 && last_own_size >= PICTURE && last_own_size - PICTURE < 0x10000);
        CHECK(s.n == 2 && own_bytes == 2 * 0x300000 && own_freed == 0);
        CHECK(spares_release(&s, &bytes) == 2 && bytes == 2 * 0x300000 && own_freed == 2 && own_bytes == 0 && s.n == 0);   /* (playback stops) */
        CHECK(spares_release(&s, &bytes) == 0 && bytes == 0);
        /* the stream's end leaves eight pictures at once: all back, MAX_SPARE kept, the rest gone at once */
        for (int i = 0; i < 8; i++) CHECK(spare_get(&s, &q[i], PICTURE, 0));
        CHECK(own_made == 10 && own_bytes == 8 * 0x300000);
        for (int i = 0; i < 8; i++) {
            spare_put(&s, &q[i]);
            CHECK(!q[i].p);
        }
        CHECK(s.n == MAX_SPARE && own_bytes == MAX_SPARE * 0x300000 && own_freed == 2 + 8 - MAX_SPARE);
        /* a picture of another size does not take a block that is not its own size, and one asked zeroed is zeros */
        VBlock small, z;
        CHECK(spare_get(&s, &small, 1280 * 720 * 3 / 2, 0) && small.size == 0x160000 && s.n == MAX_SPARE && own_made == 11);
        CHECK(spare_get(&s, &z, PICTURE, 1) && s.n == MAX_SPARE - 1 && own_made == 11);
        for (size_t i = 0; i < PICTURE; i += 4099) CHECK(z.p[i] == 0);
        spare_put(&s, &z), spare_put(&s, &small);
        CHECK(s.n == MAX_SPARE && own_freed == 6 + 1);   /* (no room for the small one: let go) */
        VBlock fresh;
        CHECK(vblock_make(&fresh, 100000, 1) && fresh.own && fresh.size == 0x20000 && fresh.p[0] == 0 && fresh.p[99999] == 0);
        vblock_free(&fresh);
        CHECK(!fresh.p && !fresh.size && !fresh.own);
        /* a block refused: the heap is asked, and gets it back */
        CHECK(spares_release(&s, &bytes) == MAX_SPARE && own_bytes == 0);
        own_refuse = 1;
        VBlock h;
        CHECK(spare_get(&s, &h, PICTURE, 0) && !h.own && h.size == PICTURE && heap_made == 1);
        spare_put(&s, &h);
        CHECK(spares_release(&s, &bytes) == 1 && bytes == PICTURE && heap_freed == 1 && own_made == own_freed);
        CHECK(!vblock_make(&h, 0, 0) && !h.p);
        printf("PASS: the console: two blocks of 3 MiB for a movie's 600 pictures, none from the heap, all given back when it stops\n");
        return 0;
    }
    CHECK(own_made == 0);   /* (HOS_VIDEO_BLOCKS=0: the heap, a picture at a time, as before) */
#endif
    if (on) {   /* (a PC with HOS_VIDEO_BLOCKS=1: the heap's, kept and taken again) */
        CHECK(heap_made == 2 && heap_freed == 0 && s.n == 2 && s.reused == 598);
        CHECK(spares_release(&s, &bytes) == 2 && bytes == 2 * PICTURE && heap_freed == 2);
        printf("PASS: HOS_VIDEO_BLOCKS=1 off the console: a movie's pictures in two buffers of the heap's, freed when it stops\n");
        return 0;
    }
    CHECK(heap_made == 600 && heap_freed == 600 && s.n == 0 && s.made == 600 && s.reused == 0);   /* (as upstream) */
    CHECK(spares_release(&s, &bytes) == 0 && bytes == 0);
    VBlock z;
    CHECK(spare_get(&s, &z, 4096, 1) && !z.own && z.size == 4096 && z.p[0] == 0 && z.p[4095] == 0);
    spare_put(&s, &z);
    CHECK(!z.p && heap_freed == 601);
    printf("PASS: without the blocks: a malloc and a free for each picture, nothing kept\n");
    return 0;
}
'''


def check_blocks(video, cc):
    parts = {
        "@BLOCKS@": block(video, "static int video_blocks(void)"),
        "@VBLOCK@": re.search(r"typedef struct \{ uint8_t \*p; size_t size; int own; \} VBlock;", video).group(0),
        "@MAKE@": block(video, "static int vblock_make(VBlock *b, size_t n, int zero)"),
        "@FREE@": block(video, "static void vblock_free(VBlock *b)"),
        "@SPARE@": re.search(r"#define MAX_SPARE \d+", video).group(0),
        "@VSPARES@": re.search(r"typedef struct \{ VBlock b\[MAX_SPARE\]; int n; int64_t made, reused; \} VSpares;", video).group(0),
        "@GET@": block(video, "static int spare_get(VSpares *s, VBlock *b, size_t n, int zero)"),
        "@PUT@": block(video, "static void spare_put(VSpares *s, VBlock *b)"),
        "@RELEASE@": block(video, "static int spares_release(VSpares *s, size_t *bytes)"),
    }
    source = FIXTURE
    for key, text in parts.items():
        source = source.replace(key, text)
    with tempfile.TemporaryDirectory(prefix="movie-memory-") as tmp:
        c = Path(tmp) / "fixture.c"
        c.write_text(source, encoding="utf-8")
        for name, flags, runs in (("console", ["-DCONSOLE"], ([], ["0"])), ("pc", [], ([], ["1"]))):
            exe = Path(tmp) / name
            subprocess.run([cc, "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", *flags, str(c), "-o", str(exe)], check=True)
            for args in runs:
                subprocess.run([str(exe), *args], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    a = p.parse_args()
    video = VIDEO.read_text(encoding="utf-8")
    check_video(video)
    check_others(DAV1D.read_text(encoding="utf-8"), PLATFORM.read_text(encoding="utf-8"), HEADER.read_text(encoding="utf-8"))
    check_blocks(video, a.cc)


if __name__ == "__main__":
    main()
