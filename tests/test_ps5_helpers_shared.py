"""(ps5, lot 66) HOOKSHOT_PS5_HELPERS_SHARED=1: the threads the render thread makes leave its CPU (host/posix/ps5_helpers.h).

On the console a new thread starts on its maker's CPUs, and the render thread is pinned to CPU 12 alone: frame
interpolation's display thread, the compile workers and the GPU hang watch ran there with it ("CPUs 0x1000" in the sched
lines of essai-lot65-1). Checked here without the game:

- the rule, ps5_helper_mask compiled from the header: a thread that inherited exactly the render thread's CPUs goes to the
  title's others; any other thread (made by a game thread, the main thread, before the render thread is pinned) is left
  where it is, and so is one for which nothing would be left;
- in the source: win32_posix.c applies it on the console where a thread first registers (CreateThread's thread_entry
  when HOOKSHOT_PS5_AFFINITY=pinned does not place it, and a std::thread's thread_self), reads the switch only once the
  render thread has published its CPUs (its settings are loaded by then), takes the title's CPUs from hos_thread.c
  (stage 6, the main thread's), not the thread's own; vk_replay.cpp publishes the mask only after its pin succeeded; the
  PC's code is untouched.

What this cannot show: what the render thread gains on the console (dist/next/66-image-de-base/LISEZMOI.md).

python -I -B tests/test_ps5_helpers_shared.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "the console's thread code: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]

RULE_C = r"""
#include <stdio.h>
#include "ps5_helpers.h"
static int failed;
static void expect(uint64_t inherited, uint64_t replay, uint64_t title, uint64_t want, const char *what) {
    const uint64_t got = ps5_helper_mask(inherited, replay, title);
    if (got != want) {
        printf("FAIL: %s: inherited %#llx replay %#llx title %#llx -> %#llx, want %#llx\n", what, (unsigned long long)inherited,
               (unsigned long long)replay, (unsigned long long)title, (unsigned long long)got, (unsigned long long)want);
        failed = 1;
    }
}
int main(void) {
    /* the console: the title's CPUs 0-12, the render thread on 12 */
    expect(0x1000, 0x1000, 0x1fff, 0x0fff, "a thread the render thread made goes to CPUs 0-11");
    expect(0x0fff, 0x1000, 0x1fff, 0, "a thread a game thread made (kept off 12) stays");
    expect(0x1fff, 0x1000, 0x1fff, 0, "a thread the main thread made stays");
    expect(0x1000, 0, 0x1fff, 0, "before the render thread has pinned itself: nothing moves");
    expect(0, 0x1000, 0x1fff, 0, "no inherited mask read: nothing moves");
    expect(0x1000, 0x1000, 0x1000, 0, "nothing would be left: it stays");
    /* HOS_RESERVE_CORE_MASK=c00: the render thread on CPUs 10-11 */
    expect(0x0c00, 0x0c00, 0x1fff, 0x13ff, "with a two-CPU render core, its helpers go to the rest");
    expect(0x0400, 0x0c00, 0x1fff, 0, "a thread on one of them only (not inherited from it) stays");
    if (!failed) printf("rule ok\n");
    return failed;
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


def check_rule(cc):
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d) / "rule.c", Path(d) / "rule"
        src.write_text(RULE_C, encoding="utf-8")
        r = subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Werror", "-I", str(ROOT / "host/posix"), str(src), "-o", str(exe)],
                           capture_output=True, text=True)
        check(r.returncode == 0, "the rule does not compile: " + r.stderr[-2000:])
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode == 0, "the rule: " + r.stdout + r.stderr)


def check_sources():
    text = lambda p: (ROOT / p).read_text(encoding="utf-8")
    posix, thread, replay = text("host/posix/win32_posix.c"), text("host/hos_thread.c"), text("gpu/vk/vk_replay.cpp")
    check('#include "ps5_helpers.h"' in posix, "win32_posix.c does not include ps5_helpers.h")
    fn = between(posix, "static void ps5_helper_leave_replay_cpu(pthread_t th, int tid) {", "\n}\n")
    load, env = fn.find("__atomic_load_n(&hos_ps5_replay_cpus, __ATOMIC_ACQUIRE)"), fn.find('getenv("HOOKSHOT_PS5_HELPERS_SHARED")')
    check(0 <= load < fn.find("if (!replay) return;") < env, "win32_posix.c: the switch is read before the render thread published its CPUs")
    check("ps5_helper_mask(inherited, replay, hos_ps5_title_cpus())" in fn, "win32_posix.c: the rule is not given the title's CPUs")
    check("ps5_pthread_getaffinity_np(th" in fn and "ps5_pthread_setaffinity_np(th" in fn, "win32_posix.c: the thread's own mask is not read and set")
    check("hos_ps5_cpus(" not in fn, "win32_posix.c: the title's CPUs asked of the thread itself (its maker's on the console)")
    # (where it applies: the console's registrations, both kinds)
    entry = between(posix, "static void *thread_entry(void *p) {", "#else\n")
    check("hos_ps5_new_thread_cpus" in entry and "} else {\n        ps5_helper_leave_replay_cpu(pthread_self(), (int)t->tid);" in entry,
          "thread_entry: a CreateThread thread not placed by HOOKSHOT_PS5_AFFINITY=pinned is not given the rule")
    self_ = between(posix, "static HThread *thread_self(void) {", "#else\n")
    check("ps5_helper_leave_replay_cpu(t->th, (int)t->tid);" in self_, "thread_self: a std::thread's first registration is not given the rule")
    check(posix.count("ps5_helper_leave_replay_cpu(") == 3, "win32_posix.c: the rule is applied elsewhere than at the two registrations")
    pc_self = between(posix[posix.index(self_) + len(self_):], "        pthread_once(&self_key_once, self_key_make);", "#endif\n")
    check("apply_process_affinity(t->th);" in pc_self and "ps5_helper" not in pc_self, "thread_self: the PC's side changed")
    # (the title's CPUs: hos_thread.c's, taken from the first caller of ps5_affinity_mode)
    title = between(thread, "uint64_t hos_ps5_title_cpus(void) {", "\n}\n")
    check("(void)ps5_affinity_mode();" in title and "return ps5_title_cpus;" in title, "hos_thread.c: hos_ps5_title_cpus is not ps5_title_cpus")
    check("hos_ps5_cpus(&ps5_title_cpus);   /* (the first caller's: main's or the game's thread, before any of ours is pinned) */" in thread,
          "hos_thread.c: ps5_title_cpus is no longer the first caller's")
    # (published by the render thread once pinned, on the console only)
    pin = between(replay, "        if (const uint64_t m = host->reserved_cpus()) {", "logf(\"vk: the replay thread runs on logical processors")
    ps5 = between(pin, "#ifdef __PROSPERO__\n", "#else\n")
    set_at, pub_at = ps5.find("const bool pinned = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)use) != 0;"), \
        ps5.find("if (pinned) __atomic_store_n(&hos_ps5_replay_cpus, (uint64_t)use, __ATOMIC_RELEASE);")
    check(0 <= set_at < pub_at, "vk_replay.cpp: the render thread's CPUs are not published after its pin succeeded")
    check('extern "C" volatile uint64_t hos_ps5_replay_cpus;' in replay, "vk_replay.cpp: hos_ps5_replay_cpus not declared")
    check(replay.count("hos_ps5_replay_cpus") == 2, "vk_replay.cpp: hos_ps5_replay_cpus written elsewhere")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="cc")
    a = ap.parse_args()
    check_rule(a.cc)
    check_sources()
    print("all checks passed")


if __name__ == "__main__":
    main()
