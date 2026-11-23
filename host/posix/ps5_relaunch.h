/* (ps5, lot 73) The title started again by the system, for the in-game menu's "Back to the Grapnel screen" (gpu/vk/vk_menu.cpp,
   gpu/vk/launcher_back.h), as plain C with the console's calls passed in: host/posix/ps5_relaunch.c gives it the real ones,
   tests/test_ps5_back_to_launcher.py fakes. HOOKSHOT_PS5_BACK_TO_LAUNCHER=1 only (the menu's row is not there without it).

   The order, each step said in the log (and flushed: the process may end at the next one):
     1. the close work, as the menu's Quit asks it on a PC (mkwvk_close_asked: the renderer's caches kept, the steps of the start
        a close is not to count as crashed), bounded: 15 s at most (kHosPs5CloseWaitMs), then on all the same (it never ran on
        the console before this lot: a hang there must not leave the player in the game with nothing happening);
     2. sceSystemServiceLoadExec("/app0/eboot.bin", NULL): the system ends this process ("Kill for LoadExec" in klog, as at the
        title's usual exit through "exit") and runs the title's own executable in a new one, which reads hookshot_env.txt and
        the options again, starts the renderer and shows the launch screen (it finds the file the menu left);
     3. the safe way out (the fallback) when it returns: refused (not 0), or accepted (0) yet this process still runs 5 s later
        (kHosPs5AcceptWaitMs): the title closes through hos_ps5_exit (LoadExec "exit", the way the title closes itself), and the
        player starts it again from the home screen, where the file the menu left shows the launch screen.
   The same call restarts the app in EdGlantine-PS5 (eden/src/lifecycle.c, after a crash report) and in Forever Launcher (after
   its first setup); neither has been run with this title on firmware 12.60: the first console test says. */
#pragma once

#include <stdio.h>

#define HOS_PS5_RELAUNCH_PATH "/app0/eboot.bin"   /* (the title's own executable: its folder is /app0, ShadowMount's) */

enum { kHosPs5CloseWaitMs = 15000, kHosPs5AcceptWaitMs = 5000 };

typedef struct HosPs5Relaunch {
    void (*say)(const char *line);       /* a line of the log, flushed */
    int (*close_work)(unsigned wait_ms); /* the close work, waited for wait_ms at most: 1 done, 0 not done in time */
    int (*load_exec)(const char *path);  /* sceSystemServiceLoadExec(path, NULL): returns only when it did not end this process */
    void (*sleep_ms)(unsigned ms);
    void (*exit_title)(void);            /* hos_ps5_exit(0): on the console it does not return */
} HosPs5Relaunch;

/* The steps above; returns only where exit_title returns (a test) */
static inline void hos_ps5_relaunch_run(const HosPs5Relaunch *o) {
    char line[320];
    o->say("relaunch: back to the Grapnel screen: the close work first (the renderer's caches kept, as the menu's Quit on a PC)");
    if (o->close_work(kHosPs5CloseWaitMs)) o->say("relaunch: close work done");
    else {
        snprintf(line, sizeof line, "relaunch: the close work did not end within %u s: going on without it", kHosPs5CloseWaitMs / 1000);
        o->say(line);
    }
    snprintf(line, sizeof line, "relaunch: sceSystemServiceLoadExec(%s): the system ends this process and starts the title again "
             "(its launch screen shows)", HOS_PS5_RELAUNCH_PATH);
    o->say(line);
    const int r = o->load_exec(HOS_PS5_RELAUNCH_PATH);
    if (r == 0) {
        for (unsigned waited = 0; waited < kHosPs5AcceptWaitMs; waited += 250) o->sleep_ms(250);
        snprintf(line, sizeof line, "relaunch: FALLBACK: accepted (0), yet this process still runs %u s later: the title closes; start it "
                 "again from the home screen: its launch screen shows", kHosPs5AcceptWaitMs / 1000);
    } else
        snprintf(line, sizeof line, "relaunch: FALLBACK: refused (%#x): the title closes; start it again from the home screen: its launch "
                 "screen shows", (unsigned)r);
    o->say(line);
    o->exit_title();
}

/* (lot 74) The in-game menu's Quit on the console: the same close work, bounded the same way, then the title closes as it closes
   itself (exit_title: hos_ps5_exit, LoadExec "exit"). No new console call. Returns only where exit_title returns (a test) */
static inline void hos_ps5_quit_run(const HosPs5Relaunch *o) {
    char line[160];
    o->say("quit: the in-game menu's Quit: the close work first (the renderer's caches kept, as the menu's Quit on a PC)");
    if (o->close_work(kHosPs5CloseWaitMs)) o->say("quit: close work done");
    else {
        snprintf(line, sizeof line, "quit: the close work did not end within %u s: going on without it", kHosPs5CloseWaitMs / 1000);
        o->say(line);
    }
    o->say("quit: hos_ps5_exit: the system closes the title");
    o->exit_title();
}
