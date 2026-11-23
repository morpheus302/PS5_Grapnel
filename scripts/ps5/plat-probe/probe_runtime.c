/* The platform probe's stand-ins for the parts of the runtime it does not link: the logging, thread-naming and statistics
   hooks host/mkw_glue.c and host/pad_sdl.c call (hos_thread.c, hos_prof.c, hos_main.c and display_sdl.c in the game), and
   the lift's function table a64-kit's runtime reads (none here: no game code). Nothing of the platform layer is here.
   SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat_probe.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

unsigned probe_pad_disconnects;   /* (pad_sdl.c's "controller ... disconnected" lines: plat_probe.c pad_text) */
void hos_log(const char *fmt, ...) {
    char line[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, "hid: controller") && strstr(line, "disconnected")) __atomic_add_fetch(&probe_pad_disconnects, 1, __ATOMIC_RELAXED);
    out("[host] %s", line);
}
void hos_log_flush(void) {}
void *hos_ctx(void) { return NULL; }           /* (no guest thread: the crash report skips the guest's registers) */
void *hos_self(void) { return NULL; }
const char *hos_thread_name(void *t) { (void)t; return "probe"; }
void hos_thread_dump(void) {}
void hos_hostcall_stats(void) {}
void hos_vcore_checkpoint(void) {}
int hos_display_owns_sdl(void) { return 0; }   /* (no window: pad_sdl.c polls on a thread of its own) */
int64_t hos_now_ns(void) {
    const struct timespec t = probe_now();
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}
int rx_option(int id, int otherwise) { (void)id; return otherwise; }

/* (a64-kit's runtime: the lift's function table, empty) */
const uint32_t a64_fn_count = 0;
const uint32_t a64_fn_addr[1] = {0};
const void *const a64_fn_ptr[1] = {0};
