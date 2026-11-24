/* (ps5, lot 88) Which game file the RomFS is read from, on the console: game.nup in the data folder, as before, or the file
   HOOKSHOT_PS5_NUP names in hookshot_env.txt (the extended storage: HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup, which the Grapnel
   builder's step 6 writes when it installs there; ShadowMountPlus 1.7beta4 and later mount the USB and extended storage drives
   into the titles it registers, as it mounts /data). host/hos_main.c rom_from_package asks it after stage 4, once the settings
   are in the environment; tests/test_ps5_game_file.py runs it with a file list of its own.

   - no setting (unset or empty): game.nup in the data folder, as before;
   - the setting's file there: that file;
   - the setting's file not there, game.nup in the data folder there: that one, and the line says why (the drive unplugged, a
     path mistyped: the game still starts);
   - neither: the setting's file, whose open fails and says why; host/hos_main.c then tells the launch screen, which says so
     and does not start the game (gpu/vk/vk_launcher.cpp), where it used to stop later without a word. */
#pragma once
#include <stdio.h>
#include <string.h>

/* The file to open; *from_setting: 1 when it is the setting's. exists(path): the file is there. line: what was chosen and why */
static inline const char *ps5_game_file_choose(const char *setting, const char *beside, int (*exists)(const char *),
                                               int *from_setting, char *line, size_t cap) {
    *from_setting = 0;
    if (!setting || !*setting) {
        snprintf(line, cap, "game.nup in the data folder (%s)", beside);
        return beside;
    }
    if (exists(setting)) {
        *from_setting = 1;
        snprintf(line, cap, "HOOKSHOT_PS5_NUP=%s", setting);
        return setting;
    }
    if (exists(beside)) {
        snprintf(line, cap, "HOOKSHOT_PS5_NUP=%s is NOT THERE (the drive not connected, or the path mistyped): game.nup in the "
                 "data folder (%s) instead", setting, beside);
        return beside;
    }
    *from_setting = 1;
    snprintf(line, cap, "HOOKSHOT_PS5_NUP=%s is NOT THERE, and no game.nup in the data folder (%s): the game cannot start", setting,
             beside);
    return setting;
}

/* The drive a path is on, for the log, by where the console mounts it */
static inline const char *ps5_game_file_volume(const char *path) {
    if (!strncmp(path, "/mnt/ext", 8)) return "the extended storage";
    if (!strncmp(path, "/mnt/usb", 8)) return "a USB drive";
    if (!strncmp(path, "/data/", 6)) return "the console's own storage (/data)";
    if (!strncmp(path, "/app0/", 6)) return "the title's folder (/app0)";
    return "a drive not known here";
}
