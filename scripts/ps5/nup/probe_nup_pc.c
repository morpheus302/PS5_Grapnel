/* The platform probe's nup check (scripts/ps5/plat-probe/probe_nup.c, as the console runs it) run on the PC, against the PC's
   package and the list the probe carries (scripts/ps5/nup/check.sh probe <file.nup> <list>): the same code, its report lines
   on standard output.
   SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat_probe.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
static void line(const char *head, const char *id, const char *format, va_list ap) {
    printf("%s%s%s", head, id ? id : "", id ? ": " : "");
    vprintf(format, ap);
    putchar('\n');
    fflush(stdout);
}
void out(const char *format, ...) { va_list ap; va_start(ap, format); line("", NULL, format, ap); va_end(ap); }
void probe_result(int ok, const char *id, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    line(ok ? "PASS " : "FAIL ", id, format, ap);
    va_end(ap);
    fails += !ok;
}
void probe_info(const char *id, const char *format, ...) { va_list ap; va_start(ap, format); line("INFO ", id, format, ap); va_end(ap); }
void probe_verdict(const char *id, const char *format, ...) { va_list ap; va_start(ap, format); line("VERDICT ", id, format, ap); va_end(ap); }
void probe_progress(const char *format, ...) { (void)format; }
void probe_check_begin(const char *id, unsigned seconds) { printf("BEGIN %s (deadline %u s)\n", id, seconds); }
void probe_check_end(void) {}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: probe_nup_pc <file.nup> <nup-hashes.txt> [threads] [seconds]\n"); return 2; }
    check_nup(argv[1], argv[2], argc > 3 ? (unsigned)atoi(argv[3]) : 8, argc > 4 ? (unsigned)atoi(argv[4]) : 5);
    printf("SUMMARY fail=%d\n", fails);
    return fails != 0;
}
