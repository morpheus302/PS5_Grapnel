/* The .nup RomFS layer (host/nup_fs.c) checked on the PC against an extracted romfs/ folder, with no game code:
     nup_check <file.nup> <romfs folder> [--threads N] [--verify-toc] [--no-contents]
       every file of the package's romfs read through the layer and compared, size and every byte, with the folder's;
       every folder's listing compared both ways (the package's entries with the folder's, and every entry of the folder
       looked up through the layer, its kind and size); the path forms the game may pass (leading slashes, '\', case,
       '.', '..', doubled separators); reads at and past a file's end.
     nup_check --hashlist <file.nup> <romfs folder> <out> [--spread N] [--dir <path>]
       the platform probe's list (scripts/ps5/plat-probe, test nup): N files spread over the package's tree (2000) and every
       file of one folder (Pack/Actor), each with its size and the SHA-256 of the FOLDER's copy, and a line per folder
       checked (its entry count and the SHA-256 of its sorted names). Game-derived: it stays in hookshot-ps5/, never shared.
   Build: cc -O2 -D_GNU_SOURCE -Ihost scripts/ps5/nup/nup_check.c host/nup_fs.c -lpthread (scripts/ps5/nup/check.sh)
   SPDX-License-Identifier: GPL-3.0-or-later */
#include "nup_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static NupFs *nup;
static const char *folder;

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
static char *xstrdup(const char *s) { return strcpy((char *)xmalloc(strlen(s) + 1), s); }

/* ---- the package's tree, as paths ---- */
typedef struct { char *path; uint32_t node; } Entry;
static Entry *files, *dirs;
static size_t nfiles, ndirs, cap_files, cap_dirs;
static void add(Entry **v, size_t *n, size_t *cap, const char *path, uint32_t node) {
    if (*n == *cap) *v = (Entry *)realloc(*v, sizeof **v * (*cap = *cap ? *cap * 2 : 1024));
    (*v)[*n].path = xstrdup(path), (*v)[(*n)++].node = node;
}
static void walk(uint32_t dir, const char *prefix) {
    add(&dirs, &ndirs, &cap_dirs, prefix, dir);
    const uint32_t count = nup_child_count(nup, dir);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t c = nup_child(nup, dir, i);
        char name[1024], path[4096];
        nup_name_copy(nup, c, name, sizeof name);
        snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", name);
        if (nup_is_dir(nup, c)) walk(c, path);
        else add(&files, &nfiles, &cap_files, path, c);
    }
}
static int by_path(const void *a, const void *b) { return strcmp(((const Entry *)a)->path, ((const Entry *)b)->path); }

/* ---- contents: every file, every byte ---- */
static volatile size_t next_file;
static volatile uint64_t bytes_done, files_same, files_diff;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int bad_lines;
static void bad(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bad(const char *fmt, ...) {
    pthread_mutex_lock(&mu);
    if (bad_lines++ < 50) {
        va_list ap;
        va_start(ap, fmt);
        fputs("  BAD ", stdout);
        vprintf(fmt, ap);
        fputc('\n', stdout);
        va_end(ap);
    }
    pthread_mutex_unlock(&mu);
}
static void *contents(void *unused) {
    (void)unused;
    const size_t chunk = 8u << 20;
    uint8_t *a = (uint8_t *)xmalloc(chunk), *b = (uint8_t *)xmalloc(chunk);
    for (;;) {
        const size_t i = __atomic_fetch_add(&next_file, 1, __ATOMIC_RELAXED);
        if (i >= nfiles) break;
        const Entry *e = &files[i];
        char path[4200];
        snprintf(path, sizeof path, "%s/%s", folder, e->path);
        const int fd = open(path, O_RDONLY);
        struct stat st;
        int same = fd >= 0 && !fstat(fd, &st) && (uint64_t)st.st_size == nup_size(nup, e->node) && nup_readable(nup, e->node);
        if (!same) bad("%s: %s", e->path, fd < 0 ? "not in the folder" : !nup_readable(nup, e->node) ? nup_why_unreadable(nup, e->node) : "size differs");
        uint64_t at = 0;
        while (same && at < (uint64_t)st.st_size) {
            const int64_t k = nup_read(nup, e->node, at, a, chunk);
            const ssize_t g = pread(fd, b, chunk, (off_t)at);
            if (k <= 0 || g != k || memcmp(a, b, (size_t)k)) { bad("%s: bytes differ near %llu", e->path, (unsigned long long)at); same = 0; break; }
            at += (uint64_t)k;
            __atomic_fetch_add(&bytes_done, (uint64_t)k, __ATOMIC_RELAXED);
        }
        if (fd >= 0) close(fd);
        __atomic_fetch_add(same ? &files_same : &files_diff, 1, __ATOMIC_RELAXED);
    }
    free(a), free(b);
    return NULL;
}

/* ---- listings ---- */
typedef struct { char name[512]; int dir; } Name;
static int by_name(const void *a, const void *b) { return strcmp(((const Name *)a)->name, ((const Name *)b)->name); }
static size_t folder_names(const char *rel, Name **out) {
    char path[4200];
    snprintf(path, sizeof path, "%s/%s", folder, rel);
    DIR *d = opendir(path);
    size_t n = 0, cap = 0;
    *out = NULL;
    if (!d) return (size_t)-1;
    for (struct dirent *de; (de = readdir(d));) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) *out = (Name *)realloc(*out, sizeof **out * (cap = cap ? cap * 2 : 64));
        snprintf((*out)[n].name, sizeof (*out)[n].name, "%s", de->d_name);
        char full[4800];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat st;
        (*out)[n++].dir = !stat(full, &st) && S_ISDIR(st.st_mode);
    }
    closedir(d);
    qsort(*out, n, sizeof **out, by_name);
    return n;
}
static size_t package_names(uint32_t dir, Name **out) {
    const uint32_t n = nup_child_count(nup, dir);
    *out = (Name *)xmalloc(sizeof **out * (n ? n : 1));
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t c = nup_child(nup, dir, i);
        nup_name_copy(nup, c, (*out)[i].name, sizeof (*out)[i].name);
        (*out)[i].dir = nup_is_dir(nup, c);
    }
    qsort(*out, n, sizeof **out, by_name);
    return n;
}
/* the folder's tree, every entry looked up through the layer */
static uint64_t looked_files, looked_dirs, look_bad;
static void folder_walk(const char *rel) {
    Name *v;
    const size_t n = folder_names(rel, &v);
    if (n == (size_t)-1) { bad("folder %s: cannot list it", rel); look_bad++; return; }
    for (size_t i = 0; i < n; i++) {
        char sub[4200], full[4800];
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", v[i].name);
        const uint32_t node = nup_lookup(nup, sub);
        if (node == NUP_NONE) { bad("folder entry %s: not in the package", sub); look_bad++; continue; }
        if (nup_is_dir(nup, node) != v[i].dir) { bad("folder entry %s: a %s in the package", sub, v[i].dir ? "file" : "folder"); look_bad++; continue; }
        if (v[i].dir) {
            looked_dirs++;
            folder_walk(sub);
        } else {
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", folder, sub);
            if (stat(full, &st) || (uint64_t)st.st_size != nup_size(nup, node)) { bad("folder entry %s: size differs", sub); look_bad++; }
            else looked_files++;
        }
    }
    free(v);
}

/* ---- path forms ---- */
static int forms_ok, forms_bad;
static void expect(const char *what, const char *path, uint32_t want) {
    const uint32_t got = nup_lookup(nup, path);
    if (got == want) forms_ok++;
    else forms_bad++, bad("path form %s (\"%s\"): node %u, %u expected", what, path, got, want);
}
static void path_forms(void) {
    for (size_t k = 0; k < nfiles; k += nfiles / 200 + 1) {
        const Entry *e = &files[k];
        char p[4300];
        expect("as listed", e->path, e->node);
        snprintf(p, sizeof p, "/%s", e->path), expect("leading /", p, e->node);
        snprintf(p, sizeof p, "///%s", e->path), expect("leading ///", p, e->node);
        snprintf(p, sizeof p, "%s", e->path);
        for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
        expect("backslashes", p, e->node);
        snprintf(p, sizeof p, "%s", e->path);
        for (char *c = p; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        expect("upper case", p, e->node);
        snprintf(p, sizeof p, "./%s", e->path), expect("./", p, e->node);
        const char *slash = strrchr(e->path, '/');
        if (slash) {
            snprintf(p, sizeof p, "%.*s//./%s", (int)(slash - e->path), e->path, slash + 1), expect("//./", p, e->node);
            snprintf(p, sizeof p, "%.*s/../%s", (int)(strchr(e->path, '/') - e->path), e->path, e->path), expect("x/../", p, e->node);
        }
        snprintf(p, sizeof p, "%s/", e->path), expect("a file with a trailing /", p, NUP_NONE);
        snprintf(p, sizeof p, "%s.missing", e->path), expect("a missing name", p, NUP_NONE);
        snprintf(p, sizeof p, "%s/x", e->path), expect("below a file", p, NUP_NONE);
    }
    for (size_t k = 0; k < ndirs; k += ndirs / 50 + 1) {
        char p[4300];
        snprintf(p, sizeof p, "%s/", dirs[k].path), expect("a folder with a trailing /", p, dirs[k].node);
        snprintf(p, sizeof p, "/%s", dirs[k].path), expect("a folder, leading /", p, dirs[k].node);
    }
    expect("the root", "", NUP_ROOT);
    expect("the root, /", "/", NUP_ROOT);
    expect("the root, ..", "..", NUP_ROOT);
}
/* reads at and past a file's end, and in pieces */
static int edges_ok, edges_bad;
static void edges(void) {
    uint8_t buf[4096], whole[8192];
    for (size_t k = 0; k < nfiles; k += nfiles / 100 + 1) {
        const Entry *e = &files[k];
        const uint64_t size = nup_size(nup, e->node);
        int ok = nup_read(nup, e->node, size, buf, 10) == 0 && nup_read(nup, e->node, size + 100, buf, 10) == 0;
        if (size) {
            const int64_t tail = nup_read(nup, e->node, size - 1, buf, sizeof buf);
            ok &= tail == 1;
            const uint64_t take = size < sizeof whole ? size : sizeof whole;
            ok &= nup_read(nup, e->node, 0, whole, take) == (int64_t)take;
            for (uint64_t at = 0; ok && at < take; at += 7) {   /* (in pieces of 7: the same bytes) */
                const uint64_t n = take - at < 7 ? take - at : 7;
                ok &= nup_read(nup, e->node, at, buf, n) == (int64_t)n && !memcmp(buf, whole + at, (size_t)n);
            }
        }
        if (ok) edges_ok++;
        else edges_bad++, bad("edge reads of %s", e->path);
    }
}

/* ---- the probe's hash list ---- */
static void hex(const uint8_t *d, char *out) {
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}
static int folder_sha(const char *rel, uint64_t *size, char *out) {
    char path[4200];
    snprintf(path, sizeof path, "%s/%s", folder, rel);
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    static uint8_t b[1 << 20];
    NupSha256 s;
    nup_sha256_init(&s);
    *size = 0;
    for (ssize_t k; (k = read(fd, b, sizeof b)) > 0;) nup_sha256_update(&s, b, (size_t)k), *size += (uint64_t)k;
    close(fd);
    uint8_t d[32];
    nup_sha256_final(&s, d);
    hex(d, out);
    return 1;
}
static void dir_line(FILE *f, const char *rel) {
    Name *v;
    const size_t n = folder_names(rel, &v);
    NupSha256 s;
    nup_sha256_init(&s);
    for (size_t i = 0; i < n; i++) nup_sha256_update(&s, v[i].name, strlen(v[i].name)), nup_sha256_update(&s, "\n", 1);
    uint8_t d[32];
    char h[65];
    nup_sha256_final(&s, d);
    hex(d, h);
    fprintf(f, "D %s %zu %s\n", h, n, *rel ? rel : ".");
    free(v);
}
static int hashlist(const char *out, unsigned spread, const char *dir) {
    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 2; }
    fprintf(f, "# the platform probe's check of the .nup layer (test nup): F <sha256> <size> <path> for a file, D <sha256 of the sorted\n"
               "# names, each followed by a newline> <entries> <path> for a folder, W 0 <files> <path> for the folder read whole.\n"
               "# From the extracted romfs/ folder on the PC.\n"
               "# Game-derived: keep it in hookshot-ps5/, never share it.\n");
    fprintf(f, "# package: title %016llx v%u, %u files, %u folders\n", (unsigned long long)nup_info(nup)->title_id, nup_info(nup)->version,
            nup_info(nup)->files, nup_info(nup)->dirs);
    unsigned nf = 0, nd = 0;
    uint64_t total = 0;
    char h[65];
    const size_t dl = strlen(dir);
#define IN_DIR(p) (!strncmp((p), dir, dl) && (p)[dl] == '/' && !strchr((p) + dl + 1, '/'))
    size_t others = 0;   /* (the spread files are taken among the files not in the folder read whole) */
    for (size_t k = 0; k < nfiles; k++) others += !IN_DIR(files[k].path);
    const size_t step = others / (spread ? spread : 1) ? others / (spread ? spread : 1) : 1;
    for (size_t k = 0, o = 0; k < nfiles && nf < spread; k++) {
        if (IN_DIR(files[k].path)) continue;
        if (o++ % step != step / 2) continue;
        uint64_t size;
        if (!folder_sha(files[k].path, &size, h)) { fprintf(stderr, "%s: not in the folder\n", files[k].path); return 1; }
        fprintf(f, "F %s %llu %s\n", h, (unsigned long long)size, files[k].path);
        nf++, total += size;
    }
    unsigned in_dir = 0, want_dir = 0;
    for (size_t k = 0; k < nfiles; k++) want_dir += IN_DIR(files[k].path);
    fprintf(f, "W %064d %u %s\n", 0, want_dir, dir);   /* (the folder read whole: its file count) */
    for (size_t k = 0; k < nfiles; k++) {
        if (!IN_DIR(files[k].path)) continue;
        uint64_t size;
        if (!folder_sha(files[k].path, &size, h)) { fprintf(stderr, "%s: not in the folder\n", files[k].path); return 1; }
        fprintf(f, "F %s %llu %s\n", h, (unsigned long long)size, files[k].path);
        nf++, in_dir++, total += size;
    }
    for (size_t k = 0; k < ndirs; k++) {   /* (every folder of the tree: 599 lines) */
        dir_line(f, dirs[k].path);
        nd++;
    }
    fclose(f);
    printf("hash list %s: %u files (%u spread, %u of %s), %.1f MB of them; %u folders\n", out, nf, nf - in_dir, in_dir, dir,
           (double)total / 1e6, nd);
    return in_dir ? 0 : 1;
#undef IN_DIR
}

int main(int argc, char **argv) {
    const char *list_out = NULL, *dir = "Pack/Actor", *pos[2] = {0};
    unsigned threads = 4, spread = 2000, verify = 0, skip_contents = 0, npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hashlist") && i + 3 < argc) pos[0] = argv[++i], pos[1] = argv[++i], list_out = argv[++i], npos = 2;
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spread") && i + 1 < argc) spread = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--verify-toc")) verify = 1;
        else if (!strcmp(argv[i], "--no-contents")) skip_contents = 1;
        else if (npos < 2) pos[npos++] = argv[i];
    }
    if (npos < 2) { fprintf(stderr, "usage: nup_check <file.nup> <romfs folder> [--threads N] [--verify-toc] [--no-contents]\n"
                                    "       nup_check --hashlist <file.nup> <romfs folder> <out> [--spread N] [--dir <path>]\n"); return 2; }
    if (threads < 1) threads = 1;
    if (threads > 4) threads = 4;
    folder = pos[1];
    char why[512];
    nup = nup_open(pos[0], verify ? NUP_VERIFY_TOC : 0, why, sizeof why);
    if (!nup) { printf("open: %s: %s\n", pos[0], why); return 1; }
    const NupInfo *info = nup_info(nup);
    char line[1024];
    nup_describe(nup, line, sizeof line);
    printf("open: %s\n", line);
    printf("open: index %llu bytes (%.2f MiB); %llu bytes of the TOC read; %u titles, title %u chosen; %u packed, %u bad; descriptor %d\n",
           (unsigned long long)info->index_bytes, (double)info->index_bytes / (1 << 20), (unsigned long long)info->toc_bytes_read,
           info->titles, info->title_index, info->packed, info->bad, nup_fd(nup));
    walk(NUP_ROOT, "");
    qsort(files, nfiles, sizeof *files, by_path);
    qsort(dirs, ndirs, sizeof *dirs, by_path);
    if (list_out) return hashlist(list_out, spread, dir);
    int fail = 0;
    /* listings, the package's against the folder's */
    size_t list_same = 0, list_diff = 0;
    for (size_t k = 0; k < ndirs; k++) {
        Name *a, *b;
        const size_t na = package_names(dirs[k].node, &a), nb = folder_names(dirs[k].path, &b);
        int same = na == nb;
        for (size_t i = 0; same && i < na; i++) same = !strcmp(a[i].name, b[i].name) && a[i].dir == b[i].dir;
        if (same) list_same++;
        else list_diff++, bad("listing of %s: %zu entries in the package, %zu in the folder, or names/kinds differ", dirs[k].path, na,
                              nb == (size_t)-1 ? 0 : nb);
        free(a), free(b);
    }
    printf("listings, package -> folder: %zu/%zu folders the same\n", list_same, ndirs);
    fail |= list_diff != 0;
    folder_walk("");
    printf("listings, folder -> package: %llu files and %llu folders of the folder found through the layer with the same kind and size, "
           "%llu not\n", (unsigned long long)looked_files, (unsigned long long)looked_dirs, (unsigned long long)look_bad);
    fail |= look_bad != 0 || looked_files != nfiles || looked_dirs + 1 != ndirs;
    path_forms();
    printf("path forms: %d right, %d wrong\n", forms_ok, forms_bad);
    edges();
    printf("reads at the ends and in pieces: %d files right, %d wrong\n", edges_ok, edges_bad);
    fail |= forms_bad || edges_bad;
    if (!skip_contents) {
        const double t0 = now_s();
        pthread_t t[4];
        for (unsigned i = 0; i < threads; i++) pthread_create(&t[i], NULL, contents, NULL);
        for (;;) {
            sleep(15);
            const size_t at = next_file;
            printf("  contents: %zu/%zu files, %.1f GB, %.0f s\n", at < nfiles ? at : nfiles, nfiles, (double)bytes_done / 1e9, now_s() - t0);
            fflush(stdout);
            if (at >= nfiles) break;
        }
        for (unsigned i = 0; i < threads; i++) pthread_join(t[i], NULL);
        const double s = now_s() - t0;
        printf("contents: %llu/%zu files the same, size and every byte (%llu differ); %.2f GB compared in %.0f s with %u threads (%.0f MB/s "
               "from the package)\n", (unsigned long long)files_same, nfiles, (unsigned long long)files_diff, (double)bytes_done / 1e9, s,
               threads, (double)bytes_done / 1e6 / s);
        fail |= files_same != nfiles;
    }
    printf("descriptor: %d, %llu reads through it\n", nup_fd(nup), (unsigned long long)nup_reads(nup));
    printf("%s\n", fail ? "RESULT: FAIL" : "RESULT: PASS");
    nup_close(nup);
    return fail;
}
