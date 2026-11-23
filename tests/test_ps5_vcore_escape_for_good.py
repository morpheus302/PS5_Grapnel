"""On the console a guest thread that escaped a long wait for a virtual core runs without one until it ends, as it did in
every lot before the merge, and a wait that long ends in that escape, not in a starved grant (host/hos_thread.c
vc_escape_for_life, HOOKSHOT_PS5_VCORE_ESCAPE). The merged eboot took upstream's rules there: the escaped thread waits for a
core again after its next block (#1929) and a starved thread is granted one at 125 ms (#594). The game's main thread and
most of its job workers, which escaped at the start of every earlier run and never came back, then waited for one of the
six cores after each of their blocks: 40 times more waits for a core, 27 fps in Kakariko against 35.8 before
(logs/udp/20261116-131147.log against 20261117-000427.log, 2026-11-17).

The virtual-core block of hos_thread.c (from vc_lock to hos_cv_sleep), with HosThread and hos_thread_core, is compiled as
the console builds it (__PROSPERO__: the ticket lock, the 10 ms steps, the 250 ms escape) with a single-threaded harness:
a clock the waits advance, a log that counts escapes. Three runs, one per setting, as the switches are read once:
- no setting: the console's rule. An escaped thread takes no core after its blocks (hos_vcore_away answers 0, no take), a
  thread starved by better ones escapes after 250 ms, no starved grant; the start says the rule in one line.
- HOOKSHOT_PS5_VCORE_ESCAPE=block: upstream's. The escaped thread takes the free core after its next block; the starved
  thread is granted the core after 125 ms, with no escape.
- no setting but HOS_VCORE_STARVE_US=125000: the grant, as set; escapes still last.
No game, GPU or console.

    python -I -B tests/test_ps5_vcore_escape_for_good.py [--cxx clang++] [--source host/hos_thread.c]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block, between  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void *HANDLE;
typedef int BOOL;
typedef unsigned long DWORD;
#define INFINITE 0xFFFFFFFFul
typedef struct { void *p; } SRWLOCK, *PSRWLOCK;
typedef struct { uint32_t seq; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;   /* (the console's has a futex word: vc_lk_wait) */
#define SRWLOCK_INIT {0}
#define HOS_TLS_SLOTS 64
typedef struct { uint64_t x[32]; volatile int poll; } Ctx;
static int64_t now_ns = 1000000000;
static int64_t hos_now_ns(void) { return now_ns; }
enum { HOS_CHAIN_WAIT = 1, HOS_CHAIN_WOKE, HOS_CHAIN_SIGNAL, HOS_CHAIN_SLEEP, HOS_CHAIN_AWAKE, HOS_CHAIN_PRESENT };
#define HOS_CHAIN(kind, key) ((void)(kind), (void)(key))
#define HOS_CPU_MARK(blocking) ((void)0)
static int escapes_logged, rule_lines;
static char rule_line[1024];
static void hos_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, "ran without a core")) escapes_logged++;
    if (strstr(line, "an escape lasts")) rule_lines++, snprintf(rule_line, sizeof rule_line, "%s", line);
}
static int hos_cores(void) { return 2; }
static int hos_ps5_current_cpu(void) { return 0; }
static int sleeps;
static void (*on_sleep)(void);   /* (what the other threads do during one look) */
/* (one thread: a look for a core lets the clock go on by its step, as the console's timed futex wait does) */
static BOOL look(DWORD ms) {
    if (ms == INFINITE) { puts("FAIL: an untimed wait in a single-threaded harness"); exit(1); }
    now_ns += (int64_t)(ms ? ms : 1) * 1000000;
    sleeps++;
    if (on_sleep) on_sleep();
    return 1;
}
static BOOL WaitOnAddress(volatile void *a, void *cmp, size_t n, DWORD ms) { (void)a, (void)cmp, (void)n; return look(ms); }
static void WakeByAddressAll(volatile void *a) { (void)a; }
static BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lk, DWORD ms, unsigned long f) {
    (void)cv, (void)lk, (void)f;
    return look(ms);
}
static void WakeConditionVariable(PCONDITION_VARIABLE cv) { cv->seq++; }
static void AcquireSRWLockExclusive(PSRWLOCK l) { (void)l; }
static BOOL TryAcquireSRWLockExclusive(PSRWLOCK l) { (void)l; return 1; }
static void ReleaseSRWLockExclusive(PSRWLOCK l) { (void)l; }
@STRUCT@
static HosThread *cur;
@CORE@
@VCORES@
static void vc_place(HosThread *t) { (void)t; }   /* (HOOKSHOT_PS5_AFFINITY=all, the default: nothing to place) */
static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
static void thread(HosThread *t, const char *name, int prio, uint64_t mask) {
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->priority = prio, t->core_mask = mask, t->ideal_core = -2;
}
/* both cores held by threads that never give them up while t waits: t escapes; then holder 1 blocks, its core is free */
static void escape(HosThread *t, HosThread *h0, HosThread *h1) {
    memset(vc_owner, 0, sizeof vc_owner), vc_nq = 0, on_sleep = NULL;
    thread(h0, "holder0", 16, 1), thread(h1, "holder1", 16, 2), thread(t, "waiter", 16, 3);
    cur = h0; hos_vcore_back();
    cur = h1; hos_vcore_back();
    check(h0->vc_held == 1 && h1->vc_held == 2, "the two holders hold the two cores");
    const int before = escapes_logged;
    const int64_t t0 = now_ns;
    cur = t; hos_vcore_back();
    check(escapes_logged == before + 1 && !t->vc_held, "the waiter escaped (ran without a core)");
    check(now_ns - t0 >= 250000000 && now_ns - t0 < 300000000, "the console's escape, after 250 ms");
    cur = h1; check(hos_vcore_away() == 1 && !vc_owner[1], "a holder gives its core up for a block");
    cur = t;
}
/* issue 262's case: sw (28) is pinned to core 1, which better threads (16, 17) take in turns at every block */
static HosThread sa, sb, sw;
static void enqueue(HosThread *t) { t->vc_since = now_ns; t->vc_queued = 1; vc_queue[vc_nq++] = t; }
static void take_turns(void) {
    HosThread *old = vc_owner[1];
    vc_release(old);
    if (vc_owner[1] != &sw) enqueue(old);
}
/* returns how long sw waited, in ms */
static int64_t starve(void) {
    memset(vc_owner, 0, sizeof vc_owner), vc_nq = 0, memset(&vc_st, 0, sizeof vc_st);
    sleeps = 0, escapes_logged = 0;
    thread(&sa, "better0", 16, 2), thread(&sb, "better1", 17, 2), thread(&sw, "starved", 28, 2);
    vc_take(&sa, 1), enqueue(&sb);
    cur = &sw;
    on_sleep = take_turns;
    const int64_t t0 = now_ns;
    hos_vcore_back();
    on_sleep = NULL;
    return (now_ns - t0) / 1000000;
}
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    HosThread h0, h1, t;
    escape(&t, &h0, &h1);   /* then a block of the escaped thread (a sleep, an NVN wait, an nn::os wait) */
    const unsigned takes = t.vc_ntakes;
    const int away = hos_vcore_away();
    if (!strcmp(mode, "block")) {
        check(away == 1, "block: hos_vcore_away tells the escaped thread to take a core after its block (#1929)");
        if (away) hos_vcore_back();
        check(t.vc_held == 2, "block: after its block the escaped thread holds the free core");
    } else {
        check(away == 0, "the console's rule: an escaped thread takes no core after its block, it runs without one until it ends");
        if (away) hos_vcore_back();
        check(!t.vc_held && !vc_owner[1], "the console's rule: the free core stays free after the escaped thread's block");
        for (int i = 0; i < 100; i++) {
            const int a = hos_vcore_away();
            if (a) hos_vcore_back();
        }
        SRWLOCK lk = SRWLOCK_INIT;
        CONDITION_VARIABLE cv = {0};
        hos_cv_sleep(&cv, &lk, 1);
        check(!t.vc_held && t.vc_ntakes == takes, "the console's rule: 100 blocks and an nn::os wait later it has taken no core");
    }
    const int64_t waited = starve();
    if (!strcmp(mode, "console")) {
        check(escapes_logged == 1 && !sw.vc_held && vc_st.boosts == 0,
              "the console's rule: the starved thread escapes, no starved grant (as before the merge)");
        check(waited >= 250 && waited < 300, "the console's rule: after 250 ms");
    } else {
        check(escapes_logged == 0 && sw.vc_held == 2 && vc_st.boosts == 1, "the starved thread is granted the core, no escape");
        check(waited >= 125 && waited < 150, "and after 125 ms");
    }
    check(rule_lines == 1, "the start says the escape rule in one line");
    check(strstr(rule_line, !strcmp(mode, "block") ? "until the thread's next block" : "until the thread ends") != NULL,
          "the line names the rule in force");
    if (!bad) printf("ok: %s\n", mode);
    return bad;
}
"""

RUNS = [
    ("console", {}),
    ("block", {"HOOKSHOT_PS5_VCORE_ESCAPE": "block"}),
    ("starve-set", {"HOS_VCORE_STARVE_US": "125000"}),
]
SWITCHES = ("HOOKSHOT_PS5_VCORE_ESCAPE", "HOS_VCORE_STARVE_US", "HOS_VCORE_ESCAPE_US", "HOS_VCORE_ESCAPE_EQ_US",
            "HOS_VCORE_SLICE_US", "HOS_VCORE_CLAIM_US", "HOS_VCORE_STEP_US", "HOS_VCORES", "HOOKSHOT_PS5_VCLOCK")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "host/hos_thread.c")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    vcores = between(src, "static SRWLOCK vc_lock = SRWLOCK_INIT;", "\n#else\nint hos_vcores_on(void) { return 0; }")
    fixture = (HARNESS.replace("@STRUCT@", block(src, "struct HosThread {") + ";\ntypedef struct HosThread HosThread;")
               .replace("@CORE@", block(src, "int hos_thread_core(HosThread *t) {"))
               .replace("@VCORES@", vcores))
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    failed = []
    with tempfile.TemporaryDirectory(prefix="ps5-vcore-escape-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture, encoding="utf-8")
        exe = folder / ("fixture.exe" if os.name == "nt" else "fixture")
        subprocess.run([compiler, "-std=c++17", "-O1", "-w", "-D__PROSPERO__=1", str(folder / "fixture.cpp"), "-o", str(exe)],
                       check=True, timeout=120)
        base = {k: v for k, v in os.environ.items() if k not in SWITCHES}
        for mode, env in RUNS:
            result = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=30, env={**base, **env})
            print(result.stdout, end="")
            if result.returncode or "ok: " + mode not in result.stdout:
                failed.append(mode)
    if failed:
        raise SystemExit("FAIL (%s): on the console an escape must last until the thread ends, with no starved grant, unless "
                         "HOOKSHOT_PS5_VCORE_ESCAPE=block (host/hos_thread.c vc_escape_for_life)" % ", ".join(failed))
    print("ok: the console's escape rule, upstream's with HOOKSHOT_PS5_VCORE_ESCAPE=block, and HOS_VCORE_STARVE_US as set")
    return 0


if __name__ == "__main__":
    sys.exit(main())
