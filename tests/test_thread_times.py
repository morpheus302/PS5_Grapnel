"""(ps5, lot 40) The per-thread CPU report, HOOKSHOT_PS5_THREAD_TIMES=<seconds> (host/hos_thread_times.c, .h).

The scheduling report's per-thread figures are wrong on the console (pthread_getcpuclockid gives every thread the caller's
clock: in the run of 15/11 every thread read "1.13 s in all", the report thread's own). This report tries five readings
against a witness thread that burns and sleeps known times, keeps those that read it right, and reports by the best kept. It
is an instrument: these checks hold it to measuring and to nothing else.

- the switch is read in hos_thread_times_start alone, which returns before anything when it is unset or 0: no thread, no
  allocation, no system call; it is started from hos_sched_watch_start only; the marks are turned on in one place, after
  the calibration, and HOS_CPU_MARK / VK_CPU_MARK test that before any call (elsewhere than the console they are nothing);
- a mark takes no lock, writes no line, sends no signal, allocates nothing and keeps errno; the report's thread is the only
  one that logs; nothing in the files suspends or signals a thread;
- the marks are where the threads wait: win32_posix.c's futex waits, Sleep and monitor waits (the timeout's way out too),
  the virtual cores' away (once a core is let go) and back, each present, and the renderer's thread loops (job workers,
  serial builds, the replay thread, the replay workers, the capture copy helpers), each outside the loop's lock;
- the files this lot touches are none of the shader translator's stamp (its caches on the console stay valid);
- kinfo_proc's offsets are the SDK's sys/user.h ones (checked with the SDK's compiler when ps5-stack is above the repository);
  the per-thread clock id is FreeBSD's CPUCLOCK_BIT | tid;
- tests/thread_times_test.c is built with the module and run on Linux: every reading kept as Linux has them; each as the
  one in use, its shares of the harness's threads held against /proc/self/task/<tid>/schedstat (3 points by default),
  the thread that never marks said to run unseen by a thread's own reading, the unregistered one seen by kern-proc alone
  and by the kernel's name; then a wrong reading at a time, each refused: the console's pthread_getcpuclockid (the
  caller's clock), getrusage of the process for a thread's, a wall clock, a failing call, kern-proc with its fields 8 bytes
  further (the line then says where the tid, the name and ki_runtime are), with no names, and refused by sysctl;
- where tools/thread-times.py is above the repository, it reads the harness's lines back.

python -I -B tests/test_thread_times.py [--cc cc] [--quick] [--bench]
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "host" / "hos_thread_times.c"
HEADER = ROOT / "host" / "hos_thread_times.h"
POSIX = ROOT / "host" / "posix" / "win32_posix.c"
THREAD = ROOT / "host" / "hos_thread.c"
BACKEND = ROOT / "host" / "rx_backend.c"
VK = ROOT / "gpu" / "vk"
HARNESS = ROOT / "tests" / "thread_times_test.c"
LOT_FILES = [MODULE, HEADER, POSIX, THREAD, BACKEND, VK / "vk_internal.h", VK / "vk_jobs.cpp", VK / "vk_replay.cpp",
             VK / "vk_memory.cpp"]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def code_of(text):
    """The text without its comments (a comment may name what the code must not call)"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(line.split("//")[0] for line in text.split("\n"))


def above(*parts):
    d = ROOT
    while True:
        p = d.joinpath(*parts)
        if p.exists():
            return p
        if d.parent == d:
            return None
        d = d.parent


def check_text():
    module, header, posix = code_of(MODULE.read_text()), HEADER.read_text(), POSIX.read_text()
    thread, backend = THREAD.read_text(), BACKEND.read_text()
    host_c = "".join(code_of(p.read_text(errors="replace")) for p in sorted((ROOT / "host").rglob("*.c")))
    vk_all = "".join(code_of(p.read_text(errors="replace")) for p in sorted(VK.glob("*.cpp")) + sorted(VK.glob("*.h")))

    # the switch: read once, in the start; nothing before the return when it is off
    need(host_c.count('getenv("HOOKSHOT_PS5_THREAD_TIMES")') == 1, "HOOKSHOT_PS5_THREAD_TIMES is read in more than one place")
    start = block(module, "void hos_thread_times_start(void)")
    need('getenv("HOOKSHOT_PS5_THREAD_TIMES")' in start, "the switch is not read in hos_thread_times_start")
    off = start.index("if (p <= 0 || started) return;")
    need(start.index('getenv("HOOKSHOT_PS5_THREAD_TIMES")') < off and all(start.index(x) > off for x in ("tt_spawn", "started = 1")),
         "hos_thread_times_start does something before it returns with the setting off")
    need(re.findall(r"\bhos_thread_times_start\(\);", host_c) == ["hos_thread_times_start();"] and
         "hos_thread_times_start();" in block(code_of(thread), "void hos_sched_watch_start(void)"),
         "the report is not started from hos_sched_watch_start alone")
    need("#if defined(__PROSPERO__) || defined(HOOKSHOT_THREAD_TIMES_TEST)" in MODULE.read_text() and
         "typedef int hos_thread_times_off;" in module, "the module is not empty off the console")

    # the marks: on in one place, after the calibration; the macros test the switch first
    stores = re.findall(r"hos_cpu_marks\s*=[^=]|__atomic_store_n\(&hos_cpu_marks", host_c + vk_all)
    need(len(stores) == 1, "hos_cpu_marks is written in %d places (1 expected)" % len(stores))
    main = block(module, "static void tt_main(void) {")
    need(main.index("tt_calibrate()") < main.index("__atomic_store_n(&hos_cpu_marks, 1"), "the marks are on before the calibration")
    need("#define HOS_CPU_MARK(blocking) do { if (__builtin_expect(hos_cpu_marks != 0, 0)) hos_ps5_cpu_mark(blocking); } while (0)" in header
         and "#define HOS_CPU_MARK(blocking) ((void)0)" in header, "HOS_CPU_MARK does not test the switch first, or is something off the console")
    internal = (VK / "vk_internal.h").read_text()
    need("#define VK_CPU_MARK(blocking) do { if (__builtin_expect(hos_cpu_marks != 0, 0)) hos_ps5_cpu_mark(blocking); } while (0)" in internal
         and "#define VK_CPU_MARK(blocking) ((void)0)" in internal, "VK_CPU_MARK does not test the switch first")
    calls = re.findall(r"(?<!void )\bhos_ps5_cpu_mark\(", re.sub(r"#define (HOS|VK)_CPU_MARK.*", "", host_c + vk_all))
    need(not calls, "hos_ps5_cpu_mark is called other than through the macros (%d times)" % len(calls))

    # a mark: no lock, no line, no signal, no allocation; errno kept
    mark = block(code_of(posix), "void hos_ps5_cpu_mark(int blocking)")
    slot = block(module, "void hos_cpu_slot_mark(struct hos_cpu_slot *s, int blocking)")
    for body, name in ((mark, "hos_ps5_cpu_mark"), (slot, "hos_cpu_slot_mark")):
        for bad in (r"\bhos_log\b", r"\bhos_ps5_trail\b", r"printf\b", r"\bpthread_mutex_\w+", r"\bAcquire\w*", r"\b(\w+_)?lock\(",
                    r"\b(m|c|re)alloc\(", r"\bfree\(", r"\bpthread_kill\b", r"\bsignal\b", r"\bSleep\(", r"\bWait\w*\(", r"\bnanosleep\b"):
            need(not re.search(bad, body), "%s calls %s" % (name, bad))
    need("const int e = errno;" in mark and "errno = e;" in mark, "hos_ps5_cpu_mark does not keep errno")
    for bad in ("pthread_kill", "SIGUSR", "thr_suspend", "thr_kill", "SuspendThread", "hos_posix_ask_where"):
        need(bad not in module, "the module %s" % bad)
    loop = block(module, "static void tt_loop(void)")
    need("hos_log(" in loop and "hos_cpu_slot_mark" not in loop, "the report's lines are not the report thread's")

    # the marks' places
    futex = block(code_of(posix), "static long futex(volatile uint32_t *a, int op, uint32_t v, const struct timespec *ts) {\n    if (op")
    need(re.search(r"HOS_CPU_MARK\(1\);\s*const long r = hos_ps5_futex_wait\(a, v, ts\);\s*HOS_CPU_MARK\(0\);", futex),
         "the console's futex wait is not between its marks")
    need(re.search(r"HOS_CPU_MARK\(1\);\s*while \(nanosleep\(&ts, &ts\) && errno == EINTR\) \{\}\s*HOS_CPU_MARK\(0\);",
                   block(code_of(posix), "void Sleep(DWORD ms)")), "Sleep is not between its marks")
    mon = posix[posix.index("#define MONITOR_WAIT"):posix.index("} while (0)", posix.index("#define MONITOR_WAIT"))]
    need(mon.count("HOS_CPU_MARK(1);") == 1 and mon.count("HOS_CPU_MARK(0);") == 2, "the monitor waits are not between their marks")
    need('#include "../hos_thread_times.h"' in posix.split("#ifdef __PROSPERO__")[0], "win32_posix.c takes the header on the console only")
    away = block(code_of(thread), "int hos_vcore_away(void)")
    need(away.index("vc_release(t);") < away.index("HOS_CPU_MARK(1);") < away.index("return 1;"),
         "hos_vcore_away marks before letting its core go, or where it does not")
    need(block(code_of(thread), "void hos_vcore_back(void)").count("HOS_CPU_MARK(0);") == 1, "hos_vcore_back has no mark")
    need("HOS_CPU_MARK(0);" in block(code_of(backend), "void rx_nvn_present("), "the present has no mark")
    for path, marker, lock in ((VK / "vk_jobs.cpp", "void worker()", "std::unique_lock lk(mu);"),
                               (VK / "vk_jobs.cpp", "void jobs_push_serial(", "std::unique_lock lk(smu);"),
                               (VK / "vk_replay.cpp", "void replay_thread_main()", "std::unique_lock lk(q_mu);"),
                               (VK / "vk_replay.cpp", "void par_worker_main()", "std::unique_lock lk(par_pool.mu);"),
                               (VK / "vk_memory.cpp", "void copy_helper_main()", "std::unique_lock lk(copy_helpers.mu);")):
        body = block(code_of(path.read_text()), marker)
        need(re.search(r"VK_CPU_MARK\(1\);\s*\{\s*" + re.escape(lock) + r".*?\n\s*\}\s*VK_CPU_MARK\(0\);", body, re.S),
             "%s %s: the wait is not between its marks outside the lock" % (path.name, marker))

    # the shader translator's stamp (nx2/build.py's recipe): none of the lot's files
    stamp = [p for top in ("eden", "sirit", "compat") for p in (VK / top).rglob("*") if p.is_file()]
    stamp += [VK / n for n in ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h")]
    need(not set(stamp) & set(LOT_FILES), "a file of the lot is in the shader translator's stamp")

    # FreeBSD's ids
    need("(clockid_t)(0x80000000u | (uint32_t)tid)" in module, "the console's per-thread clock id is not CPUCLOCK_BIT | tid")
    raw = MODULE.read_text()
    need("1 /* KERN_PROC_PID */ | 0x10 /* KERN_PROC_INC_THREAD */" in raw and "1 /* CTL_KERN */, 14 /* KERN_PROC */" in raw,
         "the sysctl's name is not kern.proc.pid with its threads")
    offs = dict(re.findall(r"\b(KP_PID|KP_RUNTIME|KP_TDNAME|KP_TID) = (\d+)", module))
    need(offs == {"KP_PID": "72", "KP_RUNTIME": "328", "KP_TDNAME": "394", "KP_TID": "600"}, "kinfo_proc's offsets changed: %s" % offs)
    sdk = above("ps5-stack", "PS5_VulkanTemplate", ".deps", "native", "ps5-payload-sdk")
    clang = sdk / "bin" / "prospero-clang" if sdk else None
    if clang and clang.exists():
        with tempfile.TemporaryDirectory(prefix="kinfo-") as tmp:
            src = Path(tmp) / "k.c"
            src.write_text("#include <sys/param.h>\n#include <sys/sysctl.h>\n#include <sys/user.h>\n#include <stddef.h>\n" +
                           "".join("_Static_assert(offsetof(struct kinfo_proc, %s) == %s, \"%s\");\n" % (f, offs[k], f) for k, f in
                                   (("KP_PID", "ki_pid"), ("KP_RUNTIME", "ki_runtime"), ("KP_TDNAME", "ki_tdname"), ("KP_TID", "ki_tid"))) +
                           "_Static_assert(KERN_PROC == 14 && KERN_PROC_PID == 1 && KERN_PROC_INC_THREAD == 0x10, \"mib\");\n"
                           "_Static_assert(sizeof(((struct kinfo_proc *)0)->ki_runtime) == 8 && sizeof(((struct kinfo_proc *)0)->ki_tid) == 4, \"sizes\");\n")
            r = subprocess.run([str(clang), "-fsyntax-only", str(src)], capture_output=True, text=True)
            need(r.returncode == 0, "kinfo_proc's offsets are not the SDK's: " + r.stderr[-800:])
        print("PASS kinfo_proc's offsets and the sysctl's name are the SDK's sys/user.h ones (%s)" % clang)
    else:
        print("note: the SDK's compiler (ps5-stack above the repository) is not here: kinfo_proc's offsets checked against the numbers only")
    print("PASS the switch, the marks' places and their rules, the stamp's files", flush=True)


RUNS = [   # (HOOKSHOT_PS5_THREAD_TIMES_SKIP, the reading in use)
    ("", "kern-proc"),
    ("kern-proc,tid-clock,pthread-clock", "own-clock"),
    ("kern-proc,tid-clock,pthread-clock,own-clock", "own-rusage"),
    ("kern-proc,pthread-clock", "tid-clock"),
    ("kern-proc,tid-clock", "pthread-clock"),
]
FAULTS = ["pthread-caller", "rusage-process", "tid-wall", "own-einval", "kp-shift", "kp-noname", "kp-refused"]


def run_harness(cc, quick, bench, tolerance):
    if not shutil.which(cc):
        print("SKIP: no C compiler (%s)" % cc)
        return
    with tempfile.TemporaryDirectory(prefix="thread-times-") as tmp:
        exe = Path(tmp) / "thread_times_test"
        subprocess.run([cc, "-O2", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-pthread", "-D_GNU_SOURCE",
                        "-DHOOKSHOT_THREAD_TIMES_TEST", "-I", str(ROOT / "host"), str(HARNESS), str(MODULE), "-o", str(exe), "-lm"], check=True)
        if bench:
            print(subprocess.run([str(exe), "bench"], capture_output=True, text=True, timeout=120, check=True).stdout.rstrip())
            return
        logs = []
        for skip, reading in RUNS[:2] if quick else RUNS:
            env = dict(os.environ, TT_TOLERANCE=str(tolerance))
            env.pop("HOOKSHOT_PS5_THREAD_TIMES_SKIP", None)
            if skip:
                env["HOOKSHOT_PS5_THREAD_TIMES_SKIP"] = skip
            r = subprocess.run([str(exe), "run", "3"], capture_output=True, text=True, timeout=120, env=env)
            out = r.stdout
            for line in out.split("\n"):
                if line.startswith(("CHECK", "FAIL")):
                    print("     " + line)
            need(r.returncode == 0 and "PASSED:" in out, "the run by %s failed:\n%s" % (reading, out[-4000:]))
            need("reports by %s" % reading in out, "the run skipping %r did not report by %s" % (skip, reading))
            print("PASS %s: %s" % (reading, out.strip().split("\n")[-1]), flush=True)
            logs.append(out)
        for fault in FAULTS:
            r = subprocess.run([str(exe), fault], capture_output=True, text=True, timeout=120)
            need(r.returncode == 0 and "PASSED:" in r.stdout, "the wrong reading %s was not refused as it should:\n%s" % (fault, r.stdout[-4000:]))
            print("PASS %s refused" % fault, flush=True)
            logs.append(r.stdout)
        reader = above("tools", "thread-times.py")
        if reader:   # (the PC's reader, outside the repository: its lines read back from a UDP log's shape)
            udp = Path(tmp) / "udp.txt"
            with udp.open("w") as f:
                for i, line in enumerate(l for l in logs[0].split("\n") if l.startswith("LOG ")):
                    f.write("1794733506.%03d [%8.3f] [hos host] %s\n" % (i % 1000, 1.0 + i * 0.1, line[4:]))
            r = subprocess.run(["python3", "-I", str(reader), str(udp)], capture_output=True, text=True, timeout=60)
            need(r.returncode == 0 and "tt busy" in r.stdout and "kern-proc" in r.stdout, "tools/thread-times.py:\n" + r.stdout + r.stderr)
            print("PASS %s reads the lines back" % reader)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.environ.get("CC") or "cc")
    ap.add_argument("--quick", action="store_true", help="two readings in use instead of five")
    ap.add_argument("--bench", action="store_true", help="the marks' costs only")
    ap.add_argument("--tolerance", type=float, default=3.0, help="points of one CPU a share may differ from schedstat's")
    a = ap.parse_args()
    if not a.bench:
        check_text()
    run_harness(a.cc, a.quick, a.bench, a.tolerance)
    print("ok")


if __name__ == "__main__":
    main()
