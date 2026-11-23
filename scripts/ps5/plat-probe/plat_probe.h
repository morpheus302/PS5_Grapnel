/* The platform probe title, PPSA99147 "Hookshot Platform Probe" (scripts/ps5/plat-probe/plat_probe.c): what its parts share.
   SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HOOKSHOT_PLAT_PROBE_H
#define HOOKSHOT_PLAT_PROBE_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

static inline struct timespec probe_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}
static inline double probe_ms_since(struct timespec t0) {
    const struct timespec t = probe_now();
    return (double)(t.tv_sec - t0.tv_sec) * 1e3 + (double)(t.tv_nsec - t0.tv_nsec) / 1e6;
}

/* one line to the results file and klog, flushed (plat_probe.c) */
void out(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* a check in a file of its own reports as plat_probe.c's do: PASS/FAIL, INFO and VERDICT lines, the watchdog's deadline and
   progress text (plat_probe.c; scripts/ps5/nup/probe_nup_pc.c on the PC) */
void probe_result(int ok, const char *id, const char *format, ...) __attribute__((format(printf, 3, 4)));
void probe_info(const char *id, const char *format, ...) __attribute__((format(printf, 2, 3)));
void probe_verdict(const char *id, const char *format, ...) __attribute__((format(printf, 2, 3)));
void probe_progress(const char *format, ...) __attribute__((format(printf, 1, 2)));
void probe_check_begin(const char *id, unsigned seconds);
void probe_check_end(void);

/* probe_nup.c: the game's RomFS read in place from the package (host/nup_fs.c), checked against the PC's list */
void check_nup(const char *file, const char *hashes, unsigned threads, unsigned seconds);

/* probe_sched.c: the console's scheduling of a title's threads, and the game's virtual cores' pattern on it */
void check_sched(unsigned threads, unsigned cores, unsigned spinners, unsigned seconds);

/* probe_vk.c */
int vk_probe_init(char *why, size_t why_size, char *info, size_t info_size);
uint64_t vk_probe_import_alignment(void);
/* imports ptr..+bytes, between(ctx) once it is bound, then the GPU writes and the CPU checks: the wrong words, or -1 */
int64_t vk_probe_import(void *ptr, uint64_t bytes, uint32_t seed, void (*between)(void *), void *ctx, double *ms, char *why,
                        size_t why_size);
void vk_probe_fini(void);
#endif
