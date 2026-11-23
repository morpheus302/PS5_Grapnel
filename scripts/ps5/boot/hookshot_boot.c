/* (ps5) The boot test: PPSA99145 "Hookshot PS5" built with nx2/build.py --runtime, Hookshot's runtime and renderer with none
   of the game's code. host/hos_main.c's main makes the game's start (stages 1 to 5: klog, the guest window, the data root,
   hookshot_env.txt, the write helper) and then calls hos_ps5_boot in place of the game, which goes on with the host's own
   calls, the game's way, as far as they go without the game:

     stage 6  files      the boot log (hookshot_boot.txt in the data root, beside klog), what the root holds for the game
     stage 7  thread     a thread as the game's (64 MiB of stack in direct memory, the IEEE floating point)
     stage 8  memory     the console's direct memory; a commit, write, read and decommit in the guest window; the host's
                         commit-on-touch and crash handlers
     stage 9  renderer   the host's display path (host/posix/display_sdl.c hos_display_start -> host/rx_backend.c rx_start ->
                         the module table -> mkwvk_init -> start on the console's display, VK_KHR_display over VideoOut)
     stage 10 frames     presents through the renderer's own path (rx_nvn_present with no picture: a black frame each,
                         paced by the display)
     stage 11 touch      (HOOKSHOT_BOOT_TOUCH=1 only) a guest page backed by the fault handler on first touch
     stage 12 exit       the shell closes the title (sceSystemServiceLoadExec "exit"), no crash report

   Each stage says one line, "ps5: stage <n> <name>: ...", in klog (prefix [hookshot]) and in <root>/hookshot_boot.txt
   (/data/homebrew/PPSA99145/hookshot_boot.txt when the root is /app0): the last line of a run says how far it got.
   HOOKSHOT_BOOT_FRAMES (240), HOOKSHOT_BOOT_MENU=1 (the Insert menu opened over the frames: the renderer's own pipelines
   and text), HOOKSHOT_BOOT_TOUCH=1: in hookshot_env.txt. dist/README-console.md has what each stage should say.

   SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef __PROSPERO__
#include "hos.h"
#include "nup_fs.h"
#include "ps5.h"
#include "../../../gpu/render/rx.h"
#include "../../../gpu/render/rx_nvn.h"

#include <windows.h>   /* (host/posix's) */
#include <ps5platform/memory.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef HOOKSHOT_VERSION
#define HOOKSHOT_VERSION "unknown"
#endif

extern char hos_ps5_stages[];    /* (host/hos_main.c: stages 1 to 5) */
extern FILE *hos_log_tee;         /* (host/hos_thread.c: hos_log's copy) */
extern volatile uint8_t hos_posix_keys[256];   /* (host/posix/display_sdl.c: GetAsyncKeyState's) */
int hos_display_init(void);
void hos_display_start(void);
void hos_commit_init(void);

static FILE *boot_log;
static char boot_path[1024];

static void say(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
    if (boot_log) fprintf(boot_log, "%s\n", line), fflush(boot_log);
    hos_ps5_trail("%s", line);   /* (the stage trail, hookshot_stages.txt: host/posix/ps5_early.c) */
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int env_int(const char *name, int otherwise) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : otherwise;
}
/* <root>/<name>'s size, -1 when missing, -2 for a folder */
static long long root_file(const char *name) {
    char path[1024];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", hos_ps5_data_root(), name);
    if (stat(path, &st)) return -1;
    return S_ISDIR(st.st_mode) ? -2 : (long long)st.st_size;
}
static const char *size_text(long long n, char *buf, size_t size) {
    if (n == -1) snprintf(buf, size, "missing");
    else if (n == -2) snprintf(buf, size, "a folder");
    else if (n >= 1 << 20) snprintf(buf, size, "%.1f MB", (double)n / (1 << 20));
    else snprintf(buf, size, "%lld bytes", n);
    return buf;
}

/* ---- stage 6: the files ---- */
static void stage_files(void) {
    const char *root = hos_ps5_data_root();
    snprintf(boot_path, sizeof boot_path, "%s/hookshot_boot.txt", root);
    boot_log = fopen(boot_path, "w");
    if (boot_log) {
        fputs(hos_ps5_stages, boot_log);   /* (main's stages, said before this file was open) */
        fflush(boot_log);
        chmod(boot_path, 0666);   /* (read and replaced over FTP) */
        hos_log_tee = boot_log;   /* (the host's and the renderer's own lines too) */
    }
    say("ps5: stage 6 files: the boot log %s: %s", boot_path, boot_log ? "open" : strerror(errno));
    char a[32], b[32], c[32], d[32], e[32], f[32], g[32];
    say("ps5: stage 6 files: image.bin %s, hookshot_options.ini %s, language.txt %s, hookshot_programs.bin %s, "
        "hookshot_shaders.bin %s, mkw_vk_pipelines.bin %s, hookshot-broker.elf (/app0) %s",
        size_text(root_file("image.bin"), a, sizeof a), size_text(root_file("hookshot_options.ini"), b, sizeof b),
        size_text(root_file("language.txt"), c, sizeof c), size_text(root_file("hookshot_programs.bin"), d, sizeof d),
        size_text(root_file("hookshot_shaders.bin"), e, sizeof e), size_text(root_file("mkw_vk_pipelines.bin"), f, sizeof f),
        size_text(({ struct stat st; stat("/app0/hookshot-broker.elf", &st) ? -1LL : (long long)st.st_size; }), g, sizeof g));
    {   /* romfs: the game's 186,583 files (24.4 GiB) in 52 folders at the top */
        char path[1024];
        snprintf(path, sizeof path, "%s/romfs", root);
        DIR *dir = opendir(path);
        int entries = 0;
        if (dir) {
            for (struct dirent *de; (de = readdir(dir));)
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) entries++;
            closedir(dir);
        }
        say("ps5: stage 6 files: romfs/: %s", dir ? "present" : hos_romfs_nup ? "not there: the game reads game.nup (stage 3)"
                                                               : "MISSING, and no game.nup either (the game needs one: dist/README-console.md)");
        if (dir) say("ps5: stage 6 files: romfs/ has %d entries at its top (the PC build's: 50 or so)", entries);
    }
    if (hos_romfs_nup) {   /* (the package stage 3 opened, host/nup_fs.c: 64 files spread over its tree read, their first MiB and
                              their last 4 KiB, as the game's nn_fs.c reads them, through its one descriptor) */
        static uint8_t piece[1 << 20];
        const NupInfo *ni = nup_info(hos_romfs_nup);
        unsigned seen = 0, files = 0, right = 0, top = nup_child_count(hos_romfs_nup, NUP_ROOT);
        uint64_t bytes = 0;
        const double t0 = now();
        static uint32_t stack[1024];   /* (folders still to look in: 599 in all) */
        unsigned depth = 0;
        const unsigned step = ni->files / 64 ? ni->files / 64 : 1;
        stack[depth++] = NUP_ROOT;
        while (depth && files < 64) {   /* (depth first, every step-th file) */
            const uint32_t d = stack[--depth];
            for (uint32_t i = 0, k = nup_child_count(hos_romfs_nup, d); i < k && files < 64; i++) {
                const uint32_t e = nup_child(hos_romfs_nup, d, i);
                if (nup_is_dir(hos_romfs_nup, e)) { if (depth < 1024) stack[depth++] = e; continue; }
                if (seen++ % step) continue;
                const uint64_t size = nup_size(hos_romfs_nup, e), head = size < sizeof piece ? size : sizeof piece;
                const uint64_t tail = size < 4096 ? size : 4096;
                const int64_t a = nup_read(hos_romfs_nup, e, 0, piece, head), b = nup_read(hos_romfs_nup, e, size - tail, piece, tail);
                files++, right += a == (int64_t)head && b == (int64_t)tail && nup_read(hos_romfs_nup, e, size, piece, 1) == 0;
                bytes += (uint64_t)(a > 0 ? a : 0) + (uint64_t)(b > 0 ? b : 0);
            }
        }
        say("ps5: stage 6 files: game.nup: %u entries at its top (50 expected); %u of %u files read right (their first MiB and last "
            "4 KiB: %.1f MB in %.0f ms), descriptor %d, %llu reads through it", top, right, files, (double)bytes / 1e6,
            (now() - t0) * 1e3, nup_fd(hos_romfs_nup), (unsigned long long)nup_reads(hos_romfs_nup));
    }
    {   /* user/: the saves and the game's own cache, written by the title */
        char dir[1024], probe[1100];
        snprintf(dir, sizeof dir, "%s/user", root);
        const BOOL made = CreateDirectoryA(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
        snprintf(probe, sizeof probe, "%s/.hookshot_boot_probe", dir);
        FILE *w = fopen(probe, "w");
        int ok = w && fputs("hookshot", w) >= 0;
        if (w) fclose(w);
        char back[16] = "";
        FILE *r = ok ? fopen(probe, "r") : NULL;
        if (r) ok = fgets(back, sizeof back, r) && !strcmp(back, "hookshot"), fclose(r);
        remove(probe);
        say("ps5: stage 6 files: user/ (the saves): %s, a file written and read back: %s", made ? "there" : "NOT MADE",
            ok ? "yes" : "NO");
    }
    {   /* the settings in effect (hookshot_env.txt sets them: stage 4) */
        static const char *const names[] = {"RX_VK_MIRROR", "RX_VK_VRAM_MB", "RX_VK_PRECOMPILE", "RX_VK_NO_PROGRAM_CACHE",
                                            "RX_BACKEND", "HOOKSHOT_PS5_ROOT", "HOOKSHOT_PS5_DISPLAY", "HOOKSHOT_PS5_EAGER_MB",
                                            "HOOKSHOT_PS5_STACK_MB", "HOOKSHOT_PS5_FAULT_KB", "HOOKSHOT_PS5_BROKER",
                                            "HOS_MEMORY_MB", "RADV_PS5_TASK_WORKGROUPS", "RADV_PS5_TASK_COND_EXEC",
                                            "HOOKSHOT_BOOT_FRAMES", "HOOKSHOT_BOOT_MENU", "HOOKSHOT_BOOT_TOUCH"};
        char line[1024] = "";
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            const char *v = getenv(names[i]);
            if (v) snprintf(line + strlen(line), sizeof line - strlen(line), " %s=%s", names[i], v);
        }
        say("ps5: stage 6 files: settings:%s", line[0] ? line : " none set");
    }
}

/* ---- stage 7: a thread as the game's ---- */
static int boot_main(void);
static DWORD WINAPI boot_thread(void *p) {
    (void)p;
    unsigned mxcsr = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    volatile char here;
    say("ps5: stage 7 thread: the game's kind of thread runs (64 MiB of stack asked, at %p), MXCSR %#x (%s)", (void *)&here,
        mxcsr, (mxcsr & 0xffc0) == 0x1f80 ? "IEEE, as the lifted code needs" : "NOT IEEE: FTZ or DAZ or rounding set");
    return (DWORD)boot_main();
}

/* ---- stage 8: memory ---- */
/* (the platform layer's account of the window: host/posix/ps5_window.c) */
static void window_stats(const char *stage) {
    struct hos_ps5_guest_stats w;
    hos_ps5_guest_stats(&w);
    say("ps5: %s: the window's account: %llu MB backed (peak %llu, its tables %llu KB), %llu commits (%llu failed, last error %#x), "
        "%llu faults backed %llu KB each (%llu refused, %llu failed, the slowest %.2f ms)",
        stage, (unsigned long long)(w.backed_bytes >> 20), (unsigned long long)(w.peak_backed_bytes >> 20),
        (unsigned long long)(w.meta_bytes >> 10), (unsigned long long)w.commits, (unsigned long long)w.commit_failures,
        (unsigned)w.last_error, (unsigned long long)w.faults, (unsigned long long)(w.fault_bytes >> 10),
        (unsigned long long)w.fault_refused, (unsigned long long)w.fault_failures, (double)w.fault_ns_max / 1e6);
}
static void stage_memory(void) {
    struct ps5_memory_stats m;
    if (ps5_memory_query(&m))
        say("ps5: stage 8 memory: direct memory %llu MB in all, %llu MB free (largest %llu MB, %u ranges), flexible %llu MB; "
            "the game is told %d MB",
            (unsigned long long)(m.total_bytes >> 20), (unsigned long long)(m.free_bytes >> 20),
            (unsigned long long)(m.largest_bytes >> 20), m.free_ranges, (unsigned long long)(m.flexible_bytes >> 20),
            env_int("HOS_MEMORY_MB", 9809));
    else
        say("ps5: stage 8 memory: ps5_memory_query answered nothing");
    /* the host's handlers, as the game's start installs them (host/hos_main.c reserve_guest) */
    hos_commit_init();
    say("ps5: stage 8 memory: the host's commit-on-touch and crash handlers installed");
    uint8_t *const heap = (uint8_t *)(uintptr_t)(A64_HOST_BASE + HOS_HEAP_BASE);
    const size_t n = 1u << 20;
    if (!hos_ps5_in_guest(heap, n)) {
        say("ps5: stage 8 memory: the guest window is not reserved: no commit test (stage 2)");
        return;
    }
    const double t0 = now();
    const int r = hos_ps5_guest_commit(heap, n);
    const double t1 = now();
    if (r) {
        say("ps5: stage 8 memory: committing 1 MiB of the guest heap FAILED (%#x)", (unsigned)r);
        return;
    }
    for (size_t i = 0; i < n; i += 4096) heap[i] = (uint8_t)(i >> 12);
    int good = 1;
    for (size_t i = 0; i < n; i += 4096) good &= heap[i] == (uint8_t)(i >> 12);
    size_t run = 0;
    const int committed = hos_ps5_guest_committed(heap, &run);
    const int d = hos_ps5_guest_decommit(heap, n);
    say("ps5: stage 8 memory: 1 MiB of the guest heap (%p) committed in %.2f ms, written and read back: %s; the unit map says "
        "committed: %s; decommitted: %s",
        (void *)heap, (t1 - t0) * 1000.0, good ? "same" : "DIFFERENT", committed ? "yes" : "NO", d ? "FAILED" : "yes");
    window_stats("stage 8 memory");
}

/* ---- stage 9: the renderer, the host's way ---- */
static int stage_renderer(void) {
    const int window = hos_display_init();
    say("ps5: stage 9 renderer: hos_display_init: %s", window ? "an SDL window (the platform layer's)" :
                                                       "no SDL window: the renderer makes its surface on the TV");
    const double t0 = now();
    hos_display_start();   /* (rx_start: LoadLibraryA("hookshot_vk.dll") -> mkwvk_init -> start(MkwVkDisplay)) */
    const char *what = rx_describe();
    const int up = what && strstr(what, "Vulkan") != NULL;
    say("ps5: stage 9 renderer: %s in %.0f ms: %s", up ? "started" : "NOT STARTED", (now() - t0) * 1000.0, what ? what : "?");
    return up;
}

/* ---- stage 10: frames ---- */
static void stage_frames(void) {
    const int frames = env_int("HOOKSHOT_BOOT_FRAMES", 240), menu = env_int("HOOKSHOT_BOOT_MENU", 0);
    say("ps5: stage 10 frames: %d black frames through rx_nvn_present%s", frames, menu ? ", the Insert menu open over them" : "");
    double t0 = now(), t30 = 0, last = t0, worst = 0;
    int shown = 0, late = 0;
    for (int i = 0; i < frames; i++) {
        if (menu) hos_posix_keys[VK_INSERT] = i >= 2 && i < 6;   /* (a press of Insert, as a keyboard's: vk_menu.cpp menu_tick) */
        rx_nvn_present(NULL, NULL);
        if (!rx_nvn_wait(rx_nvn_fence(), 2000000000ull)) late++;
        const double t = now();
        if (i == 30) t30 = t;   /* (the first frames fill the swapchain: timed after them) */
        if (i > 30 && t - last > worst) worst = t - last;
        last = t;
        shown++;
        if (shown % 60 == 0) say("ps5: stage 10 frames: %d presented", shown);
    }
    if (menu) hos_posix_keys[VK_INSERT] = 0;
    rx_nvn_finish();
    int64_t draws = 0, skipped = 0, pipelines = 0;
    rx_stats(&draws, &skipped, &pipelines);
    if (shown > 31)
        say("ps5: stage 10 frames: %d presented in %.2f s; frames 31-%d at %.2f ms each (16.68: 59.94 Hz, 8.34: 119.88 Hz), the "
            "slowest %.2f ms; %d waits over 2 s; %lld pipelines",
            shown, last - t0, shown - 1, (last - t30) * 1000.0 / (shown - 31), worst * 1000.0, late, (long long)pipelines);
    else
        say("ps5: stage 10 frames: %d presented in %.2f s", shown, last - t0);
}

/* ---- stage 11: a guest page backed on first touch (host/mkw_glue.c commit_on_touch, from the SIGSEGV handler) ---- */
static void stage_touch(void) {
    if (!env_int("HOOKSHOT_BOOT_TOUCH", 0)) {
        say("ps5: stage 11 touch: skipped (HOOKSHOT_BOOT_TOUCH=1 in hookshot_env.txt to run it)");
        return;
    }
    volatile uint8_t *const p = (volatile uint8_t *)(uintptr_t)(A64_HOST_BASE + HOS_HEAP_BASE + (2ull << 30));
    if (!hos_ps5_in_guest((const void *)p, 1)) {
        say("ps5: stage 11 touch: the guest window is not reserved");
        return;
    }
    say("ps5: stage 11 touch: writing an unbacked guest page at %p: if nothing follows, the fault handler hung or crashed "
        "(close the title from the home screen)", (void *)p);
    const double t0 = now();
    p[0] = 0x5a;
    const int ok = p[0] == 0x5a;
    say("ps5: stage 11 touch: %s in %.2f ms", ok ? "backed by the handler, the write kept" : "the write was LOST",
        (now() - t0) * 1000.0);
    window_stats("stage 11 touch");
    hos_ps5_guest_decommit((void *)p, 1u << 16);
}

static int boot_main(void) {
    stage_memory();
    const int up = stage_renderer();
    if (up) stage_frames();
    else say("ps5: stage 10 frames: skipped: no renderer");
    stage_touch();
    say("ps5: stage 12 exit: %s; the shell closes the title (no crash report expected)",
        up ? "the boot test went through" : "the boot test stopped at the renderer");
    hos_log_tee = NULL;
    if (boot_log) fclose(boot_log), boot_log = NULL;
    return up ? 0 : 1;
}

int hos_ps5_boot(int argc, char **argv) {
    (void)argc, (void)argv;
    stage_files();
    HANDLE t = CreateThread(NULL, 64u << 20, boot_thread, NULL, 0, NULL);
    int code;
    if (t) {
        WaitForSingleObject(t, INFINITE);
        DWORD c = 1;
        GetExitCodeThread(t, &c);
        code = (int)c;
    } else {
        say("ps5: stage 7 thread: CreateThread with 64 MiB of stack FAILED: the rest on the main thread");
        code = boot_main();
    }
    fflush(NULL);
    ExitProcess((UINT)code);   /* (host/posix: the shell's exit, hos_ps5_exit) */
    return code;
}
#endif
