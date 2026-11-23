/* The platform probe's nup check (PPSA99147, scripts/ps5/plat-probe): the game's RomFS read in place from game.nup through
   host/nup_fs.c, the eboot's own code. In a file of its own so that scripts/ps5/nup/probe_nup_pc.c runs the same check on the
   PC against the PC's package and list before it goes to the console.
   SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "plat_probe.h"
#include "nup_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIB (1ull << 20)

/* ===================================================================================== 11. the RomFS from game.nup */
/* The game reads its RomFS in place from the user's .nup package (host/nup_fs.c, the eboot's own code; nn_fs.c mounts it
   when /data/hookshot has game.nup and no romfs/): 186,583 files made one by one ran at about 2 MB/s, and a title holds
   about 249 files open by path. Here: the index's time and memory; files spread over the tree and every file of one big
   folder read through the layer and checked against nup-hashes.txt (made on the PC from the extracted romfs/ folder,
   scripts/ps5/nup/check.sh hashlist: never shared); every folder's listing against the PC's; random reads from 8 threads
   at once, with the descriptors counted (the package's one, whatever the threads do). */
/* the open descriptors, or -1 when this console does not say: fcntl(F_GETFD) answered -1 for every one of them on the
   PS5 (run 3: "descriptors 0", with the results file and the package open), so fstat is asked too; a way that never once
   answers EBADF (a closed number) cannot count */
static int count_with(int way) {
    int n = 0, closed = 0, top = getdtablesize();
    if (top <= 0 || top > 4096) top = 4096;
    for (int fd = 0; fd < top; fd++) {
        struct stat st;
        const int r = way ? fstat(fd, &st) : fcntl(fd, F_GETFD);
        if (r != -1) n++;
        else if (errno == EBADF) closed++;
    }
    return closed ? n : -1;
}
static int count_fds(void) {
    const int n = count_with(0);
    if (n > 0) return n;
    const int m = count_with(1);
    return m > 0 ? m : -1;
}
static const char *fds_text(int n, char buf[16]) {
    if (n < 0) return "unknown";
    snprintf(buf, 16, "%d", n);
    return buf;
}
typedef struct { char *path; uint64_t size; uint8_t sha[32]; int dir; } NupWant;
static int unhex(const char *h, uint8_t *out) {
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}
/* a file read whole through the layer, in 1 MiB pieces, into SHA-256: 1 when it is the size and bytes wanted */
static int nup_check_file(NupFs *n, uint32_t node, const NupWant *w, uint8_t *buf, uint64_t *bytes) {
    if (node == NUP_NONE || nup_is_dir(n, node) || nup_size(n, node) != w->size) return 0;
    NupSha256 s;
    nup_sha256_init(&s);
    uint64_t at = 0;
    while (at < w->size) {
        const int64_t k = nup_read(n, node, at, buf, 1 << 20);
        if (k <= 0) return 0;
        nup_sha256_update(&s, buf, (size_t)k);
        at += (uint64_t)k;
    }
    *bytes += at;
    uint8_t d[32];
    nup_sha256_final(&s, d);
    return !memcmp(d, w->sha, 32);
}
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
typedef struct {
    NupFs *n;
    const NupWant *w;
    const uint32_t *nodes;
    unsigned count, id;
    double until_ms;
    struct timespec t0;
    uint64_t reads, bytes, whole, whole_bad, errors;
    double max_ms;
} NupThread;
static void *nup_thread(void *arg) {
    NupThread *t = (NupThread *)arg;
    uint8_t *buf = (uint8_t *)malloc(1 << 20);
    uint64_t x = 0x9e3779b97f4a7c15ull * (t->id + 1);
    while (buf && probe_ms_since(t->t0) < t->until_ms) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        const unsigned i = (unsigned)(x % t->count);
        const NupWant *w = &t->w[i];
        const struct timespec r0 = probe_now();
        if ((x >> 32) % 8 == 0 && w->size <= (8u << 20)) {   /* (one read in 8: a whole file, checked again) */
            uint64_t got = 0;
            const int ok = nup_check_file(t->n, t->nodes[i], w, buf, &got);
            t->whole++, t->whole_bad += !ok, t->bytes += got, t->reads += (got >> 20) + 1;
        } else if (w->size) {   /* (else a range: anywhere in the file, 4 KiB to 1 MiB) */
            const uint64_t at = (x >> 20) % w->size, len = 4096u << ((x >> 40) % 9);
            const int64_t k = nup_read(t->n, t->nodes[i], at, buf, len);
            const uint64_t want = w->size - at < len ? w->size - at : len;
            if (k != (int64_t)want) t->errors++;
            else t->bytes += (uint64_t)k;
            t->reads++;
        }
        const double ms = probe_ms_since(r0);
        if (ms > t->max_ms) t->max_ms = ms;
    }
    free(buf);
    return NULL;
}
void check_nup(const char *file, const char *hashes, unsigned threads, unsigned seconds) {
    probe_check_begin("nup", 600);
    const int fds0 = count_fds();
    char why[512], line[1200];
    struct timespec t0 = probe_now();
    NupFs *n = nup_open(file, 0, why, sizeof why);
    const double open_ms = probe_ms_since(t0);
    if (!n) {
        probe_result(0, "nup.open", "%s: %s (upload the package there: dist/README-console.md)", file, why);
        probe_check_end();
        return;
    }
    const int fds1 = count_fds();
    nup_describe(n, line, sizeof line);
    const NupInfo *ni = nup_info(n);
    char b0[16], b1[16], b2[16];
    probe_result((fds0 < 0 || fds1 < 0 || fds1 == fds0 + 1) && !ni->bad && !ni->packed, "nup.open",
                 "%s; descriptors %s before, %s after (one for the package)", line, fds_text(fds0, b0), fds_text(fds1, b1));
    probe_info("nup.index", "index %llu bytes (%.2f MiB: %u nodes of 24 bytes, the hash table, the names); %llu bytes of the TOC read; title %u "
         "of %u; open %.1f ms as timed here", (unsigned long long)ni->index_bytes, (double)ni->index_bytes / MIB, ni->files + ni->dirs,
         (unsigned long long)ni->toc_bytes_read, ni->title_index, ni->titles, open_ms);
    {   /* (the TOC's own SHA-256, as a damaged upload would show: a second open with the check) */
        t0 = probe_now();
        NupFs *v = nup_open(file, NUP_VERIFY_TOC, why, sizeof why);
        if (v) probe_result(1, "nup.toc", "the TOC's SHA-256 matches the package header's (an open with the check: %.0f ms)", probe_ms_since(t0));
        else probe_result(0, "nup.toc", "%s", why);
        if (v) nup_close(v);
    }
    /* the list made on the PC */
    FILE *f = fopen(hashes, "r");
    NupWant *w = NULL;
    unsigned nw = 0, capw = 0, big_want = 0;
    char l[2048], big_dir[600] = "";
    while (f && fgets(l, sizeof l, f)) {
        char kind, hex[80], path[1600];
        unsigned long long size;
        if (l[0] == '#' || sscanf(l, "%c %70s %llu %1500[^\n]", &kind, hex, &size, path) != 4) continue;
        if (kind == 'W') { snprintf(big_dir, sizeof big_dir, "%s", path), big_want = (unsigned)size; continue; }   /* (the folder read whole) */
        if (nw == capw) w = (NupWant *)realloc(w, sizeof *w * (capw = capw ? capw * 2 : 4096));
        NupWant *e = &w[nw];
        if (!unhex(hex, e->sha)) continue;
        e->path = strdup(path), e->size = size, e->dir = kind == 'D';
        nw++;
    }
    if (f) fclose(f);
    if (!nw) {
        probe_result(0, "nup.files", "no list at %s (scripts/ps5/nup/check.sh hashlist, then rebuild the probe)", hashes);
        nup_close(n);
        probe_check_end();
        return;
    }
    uint32_t *nodes = (uint32_t *)malloc(sizeof *nodes * nw);
    uint8_t *buf = (uint8_t *)malloc(1 << 20);
    unsigned files = 0, files_ok = 0, dirs = 0, dirs_ok = 0, big = 0, big_ok = 0;
    uint64_t bytes = 0;
    const size_t big_len = strlen(big_dir);
    t0 = probe_now();
    double lookup_ms = 0;
    for (unsigned i = 0; i < nw; i++) {
        const struct timespec l0 = probe_now();
        nodes[i] = nup_lookup(n, strcmp(w[i].path, ".") ? w[i].path : "");
        lookup_ms += probe_ms_since(l0);
        if (w[i].dir) continue;
        files++;
        const int ok = nup_check_file(n, nodes[i], &w[i], buf, &bytes);
        files_ok += ok;
        if (!ok && files - files_ok <= 10) probe_info("nup.files", "WRONG: %s (%s)", w[i].path, nodes[i] == NUP_NONE ? "not found" : "size or bytes");
        probe_progress("nup: %u files read, %llu MiB", files, (unsigned long long)(bytes >> 20));
        if (big_len && !strncmp(w[i].path, big_dir, big_len) && w[i].path[big_len] == '/' && !strchr(w[i].path + big_len + 1, '/'))
            big++, big_ok += ok;
    }
    const uint32_t big_node = big_len ? nup_lookup(n, big_dir) : NUP_NONE;
    const unsigned big_has = big_node == NUP_NONE ? 0 : nup_child_count(n, big_node);
    const double files_ms = probe_ms_since(t0);
    probe_result(files_ok == files, "nup.files", "%u/%u files right through the layer (size, and the SHA-256 of every byte as the PC's romfs/ has it), "
           "%.1f MB in %.0f ms (%.0f MB/s with the hashing); %u lookups in %.1f ms", files_ok, files, (double)bytes / 1e6, files_ms,
           files_ms > 0 ? (double)bytes / 1e3 / files_ms : 0.0, nw, lookup_ms);
    probe_result(big_len && big_ok == big && big == big_want && big_has == big_want, "nup.folder", "every file of %s: %u/%u right; %u in the list, "
           "%u entries in the package's folder", big_len ? big_dir : "(no W line in the list)", big_ok, big, big_want, big_has);
    /* every folder's listing, as the PC's (the names sorted, each with a newline, hashed) */
    for (unsigned i = 0; i < nw; i++) {
        if (!w[i].dir) continue;
        dirs++;
        const uint32_t d = nodes[i];
        if (d == NUP_NONE || !nup_is_dir(n, d)) continue;
        const uint32_t k = nup_child_count(n, d);
        char **names = (char **)malloc(sizeof *names * (k ? k : 1));
        for (uint32_t j = 0; j < k; j++) {
            names[j] = (char *)malloc(0x301);
            nup_name_copy(n, nup_child(n, d, j), names[j], 0x301);
        }
        qsort(names, k, sizeof *names, name_cmp);
        NupSha256 s;
        nup_sha256_init(&s);
        for (uint32_t j = 0; j < k; j++) nup_sha256_update(&s, names[j], strlen(names[j])), nup_sha256_update(&s, "\n", 1), free(names[j]);
        free(names);
        uint8_t dg[32];
        nup_sha256_final(&s, dg);
        const int ok = k == w[i].size && !memcmp(dg, w[i].sha, 32);
        dirs_ok += ok;
        if (!ok && dirs - dirs_ok <= 10) probe_info("nup.dirs", "DIFFERENT: %s (%u entries, %llu on the PC)", w[i].path, k, (unsigned long long)w[i].size);
    }
    probe_result(dirs_ok == dirs && dirs, "nup.dirs", "%u/%u folders list as on the PC (their entry counts and sorted names)", dirs_ok, dirs);
    /* random reads from several threads at once, through the one descriptor */
    unsigned nf = 0;
    for (unsigned i = 0; i < nw; i++) if (!w[i].dir && nodes[i] != NUP_NONE) w[nf] = w[i], nodes[nf++] = nodes[i];
    NupThread th[32];
    pthread_t tid[32];
    const uint64_t reads0 = nup_reads(n);
    t0 = probe_now();
    for (unsigned i = 0; i < threads; i++) {
        th[i] = (NupThread){.n = n, .w = w, .nodes = nodes, .count = nf, .id = i, .until_ms = seconds * 1000.0, .t0 = t0};
        pthread_create(&tid[i], NULL, nup_thread, &th[i]);
    }
    usleep(seconds * 500000u);
    const int fds_during = count_fds();
    probe_progress("nup: %u threads reading", threads);
    uint64_t reads = 0, rbytes = 0, whole = 0, whole_bad = 0, errors = 0;
    double max_ms = 0;
    for (unsigned i = 0; i < threads; i++) {
        pthread_join(tid[i], NULL);
        reads += th[i].reads, rbytes += th[i].bytes, whole += th[i].whole, whole_bad += th[i].whole_bad, errors += th[i].errors;
        if (th[i].max_ms > max_ms) max_ms = th[i].max_ms;
    }
    const double s = probe_ms_since(t0) / 1e3;
    const int fds_after = count_fds();
    probe_result(!errors && !whole_bad && whole && (fds1 < 0 || (fds_during == fds1 && fds_after == fds1)), "nup.threads",
           "%u threads for %.1f s: %llu reads (%.0f a second), %.0f MiB/s, the slowest %.1f ms; %llu whole files checked again (%llu wrong); "
           "%llu short or failed reads; descriptors %s during and %s after (%s with the package open): the package's one, fd %d, "
           "%llu preads through it", threads, s, (unsigned long long)reads, s > 0 ? reads / s : 0.0,
           s > 0 ? (double)rbytes / MIB / s : 0.0, max_ms, (unsigned long long)whole, (unsigned long long)whole_bad,
           (unsigned long long)errors, fds_text(fds_during, b0), fds_text(fds_after, b2), fds_text(fds1, b1), nup_fd(n),
           (unsigned long long)(nup_reads(n) - reads0));
    nup_close(n);
    probe_info("nup.close", "closed: descriptors %s (%s before the open)", fds_text(count_fds(), b1), fds_text(fds0, b0));
    probe_verdict("nup", "%s", files_ok == files && dirs_ok == dirs && !errors && !whole_bad
                             ? "the game can read its RomFS from game.nup: no romfs/ folder needed on the console"
                             : "the layer's reads differ from the PC's romfs/: see the FAIL lines");
    for (unsigned i = 0; i < nw; i++) free(w[i].path);
    free(w), free(nodes), free(buf);
    probe_check_end();
}
