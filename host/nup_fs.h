/* The game's RomFS read in place from the user's .nup package (a decrypted Switch 2 dump), with no romfs/ folder made from
   it: 186,583 files written one by one ran at about 2 MB/s on the PS5, and a title may hold only about 249 files open by
   path. One descriptor serves every read (positional: pread, or ReadFile with an offset on Windows), so any number of
   threads read at once. The format is Hookshot's nx2/nup.py's (the layout in its docstring); this is its reading side in C.

   At open: the header and the TOC are checked, the newest title's romfs is taken (nx2/nup.py pick_title: the update over
   the base), and an index of its folders and files is built: a node per entry (24 bytes), a hash table of node numbers
   keyed by (folder, name without case), and the package's own name strings. The nodes are numbered breadth first, so a
   folder's entries are one run of numbers in the package's order. Nothing is written; the index never changes after open,
   so lookups and reads need no lock.

   Paths are inside the romfs: "UI/x.zs", "/UI/x.zs", "UI\x.zs", "./UI//x.zs", "Foo/../UI/x.zs" all name the same file; the
   exact spelling is taken first, else one that differs only in ASCII case (as the Windows build tolerates).

   Not read: a file the package keeps zstd-compressed (codec 1 in its record, or, from newer dumpers, every file as a zstd
   frame of itself: nx2/nup.py is_packed). The first are counted and refused one by one (nup_readable); a package of the
   second kind is refused at open, with the reason. Extract such a package with nx2/nup.py --extract and use the folder.

   SPDX-License-Identifier: GPL-3.0-or-later (Hookshot's) */
#ifndef HOOKSHOT_NUP_FS_H
#define HOOKSHOT_NUP_FS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct NupFs NupFs;
#define NUP_NONE 0xffffffffu   /* no such node */
#define NUP_ROOT 0u            /* the romfs's own folder */

/* nup_open's flags */
#define NUP_VERIFY_TOC 1u      /* check the TOC's SHA-256 too (36.8 MB hashed: about 0.15 s more) */

typedef struct NupInfo {
    uint64_t title_id;
    uint32_t version, title_index, titles;
    uint32_t files, dirs;          /* in the chosen title's romfs; dirs counts its root */
    uint32_t packed, bad;          /* files that will not open: zstd-packed in the package (codec 1), bad extents */
    uint64_t bytes;                /* the files' sizes, together */
    uint64_t package_size;
    uint64_t index_bytes;          /* what the index keeps in memory (nodes, hash table, names) */
    uint64_t toc_bytes_read;       /* what open read of the TOC */
    double open_ms;                /* open, checks and index */
    int toc_verified;              /* 1: NUP_VERIFY_TOC and the hash matched */
} NupInfo;

/* NULL on failure, with the reason in why. path: the package */
NupFs *nup_open(const char *path, unsigned flags, char *why, size_t why_cap);
void nup_close(NupFs *n);
const NupInfo *nup_info(const NupFs *n);
const char *nup_path(const NupFs *n);
/* one line: "<file>: title <id> v<version>, <files> files, <dirs> folders, <GB>; index <MB> in memory, opened in <ms> ms" */
void nup_describe(const NupFs *n, char *out, size_t cap);

uint32_t nup_lookup(const NupFs *n, const char *rel);   /* the node, or NUP_NONE */
/* the same, and whether only a spelling in another case matched (exact: 1 when the spelling was the package's) */
uint32_t nup_lookup_ex(const NupFs *n, const char *rel, int *exact);
int nup_is_dir(const NupFs *n, uint32_t node);
int nup_readable(const NupFs *n, uint32_t node);        /* a file this layer reads (not packed, extent in the package) */
const char *nup_why_unreadable(const NupFs *n, uint32_t node);   /* NULL when readable */
uint64_t nup_size(const NupFs *n, uint32_t node);       /* a file's size; 0 for a folder */
/* size bytes at offset at of the file, into buf: what was read (less at the file's end, 0 past it), -1 on an error */
int64_t nup_read(const NupFs *n, uint32_t node, uint64_t at, void *buf, uint64_t size);
uint32_t nup_child_count(const NupFs *n, uint32_t dir);
uint32_t nup_child(const NupFs *n, uint32_t dir, uint32_t i);   /* the i-th entry of a folder, in the package's order */
const char *nup_name(const NupFs *n, uint32_t node, size_t *len);   /* not 0-terminated; "" for the root */
size_t nup_name_copy(const NupFs *n, uint32_t node, char *out, size_t cap);   /* 0-terminated, cut to cap - 1 */
/* the descriptor every read goes through (-1 on Windows, where it is a HANDLE): one per package */
int nup_fd(const NupFs *n);
/* reads made through the package's descriptor since open */
uint64_t nup_reads(const NupFs *n);

/* SHA-256, for the TOC check and the tests' and the probe's own checks of what was read */
typedef struct NupSha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } NupSha256;
void nup_sha256_init(NupSha256 *s);
void nup_sha256_update(NupSha256 *s, const void *data, size_t n);
void nup_sha256_final(NupSha256 *s, uint8_t out[32]);

/* ---- host/nn_fs.c: the RomFS mount's package, when the game reads its RomFS from one (host/hos_main.c chooses) ---- */
extern NupFs *hos_romfs_nup;
/* opens the package for the RomFS mount (once: a second call answers what the first did) and writes the one line that
   says so (nup_describe's, or why not) into line. 1 when the package is in use */
int hos_fs_use_nup(const char *path, char *line, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
