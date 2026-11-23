/* The game's own nn::fs entry points (host/nn_fs.c, host/mods.c, host/nup_fs.c and host/posix/win32_posix.c, compiled as the
   Linux build compiles them) run on the PC with no game code, over two RomFS mounts made the game's way (MountRom): "fold",
   the extracted romfs/ folder, and "pack", the same RomFS read from the .nup package (hos_fs_use_nup). Every folder of the
   tree is listed through both (OpenDirectory in each mode, GetDirectoryEntryCount, ReadDirectory in batches) and every
   file opened, sized and read whole into guest memory through both (OpenFile, GetFileSize, ReadFile), GetEntryType of
   each, and the answers compared. A test mod (HOS_MODS: one file replaced, one added to a folder, one folder of its own)
   is over both mounts and checked on each, and 1000 files of the package are held open at once with no descriptor more.
     nnfs_check <file.nup> <romfs folder> [--threads N] [--sample K]   (K: every K-th file read, 1 = all)
   Built by scripts/ps5/nup/check.sh. SPDX-License-Identifier: GPL-3.0-or-later */
#include "hos.h"
#include "nup_fs.h"

#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- the runtime's hooks nn_fs.c and mods.c call, here plain ---- */
int hos_trace;
void hos_log(const char *fmt, ...) {
    static int quiet = -1;
    if (quiet < 0) quiet = getenv("NNFS_LOG") == NULL;
    va_list ap;
    va_start(ap, fmt);
    char line[2048];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!quiet || !strncmp(line, "fs: mounted", 11) || !strncmp(line, "mods: ", 6) || strstr(line, "NOT"))
        fprintf(stdout, "  [hos] %s\n", line);
}
void hos_fatal(Ctx *c, const char *fmt, ...) {
    (void)c;
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    exit(3);
}
void hos_mark_written(const void *p, size_t n) { (void)p, (void)n; }
int64_t hos_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}
void hos_sleep_ns(int64_t ns) { (void)ns; }
int hos_vcore_away(void) { return 0; }
void hos_vcore_back(void) {}
int hos_blank_cached_icon(const char *guest, const char *host) { (void)guest, (void)host; return 0; }
int hos_ultrawide_redirect(const char *guest, char *out, size_t cap) { (void)guest, (void)out, (void)cap; return 0; }
extern const char *hos_romfs_dir;
void hos_mods_init(const char *image_path);

/* ---- nn::fs, as the game calls it ---- */
#define FS(sym) void imp_##sym(Ctx *c);
FS(_ZN2nn2fs8MountRomEPKc)
FS(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci)
FS(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm)
FS(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm)
FS(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE)
FS(_ZN2nn2fs9CloseFileENS0_10FileHandleE)
FS(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc)
FS(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci)
FS(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl)
FS(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE)
FS(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE)

/* guest memory: a scratch range of the window, per thread */
#define SCRATCH 0x10000000ull
#define PER_THREAD (24ull << 20)
#define CHUNK (8u << 20)
typedef struct { Ctx c; uint64_t base; } T;   /* base: str at +0, out values at +4 KiB, entries at +64 KiB, data at +8 MiB */
static uint64_t str(T *t, const char *s) { snprintf((char *)GP(t->base), 4096, "%s", s); return t->base; }
static uint64_t outv(T *t, int k) { return t->base + 4096 + 8 * (uint64_t)k; }
#define CALL(sym, ...) do { uint64_t a_[] = {__VA_ARGS__}; for (size_t i_ = 0; i_ < sizeof a_ / 8; i_++) t->c.x[i_] = a_[i_]; \
                            imp_##sym(&t->c); } while (0)

static int entry_type(T *t, const char *g, int *type) {   /* the result; type 0 folder 1 file */
    CALL(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc, outv(t, 0), str(t, g));
    *type = (int)G32(outv(t, 0));
    return (int)t->c.x[0];
}
typedef struct { char name[0x301]; int type; uint64_t size; } Ent;
static int by_name(const void *a, const void *b) { return strcmp(((const Ent *)a)->name, ((const Ent *)b)->name); }
/* a listing: the result, the count GetDirectoryEntryCount gave, the entries read (sorted) */
static int list(T *t, const char *g, int mode, int64_t *count, Ent **out, size_t *n) {
    *out = NULL, *n = 0, *count = -1;
    CALL(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci, outv(t, 1), str(t, g), (uint64_t)mode);
    if (t->c.x[0]) return (int)t->c.x[0];
    const uint64_t h = G64(outv(t, 1));
    CALL(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE, outv(t, 2), h);
    *count = (int64_t)G64(outv(t, 2));
    size_t cap = 0;
    for (;;) {   /* (batches of 7: the handle's place kept between calls) */
        const uint64_t ents = t->base + 65536;
        CALL(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl, outv(t, 3), ents, h, 7);
        const int64_t got = (int64_t)G64(outv(t, 3));
        if (got <= 0) break;
        for (int64_t i = 0; i < got; i++) {
            if (*n == cap) *out = (Ent *)realloc(*out, sizeof **out * (cap = cap ? cap * 2 : 64));
            Ent *e = &(*out)[(*n)++];
            snprintf(e->name, sizeof e->name, "%s", (const char *)GP(ents + 0x310 * (uint64_t)i));
            e->type = *(uint8_t *)GP(ents + 0x310 * (uint64_t)i + 0x304);
            e->size = G64(ents + 0x310 * (uint64_t)i + 0x308);
        }
    }
    CALL(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE, h);
    qsort(*out, *n, sizeof **out, by_name);
    return 0;
}
/* a file read whole into guest memory: the result, its size, and the SHA-256 of what was read */
static int read_whole(T *t, const char *g, uint64_t *size, uint8_t digest[32], uint64_t *read_bytes) {
    CALL(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci, outv(t, 4), str(t, g), 1);
    if (t->c.x[0]) return (int)t->c.x[0];
    const uint64_t h = G64(outv(t, 4));
    CALL(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE, outv(t, 5), h);
    *size = G64(outv(t, 5));
    NupSha256 s;
    nup_sha256_init(&s);
    const uint64_t buf = t->base + (8u << 20);
    uint64_t at = 0;
    *read_bytes = 0;
    for (;;) {
        CALL(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm, outv(t, 6), h, at, buf, CHUNK);
        const uint64_t got = G64(outv(t, 6));
        if (!got) break;
        nup_sha256_update(&s, GP(buf), (size_t)got);
        at += got, *read_bytes += got;
    }
    /* the must-read-all form past the end: ResultOutOfRange */
    CALL(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm, h, *size, buf, 16);
    const int past = (int)t->c.x[0];
    CALL(_ZN2nn2fs9CloseFileENS0_10FileHandleE, h);
    nup_sha256_final(&s, digest);
    return past == 0x2EE202 || (*size == 0 && past == 0) ? 0 : -1;
}

/* ---- the tree, from the package (every path checked through both mounts) ---- */
static NupFs *pkg;
typedef struct { char *path; int dir; } Path;
static Path *paths;
static size_t npaths, cap_paths;
static void walk(uint32_t d, const char *prefix) {
    if (npaths == cap_paths) paths = (Path *)realloc(paths, sizeof *paths * (cap_paths = cap_paths ? cap_paths * 2 : 4096));
    paths[npaths].path = strdup(prefix), paths[npaths++].dir = 1;
    for (uint32_t i = 0, k = nup_child_count(pkg, d); i < k; i++) {
        const uint32_t e = nup_child(pkg, d, i);
        char name[1024], p[4096];
        nup_name_copy(pkg, e, name, sizeof name);
        snprintf(p, sizeof p, "%s%s%s", prefix, *prefix ? "/" : "", name);
        if (nup_is_dir(pkg, e)) { walk(e, p); continue; }
        if (npaths == cap_paths) paths = (Path *)realloc(paths, sizeof *paths * (cap_paths *= 2));
        paths[npaths].path = strdup(p), paths[npaths++].dir = 0;
    }
}

static volatile size_t next;
static volatile uint64_t files_same, files_diff, dirs_same, dirs_diff, bytes_read, types_same, types_diff;
static unsigned sample = 1;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int bad_lines;
static void bad(const char *fmt, ...) {
    pthread_mutex_lock(&mu);
    if (bad_lines++ < 40) {
        va_list ap;
        va_start(ap, fmt);
        fputs("  BAD ", stdout);
        vprintf(fmt, ap);
        fputc('\n', stdout);
        va_end(ap);
    }
    pthread_mutex_unlock(&mu);
}
static int same_listing(const Ent *a, size_t na, const Ent *b, size_t nb) {
    if (na != nb) return 0;
    for (size_t i = 0; i < na; i++)   /* (a folder's own size: the host's stat says 4096 on Linux, the package and Windows 0) */
        if (strcmp(a[i].name, b[i].name) || a[i].type != b[i].type || (a[i].type == 1 && a[i].size != b[i].size)) return 0;
    return 1;
}
static void *worker(void *arg) {
    T *t = (T *)arg;
    for (;;) {
        const size_t i = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
        if (i >= npaths) break;
        const Path *p = &paths[i];
        char gf[4200], gp[4200];
        snprintf(gf, sizeof gf, "fold:/%s", p->path);
        snprintf(gp, sizeof gp, "pack:/%s", p->path);
        int tf = -1, tp = -1;
        const int rf = entry_type(t, gf, &tf), rp = entry_type(t, gp, &tp);
        if (rf == rp && (rf || tf == tp) && !rf && tf == !p->dir) __atomic_fetch_add(&types_same, 1, __ATOMIC_RELAXED);
        else __atomic_fetch_add(&types_diff, 1, __ATOMIC_RELAXED), bad("GetEntryType(%s): fold %#x/%d, pack %#x/%d", p->path, rf, tf, rp, tp);
        if (p->dir) {
            int ok = 1;
            for (int mode = 1; mode <= 3; mode++) {
                Ent *a, *b;
                size_t na, nb;
                int64_t ca, cb;
                const int ra = list(t, gf, mode, &ca, &a, &na), rb = list(t, gp, mode, &cb, &b, &nb);
                if (ra || rb || ca != cb || ca != (int64_t)na || !same_listing(a, na, b, nb)) {
                    ok = 0;
                    bad("listing of %s, mode %d: fold %#x %lld/%zu, pack %#x %lld/%zu", p->path, mode, ra, (long long)ca, na, rb, (long long)cb, nb);
                }
                free(a), free(b);
            }
            __atomic_fetch_add(ok ? &dirs_same : &dirs_diff, 1, __ATOMIC_RELAXED);
            /* (a folder opened as a file, and listed below as one: not found through both) */
            CALL(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci, outv(t, 7), str(t, gp), 1);
            const int fp = (int)t->c.x[0];
            CALL(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci, outv(t, 7), str(t, gf), 1);
            if (fp != 0x202 || t->c.x[0] != 0x202) bad("OpenFile of the folder %s: fold %#llx, pack %#x", p->path, (unsigned long long)t->c.x[0], fp);
            continue;
        }
        if (i % sample) continue;
        uint64_t sf = 0, sp = 0, bf = 0, bp = 0;
        uint8_t df[32], dp[32];
        const int rf2 = read_whole(t, gf, &sf, df, &bf), rp2 = read_whole(t, gp, &sp, dp, &bp);
        if (!rf2 && !rp2 && sf == sp && bf == sf && bp == sp && !memcmp(df, dp, 32)) __atomic_fetch_add(&files_same, 1, __ATOMIC_RELAXED);
        else __atomic_fetch_add(&files_diff, 1, __ATOMIC_RELAXED), bad("%s: fold %#x %llu/%llu, pack %#x %llu/%llu, %s", p->path, rf2,
                                                                       (unsigned long long)bf, (unsigned long long)sf, rp2, (unsigned long long)bp,
                                                                       (unsigned long long)sp, memcmp(df, dp, 32) ? "bytes differ" : "same bytes");
        __atomic_fetch_add(&bytes_read, bp, __ATOMIC_RELAXED);
        if (i % 97 == 0) {   /* (a listing below a file, a missing name: not found through both) */
            char m[4300];
            Ent *a;
            size_t na;
            int64_t ca;
            const int la = list(t, gp, 3, &ca, &a, &na);
            free(a);
            snprintf(m, sizeof m, "%s.missing", gp);
            int ty;
            const int me = entry_type(t, m, &ty);
            if (la != 0x202 || me != 0x202) bad("%s: listed as a folder %#x, a missing name %#x", p->path, la, me);
        }
    }
    return NULL;
}

static int count_fds(void) {
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    for (struct dirent *e; (e = readdir(d));) n += e->d_name[0] != '.';
    closedir(d);
    return n - 1;   /* (opendir's own) */
}
static void put_file(const char *path, const char *text) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    FILE *f = fopen(path, "w");
    if (f) fputs(text, f), fclose(f);
}

int main(int argc, char **argv) {
    const char *nup = NULL, *folder = NULL;
    unsigned threads = 4;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sample") && i + 1 < argc) sample = (unsigned)atoi(argv[++i]);
        else if (!nup) nup = argv[i];
        else folder = argv[i];
    }
    if (!nup || !folder) { fprintf(stderr, "usage: nnfs_check <file.nup> <romfs folder> [--threads N] [--sample K]\n"); return 2; }
    if (threads < 1) threads = 1;
    if (threads > 4) threads = 4;
    if (!sample) sample = 1;
    if (mmap(GP(SCRATCH), PER_THREAD * (threads + 1), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) {
        perror("the guest scratch range");
        return 2;
    }
    T *ts = (T *)calloc(threads + 1, sizeof *ts);
    for (unsigned i = 0; i <= threads; i++) ts[i].base = SCRATCH + PER_THREAD * i;
    T *t = &ts[threads];
    int fail = 0;
    /* the test mod */
    char mods[4096], file0[4096];
    snprintf(mods, sizeof mods, "%s/nnfs-mods-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    /* the two mounts: the folder first (no package yet), then the package */
    hos_romfs_dir = folder;
    CALL(_ZN2nn2fs8MountRomEPKc, str(t, "fold"));
    char line[1200];
    const double t0 = (double)hos_now_ns() / 1e6;
    const int fds0 = count_fds();
    if (!hos_fs_use_nup(nup, line, sizeof line)) { printf("package: %s\n", line); return 1; }
    printf("package: %s (hos_fs_use_nup: %.0f ms; descriptors %d -> %d)\n", line, (double)hos_now_ns() / 1e6 - t0, fds0, count_fds());
    CALL(_ZN2nn2fs8MountRomEPKc, str(t, "pack"));
    pkg = hos_romfs_nup;
    walk(NUP_ROOT, "");
    /* a mod over both: Pack/Actor's first file replaced, a file added to Pack/Actor, a folder of its own */
    const uint32_t actor = nup_lookup(pkg, "Pack/Actor");
    char first[1024];
    nup_name_copy(pkg, nup_child(pkg, actor, 0), first, sizeof first);
    snprintf(file0, sizeof file0, "%s/01-test/romfs/Pack/Actor/%s", mods, first), put_file(file0, "MODDED");
    snprintf(file0, sizeof file0, "%s/01-test/romfs/Pack/Actor/zz_mod_only.txt", mods), put_file(file0, "mod only");
    snprintf(file0, sizeof file0, "%s/01-test/romfs/ModOnly/hello.txt", mods), put_file(file0, "hello");
    setenv("HOS_MODS", mods, 1);
    hos_mods_init(NULL);
    {
        int ok = 1;
        for (int k = 0; k < 2; k++) {
            const char *m = k ? "pack" : "fold";
            char g[1200];
            uint64_t size, got;
            uint8_t d[32], want[32];
            NupSha256 s;
            nup_sha256_init(&s), nup_sha256_update(&s, "MODDED", 6), nup_sha256_final(&s, want);
            snprintf(g, sizeof g, "%s:/Pack/Actor/%s", m, first);
            ok &= !read_whole(t, g, &size, d, &got) && size == 6 && !memcmp(d, want, 32);
            Ent *e;
            size_t n;
            int64_t count;
            snprintf(g, sizeof g, "%s:/Pack/Actor", m);
            ok &= !list(t, g, 3, &count, &e, &n) && count == (int64_t)n && n == nup_child_count(pkg, actor) + 1;
            int firsts = 0, extra = 0;
            for (size_t i = 0; i < n; i++) firsts += !strcmp(e[i].name, first), extra += !strcmp(e[i].name, "zz_mod_only.txt");
            ok &= firsts == 1 && extra == 1;
            free(e);
            int ty = -1;
            snprintf(g, sizeof g, "%s:/ModOnly", m);
            ok &= entry_type(t, g, &ty) == 0 && ty == 0;
            ok &= !list(t, g, 3, &count, &e, &n) && n == 1 && count == 1 && !strcmp(e[0].name, "hello.txt");
            free(e);
            snprintf(g, sizeof g, "%s:/", m);
            ok &= !list(t, g, 1, &count, &e, &n) && n == nup_child_count(pkg, NUP_ROOT) + 1;
            free(e);
            printf("mods over %s: %s\n", m, ok ? "a file replaced, a file added to a folder, a folder of its own: all seen" : "WRONG");
        }
        fail |= !ok;
    }
    /* 1000 files of the package open at once: no descriptor more */
    {
        const int before = count_fds();
        uint64_t hs[1000];
        unsigned opened = 0;
        for (size_t i = 0; i < npaths && opened < 1000; i++) {
            if (paths[i].dir) continue;
            char g[4200];
            snprintf(g, sizeof g, "pack:/%s", paths[i].path);
            CALL(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci, outv(t, 8), str(t, g), 1);
            if (!t->c.x[0]) hs[opened++] = G64(outv(t, 8));
        }
        const int during = count_fds();
        for (unsigned i = 0; i < opened; i++) CALL(_ZN2nn2fs9CloseFileENS0_10FileHandleE, hs[i]);
        printf("open at once through OpenFile: %u files of the package; descriptors %d before, %d with them open\n", opened, before, during);
        fail |= opened != 1000 || during != before;
    }
    /* every path through both */
    const double s0 = (double)hos_now_ns() / 1e9;
    pthread_t th[4];
    for (unsigned i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker, &ts[i]);
    for (unsigned i = 0; i < threads; i++) pthread_join(th[i], NULL);
    const double secs = (double)hos_now_ns() / 1e9 - s0;
    size_t nfiles = 0;
    for (size_t i = 0; i < npaths; i++) nfiles += !paths[i].dir;
    printf("GetEntryType: %llu/%zu paths the same through both mounts (%llu differ)\n", (unsigned long long)types_same, npaths,
           (unsigned long long)types_diff);
    printf("listings: %llu/%llu folders the same through both mounts, in modes 1, 2 and 3, with GetDirectoryEntryCount (%llu differ)\n",
           (unsigned long long)dirs_same, (unsigned long long)(dirs_same + dirs_diff), (unsigned long long)dirs_diff);
    printf("files: %llu/%llu read whole through both mounts, the same size and bytes (%llu differ); %.2f GB from the package; %.0f s, "
           "%u threads%s\n", (unsigned long long)files_same, (unsigned long long)(files_same + files_diff), (unsigned long long)files_diff,
           (double)bytes_read / 1e9, secs, threads, sample > 1 ? " (a sample)" : "");
    fail |= types_diff || dirs_diff || files_diff || types_same != npaths || (sample == 1 && files_same != nfiles);
    printf("descriptor: %d, %llu reads through it\n", nup_fd(pkg), (unsigned long long)nup_reads(pkg));
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", mods);
    if (system(cmd)) {}
    printf("%s\n", fail ? "RESULT: FAIL" : "RESULT: PASS");
    return fail;
}
