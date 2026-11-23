"""(ps5) A capture put back for reuse lets go of page lists bigger than RX_VK_CAPTURE_LIST_KEEP_KB.

gpu/vk/vk_memory.cpp keeps up to eight taken captures (spare) for the next ones, with their four page lists (pages, copied,
slots, late: 4 bytes a page). A list keeps its capacity, so each spare held the lists of the largest capture it ever was for
good. These checks: the lists of more than RX_VK_CAPTURE_LIST_KEEP_KB (1024 KB by default) are let go when a capture is put
back, under the captures' lock, and only then; 0 keeps them all, as before; the line every 600 takes says what the spares'
lists hold. capture_lists_trim and its switch are compiled from their own text.

python -I -B tests/test_capture_list_keep.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]   # (tests/run_all.py reads it)

ROOT = Path(__file__).resolve().parents[1]
MEMORY = ROOT / "gpu" / "vk" / "vk_memory.cpp"


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


FIXTURE = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Capture { std::vector<uint32_t> pages, copied, slots, late; };
@HELPERS@
int main(int argc, char** argv) {
    const bool keep_all = argc > 1;
    Capture c;
    c.pages.assign(300000, 1), c.copied.assign(300000, 2), c.slots.assign(300000, 3), c.late.assign(10, 4);   // (a load's capture)
    c.pages.clear(), c.copied.clear(), c.slots.clear(), c.late.clear();   // (as capture_make clears them for reuse)
    CHECK(capture_lists_bytes(c) >= 3 * 300000 * 4);
    capture_lists_trim(c);
    if (keep_all) {
        CHECK(c.pages.capacity() >= 300000 && st_lists_let_go == 0);
    } else {
        CHECK(c.pages.capacity() == 0 && c.copied.capacity() == 0 && c.slots.capacity() == 0 && c.late.capacity() >= 10);
        CHECK(st_lists_let_go == 3);
        c.pages.reserve(200000);   // (800 KB: under the 1024 KB kept)
        capture_lists_trim(c);
        CHECK(c.pages.capacity() >= 200000 && st_lists_let_go == 3);
    }
    std::printf("ok\n");
}
'''


def run(cmd, env=None):
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        raise SystemExit("FAIL: %s\n%s%s" % (" ".join(map(str, cmd)), r.stdout[-3000:], r.stderr[-3000:]))
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    mem = MEMORY.read_text(encoding="utf-8")
    take = block(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed)")
    put = block(take, "if (spare.size() < 8) {")
    need(put.index("capture_lists_trim(*c);") < put.index("spare.push_back(std::move(c));"), "a capture is put back with its lists whole")
    need(take.index("std::lock_guard lk(cap_mu);") < take.index("capture_lists_trim(*c);"), "the lists are trimmed outside the captures' lock")
    need(mem.count("capture_lists_trim(") == 2, "the lists are trimmed somewhere else too")
    need("vk: the captures' page lists: %zu put back hold %llu KB" in take, "the 600-take line does not say what the spares' lists hold")
    lo = mem.index("uint64_t capture_list_keep() {")
    # (through capture_lists_trim: since the Longshot merge, upstream's late fixes come before take_capture)
    hi = mem.index("\n}\n", mem.index("void capture_lists_trim(Capture& c) {")) + 3
    keep = block(mem, "uint64_t capture_list_keep() {")
    need('getenv("RX_VK_CAPTURE_LIST_KEEP_KB") ? (uint64_t)atoll(getenv("RX_VK_CAPTURE_LIST_KEEP_KB")) : 1024) << 10' in keep and
         "static const uint64_t v" in keep, "RX_VK_CAPTURE_LIST_KEEP_KB is not read once, 1024 KB by default")
    with tempfile.TemporaryDirectory(prefix="capture-lists-") as tmp:
        tmp = Path(tmp)
        (tmp / "f.cpp").write_text(FIXTURE.replace("@HELPERS@", mem[lo:hi]), encoding="utf-8")
        run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Werror", str(tmp / "f.cpp"), "-o", str(tmp / "f")])
        env = dict(os.environ)
        env.pop("RX_VK_CAPTURE_LIST_KEEP_KB", None)
        run([str(tmp / "f")], env)
        run([str(tmp / "f"), "all"], dict(env, RX_VK_CAPTURE_LIST_KEEP_KB="0"))
    print("PASS: a capture put back lets go of its page lists past RX_VK_CAPTURE_LIST_KEEP_KB (1024 KB), under the captures' lock; "
          "0 keeps them all")


if __name__ == "__main__":
    main()
