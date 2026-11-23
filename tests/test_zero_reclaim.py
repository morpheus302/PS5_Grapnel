"""(ps5) The window's units that are all zeros go back behind the shared zeros.

About 400 MiB of the guest window's backed units held nothing but zeros through a whole session on the console (the system
region committed at start, the rest of a fault's block nothing wrote, heaps the game cleared), of 12 GiB where the memory
floor ends play at scene loads. A pass (host/posix/ps5_window.c hos_ps5_guest_reclaim, every 30 s from host/hos_thread.c's
"hookshot contents" thread) puts each such unit back behind the one read-only unit of zeros a read fault maps, and gives
its direct memory back, while the game's threads run.

These checks keep what makes that lose no write:
- a unit is made read-only under its GiB's lock before it is read, and only then mapped over or made read-write again;
- the shared zeros are mapped over it before its memory is given back, and the zeroed bit is set before the backed bit
  is cleared;
- a fault on a backed unit takes the lock before it asks the kernel anything (it used to refuse a write that met a
  read-only unit);
- a commit holds its units under the same lock (a system call's write into guest memory takes no fault), a pinned unit
  is never looked at, and the rest of a fault's block leaves the units the pass put back;
- a unit behind the shared zeros is given memory that was zeroed before it is mapped there;
- HOOKSHOT_PS5_ZERO_RECLAIM is read at the first pass and the reclaim is off unless it is 1 (since 2026-11-12), and the pass
  is off without the shared zeros or the pins;
- a unit is left alone unless the kernel answers, for the whole unit, that it is plain CPU read-write with no GPU access.
Where PS5_PayloadSDK's model of the console's kernel is there (hookshot-ps5's ps5-stack, or PS5_STACK), the window's code
is built over it and run: the switches, the holds, a write and a commit placed at each moment of a unit's sequence, and
four threads writing beside the passes with every byte compared; units granted GPU access with no pin, over half a unit, or
that the kernel does not answer for; and eight writers, two readers, file reads and pins all at once beside the passes
(scripts/ps5/plat-probe/host_window_test.c, linked with the kernel's protection question answered as the console does).

python -I -B tests/test_zero_reclaim.py [--cc cc] [--no-host-model]
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WINDOW = ROOT / "host" / "posix" / "ps5_window.c"
HEADER = ROOT / "host" / "posix" / "ps5.h"
THREAD = ROOT / "host" / "hos_thread.c"
HOST_TEST = ROOT / "scripts" / "ps5" / "plat-probe" / "host_window_test.c"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def in_order(text, parts, why):
    at = -1
    for part in parts:
        nxt = text.find(part, at + 1)
        need(nxt >= 0, why + ": missing or out of order: " + part)
        at = nxt


def check_window(window, header):
    need("int hos_ps5_guest_reclaim(struct hos_ps5_guest_reclaim *pass);" in header, "host/posix/ps5.h does not declare hos_ps5_guest_reclaim")

    unit = block(window, "static int reclaim_unit(uint64_t u, uint16_t pass, struct hos_ps5_guest_reclaim *o)")
    in_order(unit, ("sceKernelQueryMemoryProtection(at,",
                    "sceKernelMprotect(at, UNIT, PS5_KERNEL_PROT_CPU_READ);",
                    "(prot & PS5_KERNEL_PROT_CPU_WRITE)) {",
                    "reclaim_refused(back ? 2 : 4,",
                    "if (!all_zero(at)) {",
                    "writable_again(u)",
                    "PS5_KERNEL_PROT_CPU_READ, PS5_KERNEL_MAP_FIXED, zero_start",
                    "sceKernelReleaseDirectMemory(table[u % GIB_UNITS] - 1, UNIT);",
                    "set_zero_bit(u, 1);",
                    "set_bit(u, 0);"),
             "a unit is not made read-only, read, mapped over with the zeros and only then given back, in that order")
    query = unit[unit.index("sceKernelQueryMemoryProtection(at,"):unit.index("sceKernelMprotect(at, UNIT, PS5_KERNEL_PROT_CPU_READ);")]
    for part, why in (("!answered ||", "a unit the kernel does not answer for"),
                      ("(uintptr_t)s > (uintptr_t)at ||", "a unit whose answer starts after it"),
                      ("(uintptr_t)e < (uintptr_t)at + UNIT ||", "a unit whose answer ends inside it (a grant over part of it)"),
                      ("(prot & PROT_RW) != PROT_RW ||", "a unit not read-write"),
                      ("(prot & PROT_GPU)) {", "a unit with GPU access")):
        need(part in query, why + " is not left alone")
    need(query.count("return 0;") == 1 and "o->unanswered += !answered;" in query, "a unit left alone is not counted, or not left")
    for never in ("memset(", "lock_gib(", "back_unit("):
        need(never not in unit, "reclaim_unit writes the unit, takes a lock or backs: " + never)
    again = block(window, "static int32_t writable_again(uint64_t u)")
    in_order(again, ("sceKernelMprotect(at, UNIT, PROT_RW);", "sceKernelMapDirectMemory(&placed, UNIT, PROT_RW, PS5_KERNEL_MAP_FIXED, meta["),
             "a kept unit is not made read-write again by the protection, then by its own memory mapped over it")

    run = block(window, "int hos_ps5_guest_reclaim(struct hos_ps5_guest_reclaim *o)")
    # (2026-11-12) the free pieces: asked before each unit of zeros is locked, a piece counted for each unit put back
    in_order(run, ("o->zeros++;", "if (too_many_pieces(&seen, &since, reclaim_walks, &o->pieces)) { o->crowded = crowded = 1; break; }",
                   "lock_gib(g);", "since += did > 0;", "hos_ps5_memory_pieces(&reclaim_walks);"),
             "a pass does not stop when the memory watch counts too many free pieces")
    need("uint64_t hos_ps5_memory_pieces(uint64_t *no_walk) __attribute__((weak));" in window and "#define PIECES_MAX 3000" in window,
         "the watch's count of free pieces is not a weak reference, or the limit moved")
    need(window.count('getenv("HOOKSHOT_PS5_ZERO_RECLAIM")') == 1 and 'getenv("HOOKSHOT_PS5_ZERO_RECLAIM")' in run,
         "HOOKSHOT_PS5_ZERO_RECLAIM is not read inside hos_ps5_guest_reclaim alone (a static initializer runs before the console's settings are read)")
    need("e && e[0] && e[0] != '0' ? 1 : -1" in run, "the reclaim is not off unless HOOKSHOT_PS5_ZERO_RECLAIM is set (and not 0)")
    in_order(run, ("if (pin_bit(u)) {", "if (!reads_zero(u)) continue;", "lock_gib(g);", "state = reclaim_state();",
                   "if (state == HOS_PS5_RECLAIM_RAN && bit(u) && !pin_bit(u) && !before(pass, note_of(u)->until) &&",
                   "!(cstate(cnote(u)) & (C_CLEAN | C_STAGED | C_PARKED | C_SPILLED)))",   # (2026-11-12: the cold units' own)
                   "did = reclaim_unit(u, pass, o);",
                   "unlock_gib(g);"),
             "a unit is not read first without the lock, then asked about again and reclaimed under it")
    # (one lock and one unlock, both inside the loop over the GiB's units: never held from one unit to the next)
    locks = [m.start() for m in re.finditer(r"(?<!un)lock_gib\(g\);", run)]
    need(len(locks) == 1 and run.count("unlock_gib(g);") == 1 and run.index("for (uint64_t u = g * GIB_UNITS;") < locks[0],
         "the pass holds a GiB's lock for more than one unit")
    state = block(window, "static int reclaim_state(void)")
    need("if (__atomic_load_n(&zero_mode, __ATOMIC_ACQUIRE) != 1) return HOS_PS5_RECLAIM_NO_ZEROS;" in state, "the reclaim runs without the shared zeros")
    need("if (__atomic_load_n(&pin_mode, __ATOMIC_SEQ_CST) == 0) return HOS_PS5_RECLAIM_NO_PINS;" in state, "the reclaim runs without pins")
    pin = block(window, "int hos_ps5_guest_pin(void *p, size_t n)")
    in_order(pin, ("__atomic_store_n(&pin_mode, !(e && e[0] == '0'), __ATOMIC_SEQ_CST);", "lock_gib(g);"), "the pins' switch is not written before a lock is taken")

    fault = block(window, "int hos_ps5_guest_fault2(const void *p, int write) {")
    in_order(fault, ("if (bit(u)) {", "lock_gib(u / GIB_UNITS);", "if (bit(u)) {", "sceKernelQueryMemoryProtection(", "unlock_gib(u / GIB_UNITS);",
                     "if (answer >= 0) {", "back_units(u, u + 1, &done, 0);", "back_units(blo, bhi, &more, 1);"),
             "a fault on a backed unit does not take the lock before it asks the kernel, or the rest of its block takes in what the reclaim put back")
    units = block(window, "static int32_t back_units(uint64_t lo, uint64_t hi, uint64_t *done, int around)")
    in_order(units, ("lock_gib(g);", "if (around && zero_bit(u) && meta[g] && note_of(u)->reclaimed) continue;", "back_unit(u)", "unlock_gib(g);"),
             "the rest of a fault's block backs the units the reclaim put behind the zeros")

    commit = block(window, "int hos_ps5_guest_commit(void *p, size_t n)")
    in_order(commit, ("lock_gib(g);", "if (!bit(u)) {", "back_unit(u)", "if (before(note->until, until)) note->until = until;", "unlock_gib(g);"),
             "a commit does not hold every unit of its range under the lock")
    need("back_units(" not in commit, "a commit skips the lock for units already backed (the reclaim may hold one read-only)")

    back = block(window, "static int32_t back_unit(uint64_t u)")
    need("const int readable = zero_bit(u) && scratches;" in back, "back_unit does not tell a unit behind the shared zeros from one nothing touched")
    in_order(back, ("if (readable) {", "sceKernelMapDirectMemory(&placed, UNIT, PROT_RW, PS5_KERNEL_MAP_FIXED, start", "memset(scratch, 0, UNIT);",
                    "placed = at;", "sceKernelMapDirectMemory(&placed, UNIT, PROT_RW, PS5_KERNEL_MAP_FIXED, start", "if (!readable) memset(at, 0, UNIT);",
                    "set_bit(u, 1);", "if (n->reclaimed) {", "if (before(pass, n->until)) {"),
             "a unit behind the shared zeros is not given memory zeroed before it is mapped there, or a write soon after a reclaim is not counted")
    print("PASS: host/posix/ps5_window.c: a zero unit is read-only under its lock before it is read, mapped over before its memory goes back; faults, "
          "commits and pins meet it under the same lock", flush=True)


def check_thread(thread, host_test):
    main = block(thread, "static DWORD WINAPI memory_contents_main(void *p)")
    in_order(main, ("Sleep(60000);", "hos_ps5_guest_contents(", "zero_reclaim_pass();", "Sleep(30000);"),
             "the pass does not follow the count of the window's contents, 30 s apart")
    need(main.count("zero_reclaim_pass();") == 1, "more than one pass a turn")
    one = block(thread, "static void zero_reclaim_pass(void)")
    for part in ("hos_ps5_guest_reclaim(&r)", "memory: zero units: pass %llu looked at %llu backed units", "MiB put back behind the shared zeros",
                 "MiB given memory again since the last pass", "the longest lock", "so far"):
        need(part in one, "the pass's line does not say: " + part)
    start = block(thread, "void hos_sched_watch_start(void)")
    need('if (!c || atoi(c) || (z && z[0] && z[0] != \'0\')) {' in start, "the thread is not started for the reclaim alone (HOOKSHOT_PS5_MEMORY_CONTENTS=0)")
    need("reclaim_switches();" in host_test and "reclaim_tests();" in host_test and "stress_writer" in host_test and
         "reclaim_fences();" in host_test and "reclaim_valve();" in host_test and "reclaim_stress();" in host_test and 'setenv("HOOKSHOT_PS5_ZERO_RECLAIM", "1", 1);' in host_test,
         "scripts/ps5/plat-probe/host_window_test.c no longer runs the reclaim's checks")
    print("PASS: host/hos_thread.c: one pass after each count of the window's contents, a line a pass", flush=True)


def check_host_model(cc):
    stack = Path(os.environ.get("PS5_STACK") or ROOT / ".." / ".." / "ps5-stack")
    model = stack / "PS5_PayloadSDK" / "platform" / "tests" / "host_kernel.c"
    if os.name == "nt" or not model.is_file() or not shutil.which("bash") or not shutil.which(cc):
        print("skipped: the window's code over the console's kernel model (no PS5_PayloadSDK platform, or no bash or %s here)" % cc, flush=True)
        return
    with tempfile.TemporaryDirectory(prefix="zero-reclaim-") as tmp:   # (its own program: tests/test_zero_upload.py builds the same one)
        r = subprocess.run(["bash", str(ROOT / "scripts" / "ps5" / "build-plat-probe.sh"), "host-test"], cwd=ROOT,
                           env=dict(os.environ, CC=cc, PS5_STACK=str(stack), HOST_TEST_BIN=str(Path(tmp) / "host_window_test")),
                           capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
    out = r.stdout + r.stderr
    wanted = ("PASS HOOKSHOT_PS5_ZERO_RECLAIM unset: off, as by default",
              "PASS HOOKSHOT_PS5_ZERO_RECLAIM=0: no pass, 1 MiB of zeros stays backed",
              "PASS HOOKSHOT_PS5_ZERO_READS=0 (no shared zeros): no pass",
              "PASS HOOKSHOT_PS5_GPU_PIN=0 (the window does not know what the GPU reaches): no pass",
              "PASS HOOKSHOT_PS5_ZERO_RECLAIM=1 and none of them off: the 1 MiB is held two passes after its commit and put back by the third (16 units)",
              "PASS the read-only protection refused: the reclaim says so and turns itself off",
              "PASS the shared zeros' mapping refused: the unit is made read-write again, the reclaim off",
              "PASS the protection refusing to make a written unit read-write again: its own memory mapped over it, the byte kept",
              "PASS the read-only protection made but the kernel still saying the unit is writable: read-write again, backed, the reclaim off (step 4)",
              "PASS the pinned units stay, all zeros or not",
              "PASS a commit holds its units through the two passes that follow (64 held, 0 put back)",
              "PASS the third pass put the 61 units of zeros back behind the shared zeros and gave their memory back",
              "PASS they read zero with no fault and no memory of their own (mapped r--s)",
              "PASS untouched answers for them as for units nothing wrote",
              "PASS a byte written into one: that unit alone gets memory again",
              "PASS written within two passes of being put back, and zero again: left alone for 8 passes, put back by the 9th",
              "PASS the same again: 16 passes, put back by the 17th",
              "PASS a unit written long after it was put back, and zero again, goes back at the next pass",
              "PASS the address a unit's new memory is zeroed at: its reservation refused once, unmapped and reserved again",
              "PASS a read() into a unit behind the shared zeros is refused by the kernel",
              "PASS after the commit the read lands; two passes later one still lands in the unit beside it",
              "PASS an import over 8 of them backs and pins them: four passes leave them",
              "PASS a decommit of the 4 MiB leaves the 8 pinned units and the GiB's table",
              "PASS one pass over 16 units of zeros with a write or a commit placed in five of them: 13 put back, 2 found written and kept, 1 found committed",
              "PASS a byte written between the pass's look and its lock: the unit is kept, the byte there",
              "PASS a write that faults while the unit is read-only and all zeros: it waits, the unit is put back, then given memory for the write",
              "PASS a write that faults while a written unit is read-only: it waits, the unit is kept, the write is retried: both bytes there",
              "PASS a commit between the pass's look and its lock: the unit is left read-write, a read() into it lands",
              "PASS a commit while the unit is read-only: it waits, backs the unit the pass put back, and the read() that follows lands",
              "PASS 4 threads writing 8 MiB beside",
              "every byte as written (0 wrong)",
              "PASS the model's kernel answers the question as the console does",
              "PASS a unit granted GPU access with no pin, one with a grant over its second half, one the kernel does not answer for",
              "PASS the grants gone and the kernel answering again, four passes leave the three alone, not asked about again this session",
              "PASS the watch counting 5000 free pieces: the pass stops at the first unit of zeros",
              "PASS 2990: the pass puts back units until 3000 pieces could be reached, and stops",
              "PASS a walk of the pieces failed since the last pass: that pass puts none back",
              "PASS 8 threads writing 16 MiB beside",
              "PASS 2 readers going round the same units",
              "PASS a thread's file reads into units the passes take back",
              "units put back by the passes, then pinned one by one beside them: every one stays backed")
    missing = [w for w in wanted if w not in out]
    if r.returncode or missing or "PASSED: 0 failed" not in out or "FAIL " in out:
        print(out[-6000:])
        raise SystemExit("FAIL: the window's host test (exit %d)%s" % (r.returncode, "; not passed: " + "; ".join(missing) if missing else ""))
    stress = [line[5:] for line in out.split("\n") if line.startswith(("PASS 4 threads writing 8 MiB beside", "PASS 8 threads writing 16 MiB beside",
                                                                         "PASS 2 readers", "PASS a thread's file reads"))]
    print("PASS: host/posix/ps5_window.c over the console's kernel model: %d checks, the reclaim's %d among them; %s" %
          (out.count("\nPASS "), len(wanted) - 2, "; ".join(stress)), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    p.add_argument("--no-host-model", action="store_true")
    a = p.parse_args()
    check_window(WINDOW.read_text(encoding="utf-8"), HEADER.read_text(encoding="utf-8"))
    check_thread(THREAD.read_text(encoding="utf-8"), HOST_TEST.read_text(encoding="utf-8"))
    if not a.no_host_model:
        check_host_model(a.cc)


if __name__ == "__main__":
    main()
