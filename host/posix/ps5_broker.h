/* (ps5) Hookshot's storage broker: the protocol between the title (host/posix/ps5_broker_client.c) and the broker, a payload
   (host/posix/ps5_broker_payload.c) the title starts through the console's ELF loader.

   Why. The console holds a title's writes to the SSD: about 1.3 GiB at full speed, then about 2 MiB/s (PS5_PayloadSDK
   platform/docs/PROBE.md "Sustained writes"; ps5-forever-wine's ps5_writebroker.h: 2400 MiB a launch, refilled at
   2 MiB/s). A payload process is not an application and is not held; it also sees /data whatever the title's sandbox
   has. Hookshot's renderer caches are gigabytes (hookshot_programs.bin 6.9 GB on the PC), its romfs lives under the data
   folder (/data/grapnel since lot 80, /data/hookshot before). Savage Wind does the same with its storagebroker.elf
   ("[broker] started from the title"), and Forever Launcher with its write broker, whose design this follows:

   Adapted from bizkut/ps5-forever (GPL-3.0), ps5/writebroker/writebroker.c and ps5/probes/psmkit-wine/
   writebroker-autostart.h at 0b6abf0bb924954a58ae937ab23f4397a0c44c5f (01.000.024), with its protocol header
   include/wine/ps5_writebroker.h of bizkut/ps5-forever-wine at 01a7ca665585cc7f80da2edb4e02d029c86312b2: the hello with a
   token the broker writes where only the title and the broker can read it, a request header then the path then the
   data, a reply per request, a broker of another version asked to exit and replaced, the start through the ELF loader on
   127.0.0.1:9021 when nothing answers. Hookshot's differs: its own port and root, and reads, creation, folders, stat and
   removal besides writes, because the title may not see /data at all (a title registered by a ShadowMountPlus older than
   1.7 has only /app0).

   Connection: the title connects to 127.0.0.1:HB_PORT and sends struct hb_hello (HB_VERSION and the token from
   HB_TOKEN_NAME, which the broker writes in each data folder of HB_ROOT_DIRS that exists and in each title folder of
   HB_TITLE_FOLDERS that exists); the broker
   answers struct hb_reply, result 0, or -EPROTONOSUPPORT for another version, after which it takes only HB_QUIT. Then
   any number of requests: struct hb_request, path_length bytes of an absolute path below HB_ROOT or HB_ROOT_OLD (no NUL,
   no ".."), and
   for HB_WRITE length bytes of data; the broker answers struct hb_reply (result: bytes, 0, or -errno), followed for
   HB_READ by result bytes and for HB_STAT and HB_STATFS by struct hb_stat. A malformed request closes the connection.
   Each connection is served by a thread of its own with a 1 MiB buffer; a request moves at most HB_MAX_IO bytes.

   Port: Savage Wind's storage broker listens on 9142 and outlives its game; Forever's write broker on 9141 and its Add-ons
   helper on 9143 (moved off 9142 for Savage Wind: ps5-forever ps5/addon-worker/protocol.h). Hookshot's on 9147, which no
   other homebrew we know of uses. The broker also outlives the title: the next launch finds it and connects.

   (lot 80) Version 2: the data folder is /data/grapnel (host/posix/ps5_data_folder.h), /data/hookshot before. A broker of
   version 1 serves /data/hookshot/ alone, and one may still run when the first Grapnel title starts (it outlives its title):
   the version moved so that the title does not keep it (its hello answers -EPROTONOSUPPORT to a version 2 hello) and starts
   this one, which asks it to exit with the token it had: the takeover renamed /data/hookshot, its token file with it, so
   the old broker finds no file to adopt and knows only that token (ps5_broker_payload.c replace_running). Version 2 serves
   both folders: /data/hookshot/ is the data folder when its rename failed and the title uses it in place. */
#ifndef HOOKSHOT_POSIX_PS5_BROKER_H
#define HOOKSHOT_POSIX_PS5_BROKER_H

#include <stddef.h>
#include <stdint.h>

#include "ps5_data_folder.h"   /* (the data folders' names) */

#ifndef HB_PORT   /* (another for a test on a PC: tests/test_ps5_data_folder.py) */
#define HB_PORT 9147
#endif
#define HB_ELF_LOADER_PORT 9021
#define HB_VERSION 2   /* (lot 80: 2, the data folder /data/grapnel; 1 served /data/hookshot alone) */
#define HB_HELLO_MAGIC 0x314b4248u   /* "HBK1" */
#define HB_TOKEN_BYTES 16
#define HB_MAX_PATH 1024
#define HB_MAX_IO (16u << 20)
/* the data folders it serves: the game's (ps5_data_folder.h), and the one before lot 80, which the title uses in place when
   it could not rename it. HB_ROOT_DIRS: the same, without the slash, the game's first */
#define HB_ROOT PS5_DATA_DIR "/"
#define HB_ROOT_OLD PS5_DATA_OLD_DIR "/"
#define HB_ROOT_DIRS {PS5_DATA_DIR, PS5_DATA_OLD_DIR}
#define HB_TOKEN_NAME ".hookshot-broker-token"
/* (the title folders the token is also written to, so that a title that does not see /data can read it from /app0: the
   game's (GRPN00001 since lot 80; PPSA99145 before) and the probe's; a folder that does not exist is skipped) */
#define HB_TITLE_FOLDERS {"/data/homebrew/GRPN00001", "/data/homebrew/PPSA99145", "/data/homebrew/PPSA99147"}

enum {
    HB_WRITE = 0x45545257u,    /* "WRTE": pwrite to an existing regular file */
    HB_READ = 0x44414552u,     /* "READ": pread, length bytes at most */
    HB_CREATE = 0x54414552u,   /* "REAT": create (or truncate, reserved 1) a regular file, mode 0666 */
    HB_MKDIR = 0x52444b4du,    /* "MKDR": a folder, mode 0777 (an existing one is not an error) */
    HB_STAT = 0x54415453u,     /* "STAT" */
    HB_STATFS = 0x53465453u,   /* "STFS": the filesystem's space */
    HB_REMOVE = 0x564d4552u,   /* "REMV": a regular file */
    HB_QUIT = 0x54495551u,     /* "QUIT": the broker exits (a newer one replaces it) */
};

struct hb_hello {
    uint32_t magic;
    uint32_t version;
    unsigned char token[HB_TOKEN_BYTES];
};
struct hb_request {
    uint32_t magic;
    uint32_t path_length;
    uint64_t offset;
    uint32_t length;
    uint32_t reserved;
};
struct hb_reply {
    uint32_t magic;   /* HB_HELLO_MAGIC */
    int32_t result;
};
struct hb_stat {      /* (after the reply of HB_STAT, HB_STATFS) */
    uint64_t size, mode;            /* HB_STAT */
    uint64_t bsize, blocks, bavail; /* HB_STATFS */
};
_Static_assert(sizeof(struct hb_hello) == 24, "hb_hello wire size");
_Static_assert(sizeof(struct hb_request) == 24, "hb_request wire size");
_Static_assert(sizeof(struct hb_reply) == 8, "hb_reply wire size");
_Static_assert(sizeof(struct hb_stat) == 40, "hb_stat wire size");

/* Length of the root (HB_ROOT or HB_ROOT_OLD) the path is below (not the root itself), or 0: outside both, or climbing out
   with "..". */
static inline size_t hb_path_below(const char *path, const char *root, size_t length) {
    if (!path || __builtin_strncmp(path, root, length)) return 0;
    if (!path[length] || path[length] == '/') return 0;
    for (const char *p = path + length; *p; p++)
        if ((p == path + length || p[-1] == '/') && p[0] == '.' && p[1] == '.' && (!p[2] || p[2] == '/')) return 0;
    return length;
}
static inline size_t hb_path_root(const char *path) {
    const size_t n = hb_path_below(path, HB_ROOT, sizeof HB_ROOT - 1);
    return n ? n : hb_path_below(path, HB_ROOT_OLD, sizeof HB_ROOT_OLD - 1);
}
/* 1 when the folder (a data root, with or without its last slash) is one the broker serves: the title starts it for those */
static inline int hb_serves_root(const char *dir) {
    if (!dir) return 0;
    const size_t a = sizeof PS5_DATA_DIR - 1, b = sizeof PS5_DATA_OLD_DIR - 1;
    return (!__builtin_strncmp(dir, PS5_DATA_DIR, a) && (!dir[a] || (dir[a] == '/' && !dir[a + 1]))) ||
           (!__builtin_strncmp(dir, PS5_DATA_OLD_DIR, b) && (!dir[b] || (dir[b] == '/' && !dir[b + 1])));
}

/* ---- the title's side (host/posix/ps5_broker_client.c) ---- */
#ifdef __PROSPERO__
#ifdef __cplusplus
extern "C" {
#endif
enum hb_start {
    HB_PRESENT,              /* a broker of this version answered */
    HB_STARTED,              /* sent to the ELF loader, and then it answered */
    HB_LOADER_UNAVAILABLE,   /* nothing on 127.0.0.1:9021 (no ELF loader running) */
    HB_NO_ELF,               /* the broker's ELF is not in the title's folder */
    HB_NO_TOKEN,             /* it answered but its token could not be read (/data hidden, no title folder copy) */
    HB_TIMEOUT,
    HB_ERROR,
};
const char *hb_start_name(enum hb_start s);
/* Connects to a running broker of this version, or sends elf (/app0/hookshot-broker.elf) to the ELF loader and waits for
   one, replacing a broker of another version. Single connection, made once (thread-safe). *detail: errno or result. */
enum hb_start hb_start(const char *elf, int *detail);
int hb_ready(void);   /* 1 once hb_start has a connection (the writes below then go through) */
/* The calls, on one connection (a lock around each request). -errno on failure. */
int64_t hb_write(const char *path, uint64_t offset, const void *data, uint32_t length);
int64_t hb_read(const char *path, uint64_t offset, void *data, uint32_t length);
int hb_create(const char *path, int truncate);
int hb_mkdir(const char *path);
int hb_stat(const char *path, uint64_t *size, uint64_t *mode);
int hb_statfs(const char *path, uint64_t *bsize, uint64_t *blocks, uint64_t *bavail);
int hb_remove(const char *path);
#ifdef __cplusplus
}
#endif
#endif /* __PROSPERO__ */
#endif
