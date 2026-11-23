"""(ps5, lot 62) QueryPerformanceCounter on the time stamp counter behind HOS_TSC_CLOCK=1 (host/posix/tsc_clock.h).

On the console a clock_gettime is a system call (0.886 us a read); QueryPerformanceCounter is hos_now_ns's clock and the
game's (GetSystemTick, CNTPCT). Checked here without the game, on this PC's TSC:

- the console's QueryPerformanceCounter, cut from host/posix/windows.h, and tsc_clock.h's start: off, one system clock read
  a call and the value the system clock gives, the start touching nothing; on, measured over 100 ms at least, no system
  clock read a call, no step back and no jump at the switch, the rate within 5e-4 of CLOCK_MONOTONIC's over 300 ms, each
  thread's reads never going back (four threads), a TSC behind the start's reading as the start;
- in the source: the switch is read with env_on once the settings are loaded (stage 4, before the game's threads), the clock
  published after it is filled; hos_now_ns and the game's CNTPCT read QueryPerformanceCounter; the waits of
  win32_posix.c never take its value (their deadlines are on CLOCK_MONOTONIC or relative); the off side is the PC's code;
  the renderer's prof_now calibrates against QueryPerformanceCounter (with the switch on, cheap_clock.h's TSC rate is
  this one's).

What this cannot show: the time saved on the console, or the console's TSC (dist/next/62-horloge-tsc/LISEZMOI.md).

python -I -B tests/test_tsc_clock.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "the console's clock code: x86-64 POSIX (clock_gettime, the TSC), checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
QPC_PS5 = "#ifdef __PROSPERO__\n/* (ps5, lot 62) HOS_TSC_CLOCK=1"
QPC_OFF = """    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}
"""


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def between(text, start, end):
    check(start in text, "no '%s'" % start.strip()[:80])
    i = text.index(start)
    check(end in text[i:], "no '%s' after '%s'" % (end.strip()[:60], start.strip()[:60]))
    return text[i:text.index(end, i) + len(end)]


def cut_qpc(windows):
    """the console's QueryPerformanceCounter, from its #ifdef to its #else (the PC's)"""
    block = between(windows, QPC_PS5, "#else\n")
    pc = between(windows[windows.index(block) + len(block):], "static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c) {", "#endif\n")
    check(block.count("static inline BOOL QueryPerformanceCounter") == 1, "windows.h: one console QueryPerformanceCounter")
    check(block.endswith(QPC_OFF + "#else\n"), "windows.h: the console's off side is not the system clock read as before")
    check(pc.endswith("{\n" + QPC_OFF + "#endif\n"), "windows.h: the PC's QueryPerformanceCounter changed")
    check("__atomic_load_n(&hos_tsc_qpc, __ATOMIC_ACQUIRE)" in block and "hos_tsc_clock_ns(&hos_tsc_clk, __builtin_ia32_rdtsc())" in block,
          "windows.h: the console's QueryPerformanceCounter does not read the TSC clock under hos_tsc_qpc")
    return block[len("#ifdef __PROSPERO__\n"):-len("#else\n")]


def check_sources():
    text = lambda p: (ROOT / p).read_text(encoding="utf-8")
    platform, main = text("host/posix/ps5_platform.c"), text("host/hos_main.c")
    start = between(platform, "void hos_ps5_tsc_clock_start(char *line, size_t size) {", "\n}\n")
    check('hos_tsc_clock_start(env_on("HOS_TSC_CLOCK"), &tsc_early, 100000000, &clk, line, size)' in start,
          "ps5_platform.c: the switch is not HOS_TSC_CLOCK by env_on's rule, from the constructor's pair, over 100 ms")
    fill, publish = start.find("hos_tsc_clk = clk;"), start.find("__atomic_store_n(&hos_tsc_qpc, 1, __ATOMIC_RELEASE);")
    check(0 <= fill < publish, "ps5_platform.c: the clock is not filled before it is published")
    check("__attribute__((constructor)) static void tsc_clock_early(void) { hos_tsc_pair(&tsc_early, 8); }" in platform,
          "ps5_platform.c: no pair taken before main")
    env = main.index("const int env = load_longshot_env();")
    call = main.index("hos_ps5_tsc_clock_start(clock_line, sizeof clock_line);")
    check(env < call < main.index("ps5: stage 5 broker") < main.index("ps5: stage 6 game"),
          "hos_main.c: the clock is not started after the settings and before the broker and the game")
    check('hos_ps5_trail("ps5: %s", clock_line);' in main, "hos_main.c: the clock's line is not in the trail")
    # (what moves with the switch: hos_now_ns and the game's ticks; what does not: the waits' own deadlines)
    now = between(text("host/hos_thread.c"), "int64_t hos_now_ns(void) {", "\n}\n")
    check("QueryPerformanceCounter(&now);" in now, "hos_now_ns does not read QueryPerformanceCounter")
    check("uint64_t hos_ticks(void) { return recomp_cntpct(NULL); }" in text("host/hos_thread.c"), "hos_ticks is not the game's CNTPCT")
    cnt = between(text("host/mkw_glue.c"), "uint64_t a64_cntpct_host(void) {", "\n}\n")
    check("QueryPerformanceCounter(&now);" in cnt, "the game's CNTPCT (a64_cntpct_host) does not read QueryPerformanceCounter")
    posix = text("host/posix/win32_posix.c")
    check("QueryPerformanceCounter" not in posix and "hos_now_ns" not in posix,
          "win32_posix.c reads QueryPerformanceCounter or hos_now_ns: a wait could be handed a deadline on the TSC clock")
    check("static int64_t now_ns(void) {\n    struct timespec ts;\n    clock_gettime(CLOCK_MONOTONIC, &ts);" in posix,
          "win32_posix.c: the waits' now_ns is not CLOCK_MONOTONIC")
    table = between(platform, "static long table_wait(volatile uint32_t *a, uint32_t v, const struct timespec *rel) {", "\n}\n")
    check("clock_gettime(CLOCK_MONOTONIC, &until);" in table, "ps5_platform.c: the futex table's deadline is not on CLOCK_MONOTONIC")
    prof = between(text("gpu/vk/vk_internal.h"), "inline int64_t prof_now() {", "\n}\n")
    check("QueryPerformanceCounter(&a);" in prof and "QueryPerformanceCounter(&b);" in prof,
          "prof_now does not calibrate the TSC against QueryPerformanceCounter (cheap_clock.h's rate would not be this one's)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    check_sources()
    qpc = cut_qpc((ROOT / "host/posix/windows.h").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="tsc-clock-") as tmp:
        tmp = Path(tmp)
        (tmp / "qpc_cut.inc").write_text(qpc, encoding="utf-8")
        exe = tmp / "tsc-clock"
        subprocess.run([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(tmp),
                        "-I", str(ROOT / "host/posix"), str(ROOT / "tests/test_tsc_clock.c"), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        print(out.stdout, end="")
        check(out.returncode == 0 and out.stdout.rstrip().endswith("PASS"), "the program: exit %d %s" % (out.returncode, out.stderr))
    print("PASS: the TSC clock: off as before, on without the system clock, its rate, the switch, the sources")


if __name__ == "__main__":
    main()
