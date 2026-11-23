/* The game's RomFS read in place from the user's .nup package: host/nup_fs.h says what and why. The package's layout is
   Hookshot's nx2/nup.py's (little-endian):
     0x000  "NUP\x01"; u64 TOC offset at +0x08 and size at +0x10; the TOC's SHA-256 at +0x18; u64 data start at +0x38;
            u64 package size at +0x40
     0x080  "TOC\x01", u32 titles, u64 dirs, u64 files, u64 strings offset (from 0x80), u64 strings size
     0x0b0  titles, 0x40 each: u64 title id, u32 root dir, u32 exefs dir, u32 romfs dir, u32 version, ...
     then   dirs, 24 bytes each: u32 name offset, u32 name length, u32 first child, u32 children, u64 0
     then   files, 80 bytes each: u32 name offset, u32 name length, u64 size, u64 data offset, u64 stored size,
            32-byte SHA-256, u8 codec (0 raw, 1 zstd), u8 flags, 14 bytes reserved
     then   the child list, u32 each: below the dir count a dir, else (index - dirs) a file
     then   the name strings (not 0-terminated)
   Open reads the header, the titles and dirs, the child list, the file records of the chosen title (one run: the records
   from its first to its last) and the names: about 21 MB of the 36.8 MB TOC for OoT's package, the names kept.
   SPDX-License-Identifier: GPL-3.0-or-later (Hookshot's) */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "nup_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#endif

enum { K_DIR = 1, K_FILE, K_PACKED, K_OUTSIDE, K_CODEC };

typedef struct {
    uint64_t at;        /* file: where its bytes start in the package; folder: its first entry's node */
    uint64_t size;      /* file: its size; folder: how many entries */
    uint32_t name;      /* in the names */
    uint16_t name_len;
    uint8_t kind, pad;
} Node;

struct NupFs {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
    char *path;
    Node *nodes;
    uint32_t nnodes;
    uint32_t *slots;    /* node + 1, 0 empty; keyed by (folder, name without case) */
    uint32_t mask;
    char *names;
    uint64_t names_size;
    volatile uint64_t reads;
    NupInfo info;
};

/* ---- the package's descriptor ---- */
static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
#endif
}

/* n bytes at at: what was read (short only at the package's end), -1 when nothing could be */
static int64_t pread_all(NupFs *n, void *buf, uint64_t size, uint64_t at) {
    uint64_t done = 0;
    while (done < size) {
        const uint64_t want = size - done > (1u << 30) ? (1u << 30) : size - done;
        __atomic_fetch_add(&n->reads, 1, __ATOMIC_RELAXED);
#ifdef _WIN32
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)(at + done);
        ov.OffsetHigh = (DWORD)((at + done) >> 32);
        DWORD got = 0;
        if (!ReadFile(n->h, (char *)buf + done, (DWORD)want, &got, &ov)) return done ? (int64_t)done : -1;
        if (!got) break;
#else
        const ssize_t got = pread(n->fd, (char *)buf + done, (size_t)want, (off_t)(at + done));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return done ? (int64_t)done : -1;
        if (!got) break;
#endif
        done += (uint64_t)got;
    }
    return (int64_t)done;
}
static int read_exact(NupFs *n, void *buf, uint64_t size, uint64_t at) { return pread_all(n, buf, size, at) == (int64_t)size; }

/* ---- SHA-256 (FIPS 180-4) ---- */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, k) (((x) >> (k)) | ((x) << (32 - (k))))
static void sha_block(uint32_t *h, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}
#undef ROR
void nup_sha256_init(NupSha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0, s->n = 0;
}
void nup_sha256_update(NupSha256 *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    if (s->n) {
        const size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, k);
        s->n += k, p += k, n -= k;
        if (s->n < 64) return;
        sha_block(s->h, s->buf);
        s->n = 0;
    }
    for (; n >= 64; p += 64, n -= 64) sha_block(s->h, p);
    memcpy(s->buf, p, n);
    s->n = n;
}
void nup_sha256_final(NupSha256 *s, uint8_t out[32]) {
    const uint64_t bits = s->len * 8;
    uint8_t pad[72] = {0x80};
    const size_t k = (s->n < 56 ? 56 : 120) - s->n;
    for (int i = 0; i < 8; i++) pad[k + i] = (uint8_t)(bits >> (56 - 8 * i));
    nup_sha256_update(s, pad, k + 8);
    for (int i = 0; i < 8; i++)
        out[4 * i] = (uint8_t)(s->h[i] >> 24), out[4 * i + 1] = (uint8_t)(s->h[i] >> 16), out[4 * i + 2] = (uint8_t)(s->h[i] >> 8),
        out[4 * i + 3] = (uint8_t)s->h[i];
}

/* ---- names ---- */
static inline uint8_t fold(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }
static uint32_t key_hash(uint32_t parent, const char *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull ^ ((uint64_t)parent * 0x9e3779b97f4a7c15ull);
    for (size_t i = 0; i < n; i++) h = (h ^ fold((uint8_t)s[i])) * 0x100000001b3ull;
    h ^= h >> 29, h *= 0xbf58476d1ce4e5b9ull, h ^= h >> 32;
    return (uint32_t)h;
}
static int same_folded(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (fold((uint8_t)a[i]) != fold((uint8_t)b[i])) return 0;
    return 1;
}

static uint32_t u32_at(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64_at(const uint8_t *p) { return (uint64_t)u32_at(p) | (uint64_t)u32_at(p + 4) << 32; }

/* the content size a zstd frame header states, or -1 (nx2/nup.py frame_content_size) */
static int64_t frame_content_size(const uint8_t *h) {
    if (h[0] != 0x28 || h[1] != 0xb5 || h[2] != 0x2f || h[3] != 0xfd) return -1;
    const unsigned fhd = h[4], single = (fhd >> 5) & 1;
    static const unsigned dict_bytes[4] = {0, 1, 2, 4};
    const unsigned at = 5 + (single ? 0 : 1) + dict_bytes[fhd & 3];
    const unsigned n = (fhd >> 6) == 0 ? single : (fhd >> 6) == 1 ? 2 : (fhd >> 6) == 2 ? 4 : 8;
    if (!n) return -1;
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint64_t)h[at + i] << (8 * i);
    return (int64_t)(n == 2 ? v + 256 : v);
}

#define FAIL(...) do { snprintf(why, why_cap, __VA_ARGS__); goto fail; } while (0)

NupFs *nup_open(const char *path, unsigned flags, char *why, size_t why_cap) {
    char dummy[8];
    if (!why || !why_cap) why = dummy, why_cap = sizeof dummy;
    why[0] = 0;
    const double t0 = now_ms();
    NupFs *n = (NupFs *)calloc(1, sizeof *n);
    uint8_t *meta = NULL, *recs = NULL, *seen = NULL;
    uint32_t *kids = NULL, *queue = NULL;
    if (!n) FAIL("out of memory");
#ifdef _WIN32
    n->h = INVALID_HANDLE_VALUE;
#else
    n->fd = -1;
#endif
    n->path = (char *)malloc(strlen(path) + 1);
    if (!n->path) FAIL("out of memory");
    strcpy(n->path, path);
    uint64_t physical = 0;
#ifdef _WIN32
    n->h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
    if (n->h == INVALID_HANDLE_VALUE) FAIL("cannot open it (error %lu)", (unsigned long)GetLastError());
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(n->h, &sz)) FAIL("cannot size it (error %lu)", (unsigned long)GetLastError());
    physical = (uint64_t)sz.QuadPart;
#else
    n->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (n->fd < 0) FAIL("cannot open it: %s", strerror(errno));
    struct stat st;
    if (fstat(n->fd, &st)) FAIL("cannot size it: %s", strerror(errno));
    if (!S_ISREG(st.st_mode)) FAIL("not a file");
    physical = (uint64_t)st.st_size;
#endif
    n->info.package_size = physical;
    /* the header */
    uint8_t h[0x80], t[0x30];
    if (!read_exact(n, h, sizeof h, 0)) FAIL("shorter than a .nup header (%llu bytes)", (unsigned long long)physical);
    if (memcmp(h, "NUP\x01", 4)) FAIL("not a .nup package (it starts %02x %02x %02x %02x)", h[0], h[1], h[2], h[3]);
    const uint64_t declared = u64_at(h + 0x40), toc_at = u64_at(h + 8), toc_size = u64_at(h + 16), blob = u64_at(h + 0x38);
    if (declared && physical < declared)
        FAIL("incomplete: %llu MB, its header says %llu MB (copy or upload it again)", (unsigned long long)(physical >> 20),
             (unsigned long long)(declared >> 20));
    if (toc_at != 0x80 || toc_size < 0x30 || toc_at + toc_size > blob || blob > physical)
        FAIL("its TOC bounds are wrong (TOC at %#llx, %llu bytes; data from %#llx; %llu bytes in all)", (unsigned long long)toc_at,
             (unsigned long long)toc_size, (unsigned long long)blob, (unsigned long long)physical);
    if (!read_exact(n, t, sizeof t, toc_at)) FAIL("cannot read its TOC");
    if (memcmp(t, "TOC\x01", 4)) FAIL("no TOC at 0x80");
    const uint64_t nt = u32_at(t + 4), nd = u64_at(t + 8), nf = u64_at(t + 16), soff = u64_at(t + 24), ssz = u64_at(t + 32);
    if (!nt || nt > 4096 || nd >= (1u << 28) || nf >= (1u << 28) || ssz >= (1ull << 31))
        FAIL("its TOC counts are out of range (%llu titles, %llu dirs, %llu files, %llu bytes of names)", (unsigned long long)nt,
             (unsigned long long)nd, (unsigned long long)nf, (unsigned long long)ssz);
    const uint64_t dirs_at = 0x30 + 0x40 * nt, files_at = dirs_at + 24 * nd, kids_at = files_at + 80 * nf;
    if (kids_at > soff || (soff - kids_at) % 4 || soff + ssz > toc_size) FAIL("its TOC tables do not fit in the TOC");
    const uint64_t nk = (soff - kids_at) / 4;
    n->info.titles = (uint32_t)nt;
    /* titles and dirs */
    meta = (uint8_t *)malloc((size_t)dirs_at + (size_t)(24 * nd));
    if (!meta || !read_exact(n, meta, dirs_at + 24 * nd, toc_at)) FAIL("cannot read its titles and folders");
    n->info.toc_bytes_read = dirs_at + 24 * nd;
    int pick = -1;
    uint32_t best = 0;
    for (uint64_t i = 0; i < nt; i++) {   /* (the newest title, the first of equals: nx2/nup.py pick_title) */
        const uint8_t *r = meta + 0x30 + 0x40 * i;
        const uint32_t romfs = u32_at(r + 16), version = u32_at(r + 20);
        if (romfs == 0xffffffffu || romfs >= nd) continue;
        if (pick < 0 || version > best) pick = (int)i, best = version;
    }
    if (pick < 0) FAIL("no title in it has a romfs");
    const uint8_t *title = meta + 0x30 + 0x40 * (uint64_t)pick;
    n->info.title_index = (uint32_t)pick;
    n->info.title_id = u64_at(title);
    n->info.version = u32_at(title + 20);
    const uint32_t romfs = u32_at(title + 16);
    /* the child list */
    kids = (uint32_t *)malloc((size_t)(nk ? nk : 1) * 4);
    if (!kids || !read_exact(n, kids, nk * 4, toc_at + kids_at)) FAIL("cannot read its child list");
    n->info.toc_bytes_read += nk * 4;
    for (uint64_t i = 0; i < nk; i++) kids[i] = u32_at((const uint8_t *)&kids[i]);   /* (little-endian already here: no-op) */
    /* the romfs's tree, counted: every folder once (a folder reached twice is a cycle), the file records it uses */
    seen = (uint8_t *)calloc((size_t)nd, 1);
    queue = (uint32_t *)malloc((size_t)nd * 4);
    if (!seen || !queue) FAIL("out of memory");
    uint64_t nodes = 1, fmin = UINT64_MAX, fmax = 0, nqueue = 0;
    queue[nqueue++] = romfs, seen[romfs] = 1;
    uint32_t ndirs = 1, nfiles = 0;
    for (uint64_t qi = 0; qi < nqueue; qi++) {
        const uint8_t *d = meta + dirs_at + 24 * (uint64_t)queue[qi];
        const uint64_t first = u32_at(d + 8), count = u32_at(d + 12);
        if (first + count > nk) FAIL("a folder's entries are outside the child list");
        nodes += count;
        for (uint64_t j = 0; j < count; j++) {
            const uint64_t k = kids[first + j];
            if (k < nd) {
                if (seen[k]) FAIL("a folder is reached twice in the romfs tree (a cycle)");
                seen[k] = 1, queue[nqueue++] = (uint32_t)k, ndirs++;
            } else if (k - nd < nf) {
                if (k - nd < fmin) fmin = k - nd;
                if (k - nd > fmax) fmax = k - nd;
                nfiles++;
            } else {
                FAIL("a child is outside the folder and file tables");
            }
        }
    }
    if (nodes >= 0xfffffff0u) FAIL("too many entries");
    free(seen), seen = NULL;
    free(queue), queue = NULL;
    /* the file records it uses, in one run */
    const uint64_t nrecs = nfiles ? fmax - fmin + 1 : 0;
    recs = (uint8_t *)malloc((size_t)(nrecs ? nrecs : 1) * 80);
    if (!recs || (nrecs && !read_exact(n, recs, nrecs * 80, toc_at + files_at + 80 * fmin))) FAIL("cannot read its file records");
    n->info.toc_bytes_read += nrecs * 80;
    /* the names, kept */
    n->names_size = ssz;
    n->names = (char *)malloc((size_t)(ssz ? ssz : 1));
    if (!n->names || !read_exact(n, n->names, ssz, toc_at + soff)) FAIL("cannot read its names");
    n->info.toc_bytes_read += ssz;
    /* the nodes, breadth first: a folder's entries are one run, in the package's order; each keyed in the hash table */
    n->nnodes = (uint32_t)nodes;
    n->nodes = (Node *)calloc((size_t)nodes, sizeof *n->nodes);
    uint32_t cap = 1024;
    while ((uint64_t)cap * 3 < nodes * 4) cap <<= 1;   /* (at most 3/4 full) */
    n->slots = (uint32_t *)calloc(cap, 4);
    n->mask = cap - 1;
    if (!n->nodes || !n->slots) FAIL("out of memory for the index (%llu entries)", (unsigned long long)nodes);
    n->nodes[0].kind = K_DIR, n->nodes[0].at = romfs;
    uint32_t next = 1;
    for (uint32_t i = 0; i < next; i++) {
        Node *e = &n->nodes[i];
        if (e->kind != K_DIR) continue;
        const uint8_t *d = meta + dirs_at + 24 * e->at;
        const uint32_t first = u32_at(d + 8), count = u32_at(d + 12);
        e->at = next, e->size = count;
        for (uint32_t j = 0; j < count; j++) {
            const uint64_t k = kids[first + j];
            Node *c = &n->nodes[next];
            uint64_t name, len;
            if (k < nd) {
                const uint8_t *cd = meta + dirs_at + 24 * k;
                name = u32_at(cd), len = u32_at(cd + 4);
                c->kind = K_DIR, c->at = k;
            } else {
                const uint8_t *r = recs + 80 * (k - nd - fmin);
                name = u32_at(r), len = u32_at(r + 4);
                const uint64_t size = u64_at(r + 8), at = u64_at(r + 16), stored = u64_at(r + 24);
                const unsigned codec = r[64];
                c->at = at, c->size = size;
                if (codec == 1) c->kind = K_PACKED, n->info.packed++;
                else if (codec != 0) c->kind = K_CODEC, n->info.bad++;
                else if (size && (stored != size || at < blob || at > physical || size > physical - at)) c->kind = K_OUTSIDE, n->info.bad++;
                else c->kind = K_FILE;
                n->info.bytes += size;
            }
            if (!len || len > 0x300 || name + len > ssz) FAIL("an entry's name is outside the names or too long (%llu bytes)", (unsigned long long)len);
            c->name = (uint32_t)name, c->name_len = (uint16_t)len;
            uint32_t s = key_hash(i, n->names + name, (size_t)len) & n->mask;
            while (n->slots[s]) s = (s + 1) & n->mask;
            n->slots[s] = next + 1;
            next++;
        }
    }
    free(recs), recs = NULL;
    free(kids), kids = NULL;
    free(meta), meta = NULL;
    n->info.files = nfiles, n->info.dirs = ndirs;
    n->info.index_bytes = (uint64_t)nodes * sizeof(Node) + (uint64_t)cap * 4 + ssz + sizeof *n + strlen(path) + 1;
    if (nfiles && n->info.packed == nfiles)
        FAIL("every file of its romfs is stored zstd-compressed (codec 1): not read in place by this build; extract it "
             "(nx2/nup.py --extract) and use the romfs/ folder");
    /* (a newer dumper's package keeps every file as a zstd frame of itself with codec 0: nx2/nup.py is_packed. A sample of
       files that are not .zs in the game, spread over the tree: one such frame and the package is refused) */
    {
        unsigned sampled = 0, framed = 0;
        const uint32_t step = n->nnodes / 64 ? n->nnodes / 64 : 1;
        for (uint32_t i = 1; i < n->nnodes && sampled < 64; i += step) {
            for (uint32_t j = i; j < n->nnodes && j < i + step; j++) {
                const Node *e = &n->nodes[j];
                if (e->kind != K_FILE || e->size < 18) continue;
                if (e->name_len > 3 && !memcmp(n->names + e->name + e->name_len - 3, ".zs", 3)) continue;
                uint8_t head[18];
                if (!read_exact(n, head, sizeof head, e->at)) FAIL("cannot read a file's first bytes (at %#llx)", (unsigned long long)e->at);
                sampled++;
                framed += frame_content_size(head) == (int64_t)e->size;
                break;
            }
        }
        if (framed)
            FAIL("%u of %u files sampled are stored as zstd frames of themselves (a newer dumper's package, nx2/nup.py is_packed): "
                 "not read in place by this build; extract it (nx2/nup.py --extract) and use the romfs/ folder", framed, sampled);
    }
    if (flags & NUP_VERIFY_TOC) {
        NupSha256 s;
        nup_sha256_init(&s);
        const size_t chunk = 4u << 20;
        uint8_t *b = (uint8_t *)malloc(chunk), digest[32];
        if (!b) FAIL("out of memory");
        for (uint64_t at = 0; at < toc_size;) {
            const uint64_t k = toc_size - at < chunk ? toc_size - at : chunk;
            if (!read_exact(n, b, k, toc_at + at)) { free(b); FAIL("cannot read its TOC to check it"); }
            nup_sha256_update(&s, b, (size_t)k);
            at += k;
        }
        free(b);
        nup_sha256_final(&s, digest);
        if (memcmp(digest, h + 0x18, 32)) FAIL("its TOC's SHA-256 does not match its header's: the package is damaged");
        n->info.toc_verified = 1;
    }
    n->info.open_ms = now_ms() - t0;
    return n;
fail:
    free(meta), free(recs), free(seen), free(kids), free(queue);
    nup_close(n);
    return NULL;
}
#undef FAIL

void nup_close(NupFs *n) {
    if (!n) return;
#ifdef _WIN32
    if (n->h != INVALID_HANDLE_VALUE) CloseHandle(n->h);
#else
    if (n->fd >= 0) close(n->fd);
#endif
    free(n->nodes), free(n->slots), free(n->names), free(n->path);
    free(n);
}

const NupInfo *nup_info(const NupFs *n) { return &n->info; }
const char *nup_path(const NupFs *n) { return n->path; }
int nup_fd(const NupFs *n) {
#ifdef _WIN32
    (void)n;
    return -1;
#else
    return n->fd;
#endif
}
uint64_t nup_reads(const NupFs *n) { return __atomic_load_n(&n->reads, __ATOMIC_RELAXED); }

void nup_describe(const NupFs *n, char *out, size_t cap) {
    const NupInfo *i = &n->info;
    int k = snprintf(out, cap, "%s: title %016llx v%u, %u files, %u folders, %.2f GB; index %.1f MB in memory, opened in %.0f ms",
                     n->path, (unsigned long long)i->title_id, i->version, i->files, i->dirs, (double)i->bytes / 1e9,
                     (double)i->index_bytes / (1 << 20), i->open_ms);
    if (k > 0 && (size_t)k < cap && (i->packed || i->bad))
        k += snprintf(out + k, cap - (size_t)k, "; %u files stored compressed and %u bad: those do not open", i->packed, i->bad);
    if (k > 0 && (size_t)k < cap && i->toc_verified) snprintf(out + k, cap - (size_t)k, "; TOC SHA-256 checked");
}

/* ---- lookups ---- */
static uint32_t find_child(const NupFs *n, uint32_t dir, const char *name, size_t len, int *exact) {
    const Node *d = &n->nodes[dir];
    const uint64_t first = d->at, end = d->at + d->size;
    uint32_t other = NUP_NONE;
    for (uint32_t s = key_hash(dir, name, len) & n->mask; n->slots[s]; s = (s + 1) & n->mask) {
        const uint32_t c = n->slots[s] - 1;
        if (c < first || c >= end) continue;
        const Node *e = &n->nodes[c];
        if (e->name_len != len) continue;
        const char *have = n->names + e->name;
        if (!memcmp(have, name, len)) { *exact = 1; return c; }
        if (other == NUP_NONE && same_folded(have, name, len)) other = c;
    }
    if (other != NUP_NONE) *exact = 0;
    return other;
}

uint32_t nup_lookup_ex(const NupFs *n, const char *rel, int *exact) {
    int dummy, all_exact = 1;
    if (!exact) exact = &dummy;
    *exact = 1;
    if (!n) return NUP_NONE;
    if (!rel) rel = "";
    uint32_t cur = NUP_ROOT, up[128];
    unsigned depth = 0;
    int trailing = 0;
    for (const char *p = rel; *p;) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        const size_t len = (size_t)(e - p);
        trailing = *e != 0;
        if (len == 1 && p[0] == '.') { p = e; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') { cur = depth ? up[--depth] : NUP_ROOT; p = e; continue; }
        if (n->nodes[cur].kind != K_DIR || len > 0x300 || depth == sizeof up / sizeof up[0]) return NUP_NONE;
        int ex = 1;
        const uint32_t c = find_child(n, cur, p, len, &ex);
        if (c == NUP_NONE) return NUP_NONE;
        all_exact &= ex;
        up[depth++] = cur;
        cur = c;
        p = e;
    }
    if (trailing && n->nodes[cur].kind != K_DIR) return NUP_NONE;   /* ("x.zs/": as open() and CreateFile answer) */
    *exact = all_exact;
    return cur;
}
uint32_t nup_lookup(const NupFs *n, const char *rel) { return nup_lookup_ex(n, rel, NULL); }

int nup_is_dir(const NupFs *n, uint32_t node) { return node < n->nnodes && n->nodes[node].kind == K_DIR; }
int nup_readable(const NupFs *n, uint32_t node) { return node < n->nnodes && n->nodes[node].kind == K_FILE; }
const char *nup_why_unreadable(const NupFs *n, uint32_t node) {
    if (node >= n->nnodes) return "no such entry";
    switch (n->nodes[node].kind) {
    case K_FILE: return NULL;
    case K_DIR: return "a folder";
    case K_PACKED: return "stored zstd-compressed in the package (codec 1): not read in place by this build";
    case K_CODEC: return "stored with a codec this build does not know";
    default: return "its bytes are outside the package (or its stored size is not its size)";
    }
}
uint64_t nup_size(const NupFs *n, uint32_t node) {
    return node < n->nnodes && n->nodes[node].kind != K_DIR ? n->nodes[node].size : 0;
}
int64_t nup_read(const NupFs *n, uint32_t node, uint64_t at, void *buf, uint64_t size) {
    if (node >= n->nnodes || n->nodes[node].kind != K_FILE) return -1;
    const Node *e = &n->nodes[node];
    if (at >= e->size) return 0;
    if (size > e->size - at) size = e->size - at;
    return pread_all((NupFs *)n, buf, size, e->at + at);
}
uint32_t nup_child_count(const NupFs *n, uint32_t dir) { return nup_is_dir(n, dir) ? (uint32_t)n->nodes[dir].size : 0; }
uint32_t nup_child(const NupFs *n, uint32_t dir, uint32_t i) {
    return nup_is_dir(n, dir) && i < n->nodes[dir].size ? (uint32_t)(n->nodes[dir].at + i) : NUP_NONE;
}
const char *nup_name(const NupFs *n, uint32_t node, size_t *len) {
    if (node >= n->nnodes) { if (len) *len = 0; return ""; }
    if (len) *len = n->nodes[node].name_len;
    return node ? n->names + n->nodes[node].name : "";
}
size_t nup_name_copy(const NupFs *n, uint32_t node, char *out, size_t cap) {
    size_t len;
    const char *s = nup_name(n, node, &len);
    if (!cap) return 0;
    if (len > cap - 1) len = cap - 1;
    memcpy(out, s, len);
    out[len] = 0;
    return len;
}
