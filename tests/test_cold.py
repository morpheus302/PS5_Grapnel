"""(ps5) Cold units of the guest window set aside on the SSD (HOOKSHOT_PS5_COLD).

In Hyrule Field the window holds 8.7 GiB of the console's 12 and the title stops at its memory floor, while most of it is
files the game loaded and keeps without reading them again (notes-agents/donnees-froides-ssd.md). host/posix/ps5_window.c
parks units of the big heap at a rate (a reservation over the unit, its memory kept), sends the units that stay parked a
minute to a file on the SSD and gives their memory back, and brings a unit back in the fault handler at its first access.

These checks keep what makes that lose nothing:
- a unit coming back from the SSD is filled at an address of its own, its sum checked, and only then mapped at its place;
  what cannot come back ends the title with a line, never maps wrong bytes;
- a park asks the kernel first (the CPU's alone, for all of the unit) and never takes a pinned unit, one behind the shared
  zeros or one held after a commit; its state is set before the reservation, under the GiB's lock;
- the fault handler asks for a cold unit first, under the lock, and the shared zeros' branch and the backed branch ask again;
- a commit and a pin bring a cold unit back read-write before anything else, and a commit holds its units;
- the writer gives a unit's memory back only if it is still parked and did not move; a decommit of a unit it has staged
  leaves that memory to it;
- a unit read back is read-only with its copy kept, so that it goes back with no new write; a write forgets the copy;
- off unless HOOKSHOT_PS5_COLD is set; off without pins.
Where PS5_PayloadSDK's model of the console's kernel is there (hookshot-ps5's ps5-stack, or PS5_STACK), the window's code is
built over it and the cold cases of scripts/ps5/plat-probe/host_window_test.c run (each rule by hand, the SSD's file that
cannot be made, a full SSD, a copy short or changed, no memory, a refused park, the holds, units that move while the writer
has them, the measurement, and writers and readers checking every word beside the cold thread with kernel copies and pins);
then over copies of ps5_window.c with one rule taken out each, where they must fail.

python -I -B tests/test_cold.py [--cc cc] [--no-host-model] [--no-mutations]
(HOOKSHOT_TEST_TMP: where the program and the SSD's files of the cases go, a folder on disk; else the system's temporary folder)
"""
import argparse
import os
import shutil
import signal
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--no-mutations"]   # (tests/run_all.py reads it; its mutations: python -I -B tests/test_cold.py, a few minutes)

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


def check_window(w, header):
    for decl in ("int hos_ps5_cold_start(char *said, size_t cap);", "void hos_ps5_cold_tick(int calm, uint64_t free_mb);",
                 "void hos_ps5_cold_line(char *buf, size_t cap);", "int hos_ps5_cold_probe(char *buf, size_t cap);"):
        need(decl in header, "host/posix/ps5.h does not declare " + decl)

    back = block(w, "static int32_t cold_back(uint64_t u, struct cold_note *n, int write)")
    ssd = back[back.index("} else {"):]
    in_order(ssd, ("if (cold_scratch_lost[u / GIB_UNITS]) cold_fatal(", "r = allocate(UNIT, &start);", "if (reserve_pop(&start)) cold_fatal(", "map_fixed(scratch, PROT_RW, start)",
                   "cold_read(n->slot, scratch, &read_ns)", "if (cold_sum(scratch) != n->sum) cold_fatal(", "if (lay_reservation(scratch, UNIT)) cold_scratch_lost[u / GIB_UNITS] = 1",
                   "map_fixed(place(u), prot, start)", "table[u % GIB_UNITS] = start + 1;"),
             "a unit from the SSD is not filled at its scratch address, checked, and only then mapped at its place")
    need(ssd.count("map_fixed(place(u)") == 1 and "memset(scratch, 0, UNIT)" in ssd,
         "a unit from the SSD is mapped at its place more than once, or a unit of zeros is not zeroed at the scratch address")
    in_order(back, ("const int keep = !write && has_copy(n);", "const int prot = keep ? PS5_KERNEL_PROT_CPU_READ : PROT_RW;",
                    "set_bit(u, 1);", "cset(n, next);", "if (!keep) drop_copy(n);", "n->gen++;"),
             "a unit back is not read-only with its copy for a read, read-write without it for a write, or its bit is set after its state")
    for fatal in ("its bytes could not be read back from the SSD", "the bytes read back from the SSD are not the ones written",
                  "no direct memory to bring it back into", "its memory could not be mapped at its place",
                  "its own memory could not be mapped back at its place"):
        need(fatal in back, "a unit that cannot come back does not end the title: " + fatal)
    fatal = block(w, "static void cold_fatal(uint64_t u, const char *what, int32_t error) {")
    in_order(fatal, ("say_fault(", "hos_ps5_trail(\"memory: STOPPING THE TITLE:", "abort();"), "cold_fatal does not say why and end the title")
    read = block(w, "static int32_t cold_read(uint32_t slot, void *to, uint64_t *ns)")
    in_order(read, ("const int saved = errno;", "pread(fd,", "errno == EINTR", "errno = saved;"),
             "the fault handler's read does not keep errno or retry an interrupted read")

    park = block(w, "static int cold_park(uint64_t u, struct cold_note *n, uint16_t now_s)")
    first = park[:park.index("sceKernelQueryMemoryProtection")]
    for part, why in (("!bit(u) ||", "a unit not backed"), ("pin_bit(u) ||", "a pinned unit"), ("zero_bit(u) ||", "a unit behind the shared zeros"),
                      ("C_FOREIGN | C_STAGED", "a unit the kernel did not answer for, or one being written"),
                      ("before(now_s, n->hold)", "a held unit (a commit, back not long ago)")):
        need(part in first, "a park takes " + why)
    query = park[park.index("sceKernelQueryMemoryProtection"):park.index("const uint64_t t0 = now_ns();")]
    for part in ("!answered ||", "(uintptr_t)qs > (uintptr_t)at ||", "(uintptr_t)qe < (uintptr_t)at + UNIT ||", "(prot & PROT_RW) != want ||",
                 "(prot & PROT_GPU)"):
        need(part in query, "a park does not leave alone a unit the kernel answers so: " + part)
    in_order(park, ("cset(n, (s & ~C_CLEAN) | C_PARKED);", "set_bit(u, 0);", "sceKernelReserveVirtualRange(&placed, UNIT, PS5_KERNEL_MAP_FIXED, UNIT);"),
             "a park lays its reservation before the unit's state says it is parked")
    sweep = block(w, "static uint64_t cold_park_sweep(uint64_t budget)")
    in_order(sweep, ("lock_gib(g);", "cold_park(u, n, now_s)", "unlock_gib(g);"), "the sweep parks without the GiB's lock")

    fault = block(w, "int hos_ps5_guest_fault2(const void *p, int write) {")
    in_order(fault, ("if (cstate(cnote(u)) & (C_PARKED | C_SPILLED | C_CLEAN)) {", "lock_gib(u / GIB_UNITS);", "cold_fault(u, write);",
                     "unlock_gib(u / GIB_UNITS);", "if (bit(u)) {", "lock_gib(u / GIB_UNITS);", "if (cold_fault(u, write)) answer = 1;",
                     "sceKernelQueryMemoryProtection(", "lock_gib(u / GIB_UNITS);", "if (cold_fault(u, write)) r = 0;",
                     "PS5_KERNEL_PROT_CPU_READ, PS5_KERNEL_MAP_FIXED, zero_start", "back_units(u, u + 1, &done, 0);"),
             "the fault handler does not ask for a cold unit first, or its backed and zeros branches do not ask again under the lock")
    cf = block(w, "static int cold_fault(uint64_t u, int write)")
    in_order(cf, ("if (s & (C_PARKED | C_SPILLED)) {", "cold_back(u, n, write);", "if ((s & C_CLEAN) && bit(u)) {", "make_writable(u, n);"),
             "a fault on a clean unit does not make it writable whatever its sense")
    bu = block(w, "static int32_t back_unit(uint64_t u)")
    need(bu.index("if (cold_away(u)) return cold_back(u, cnote(u), 1);") < bu.index("allocate(UNIT, &start)"),
         "back_unit gives fresh memory to a unit away")
    units = block(w, "static int32_t back_units(uint64_t lo, uint64_t hi, uint64_t *done, int around)")
    in_order(units, ("if (cold_away(u)) {", "if (!around && (r = back_unit(u)) != 0) break;", "continue;"),
             "the rest of a fault's block brings back units away")

    commit = block(w, "int hos_ps5_guest_commit(void *p, size_t n)")
    in_order(commit, ("lock_gib(g);", "if (cn && (r = cold_writable(u)) != 0) break;", "if (!bit(u)) {", "if (cn && before(cn->hold, hold)) cn->hold = hold;",
                      "unlock_gib(g);"), "a commit does not bring a cold unit back read-write, or does not hold it")
    pin = block(w, "int hos_ps5_guest_pin(void *p, size_t n)")
    in_order(pin, ("lock_gib(g);", "if (cnote(u) && (r = cold_writable(u)) != 0) break;", "if (pin && !pin_bit(u)) {"),
             "a pin does not bring a cold unit back read-write before it pins it")
    dec = block(w, "int hos_ps5_guest_decommit(void *p, size_t n)")
    in_order(dec, ("if (!has_data(u)) continue;", "else if (ulo >= a && uhi <= end) r = unback_unit(u);",
                   "else if (has_data(u) && (r = cold_writable(u)) == 0) {", "memset("),
             "a decommit skips units away, or zeroes part of one before bringing it back")
    ub = block(w, "static int32_t unback_unit(uint64_t u)")
    in_order(ub, ("if (s & C_STAGED) next |= C_DEAD;", "else sceKernelReleaseDirectMemory(table[u % GIB_UNITS] - 1, UNIT);"),
             "a decommit gives back the memory of a unit the writer has staged")
    need("if (n && (s & C_STAGED) && !(s & C_DEAD)) cset(n, s | C_DEAD);" in ub, "a decommit of a backed unit the writer staged gives its memory back")

    batch = block(w, "static uint64_t cold_write_batch(struct staged *b, unsigned k)")
    in_order(batch, ("b[i].zero = (uint8_t)all_zero(p);", "b[i].sum = cold_sum(p);", "write_all(fd, cold_staging", "const int lost = lay_reservation(cold_staging",
                     "lock_gib(g);", "if (s & C_DEAD) {", "sceKernelReleaseDirectMemory(b[i].start, UNIT);",
                     "} else if (!lost && (s & C_PARKED) && n->gen == b[i].gen && (b[i].zero || b[i].slot)) {", "cold_spill_unit(u, n, b[i].start",
                     "unlock_gib(g);"),
             "the writer gives back a unit's memory before its write, before its staging mapping is gone, or when the unit moved")
    need("ck.cap_bytes" in batch and "COLD_STOP_CAP" in batch, "the session's cap on bytes written is not kept")
    spill = block(w, "static uint64_t cold_spill(uint64_t budget, uint64_t want)")
    in_order(spill, ("lock_gib(g);", "if (has_copy(n)) {", "cold_spill_unit(u, n, start, 0);", "ADD(cold_spills_again, 1);",
                     "map_fixed(cold_staging + (uint64_t)k * UNIT, PS5_KERNEL_PROT_CPU_READ, start)", "cset(n, s | C_STAGED);", "unlock_gib(g);"),
             "a unit with its copy is written again, or one is staged without its lock or writable")
    # (2026-11-13, the review) an address of the thread's own whose reservation is refused is never mapped over again; the probe
    # reserves its staging address again before its memory goes back
    mb = block(w, "static void cold_measure_back(")
    in_order(mb, ("if (cold_scratch_lost[g])", "else if (map_fixed(scratch,", "if (lay_reservation(scratch, UNIT)) cold_scratch_lost[g] = 1"),
             "the measurement maps over a scratch address it lost, or does not look at its reservation")
    probe = block(w, "int hos_ps5_cold_probe(char *buf, size_t cap)")
    in_order(probe, ("cold_space_lost", "if (lay_reservation(cold_staging, (size_t)COLD_BATCH * UNIT)) space_lost();",
                     "sceKernelReleaseDirectMemory(starts[i], UNIT);"),
             "the probe gives its memory back before its staging address is reserved again, or maps over a lost one")
    need("!cold_scratch_lost[g]" in spill and "cold_space_lost" in spill, "the writer sends a unit to the SSD that could not come back")
    tick0 = block(w, "void hos_ps5_cold_tick(int calm, uint64_t free_mb)")
    need("COLD_STOP_SPACE" in tick0, "the cold thread goes on parking and writing once an address of its own is lost")
    spill_unit = block(w, "static void cold_spill_unit(uint64_t u, struct cold_note *n, int64_t start, unsigned extra)")
    need("if (cold_mode == HOS_PS5_COLD_ON) {" in spill_unit and "sceKernelReleaseDirectMemory(start, UNIT);" in spill_unit,
         "the measurement gives memory back, or the real mode does not")

    start = block(w, "int hos_ps5_cold_start(char *said, size_t cap)")
    need(w.count('getenv("HOOKSHOT_PS5_COLD")') == 1 and 'getenv("HOOKSHOT_PS5_COLD")' in start,
         "HOOKSHOT_PS5_COLD is not read in hos_ps5_cold_start alone")
    need('!e || !e[0] || !strcmp(e, "0") || !strcmp(e, "off") ? -1' in start, "the cold units are not off unless HOOKSHOT_PS5_COLD is set")
    # (2026-11-13, the review) only "measure", "1" and "on" are understood: any other value leaves them off, never the real mode
    need('!strcmp(e, "measure") ? HOS_PS5_COLD_MEASURE' in start and '!strcmp(e, "1") || !strcmp(e, "on") ? HOS_PS5_COLD_ON : -2' in start,
         "a value of HOOKSHOT_PS5_COLD it does not know (a typo of \"measure\") turns the real mode on")
    need("__atomic_load_n(&pin_mode, __ATOMIC_SEQ_CST) == 0" in start, "the cold units start without pins")
    for setting, default in (("CAP_MB", "4096"), ("FREE_MB", "3072"), ("AFTER_S", "60"), ("WRITE_MB_S", "64"), ("LO_GIB", "256"), ("HI_GIB", "512")):
        need('env_u64("HOOKSHOT_PS5_COLD_%s", %s)' % (setting, default) in start, "HOOKSHOT_PS5_COLD_%s's default is not %s" % (setting, default))
    need("if (ck.hold_s < 10) ck.hold_s = 10;" in start, "the hold after a commit can be set under 10 s on the console")
    tick = block(w, "void hos_ps5_cold_tick(int calm, uint64_t free_mb)")
    in_order(tick, ("if (pins == 0) {", "if (!calm) {", "cold_park_sweep(", "if (free_mb + away_mb >= ck.free_mb) return;",
                    "cold_reserve_n, __ATOMIC_RELAXED) < ck.reserve_units) return;", "cold_spill("),
             "the cold thread parks or writes while the game is not calm, with no pins, above the free memory wanted, or without its reserve")
    for never in ("hos_log(", "malloc(", "printf(", "pthread_mutex"):
        for name in ("static int32_t cold_back(", "static int cold_fault(", "static int32_t cold_read(", "static void cold_measure_back(",
                     "static int32_t make_writable("):
            need(never not in block(w, name), name + " (the fault handler's) calls " + never)
    print("PASS: host/posix/ps5_window.c: a cold unit comes back filled and checked before it is mapped, is parked only when the kernel and "
          "the pins allow, is written once while it does not change, and a commit, a pin or a fault meets it under its lock", flush=True)


def check_thread(t, host_test):
    main = block(t, "static DWORD WINAPI memory_cold_main(void *p)")
    in_order(main, ("hos_ps5_cold_start(said, sizeof said);", "Sleep(100);", "rx_ps5_presents", "hos_ps5_cold_tick(calm,",
                    "hos_ps5_cold_probe(pr, sizeof pr)", "hos_ps5_cold_line(cl, sizeof cl);", "hos_ps5_trail(\"memory: cold units: %s\", cl);"),
             "the cold thread does not start, step every 100 ms with the calm and the free memory, probe once, or say its line")
    start = block(t, "void hos_sched_watch_start(void)")
    need('getenv("HOOKSHOT_PS5_COLD")' in start and "memory_cold_main" in start, "the cold thread is not started when HOOKSHOT_PS5_COLD asks")
    watch = block(t, "static DWORD WINAPI memory_watch_main(void *p)")
    need('hos_ps5_trail("memory: at the stop, cold units: %s", cl);' in watch, "the memory floor's stop does not say what the cold units were")
    need("cold_tests();" in host_test and "cold_stress_child" in host_test, "scripts/ps5/plat-probe/host_window_test.c no longer runs the cold cases")
    print("PASS: host/hos_thread.c: the cold thread, its line every 30 s, and the stop's", flush=True)


WANTED = (
    "PASS cold units: off when unset (a child)",
    "PASS cold units: HOOKSHOT_PS5_COLD=mesure (not a value it knows): off",
    "PASS cold units: off for a value not understood (a child)",
    "PASS 64 units parked: their memory held",
    "PASS the 64 on the SSD:",
    "PASS their memory given back:",
    "PASS a read of unit 0 brought it back from the SSD, every word right, read-only",
    "PASS a write into it then: read-write",
    "PASS parked again and cold: on the SSD with no new write",
    "PASS written once more, read back with what was written last",
    "PASS a write into a unit on the SSD: brought back read-write",
    "PASS it went to the SSD with nothing written, and reads zeros",
    "PASS after it: a kernel write into one of them went through",
    "PASS a pin over units on the SSD: back read-write, pinned, and the park refuses them",
    "PASS a decommit of 4 units on the SSD: nothing read back",
    "PASS a decommit of 8 KiB inside one: brought back",
    "PASS a unit with GPU access nobody pinned, one with it over half, one the kernel does not answer for: never parked",
    "PASS cold units: the rules, one by one (a child)",
    "PASS cold units: the SSD's file cannot be made (a child)",
    "PASS cold units: the SSD full (a child)",
    "PASS cold units: a park refused (a child: 0)",
    "PASS cold units: the holds (a child)",
    "PASS cold units: units that move while the writer has them (a child)",
    "PASS cold units: the measurement (a child)",
    "PASS the writer's staging address lost (its reservation refused): the 16 units stay parked with their memory",
    "PASS cold units: the writer's address lost (a child)",
    "cold_fatal: its GiB's scratch address was lost",
    "PASS cold units: a GiB's scratch address lost: the next unit of it from the SSD ends the title with its line (exit 77)",
    "cold_fatal: its bytes could not be read back from the SSD",
    "PASS cold units: a copy the SSD gives back short ends the title with its line, never runs on (exit 77)",
    "cold_fatal: the bytes read back from the SSD are not the ones written (sum)",
    "PASS cold units: a copy with a byte changed ends the title with its line, never runs on (exit 77)",
    "[nomem] the reserve: 16 units used, 0 left, every word right: 1",
    "cold_fatal: no direct memory to bring it back into, the reserve spent",
    "PASS cold units: no memory, the reserve spent ends the title with its line, never runs on (exit 77)",
    "PASS cold units: a copy with its last byte changed ends the title with its line, never runs on (exit 77)",
    "PASS 4 units freed while away (3 on the SSD, 1 parked with its copy) and used again: their copies forgotten",
    "PASS cold units: units freed while away and used again (a child)",
    "PASS the reclaim of zero units beside the cold thread, 8 writers, 2 readers and units turned all zeros and back",
    "PASS cold units: the load with the reclaim of zero units beside it (a child: 0)",
    "PASS 8 writers and 2 readers checking every word",
    "PASS cold units: the load (a child: 0)",
    # (2026-11-14, the inquiry into lot 28's save: units freed while away under the load, as a save loaded after the title screen)
    "PASS units freed while away, freed and taken again, freed in part, committed, written zeros, beside the cold thread and the reclaim",
    "PASS cold units: units freed while away, in part and taken again, beside the cold thread and the reclaim (a child: 0)",
)

# one rule taken out each: (what, the text, what it becomes[, more pairs of them]); each copy must fail the cases
MUTATIONS = (
    ("the sum of a unit from the SSD not checked", "if (cold_sum(scratch) != n->sum) cold_fatal(", "if (0) cold_fatal("),
    ("a commit leaves a clean unit read-only", "if (cn && (r = cold_writable(u)) != 0) break;", "if (0) break;"),
    ("a park takes a pinned unit", "if (!bit(u) || pin_bit(u) || zero_bit(u) || (s & (C_PARKED",
     "if (!bit(u) || zero_bit(u) || (s & (C_PARKED"),
    ("a unit read back mapped read-write with its copy kept", "const int prot = keep ? PS5_KERNEL_PROT_CPU_READ : PROT_RW;", "const int prot = PROT_RW;"),
    ("the fault handler does not ask for cold units (first, and in its zeros' branch): a read gets the shared zeros",
     ("if (cstate(cnote(u)) & (C_PARKED | C_SPILLED | C_CLEAN)) {", "if (0) {"),
     ("if (cold_fault(u, write)) r = 0;   /* (2026-11-12: away since", "if (0) r = 0;   /* (")),
    ("a commit does not hold its units", "if (cn && before(cn->hold, hold)) cn->hold = hold;", "(void)hold;"),
    ("no reserve for the units coming back", "if (reserve_pop(&start)) cold_fatal(", "if (1) cold_fatal("),
    ("a park without the kernel's word", "if (!answered || (uintptr_t)qs > (uintptr_t)at || (uintptr_t)qe < (uintptr_t)at + UNIT || (prot & PROT_RW) != want || (prot & PROT_GPU)) {",
     "if (0) {"),
    # (2026-11-13, the review's)
    ("a decommit of a unit away keeps its copy (a unit used again would come back with its old bytes)",
     "        if (n->slot) ADD(cold_stale, 1);\n        n->slot = 0;\n        cset(n, next);", "        cset(n, next);"),
    ("the sum covers half of a unit", "for (size_t i = 0; i < UNIT / 8; i += 4) a = (a ^ w[i]) * k", "for (size_t i = 0; i < UNIT / 16; i += 4) a = (a ^ w[i]) * k"),
    ("a clean unit made writable keeps its copy", "    if (r) return r;\n    drop_copy(n);\n    __atomic_sub_fetch(&st.cold_clean, 1, __ATOMIC_RELAXED);",
     "    if (r) return r;\n    __atomic_sub_fetch(&st.cold_clean, 1, __ATOMIC_RELAXED);"),
    ("a read fault that meets a clean unit is refused",
     "    if ((s & C_CLEAN) && bit(u)) {\n        const int32_t r = make_writable(u, n);\n        if (r) cold_fatal(u, \"a read-only unit could not be made writable\", r);\n        return 1;",
     "    if ((s & C_CLEAN) && bit(u) && write) {\n        const int32_t r = make_writable(u, n);\n        if (r) cold_fatal(u, \"a read-only unit could not be made writable\", r);\n        return 1;"),
    ("the writer gives back the memory of a batch whose staging address is lost", "} else if (!lost && (s & C_PARKED) && n->gen == b[i].gen",
     "} else if ((s & C_PARKED) && n->gen == b[i].gen"),
    ("a unit from the SSD is filled at a scratch address that was lost",
     "        if (cold_scratch_lost[u / GIB_UNITS]) cold_fatal(u, \"its GiB's scratch address was lost", "        if (0) cold_fatal(u, \"its GiB's scratch address was lost"),
    ("a value of HOOKSHOT_PS5_COLD it does not know is the real mode", '!strcmp(e, "1") || !strcmp(e, "on") ? HOS_PS5_COLD_ON : -2', "HOS_PS5_COLD_ON"),
    # (2026-11-14, the inquiry into lot 28's save: what would turn a unit's bytes into zeros with no line)
    ("a decommit of a unit away keeps its mark of zeros (the unit used again and parked would come back all zeros)",
     "    unsigned next = s & ~(C_PARKED | C_SPILLED | C_CLEAN | C_ZERO);", "    unsigned next = s & ~(C_PARKED | C_SPILLED | C_CLEAN);"),
    ("the writer takes a unit for zeros on its first half", "    for (size_t i = 0; i < UNIT / 8; i += 8)\n        if (w[i] | w[i + 1]",
     "    for (size_t i = 0; i < UNIT / 16; i += 8)\n        if (w[i] | w[i + 1]"),
)


def build_and_run(cc, stack, window, tmp, name):
    platform = stack / "PS5_PayloadSDK" / "platform"
    exe = Path(tmp) / name
    cmd = [cc, "-std=gnu11", "-O1", "-g", "-w", "-pthread", "-DHOOKSHOT_PS5_HOST_MODEL", "-D_GNU_SOURCE",
           "-I" + str(ROOT / "host" / "posix"), "-I" + str(platform / "include"), "-I" + str(platform / "src"), "-I" + str(platform / "tests"),
           str(window), str(ROOT / "host" / "posix" / "ps5_fiber.c"), str(HOST_TEST), str(platform / "src" / "shm.c"),
           str(platform / "src" / "placement.c"), str(platform / "tests" / "host_kernel.c"), "-Wl,--wrap=sceKernelQueryMemoryProtection", "-o", str(exe)]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode:
        raise SystemExit("FAIL: the host test does not build (%s):\n%s" % (name, b.stderr[-3000:]))
    files = Path(tmp) / (name + "-files")
    files.mkdir()
    # (2026-11-13, the review: a session of its own, so that a case that deadlocks is killed with the children it forked;
    # subprocess.run's timeout killed the program alone and left a child spinning on a GiB's lock)
    r = subprocess.Popen([str(exe)], env=dict(os.environ, HOOKSHOT_TEST_COLD_ONLY="1", HOOKSHOT_TEST_COLD_DIR=str(files)),
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                         start_new_session=True)
    try:
        out, err = r.communicate(timeout=300)
    except subprocess.TimeoutExpired:
        os.killpg(r.pid, signal.SIGKILL)
        out, err = r.communicate()
        err = (err or "") + "\nFAIL the cases did not end in 300 s (a deadlock): killed with their children\n"
    shutil.rmtree(files, ignore_errors=True)
    return r.returncode, (out or "") + (err or "")


def check_host_model(cc, mutations):
    stack = Path(os.environ.get("PS5_STACK") or ROOT / ".." / ".." / "ps5-stack")
    model = stack / "PS5_PayloadSDK" / "platform" / "tests" / "host_kernel.c"
    if os.name == "nt" or not model.is_file() or not shutil.which(cc):
        print("skipped: the window's code over the console's kernel model (no PS5_PayloadSDK platform, or no %s here)" % cc, flush=True)
        return
    with tempfile.TemporaryDirectory(prefix="cold-", dir=os.environ.get("HOOKSHOT_TEST_TMP")) as tmp:
        code, out = build_and_run(cc, stack, WINDOW, tmp, "host_window_test")
        missing = [w for w in WANTED if w not in out]
        if code or missing or "PASSED: 0 failed" not in out or "FAIL " in out:
            print(out[-8000:])
            raise SystemExit("FAIL: the cold cases over the kernel model (exit %d)%s" % (code, "; not passed: " + "; ".join(missing) if missing else ""))
        load = [line[5:] for line in out.split("\n") if line.startswith("PASS 8 writers")]
        print("PASS: the cold cases over the console's kernel model: %d checks; %s" % (out.count("\nPASS "), "; ".join(load)), flush=True)
        if not mutations:
            return
        source = WINDOW.read_text(encoding="utf-8")
        for mutation in MUTATIONS:
            what, pairs = mutation[0], mutation[1:]
            if isinstance(pairs[0], str):
                pairs = (pairs,)
            mutated = source
            for old, new in pairs:
                need(source.count(old) == 1, "the mutation's text is not in ps5_window.c once: " + old)
                mutated = mutated.replace(old, new)
            copy = Path(tmp) / "ps5_window.c"
            copy.write_text(mutated, encoding="utf-8")
            code, out = build_and_run(cc, stack, copy, tmp, "mutant")
            caught = code != 0 or "FAIL " in out or any(w not in out for w in WANTED)
            fails = [line[5:90] for line in out.split("\n") if line.startswith("FAIL ")]
            need(caught, "a copy where %s passes the cases" % what)
            print("PASS: a copy where %s fails them (%s)" % (what, "; ".join(fails[:3]) or "a case's line missing"), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    p.add_argument("--no-host-model", action="store_true")
    p.add_argument("--no-mutations", action="store_true")
    a = p.parse_args()
    check_window(WINDOW.read_text(encoding="utf-8"), HEADER.read_text(encoding="utf-8"))
    check_thread(THREAD.read_text(encoding="utf-8"), HOST_TEST.read_text(encoding="utf-8"))
    if not a.no_host_model:
        check_host_model(a.cc, not a.no_mutations)


if __name__ == "__main__":
    main()
