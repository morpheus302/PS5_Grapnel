/* zbic_unpack: ZBIC decompression (Switch firmware 22.0.0+'s zstd variant) as a program of its own.

     zbic_unpack <size>          a ZBIC (or zstd) frame on stdin, its <size> decompressed bytes on stdout
     zbic_unpack --content-size  the size the frame on stdin states, printed in decimal
     zbic_unpack --selftest      compresses a pattern with this zstd, decompresses it, checks it: "ok" and exit 0
     zbic_unpack --compress      (grapnel) the bytes on stdin as a frame of this zstd on stdout: nx2/zbic.py's check of the
                                 pipes (this zstd reads its own frames, not those of another zstd)

   Exit 0 when done; 1 for a usage or input error; 2 when the data does not decompress to the size asked for (said on stderr).

   It is compiled with third_party/zbic/zstd.c, Atmosphere-NX's single-file zstd with ZSTD_ZBIC_SUPPORT (GPL-2.0: its
   PROVENANCE.md and LICENSE), so this program is GPL-2.0 as a whole. Built by nx2/zbic.py on first use, beside this file. nx2/zbic.py runs it as a separate process and exchanges only
   bytes through pipes: the GPL-3.0 tools and this GPL-2.0 program are not combined into one work.

   SPDX-License-Identifier: GPL-2.0-or-later (this file; the program it makes is GPL-2.0 through zstd.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zstd.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static unsigned char *read_all(FILE *f, size_t *n) {
    size_t cap = 1 << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len == cap) {
            unsigned char *more = realloc(buf, cap * 2);
            if (!more) { free(buf); return NULL; }
            buf = more;
            cap *= 2;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    if (ferror(f)) { free(buf); return NULL; }
    *n = len;
    return buf;
}

static int write_all(FILE *f, const unsigned char *p, size_t n) {
    while (n) {
        size_t put = fwrite(p, 1, n, f);
        if (put == 0) return -1;
        p += put;
        n -= put;
    }
    return fflush(f);
}

static int selftest(void) {
    enum { N = 1 << 20 };
    unsigned char *src = malloc(N), *dst = malloc(N), *packed;
    size_t bound = ZSTD_compressBound(N);
    packed = malloc(bound);
    if (!src || !dst || !packed) return 2;
    unsigned x = 0x12345678u;
    for (size_t i = 0; i < N; i++) {   /* (text-like: runs and a few symbols, so the entropy stages are used) */
        x = x * 1103515245u + 12345u;
        src[i] = (unsigned char)("abcdefgh  \n"[(x >> 16) % 11]);
    }
    size_t c = ZSTD_compress(packed, bound, src, N, 3);
    if (ZSTD_isError(c)) { fprintf(stderr, "zbic_unpack: selftest: compress failed: %s\n", ZSTD_getErrorName(c)); return 2; }
    size_t d = ZSTD_decompress(dst, N, packed, c);
    if (ZSTD_isError(d) || d != N || memcmp(src, dst, N)) {
        fprintf(stderr, "zbic_unpack: selftest: the frame did not come back as it was\n");
        return 2;
    }
    printf("ok (%zu bytes through a %zu-byte frame)\n", (size_t)N, c);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: zbic_unpack <size> | --content-size | --selftest | --compress (the frame, or bytes, on stdin)\n");
        return 1;
    }
    if (!strcmp(argv[1], "--selftest")) return selftest();
#ifdef _WIN32
    /* (grapnel, Windows) stdin and stdout are text streams there: a frame's first 0x1A byte ended the read, and every 0x0A
       written became 0x0D 0x0A. nx2/zbic.py's selftest sends such bytes through both */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    size_t n = 0;
    unsigned char *src = read_all(stdin, &n);
    if (!src) { fprintf(stderr, "zbic_unpack: cannot read stdin\n"); return 1; }
    if (!strcmp(argv[1], "--compress")) {
        size_t bound = ZSTD_compressBound(n);
        unsigned char *packed = malloc(bound ? bound : 1);
        if (!packed) { fprintf(stderr, "zbic_unpack: out of memory\n"); return 1; }
        size_t c = ZSTD_compress(packed, bound, src, n, 1);
        if (ZSTD_isError(c)) { fprintf(stderr, "zbic_unpack: compress failed: %s\n", ZSTD_getErrorName(c)); return 2; }
        if (write_all(stdout, packed, c)) { fprintf(stderr, "zbic_unpack: cannot write stdout\n"); return 1; }
        return 0;
    }
    if (!strcmp(argv[1], "--content-size")) {
        unsigned long long s = ZSTD_getFrameContentSize(src, n);
        if (s == ZSTD_CONTENTSIZE_ERROR || s == ZSTD_CONTENTSIZE_UNKNOWN) {
            fprintf(stderr, "zbic_unpack: no content size in the frame header\n");
            return 2;
        }
        printf("%llu\n", s);
        return 0;
    }
    char *end = NULL;
    unsigned long long want = strtoull(argv[1], &end, 10);
    if (!end || *end || want == 0) { fprintf(stderr, "zbic_unpack: bad size %s\n", argv[1]); return 1; }
    unsigned char *dst = malloc((size_t)want);
    if (!dst) { fprintf(stderr, "zbic_unpack: out of memory\n"); return 1; }
    size_t r = ZSTD_decompress(dst, (size_t)want, src, n);
    if (ZSTD_isError(r) || r != want) {
        fprintf(stderr, "zbic_unpack: decompression failed (%s, %zu of %llu bytes)\n",
                ZSTD_isError(r) ? ZSTD_getErrorName(r) : "short", ZSTD_isError(r) ? (size_t)0 : r, want);
        return 2;
    }
    if (write_all(stdout, dst, r)) { fprintf(stderr, "zbic_unpack: cannot write stdout\n"); return 1; }
    return 0;
}
