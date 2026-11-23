"""(ps5, lot 69) The launch screen's quick shader check (gpu/vk/launcher_check.h read_facts) on cache files made here, in the
renderer's own formats (vk_progcache.cpp disk_init, vk_draw.cpp pipe_notes_refs, pipeline_share.h check_list): the right
heads give the counts and the notes' coverage; another stamp, a file cut short, a wrong magic or no file at all each say so,
and the verdict follows (launcher_logic.h judge). Nothing is written to the files; the check never reads a whole notes file
into memory at once. No game, GPU or console.

    python -I -B tests/test_launcher_check.py [--cxx g++]
"""
import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
STAMP = 0xC1F75256405EFA1E
NOTE = 1200   # (a note's size as the file says it: sizeof PipeNote, whatever it is in this build)

FIXTURE = r'''#include "launcher_check.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::launcher;
int main(int argc, char** argv) {
    CHECK(argc == 3);
    const std::string d = argv[1], c = argv[2];
    CheckPaths p{d + "/hookshot_programs.bin", d + "/hookshot_programs.idx", d + "/hookshot_pipelines.bin",
                 d + "/hookshot_pipeline_states.bin", d + "/skip.txt"};
    const uint64_t stamp = 0xc1f75256405efa1eull;
    ShaderFacts f = read_facts(p, stamp);
    f.driver_known = true, f.driver_bytes = 1009ull << 20;
    const Check k = judge(f, stamp);
    if (c == "good") {
        CHECK(f.programs_present && f.programs_magic && f.programs_stamp == stamp && f.programs_bytes == 16 + 3 * 100);
        CHECK(f.index_present && f.index_ok && f.programs == 3);
        CHECK(f.notes_present && f.notes_ok && f.notes == 600 && f.notes_covered == 400);
        CHECK(f.states_present && f.states_ok && f.states == 7 && f.skipped == 1);
        CHECK(k.verdict == V_WARN && k.why == WHY_COVERAGE && k.coverage == 66);
    } else if (c == "covered") {
        CHECK(f.notes == 600 && f.notes_covered == 600 && k.verdict == V_OK && k.coverage == 100);
    } else if (c == "other_stamp") {
        CHECK(f.programs_present && f.programs_stamp != stamp && !f.index_ok && f.programs == 0 && !f.notes_ok);
        CHECK(k.verdict == V_FAIL && k.why == WHY_VERSION);
    } else if (c == "cut") {
        CHECK(f.notes_ok && f.notes == 2 && f.notes_covered == 2);   // (a note cut short at the end: the whole ones)
        CHECK(f.states_ok && f.states == 1);
    } else if (c == "bad_magic") {
        CHECK(f.programs_present && !f.programs_magic && !f.states_ok && k.verdict == V_FAIL && k.why == WHY_PROGRAMS);
    } else if (c == "none") {
        CHECK(!f.programs_present && !f.index_present && !f.notes_present && !f.states_present && f.skipped == 0);
        CHECK(k.verdict == V_FAIL && k.why == WHY_PROGRAMS && k.notes == V_WARN);
    } else {
        CHECK(false);
    }
    std::printf("%s: verdict %d, %zu programs, %zu notes (%zu covered), %zu states, in %.3f s\n", c.c_str(), (int)k.verdict, f.programs,
                f.notes, f.notes_covered, f.states, f.seconds);
}
'''


def make(d, case):
    """The cache files of a case in d: three programs (keys 11, 22, 33); notes naming them and a key the index lacks"""
    stamp = STAMP if case != "other_stamp" else STAMP ^ 1
    magic = b"HSP1" if case != "bad_magic" else b"XXXX"
    if case == "none":
        return
    (d / "hookshot_programs.bin").write_bytes(magic + b"\0\0\0\0" + struct.pack("<Q", stamp) + b"\xab" * 300)
    keys = [11, 22, 33]
    (d / "hookshot_programs.idx").write_bytes(b"HPIX" + struct.pack("<IQ", 1, stamp) +
                                              b"".join(struct.pack("<QQI", k, 100, 0) for k in keys))
    notes = []
    for i in range(600 if case != "cut" else 2):
        prog = keys[i % 3] if case in ("covered", "cut") or i % 3 != 2 else 99
        notes.append(struct.pack("<QQQ", 1000 + i, prog, 0) + b"\0" * (NOTE - 24))
    body = b"".join(notes) + (b"\x01" * 100 if case == "cut" else b"")
    (d / "hookshot_pipelines.bin").write_bytes(struct.pack("<QQ", stamp, NOTE) + body)
    states = 7 if case != "cut" else 1
    states_magic = b"HKSTATE2" if case != "bad_magic" else b"HKSTATEX"
    (d / "hookshot_pipeline_states.bin").write_bytes(states_magic + struct.pack("<I", 552) + b"\0" * (552 * states) +
                                                     (b"\0" * 10 if case == "cut" else b""))
    (d / "skip.txt").write_text("# the programs left out\n0123456789abcdef\n\n", encoding="ascii")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="launcher-check-") as tmp:
        cpp = Path(tmp) / "check.cpp"
        exe = Path(tmp) / "check"
        cpp.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(cpp), "-o", str(exe)],
                       check=True)
        for case in ("good", "covered", "other_stamp", "cut", "bad_magic", "none"):
            d = Path(tmp) / case
            d.mkdir()
            make(d, case)
            before = {f.name: (f.stat().st_mtime_ns, f.read_bytes()) for f in d.iterdir()}
            subprocess.run([str(exe), str(d), case], check=True)
            after = {f.name: (f.stat().st_mtime_ns, f.read_bytes()) for f in d.iterdir()}
            if before != after:
                raise SystemExit("%s: the check changed the cache files" % case)
    print("PASS: the quick check reads the caches' heads, counts and coverage, and says what is wrong; it writes nothing")


if __name__ == "__main__":
    main()
