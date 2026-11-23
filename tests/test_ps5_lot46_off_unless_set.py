"""Lot 46 brings three upstream changes onto the console line: #1256 (a character's shadow map reserves stepped storage,
HOS_SHADOW_BUCKET), #1255 (the dead render targets swept between the eviction's passes, RX_VK_DEAD_EVERY and RX_VK_POOL_IDLE)
and #1222 (decoded ASTC textures used again by their blocks, RX_VK_ASTC_REUSE). The console tries one change a run, so under
__PROSPERO__ each is off unless its line is in the settings file, and with none of them set the console runs as lot 44 did:
no shadow slot, RX_VK_DEAD_EVERY 10 (the sweep never runs), RX_VK_DEAD_MIN_UNUSED 3, a pooled image's hold 120, no ASTC
reuse. Off the console upstream's defaults stay.

The sources are read as text, and gpu/vk/dead_targets.h is compiled to show that 10 never sweeps. No game, GPU or console.

    python -I -B tests/test_ps5_lot46_off_unless_set.py [--cxx clang++]
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include "dead_targets.h"
#include <cstdio>
int main() {
    using namespace mkwvk::dead_targets;
    for (uint64_t f = 0; f < 100000; f++)
        if (sweep_due(f, 10, true)) { std::printf("FAIL: RX_VK_DEAD_EVERY=10 sweeps at present %llu\n", (unsigned long long)f); return 1; }
    uint64_t n = 0;
    for (uint64_t f = 0; f < 1000; f++) n += sweep_due(f, 1, true);
    if (n != 900) { std::printf("FAIL: RX_VK_DEAD_EVERY=1 sweeps %llu of 1000 presents, not the 900 between the passes\n", (unsigned long long)n); return 1; }
    std::printf("PASS: RX_VK_DEAD_EVERY=10 never sweeps; =1 sweeps every present between the passes\n");
    return 0;
}
"""


def between(text, start, end, what):
    a = text.find(start)
    if a < 0:
        raise SystemExit("FAIL: %s: %r not found (bring this check up to date)" % (what, start))
    b = text.find(end, a)
    if b < 0:
        raise SystemExit("FAIL: %s: %r not found after %r" % (what, end, start))
    return text[a:b + len(end)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++", help="C++ compiler")
    a = p.parse_args()
    fails = []

    def need(cond, what):
        if not cond:
            fails.append(what)

    nvn = (ROOT / "host/nvn_native.c").read_text(encoding="utf-8")
    slot = between(nvn, "static int shadow_slot_layout(", "\n}", "shadow_slot_layout")
    need("#ifdef __PROSPERO__\n        /* (ps5, lot 46" in slot and "step = e ? atoi(e) : 0;\n#else\n        step = e ? atoi(e) : HOS_SHADOW_SLOT_STEP;"
         in slot, "HOS_SHADOW_BUCKET unset: 0 on the console, HOS_SHADOW_SLOT_STEP elsewhere")

    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    reuse = between(tex, "bool reuse_on() {", "\n}", "reuse_on")
    need('#ifdef __PROSPERO__\n        const bool on = getenv("RX_VK_ASTC_REUSE") && atoi(getenv("RX_VK_ASTC_REUSE")) != 0;\n#else\n'
         '        const bool on = !getenv("RX_VK_ASTC_REUSE") || atoi(getenv("RX_VK_ASTC_REUSE")) != 0;' in reuse,
         "RX_VK_ASTC_REUSE unset: off on the console, on elsewhere")
    need("#ifdef __PROSPERO__\nstatic constexpr uint64_t kDeadEveryDefault = 10, kDeadMinUnusedDefault = 3;\n#else\n"
         "static constexpr uint64_t kDeadEveryDefault = 1, kDeadMinUnusedDefault = 1;\n#endif" in tex,
         "RX_VK_DEAD_EVERY and RX_VK_DEAD_MIN_UNUSED unset: 10 and 3 on the console, 1 and 1 elsewhere")
    hold = between(tex, "uint64_t pool_hold() {", "\n}", "pool_hold")
    need("#ifdef __PROSPERO__\n        return (uint64_t)120;\n#else\n        return target_pool::kIdlePresents;" in hold,
         "the pool's hold unset: 120 on the console, target_pool.h's elsewhere")
    need(hold.find('getenv("RX_VK_POOL_IDLE")') < hold.find('getenv("RX_VK_TARGET_POOL_HOLD")'),
         "RX_VK_POOL_IDLE is read before the console's RX_VK_TARGET_POOL_HOLD")

    for f in fails:
        print("FAIL: " + f)
    if fails:
        raise SystemExit("test_ps5_lot46_off_unless_set: failed")
    print("PASS: on the console the three changes of lot 46 are off unless set")

    cxx = shutil.which(a.cxx)
    if not cxx:
        raise SystemExit("test_ps5_lot46_off_unless_set: no C++ compiler %r (pass --cxx)" % a.cxx)
    with tempfile.TemporaryDirectory(prefix="lot46-") as temp:
        d = Path(temp)
        src, exe = d / "fixture.cpp", d / "fixture.exe"
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([cxx, "-std=c++17", "-O2", "-I", str(ROOT / "gpu" / "vk"), str(src), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        print(result.stdout, end="")
        if result.returncode:
            raise SystemExit("test_ps5_lot46_off_unless_set: failed")


if __name__ == "__main__":
    main()
