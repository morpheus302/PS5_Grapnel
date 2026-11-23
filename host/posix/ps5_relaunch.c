/* (ps5, lot 73) The in-game menu's Back to the Grapnel screen (gpu/vk/vk_menu.cpp, HOOKSHOT_PS5_BACK_TO_LAUNCHER=1 only): the
   title started again by the system on its launch screen. The steps and the safe way out are host/posix/ps5_relaunch.h's; this
   file gives them the console's calls. (lot 74) And the menu's Quit: the same close work, then the title closes (hos_ps5_quit).

   The console has no window loop (hos_display_init answers no window: the main thread waits on the game's), so the menu's Restart
   and Quit messages (WM_APP + 9, WM_CLOSE, host/hos_relaunch.c) find no one to read them there: this one is called by the menu
   itself and does its work on a thread of its own, never on the render thread the menu runs on. A file of its own, not a part of
   host/hos_relaunch.c, which the console's eboot never linked: taking it in brought its Linux restart along, and with it three
   new imports (execve, execvp, environ), and an import the console does not have can keep the title from starting.

   The close work is the renderer's close hook itself, mkwvk_close_asked (gpu/vk/vk_main.cpp), called by name: on the console
   rx_close_asked would not reach it (GetProcAddress is hos_ps5_symbol there, which knows mkwvk_init alone). What it does there:
   the program census in the log, the steps of the start put back (crash_markers.h), the pipeline cache's play file (none:
   RX_VK_PCACHE_FILE=0). The caches the renderer writes as it goes (the programs and their index, the pipeline notes, the state
   list) are flushed record by record, and the menu saved the options before it called here. No new console call:
   sceSystemServiceLoadExec and sceKernelUsleep are the ones hos_ps5_exit already makes (host/posix/ps5_platform.c) */
#ifdef __PROSPERO__
#include "../hos.h"
#include <windows.h>   /* (host/posix's: CreateThread, the events) */
#include <stdint.h>
#include <stdio.h>
#include "ps5_relaunch.h"

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceKernelUsleep(uint32_t microseconds);
void hos_ps5_exit(int code) __attribute__((noreturn));
void mkwvk_close_asked(void);   /* (gpu/vk/vk_main.cpp: the renderer's close hook, linked in the same eboot) */

static void say(const char *line) {
    hos_log("%s", line);
    hos_log_flush();
}
static HANDLE close_done;
static DWORD WINAPI close_main(LPVOID unused) {
    (void)unused;
    mkwvk_close_asked();
    SetEvent(close_done);
    return 0;
}
/* (mkwvk_close_asked on a thread of its own, waited for wait_ms at most) */
static int close_work(unsigned wait_ms) {
    close_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    const HANDLE t = close_done ? CreateThread(NULL, 8u << 20, close_main, NULL, 0, NULL) : NULL;
    if (!t) {
        hos_log("relaunch: no thread for the close work: done on this one, unbounded");
        mkwvk_close_asked();
        return 1;
    }
    SetThreadDescription(t, L"relaunch-close");
    const int done = WaitForSingleObject(close_done, wait_ms) == WAIT_OBJECT_0;
    CloseHandle(t);
    return done;
}
static int load_exec(const char *path) {
    fflush(NULL);
    return sceSystemServiceLoadExec(path, NULL);
}
static void sleep_ms(unsigned ms) { sceKernelUsleep(ms * 1000u); }
static void exit_title(void) { hos_ps5_exit(0); }
static DWORD WINAPI relaunch_main(LPVOID unused) {
    (void)unused;
    static const HosPs5Relaunch ops = {say, close_work, load_exec, sleep_ms, exit_title};
    hos_ps5_relaunch_run(&ops);
    return 0;
}
static volatile LONG asked;   /* (the title started again or closed: once, whichever the menu asked first) */
/* (vk_menu.cpp, the row's second press, once the options are saved and the launch screen's file is left) 1: on its way; 0: no
   thread for it, nothing done (the game goes on, and says so) */
int hos_ps5_back_to_launcher(void) {
    if (InterlockedExchange(&asked, 1)) return 1;   /* (asked already: on its way) */
    const HANDLE t = CreateThread(NULL, 1u << 20, relaunch_main, NULL, 0, NULL);
    if (!t) {
        hos_log("relaunch: no thread for it: nothing done, the game goes on");
        InterlockedExchange(&asked, 0);
        return 0;
    }
    SetThreadDescription(t, L"relaunch");
    CloseHandle(t);
    return 1;
}
/* (lot 74) The in-game menu's Quit on the console (vk_menu.cpp quit_title, its second press, once the options are saved and the
   pipeline cache kept): the close work, then the title closes (hos_ps5_quit_run). The menu's WM_CLOSE had no one to read it here:
   Quit did nothing but save. On a thread of its own, as above. 1: on its way; 0: no thread for it, nothing done */
static DWORD WINAPI quit_main(LPVOID unused) {
    (void)unused;
    static const HosPs5Relaunch ops = {say, close_work, load_exec, sleep_ms, exit_title};
    hos_ps5_quit_run(&ops);
    return 0;
}
int hos_ps5_quit(void) {
    if (InterlockedExchange(&asked, 1)) return 1;   /* (asked already: on its way) */
    const HANDLE t = CreateThread(NULL, 1u << 20, quit_main, NULL, 0, NULL);
    if (!t) {
        hos_log("quit: no thread for it: nothing done, the game goes on");
        InterlockedExchange(&asked, 0);
        return 0;
    }
    SetThreadDescription(t, L"quit");
    CloseHandle(t);
    return 1;
}
#endif /* __PROSPERO__ */
