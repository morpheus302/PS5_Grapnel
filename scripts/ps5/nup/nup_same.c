/* Two .nup packages read through the RomFS layer (host/nup_fs.c, as the console has it) and compared, file by file: a reference
   (a dump whose files are stored as they are) and another with the same tree (the builder's flat copy of a dump that keeps game
   files compressed, scripts/grapnel-builder/grapnel_flat.py; or that dump itself, to see what the console cannot read in it).
   No game code, no folder.
     nup_same <reference.nup> <other.nup> [--threads N] [--parts K] [--crowd R] [--seed S]
     nup_same --hashes <list> <other.nup> [--threads N]
       the list: a line "<sha256> <size> <path>" a file (scripts/ps5/nup/nup_hashes.py: what nx2/nup.py extracts from a dump):
       every file of it looked up in the other package, read whole through the layer and its SHA-256 compared; and every file
       of the package is in the list
   1. every file: kind and size the same, readable in both; its whole content read through both and compared (files of more
      than 16 MiB in reads of 8 MiB, one after the other);
   2. every file: K reads of a part (random places and lengths up to 64 KiB, the last byte, past the end, across the end);
      files of more than 16 MiB: K/4 parts in order, then one back near the start;
   3. the crowd: N threads, R reads each of random parts of 64 files chosen at random (the largest one among them), all
      threads on the same files at once;
   then RESULT: PASS or FAIL. A layer that unpacks compressed files (NUP_FS_UNPACKS, the branch ps5-nup-compressed: not the
   console's) says what it unpacked too.
   For tests/test_grapnel_flat.py (small packages it makes) and the real game's (scripts/ps5/nup/check.sh same). Game-derived
   output stays on the PC.
   Build: cc -O2 -D_GNU_SOURCE -Ihost scripts/ps5/nup/nup_same.c host/nup_fs.c -lpthread
   SPDX-License-Identifier: GPL-3.0-or-later */
#include "nup_fs.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static NupFs *ref, *oth;
static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int failed;
static int bad_lines;
static void bad(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bad(const char *fmt, ...) {
    pthread_mutex_lock(&mu);
    failed = 1;
    if (bad_lines++ < 60) {
        va_list ap;
        va_start(ap, fmt);
        fputs("  BAD ", stdout);
        vprintf(fmt, ap);
        fputc('\n', stdout);
        va_end(ap);
    }
    pthread_mutex_unlock(&mu);
}
static uint64_t rnd(uint64_t *s) {   /* (splitmix64) */
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

typedef struct { char *path; uint32_t a, b; uint64_t size; } File;
static File *files;
static size_t nfiles, cap_files, ndirs;
static void walk(uint32_t da, uint32_t db, const char *prefix) {
    ndirs++;
    const uint32_t count = nup_child_count(ref, da);
    if (nup_child_count(oth, db) != count) bad("folder %s: %u entries, %u in the other", *prefix ? prefix : ".", count, nup_child_count(oth, db));
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t c = nup_child(ref, da, i);
        char name[1024], path[4096];
        nup_name_copy(ref, c, name, sizeof name);
        snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", name);
        const uint32_t o = nup_lookup(oth, path);
        if (o == NUP_NONE) { bad("%s: not in the other", path); continue; }
        if (nup_is_dir(ref, c) != nup_is_dir(oth, o)) { bad("%s: a folder in one, a file in the other", path); continue; }
        if (nup_is_dir(ref, c)) { walk(c, o, path); continue; }
        if (nfiles == cap_files) files = (File *)realloc(files, sizeof *files * (cap_files = cap_files ? cap_files * 2 : 1024));
        files[nfiles].path = strcpy((char *)xmalloc(strlen(path) + 1), path);
        files[nfiles].a = c, files[nfiles].b = o, files[nfiles++].size = nup_size(ref, c);
    }
}

#define BIG (16ull << 20)
#define STEP (8ull << 20)
static volatile size_t next_file;
static volatile uint64_t bytes_done, files_same;
static unsigned parts = 16;
static uint64_t seed = 1;

/* one read of [at, at + n) through both, compared: 1 when the same */
static int same_part(const File *f, uint64_t at, uint64_t n, uint8_t *x, uint8_t *y, const char *what) {
    const int64_t ga = nup_read(ref, f->a, at, x, n), gb = nup_read(oth, f->b, at, y, n);
    if (ga != gb || ga < 0 || memcmp(x, y, (size_t)ga)) {
        bad("%s: %s at %llu, %llu bytes: %lld read, %lld from the other%s", f->path, what, (unsigned long long)at, (unsigned long long)n,
            (long long)ga, (long long)gb, ga == gb && ga >= 0 ? ", bytes differ" : "");
        return 0;
    }
    return 1;
}
static void *contents(void *arg) {
    (void)arg;
    uint8_t *x = (uint8_t *)xmalloc(BIG), *y = (uint8_t *)xmalloc(BIG);
    for (;;) {
        const size_t i = __atomic_fetch_add(&next_file, 1, __ATOMIC_RELAXED);
        if (i >= nfiles) break;
        const File *f = &files[i];
        uint64_t s = seed ^ (i * 0x9e3779b97f4a7c15ull);
        if (nup_size(oth, f->b) != f->size) { bad("%s: %llu bytes, %llu in the other", f->path, (unsigned long long)f->size, (unsigned long long)nup_size(oth, f->b)); continue; }
        if (!nup_readable(ref, f->a)) { bad("%s: the reference does not read it: %s", f->path, nup_why_unreadable(ref, f->a)); continue; }
        if (!nup_readable(oth, f->b)) { bad("%s: not readable in the other: %s", f->path, nup_why_unreadable(oth, f->b)); continue; }
        int ok = 1;
        if (f->size <= BIG) ok = same_part(f, 0, f->size, x, y, "the whole file");
        else
            for (uint64_t at = 0; ok && at < f->size; at += STEP) ok = same_part(f, at, STEP, x, y, "a piece in order");
        __atomic_fetch_add(&bytes_done, f->size, __ATOMIC_RELAXED);
        /* parts */
        const unsigned k = f->size > BIG ? (parts / 4 ? parts / 4 : 1) : parts;
        uint64_t last = 0;
        for (unsigned j = 0; ok && j < k && f->size; j++) {
            uint64_t at = rnd(&s) % f->size, n = 1 + rnd(&s) % (64u << 10);
            if (f->size > BIG) at = last + rnd(&s) % ((f->size - last) / (k - j) + 1), last = at;   /* (in order) */
            ok = same_part(f, at, n, x, y, "a part");
        }
        if (ok && f->size > BIG) ok = same_part(f, 1000, 4096, x, y, "a part back near the start");
        if (ok && f->size) ok = same_part(f, f->size - 1, 100, x, y, "the last byte") && same_part(f, f->size, 10, x, y, "past the end") &&
                                same_part(f, f->size + 100, 10, x, y, "well past the end") &&
                                same_part(f, f->size > 10 ? f->size - 10 : 0, 64, x, y, "across the end");
        if (ok) __atomic_fetch_add(&files_same, 1, __ATOMIC_RELAXED);
    }
    free(x), free(y);
    return NULL;
}

/* the crowd: every thread on the same few files */
static size_t crowd_files[64], ncrowd;
static unsigned crowd_reads = 300;
static volatile uint64_t crowd_ok;
static void *crowd(void *arg) {
    uint64_t s = seed * 7919 + (uint64_t)(uintptr_t)arg;
    uint8_t *x = (uint8_t *)xmalloc(1 << 17), *y = (uint8_t *)xmalloc(1 << 17);
    for (unsigned r = 0; r < crowd_reads && !failed; r++) {
        const File *f = &files[crowd_files[rnd(&s) % ncrowd]];
        if (!f->size) continue;
        const uint64_t at = rnd(&s) % f->size, n = 1 + rnd(&s) % (1 << 17);
        if (same_part(f, at, n, x, y, "a crowd's part")) __atomic_fetch_add(&crowd_ok, 1, __ATOMIC_RELAXED);
        if (rnd(&s) % 8 == 0 && f->size <= (1 << 17) && same_part(f, 0, f->size, x, y, "a crowd's whole file"))
            __atomic_fetch_add(&crowd_ok, 1, __ATOMIC_RELAXED);
    }
    free(x), free(y);
    return NULL;
}

/* ---- --hashes: a list against a package ---- */
typedef struct { char *path; uint64_t size; uint8_t sha[32]; } Want;
static Want *wants;
static size_t nwants;
static volatile size_t next_want;
static volatile uint64_t wants_same, wants_bytes;
static int unhex(const char *h, uint8_t *out) {
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}
static void *hash_worker(void *arg) {
    (void)arg;
    const size_t chunk = 8u << 20;
    uint8_t *b = (uint8_t *)xmalloc(chunk);
    for (;;) {
        const size_t i = __atomic_fetch_add(&next_want, 1, __ATOMIC_RELAXED);
        if (i >= nwants) break;
        const Want *w = &wants[i];
        const uint32_t node = nup_lookup(oth, w->path);
        if (node == NUP_NONE || nup_is_dir(oth, node)) { bad("%s: not in the package", w->path); continue; }
        if (nup_size(oth, node) != w->size) { bad("%s: %llu bytes, %llu in the list", w->path, (unsigned long long)nup_size(oth, node), (unsigned long long)w->size); continue; }
        if (!nup_readable(oth, node)) { bad("%s: the layer does not read it: %s", w->path, nup_why_unreadable(oth, node)); continue; }
        NupSha256 h;
        nup_sha256_init(&h);
        uint64_t at = 0;
        int ok = 1;
        while (ok && at < w->size) {
            const int64_t k = nup_read(oth, node, at, b, chunk);
            if (k <= 0) { bad("%s: a read at %llu failed (%lld)", w->path, (unsigned long long)at, (long long)k); ok = 0; break; }
            nup_sha256_update(&h, b, (size_t)k);
            at += (uint64_t)k;
        }
        uint8_t d[32];
        nup_sha256_final(&h, d);
        if (ok && memcmp(d, w->sha, 32)) { bad("%s: its SHA-256 is not the list's", w->path); ok = 0; }
        if (ok) __atomic_fetch_add(&wants_same, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&wants_bytes, w->size, __ATOMIC_RELAXED);
    }
    free(b);
    return NULL;
}
static size_t count_files(uint32_t d) {
    size_t n = 0;
    for (uint32_t i = 0, k = nup_child_count(oth, d); i < k; i++) {
        const uint32_t c = nup_child(oth, d, i);
        n += nup_is_dir(oth, c) ? count_files(c) : 1;
    }
    return n;
}
static int hashes_main(const char *list, const char *package, unsigned threads) {
    char why[512], line[5000];
    oth = nup_open(package, 0, why, sizeof why);
    if (!oth) { printf("open: %s: %s\nRESULT: FAIL\n", package, why); return 1; }
    nup_describe(oth, line, sizeof line);
    printf("package: %s\n", line);
    const NupInfo *oi = nup_info(oth);
    printf("package info: packed %u, bad %u\n", oi->packed, oi->bad);
    FILE *f = fopen(list, "r");
    if (!f) { perror(list); return 2; }
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        char hex[80], path[4200];
        unsigned long long size;
        if (line[0] == '#' || sscanf(line, "%64s %llu %4199[^\n]", hex, &size, path) != 3) continue;
        if (nwants == cap) wants = (Want *)realloc(wants, sizeof *wants * (cap = cap ? cap * 2 : 4096));
        if (!unhex(hex, wants[nwants].sha)) continue;
        wants[nwants].path = strcpy((char *)xmalloc(strlen(path) + 1), path);
        wants[nwants++].size = size;
    }
    fclose(f);
    const size_t in_package = count_files(NUP_ROOT);
    printf("list: %zu files; the package's romfs: %zu files\n", nwants, in_package);
    if (in_package != nwants) bad("the package has %zu files, the list %zu", in_package, nwants);
    if (oi->packed || oi->bad) bad("the package has %u files stored compressed and %u bad", oi->packed, oi->bad);
    const double t0 = now_s();
    pthread_t t[16];
    for (unsigned i = 0; i < threads; i++) pthread_create(&t[i], NULL, hash_worker, NULL);
    for (unsigned i = 0; i < threads; i++) pthread_join(t[i], NULL);
    printf("hashes: %llu/%zu files read through the layer with the list's size and SHA-256; %.2f GB in %.1f s with %u threads\n",
           (unsigned long long)wants_same, nwants, (double)wants_bytes / 1e9, now_s() - t0, threads);
    if (wants_same != nwants) failed = 1;
    printf("reads: %llu through the package's descriptor\n", (unsigned long long)nup_reads(oth));
    nup_close(oth);
    printf("%s\n", failed ? "RESULT: FAIL" : "RESULT: PASS");
    return failed;
}

int main(int argc, char **argv) {
    const char *pos[2] = {0}, *list = NULL;
    unsigned threads = 4, npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hashes") && i + 1 < argc) list = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--parts") && i + 1 < argc) parts = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--crowd") && i + 1 < argc) crowd_reads = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (npos < 2) pos[npos++] = argv[i];
    }
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    if (list && npos == 1) return hashes_main(list, pos[0], threads);
    if (npos < 2) { fprintf(stderr, "usage: nup_same <reference.nup> <other.nup> [--threads N] [--parts K] [--crowd R] [--seed S]\n"
                                    "       nup_same --hashes <list> <other.nup> [--threads N]\n"); return 2; }
    char why[512], line[1600];
    ref = nup_open(pos[0], 0, why, sizeof why);
    if (!ref) { printf("open: reference %s: %s\nRESULT: FAIL\n", pos[0], why); return 1; }
    oth = nup_open(pos[1], 0, why, sizeof why);
    if (!oth) { printf("open: other %s: %s\nRESULT: FAIL\n", pos[1], why); return 1; }
    nup_describe(ref, line, sizeof line);
    printf("reference: %s\n", line);
    nup_describe(oth, line, sizeof line);
    printf("other: %s\n", line);
    const NupInfo *ri = nup_info(ref), *oi = nup_info(oth);
#ifdef NUP_FS_UNPACKS
    printf("other info: packed %u (%.3f GB), checked %u, framed %d, bad %u, unpacks %d\n", oi->packed, (double)oi->packed_bytes / 1e9,
           oi->checked, oi->framed, oi->bad, oi->unpacks);
    printf("reference info: packed %u, checked %u, framed %d, bad %u; unpacking holds %llu bytes\n", ri->packed, ri->checked, ri->framed,
           ri->bad, (unsigned long long)nup_unpack_memory(ref));
#else
    printf("other info: packed %u, bad %u (a layer that does not unpack)\n", oi->packed, oi->bad);
    printf("reference info: packed %u, bad %u\n", ri->packed, ri->bad);
#endif
    if (ri->packed || ri->bad) bad("the reference has %u files stored compressed and %u bad: it should have none", ri->packed, ri->bad);
    if (ri->files != oi->files || ri->dirs != oi->dirs) bad("%u files and %u folders, %u and %u in the other", ri->files, ri->dirs, oi->files, oi->dirs);
    walk(NUP_ROOT, NUP_ROOT, "");
    printf("tree: %zu files, %zu folders\n", nfiles, ndirs);
    double t0 = now_s();
    pthread_t t[16];
    for (unsigned i = 0; i < threads; i++) pthread_create(&t[i], NULL, contents, NULL);
    for (int shown = 0;;) {
        struct timespec ts = {0, 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
        const size_t at = next_file;
        if (now_s() - t0 > 30.0 * (shown + 1)) {
            shown++;
            printf("  contents: %zu/%zu files, %.1f GB, %.0f s\n", at < nfiles ? at : nfiles, nfiles, (double)bytes_done / 1e9, now_s() - t0);
            fflush(stdout);
        }
        if (at >= nfiles) break;
    }
    for (unsigned i = 0; i < threads; i++) pthread_join(t[i], NULL);
    double sec = now_s() - t0;
    printf("contents: %llu/%zu files the same through both (whole, %u parts, the ends); %.2f GB in %.1f s with %u threads\n",
           (unsigned long long)files_same, nfiles, parts, (double)bytes_done / 1e9, sec, threads);
    if (files_same != nfiles) failed = 1;
    if (nfiles && crowd_reads) {
        uint64_t s = seed;
        size_t largest = 0;
        for (size_t i = 0; i < nfiles; i++) if (files[i].size > files[largest].size && files[i].size <= (1ull << 30)) largest = i;
        crowd_files[ncrowd++] = largest;
        while (ncrowd < 64 && ncrowd < nfiles) crowd_files[ncrowd++] = (size_t)(rnd(&s) % nfiles);
        t0 = now_s();
        for (unsigned i = 0; i < threads; i++) pthread_create(&t[i], NULL, crowd, (void *)(uintptr_t)(i + 1));
        for (unsigned i = 0; i < threads; i++) pthread_join(t[i], NULL);
        printf("crowd: %llu reads the same, %u threads on %zu files (the largest %s, %llu bytes), %.1f s\n", (unsigned long long)crowd_ok,
               threads, ncrowd, files[largest].path, (unsigned long long)files[largest].size, now_s() - t0);
    }
#ifdef NUP_FS_UNPACKS
    nup_unpack_line(oth, line, sizeof line);
    printf("unpacking: %s\n", line[0] ? line : "(nothing: no compressed file)");
    printf("unpacking memory: %llu bytes held at the end\n", (unsigned long long)nup_unpack_memory(oth));
#endif
    printf("reads: %llu through the reference's descriptor, %llu through the other's\n", (unsigned long long)nup_reads(ref),
           (unsigned long long)nup_reads(oth));
    nup_close(ref), nup_close(oth);
    printf("%s\n", failed ? "RESULT: FAIL" : "RESULT: PASS");
    return failed;
}
