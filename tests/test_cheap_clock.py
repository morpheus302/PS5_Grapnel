"""(ps5, lot 59) The render thread's clock reads off the system clock, behind RX_VK_CHEAP_CLOCK and RX_VK_REPORT_TSC (cheap_clock.h).

A clock_gettime is a system call on the console (0.886 us a read, cheap_clock.h). Checked here without the game or Vulkan:

- cheap_clock.h's report_ns: off, or before the first present, the host's clock is read as before; on, the report's time is
  the present's host tick plus the TSC since, never before the present, and the host's clock is not read;
- skip_live as vk_draw.cpp has it (cut out intact, its steady_clock read counted): RX_VK_CHEAP_CLOCK unset, one system clock
  read a draw as before; set, none; and the skip file is looked at again after 300 ms of the clock it says, not of the other;
- in the source: the counter report's time goes through report_ns with host->ticks() as its fallback, the present takes the
  TSC anchor where it takes ts_anchor_ns, and the render thread's job timing reads steady_clock only without the switch (or
  with RX_VK_TIMELINE, which prints it).

What this cannot show: the time saved on the console (to be measured there, dist/next/59-fil-de-rendu/LISEZMOI.md).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def between(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j + len(end)]


def cut_skip_live(draw):
    code = between(draw, "bool skip_live(uint64_t key) {", "    return std::binary_search(keys.begin(), keys.end(), key);\n}\n")
    if code.count("std::chrono::steady_clock::now()") != 1:
        raise SystemExit("skip_live: expected one steady_clock read (the switch's off side)")
    if "cheap_clock::timers_on()" not in code or "prof_now() / 1000000" not in code:
        raise SystemExit("skip_live: the 300 ms are not read on the TSC under RX_VK_CHEAP_CLOCK")
    return code.replace("std::chrono::steady_clock::now()", "fake_steady_now()")


def check_replay(replay):
    report = between(replay, "case NOP_REPORT_COUNTER: {", "        case NOP_RESET_COUNTER: {")
    if "cheap_clock::report_ns(report_tsc, ts_anchor_ns, ts_anchor_tsc, report_tsc ? prof_now() : 0," not in report:
        raise SystemExit("the counter report's time does not go through cheap_clock::report_ns")
    if report.count("host->ticks()") != 1 or "[] { return host->ticks() * 32; }" not in report:
        raise SystemExit("the counter report reads host->ticks() outside report_ns's fallback")
    anchor = "        ts_anchor_ns = host->ticks() * 32;\n        if (cheap_clock::reports_on()) ts_anchor_tsc = prof_now();"
    if replay.count(anchor) != 1:
        raise SystemExit("the present does not take the reports' TSC anchor with ts_anchor_ns")
    loop = between(replay, "void replay_thread_main() {", "\nvoid enqueue(Job&& j) {")
    start = between(loop, "        static const bool cheap_jobs = cheap_clock::timers_on() && !timeline;",
                    "        if (timeline) draw_stats(&d0, &x, &x, &x);")
    if "const auto t_start = cheap_jobs ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now();" not in start:
        raise SystemExit("the job's start reads steady_clock with RX_VK_CHEAP_CLOCK")
    end = between(loop, "        bh_st.job_ns[j.kind == JOB_SUBMIT ? 0 : j.kind == JOB_PRESENT ? 1 : 2] +=", ".count();")
    if "cheap_jobs ? prof_now() - t_start_tsc" not in end:
        raise SystemExit("the job's end reads steady_clock with RX_VK_CHEAP_CLOCK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    check_replay(replay)
    with tempfile.TemporaryDirectory(prefix="cheap-clock-") as tmp:
        tmp = Path(tmp)
        (tmp / "skip_live_cut.inc").write_text(cut_skip_live(draw), encoding="utf-8")
        shutil.copy(ROOT / "gpu/vk/cheap_clock.h", tmp / "cheap_clock.h")
        (tmp / "compat").mkdir()
        shutil.copy(ROOT / "gpu/vk/compat/longshot_env.h", tmp / "compat" / "longshot_env.h")
        exe = tmp / "cheap-clock"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(tmp),
                        str(ROOT / "tests/test_cheap_clock.cpp"), "-o", str(exe)], check=True)
        skip = tmp / "skip.txt"
        for mode, value in (("off", None), ("off", "0"), ("on", "1")):
            env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_CHEAP_CLOCK", "RX_VK_REPORT_TSC")}
            env["RX_VK_SKIP_FILE"] = str(skip)
            if value is not None:
                env["RX_VK_CHEAP_CLOCK"] = value
            subprocess.run([str(exe), str(skip), mode], check=True, timeout=60, env=env)
    print("PASS: cheap clock: report_ns, skip_live on the TSC, the report and job timing sources")


if __name__ == "__main__":
    main()
