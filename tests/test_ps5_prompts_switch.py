"""Game-free (hookshot-ps5, lot 72): the button prompts on the console, behind HOOKSHOT_PS5_PROMPTS (host/prompts.c
ps5_prompts, made_dir, setup, ps5_inputs, ps5_face_parts).

Under __PROSPERO__ the switch off (unset, or 0) is the base's eboot: setup finds no prompts before any file call (not even
when prompts/ is there), says nothing, and made_dir makes no folder. On: prompts/ in the game folder (the data root) is
looked at; without it, without the font or without the size table that lets it in, the game's own icons and a line naming
what is missing; a missing part (the textures, the layouts, the face's textures.bin or hint.bin) is said and the rest is
served; made_dir makes cache/prompts/ in the game folder. Off the console nothing changes: prompts/ there is enough, with
or without the switch.

The functions are cut out of the source by their text and compiled with stubs for the Windows calls (stat and mkdir
behind them), once with __PROSPERO__ and once without; each case runs in a process of its own (setup looks once).

python -I -B tests/test_ps5_prompts_switch.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block, between   # noqa: E402

STUBS = r"""
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "gpu/vk/vk_api.h"
#include "gpu/vk/compat/longshot_env.h"
#define MAX_PATH 260
typedef uint32_t DWORD;
typedef long LONG;
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
static int file_calls;   /* (every call that looks at the disk) */
static DWORD GetFileAttributesA(const char *p) {
    struct stat st;
    file_calls++;
    return stat(p, &st) ? INVALID_FILE_ATTRIBUTES : S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
static int CreateDirectoryA(const char *p, void *attr) {
    (void)attr;
    file_calls++;
    return mkdir(p, 0755) == 0;
}
static LONG InterlockedCompareExchange(volatile LONG *d, LONG x, LONG c) { LONG o = *d; if (o == c) *d = x; return o; }
static LONG InterlockedExchange(volatile LONG *d, LONG x) { LONG o = *d; *d = x; return o; }
static void Sleep(DWORD ms) { (void)ms; }
static int log_lines;
static char logged[16384];
static void hos_log(const char *fmt, ...) {
    va_list v;
    va_start(v, fmt);
    const size_t n = strlen(logged);
    vsnprintf(logged + n, sizeof logged - n, fmt, v);
    va_end(v);
    strncat(logged, "\n", sizeof logged - strlen(logged) - 1);
    log_lines++;
}
static const char *game;   /* (the game folder: argv[1]) */
static int game_dir(char *out, size_t cap) { snprintf(out, cap, "%s/", game); return 1; }
static int choose_face(const char *dir) { (void)dir; return HOS_PROMPT_PS5; }   /* (the DualSense's: prompts.c's own is checked elsewhere) */
"""

MAIN = r"""
int main(int argc, char **argv) {
    (void)argc;
    game = argv[1];
    char dir[MAX_PATH], made[MAX_PATH] = "";
    int face = -1;
    const int ok = setup(dir, sizeof dir, &face);
    const int calls = file_calls;
    const int have = made_dir(made, sizeof made);
    struct stat st;
    printf("setup=%d calls=%d logs=%d made=%d dir=%d\n%s", ok, calls, log_lines, have,
           have && !stat(made, &st) && S_ISDIR(st.st_mode) && made[strlen(made) - 1] == '/', logged);
    if (have) printf("made: %s\n", made);
    return 0;
}
"""

FULL = ["External.bfotf", "rsizetable.zs", "Common.bntx", "Common.blarc", "ring.bin", "ps5/textures.bin", "ps5/hint.bin"]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def harness(src, ps5):
    ps5_only = [block(src, "static int ps5_inputs("), block(src, "static void ps5_face_parts(")]
    return "\n".join([
        "#define __PROSPERO__ 1" if ps5 else "", STUBS,
        between(src, "static const char *const faces[", ";\n") + ";",
        block(src, "static int ps5_prompts("),
        block(src, "static int made_dir("),
        between(src, "static volatile LONG setup_state;", "\n"),
        "#ifdef __PROSPERO__", *ps5_only, "#endif",
        block(src, "static int setup("),
        MAIN])


def run(exe, files, env_value, with_prompts=True):
    with tempfile.TemporaryDirectory() as g:
        if with_prompts:
            os.makedirs(os.path.join(g, "prompts"))
        for f in files:
            p = os.path.join(g, "prompts", f)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            Path(p).write_bytes(b"x")
        env = {k: v for k, v in os.environ.items() if k != "HOOKSHOT_PS5_PROMPTS"}
        if env_value is not None:
            env["HOOKSHOT_PS5_PROMPTS"] = env_value
        out = subprocess.run([str(exe), g], check=True, capture_output=True, text=True, env=env).stdout
        made = os.path.isdir(os.path.join(g, "cache"))
    head, _, rest = out.partition("\n")
    got = dict(kv.split("=") for kv in head.split())
    said = "".join(line for line in rest.splitlines(True) if not line.startswith("made: "))   # (the lines logged)
    return {k: int(v) for k, v in got.items()}, said, made


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
    check("if (!ps5_prompts()) return 0;" in block(src, "static int made_dir("), "made_dir does not ask the switch first")
    check("ps5_prompts() && game_dir(" in block(src, "static int setup("), "setup looks at the disk before the switch")
    with tempfile.TemporaryDirectory() as tmp:
        exes = {}
        for ps5 in (True, False):
            c, exe = Path(tmp) / ("p%d.c" % ps5), Path(tmp) / ("p%d" % ps5)
            c.write_text(harness(src, ps5), encoding="utf-8")
            subprocess.run([a.cc, "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                            "-Wno-unused-const-variable", "-Wno-format-truncation", "-I", str(ROOT), str(c), "-o", str(exe)],
                           check=True)
            exes[ps5] = exe
        ps5, pc = exes[True], exes[False]

        # the console, the switch off: the base's eboot (nothing looked at, said or made), with the files there or not
        for value in (None, "0", "off", ""):
            r, said, made = run(ps5, FULL, value)
            check(r == {"setup": 0, "calls": 0, "logs": 0, "made": 0, "dir": 0} and not said and not made,
                  "ps5, HOOKSHOT_PS5_PROMPTS=%r: %r %r, a cache folder made: %s" % (value, r, said, made))
        # on, no prompts/ folder: none, and said
        r, said, made = run(ps5, [], "1", with_prompts=False)
        check(r["setup"] == 0 and "prompts/ is not there" in said and r["logs"] == 1,
              "ps5 on, no prompts/: %r %r" % (r, said))
        # on, the size table missing (the font would be dropped): none, and said
        r, said, _ = run(ps5, [f for f in FULL if f != "rsizetable.zs"], "1")
        check(r["setup"] == 0 and "rsizetable.zs is not there" in said and "the game's own icons" in said,
              "ps5 on, no size table: %r %r" % (r, said))
        r, said, _ = run(ps5, [f for f in FULL if f != "External.bfotf"], "1")
        check(r["setup"] == 0 and "External.bfotf is not there" in said, "ps5 on, no font: %r %r" % (r, said))
        # on, everything: served, one line, cache/prompts/ made in the game folder
        r, said, made = run(ps5, FULL, "1")
        check(r["setup"] == 1 and r["made"] == 1 and r["dir"] == 1 and made and r["logs"] == 1 and
              "on (HOOKSHOT_PS5_PROMPTS)" in said and "cache/prompts/" in said, "ps5 on, every file: %r %r" % (r, said))
        # on, parts missing: served, each said
        r, said, _ = run(ps5, ["External.bfotf", "rsizetable.zs", "Common.blarc", "ring.bin", "ps5/hint.bin"], "1")
        check(r["setup"] == 1 and "Common.bntx: the textures keep the game's own" in said and
              "ps5/textures.bin: the textures keep the game's own" in said, "ps5 on, no textures: %r %r" % (r, said))
        r, said, _ = run(ps5, ["External.bfotf", "rsizetable.zs", "Common.bntx", "ps5/textures.bin"], "1")
        check(r["setup"] == 1 and "Common.blarc: the Talk ring and the ocarina hint" in said and "ring.bin: the Talk ring's places" in said and
              "ps5/hint.bin: the ocarina hint" in said, "ps5 on, no layouts: %r %r" % (r, said))

        # off the console: upstream's (prompts/ there is enough, the switch is not read)
        for value in (None, "0"):
            r, said, made = run(pc, FULL, value)
            check(r["setup"] == 1 and r["made"] == 1 and made and not said, "pc, HOOKSHOT_PS5_PROMPTS=%r: %r %r" % (value, r, said))
        r, said, made = run(pc, [], None, with_prompts=False)
        check(r["setup"] == 0 and not said, "pc, no prompts/: %r %r" % (r, said))
    print("ok: on the console the switch off looks at nothing and makes nothing; on, what is missing is said and the cache "
          "folder made; off the console as upstream")


if __name__ == "__main__":
    main()
