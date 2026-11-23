/* (ps5, lot 80) The game's data folder on the console: /data/grapnel, the port's public name (Grapnel), with the folder the port
   had before, /data/hookshot, taken over when it is the only one there. What the folder holds: game.nup, the saves (user/),
   the options (hookshot_options.ini), the settings (hookshot_env.txt), the renderer's caches, the launch screen's background,
   prompts/, cache/, the cold units, the logs. The names of the files inside stay as they were (hookshot_*): only the
   folder's name changes.

   At the start, once, before any thread (host/posix/ps5_platform.c hos_ps5_data_init), ps5_data_folder_choose:
   - /data/grapnel is a folder: it is the data folder ("there"); a /data/hookshot beside it is left as it is, never read;
   - else /data/hookshot is a folder: renamed to /data/grapnel with one rename(2) ("taken over"). Both names are in /data,
     so the rename moves no byte: the folder's entry changes name in one step, and every file in it (26 GB of game.nup
     included) is where it was. A rename cannot leave half a folder on each side: either the call succeeded and everything
     is under the new name, or it failed and everything is still under the old one;
   - and when that rename fails (the folder is a mount point, the sandbox refuses it, another name is in the way): the old
     folder is used in place, as it is, under its old name ("in place"); nothing moved, the next start tries again;
   - else neither is there: /data/grapnel made ("made");
   - else no folder at all (the title does not see /data): none, as before (the caller asks the Lapy daemon, or uses /app0).
   Why a rename and not a copy: a copy writes every byte again (the console holds a title's writes to ~16 MiB/s past about
   1 GiB: hours for game.nup) and leaves two half-copies if the title stops on the way; the rename writes none and is
   atomic. Why not a link from one name to the other: a symbolic link in /data is followed by the title's calls but not
   always by FTP and ps5upload's listings, and two names for one folder is what this lot ends.

   A rename never replaces a folder that is there: the new name is looked at first, and only an absent one is renamed to
   (rename(2) would replace an empty folder of that name; nothing else runs at that point of the start to make one).

   ps5_data_path_follow: a setting whose value names a file under the other folder (RX_VK_SKIP_FILE=/data/hookshot/skip.txt
   in a hookshot_env.txt written before the takeover, which the takeover brought along) is read under the data folder.

   No console call: the folder calls are the caller's (ps5_data_ops), so tests/test_ps5_data_folder.py runs this on a PC, in
   a temporary folder, with a rename that fails on purpose.

   SPDX-License-Identifier: GPL-3.0-or-later (hookshot-ps5) */
#ifndef HOOKSHOT_POSIX_PS5_DATA_FOLDER_H
#define HOOKSHOT_POSIX_PS5_DATA_FOLDER_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* the folders' names in /data: the one the game uses, and the one before lot 80 (PS5_DATA_PARENT: another folder for a test
   on a PC, tests/test_ps5_data_folder.py's broker) */
#ifndef PS5_DATA_PARENT
#define PS5_DATA_PARENT "/data"
#endif
#define PS5_DATA_NAME "grapnel"
#define PS5_DATA_OLD_NAME "hookshot"
#define PS5_DATA_DIR PS5_DATA_PARENT "/" PS5_DATA_NAME
#define PS5_DATA_OLD_DIR PS5_DATA_PARENT "/" PS5_DATA_OLD_NAME

/* the calls on folders, the console's (stat, lstat, rename, mkdir and chmod) or a test's; each returns as they do (0, or -1
   with errno set), is_dir 1 for a folder (or a link to one) and 0 otherwise, exists 1 for anything there (lstat's) */
struct ps5_data_ops {
    int (*is_dir)(const char *path);
    int (*exists)(const char *path);
    int (*rename)(const char *from, const char *to);
    int (*mkdir)(const char *path);   /* (made 0777, as FTP and the broker need) */
};

enum ps5_data_how {
    PS5_DATA_NONE,        /* no folder: /data not seen (or refused) */
    PS5_DATA_THERE,       /* /data/grapnel, there already */
    PS5_DATA_TAKEN_OVER,  /* /data/hookshot renamed to /data/grapnel at this start */
    PS5_DATA_IN_PLACE,    /* /data/hookshot used as it is: its rename failed */
    PS5_DATA_MADE,        /* /data/grapnel made: neither was there */
};

static inline const char *ps5_data_how_name(enum ps5_data_how h) {
    switch (h) {
    case PS5_DATA_THERE: return "there";
    case PS5_DATA_TAKEN_OVER: return "taken over";
    case PS5_DATA_IN_PLACE: return "in place";
    case PS5_DATA_MADE: return "made";
    default: return "none";
    }
}

/* The data folder in parent ("/data"; a test's own folder): its path into root (empty for none), one line saying what was
   done into line, and the errno of the call that failed into *err (the rename's for "in place", mkdir's for "none"; 0 else) */
static inline enum ps5_data_how ps5_data_folder_choose(const char *parent, const struct ps5_data_ops *ops, char *root,
                                                       size_t root_cap, char *line, size_t line_cap, int *err) {
    char now[512], old[512];
    snprintf(now, sizeof now, "%s/" PS5_DATA_NAME, parent);
    snprintf(old, sizeof old, "%s/" PS5_DATA_OLD_NAME, parent);
    if (root_cap) root[0] = 0;
    if (line_cap) line[0] = 0;
    if (err) *err = 0;
    if (ops->is_dir(now)) {
        snprintf(root, root_cap, "%s", now);
        snprintf(line, line_cap, "%s, there already%s", now,
                 ops->is_dir(old) ? " (" PS5_DATA_OLD_DIR " is there too: left as it is, not read)" : "");
        return PS5_DATA_THERE;
    }
    const int old_there = ops->is_dir(old);
    if (ops->exists(now)) {   /* (a file, or a link to nothing, of the new name: never renamed over, never removed) */
        if (old_there) {
            snprintf(root, root_cap, "%s", old);
            snprintf(line, line_cap, "%s used in place: %s is there and is not a folder (nothing renamed)", old, now);
            return PS5_DATA_IN_PLACE;
        }
        if (err) *err = EEXIST;
        snprintf(line, line_cap, "no data folder: %s is there and is not a folder", now);
        return PS5_DATA_NONE;
    }
    if (old_there) {
        if (ops->rename(old, now) == 0) {
            snprintf(root, root_cap, "%s", now);
            snprintf(line, line_cap, "%s taken over: renamed to %s in one step (the same disk: nothing copied; saves, options, "
                     "settings and caches as they were)", old, now);
            return PS5_DATA_TAKEN_OVER;
        }
        const int e = errno;
        if (err) *err = e;
        snprintf(root, root_cap, "%s", old);
        snprintf(line, line_cap, "%s used in place: renaming it to %s failed (errno %d: %s); nothing moved, the next start tries "
                 "again", old, now, e, strerror(e));
        return PS5_DATA_IN_PLACE;
    }
    if (ops->mkdir(now) == 0) {
        snprintf(root, root_cap, "%s", now);
        snprintf(line, line_cap, "%s made (new: no %s nor %s before)", now, now, old);
        return PS5_DATA_MADE;
    }
    const int e = errno;
    if (err) *err = e;
    snprintf(line, line_cap, "no data folder: %s could not be made (errno %d: %s)", now, e, strerror(e));
    return PS5_DATA_NONE;
}

/* A setting's value naming a file under the data folder the game does not use (/data/hookshot/... when the folder is
   /data/grapnel, /data/grapnel/... when /data/hookshot is used in place), put under root in out: 1 when it did (out set),
   0 when the value stays as it is (another path, no file in it, or root is neither folder) */
static inline int ps5_data_path_follow(const char *value, const char *root, char *out, size_t cap) {
    static const char *const dirs[] = {PS5_DATA_DIR, PS5_DATA_OLD_DIR};
    if (!value || !root || !cap) return 0;
    int root_known = 0;
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) root_known |= !strcmp(root, dirs[i]);
    if (!root_known) return 0;
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        const size_t n = strlen(dirs[i]);
        if (!strcmp(dirs[i], root) || strncmp(value, dirs[i], n) || value[n] != '/' || !value[n + 1]) continue;
        const int w = snprintf(out, cap, "%s%s", root, value + n);
        return w > 0 && (size_t)w < cap;
    }
    return 0;
}

#endif
